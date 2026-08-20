# Phase 7 — one event loop per core

*Written before the code, per project convention.*

---

## 1. Why this phase exists

Phase 6 measured the event loop at **3–4× lower throughput** than either threaded backend, and
RESULTS.md §1 explained it with a claim:

> A single-threaded event loop is a single thread. The threaded backends spread the same work across
> the whole machine. This is why nginx runs **one event loop per core**.

**That is a hypothesis, not a result.** It was asserted from the architecture, not measured. This
phase tests it: if the deficit is really "one core out of ten", then running N loops should recover
throughput roughly proportionally, and the curve should flatten near the core count.

If it doesn't scale, the explanation was wrong and something else is costing the throughput — in
which case the honest move is to say so and go find it.

## 2. Design: `SO_REUSEPORT`, not a shared accept loop

Each shard is a thread with **its own kqueue and its own listening socket**, all bound to the same
port with `SO_REUSEPORT`. The kernel distributes incoming connections across them.

```
                    ┌── shard 0: listener + kqueue + cache ── thread
   clients ──:8888──┼── shard 1: listener + kqueue + cache ── thread
                    ├── shard 2: ...
                    └── shard N: ...
```

The alternative — one acceptor thread handing descriptors to loops — needs a wakeup channel per loop
and a shared queue, which reintroduces exactly the synchronisation this architecture exists to
avoid. `SO_REUSEPORT` has **no shared state at all** between shards, which is the point.

**`SO_REUSEPORT` is enabled only when sharding is requested.** Setting it unconditionally would be a
trap: a second `evproxy` on the same port would silently bind and split traffic instead of failing
with `EADDRINUSE` — and phase 2's "is this really *my* process?" guard depends on that failure.

## 3. Sharding invalidates the unlocked cache — and that is the interesting part

Phase 5 gave the event loop `LruCacheImpl<NullMutex>` because it is single-threaded, with the header
saying plainly: *if the event loop ever grows a second thread, this becomes a data race.*

This phase grows it a second thread. So the choice is forced:

| option | cost |
|---|---|
| one shared cache with a mutex | reintroduces the contention the event loop was avoiding |
| **one cache per shard** | no lock, but N× the memory and a lower hit rate — a request can miss in shard 3 for something shard 1 already has |

**Chosen: one cache per shard.** It preserves the no-lock property that makes this architecture worth
having, and it is what shard-per-core servers actually do. The cost is real and is stated: with N
shards and a uniform key distribution, the effective hit rate for a given entry drops because each
shard sees only its own share of traffic.

This is the honest resolution of phase 5's safety comment, not a workaround for it.

## 4. Experiment

Throughput at a fixed concurrency (500) with `--loops` = 1, 2, 4, 8, against the same fast origin
from phase 6, cache off. Compared to the two threaded backends measured in the same session.

**What would confirm the hypothesis:** throughput scaling roughly linearly to ~4 loops and flattening
as the machine (10 cores, shared with the origin and generator) runs out.

**What would refute it:** flat or near-flat scaling, meaning the per-request cost is somewhere other
than CPU — most likely the two `kevent` syscalls per state transition that phase 4's epoch fix made
unconditional.

Correctness first: the differential suite must pass with sharding on. Multiple listeners on one port
is exactly the kind of change that breaks accept handling in ways a throughput number will not show.

## 5. Results — the hypothesis is NOT supported

### 5.1 macOS `SO_REUSEPORT` does not load-balance TCP accepts

Measured directly. Four shards, four listeners on port 18888, 19,215 connections offered:

| shard 0 | shard 1 | shard 2 | shard 3 |
|---|---|---|---|
| 0 | 0 | 0 | **19,215** |

**The last socket bound received everything.** Linux's `SO_REUSEPORT` (since 3.9) hashes the 4-tuple
across listeners; FreeBSD has a separate `SO_REUSEPORT_LB`; macOS has neither for TCP. The design in
§2 cannot work on this platform, and the extra shards were pure overhead — three idle threads and
three idle kqueues — which is why the first scaling attempt was often *slower* than one loop.

Replaced with the acceptor hand-off explicitly rejected in §2: one thread accepts and round-robins
descriptors into per-shard inboxes over a mutex plus a wakeup pipe. Distribution is then exact:

| shard 0 | shard 1 | shard 2 | shard 3 |
|---|---|---|---|
| 1,987 | 1,987 | 1,986 | 1,986 |

**This reintroduces exactly the shared state `SO_REUSEPORT` was chosen to avoid** — a mutex and a
pipe write per connection, on a single acceptor thread. Forced by the platform, not chosen.

### 5.2 Sharding did not recover throughput

400 connections, 5 s, 5 runs, cache off, fewer competing processes:

| shards | runs (req/s) | median | min–max |
|---|---|---|---|
| 1 | 2,991 / 4,491 / 5,572 / 5,381 / 5,229 | **5,229** | 2,991–5,572 |
| 8 | 4,690 / 4,590 / 2,163 / 4,439 / 5,103 | **4,590** | 2,163–5,103 |

**Eight loops are not faster than one.** RESULTS.md §1 explained the event loop's 3–4× throughput
deficit as "it uses one core out of ten, which is why nginx runs one loop per core". This phase
tested that and **the data does not support it.**

Correctness is not the problem: the differential suite passes 13/13 at 1, 2, 4 and 8 shards, and
distribution is even.

### 5.3 Two candidate explanations, not distinguished by this data

**(a) The per-transition syscall cost dominates, so more cores cannot help.** Every `want()` change
costs an `EV_DELETE` plus an `EV_ADD`, and the phase 4 epoch fix made that unconditional. A request
transitions several times, so the syscall count per request is high and is paid on whichever core
runs it. Batching the kqueue changelist into one `kevent` per loop iteration would test this.

**(b) The acceptor is the new bottleneck.** Every connection now passes through one thread, one
mutex and one pipe write. At ~5,000 conn/s that is the serialisation point, and adding shards behind
it cannot help. This is the cost macOS imposed by not supporting load-balanced `SO_REUSEPORT`.

### 5.4 How much to trust this

**Softly.** Run-to-run spread is roughly 2× within each configuration (2,991–5,572 at one shard) on
a machine where the origin, the generator and the proxy share 10 cores. The refutation is that
sharding produced *no visible improvement*, not that it produced a measured regression. A cleaner
result needs the load generator and origin on separate hardware.

What is solid: the `SO_REUSEPORT` finding (§5.1), the even distribution, and correctness at every
shard count.

## 6. Expected failures

- A shard's cache being handed out where a shared one is expected, so entries vanish between requests.
- `SO_REUSEPORT` left on for the single-shard case, silently disabling the duplicate-bind guard.
- Shutdown: N threads, N listeners, N kqueues — the phase 2 lesson applies N times over, and `stop()`
  still may only touch atomics.
- Uneven distribution: the kernel balances by connection, not by work, so one shard can end up with
  the slow clients. Worth looking at per-shard counts rather than assuming fairness.
