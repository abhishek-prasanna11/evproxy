# evproxy — an HTTP proxy with three I/O architectures, measured

**Status:** blueprint, 2026-08-20. No code yet.
**Language:** C++20, written from scratch. **Size:** ~1500 lines, ~2 weeks.
**Scope anchor:** the two bullets in §2.

---

## 1. What this is

An HTTP forward proxy that implements **three different I/O architectures behind one flag** and
measures where each one breaks:

1. **thread-per-connection** — one thread per client
2. **bounded thread pool** — N workers draining a queue
3. **kqueue event loop** — one thread, non-blocking sockets, explicit connection state machines

The proxy is not the project. The project is the **ladder** — three architectures, one workload, and
a specific failure mode measured for each. Each rung fixes the previous rung's limit and introduces
a new one, and the last rung is not universally best.

**Why this is the interesting version of "I built a proxy":** most people build one architecture and
assert it's the good one. The event loop is *not* faster at low concurrency — it pays multiplexing
overhead for nothing. The whole finding is where the crossover sits and what each design actually
dies of.

### 1.1 The claim you must defend

**A bounded thread pool does not solve C10K. It solves the wrong half of it.**

Thread-per-connection dies of *resources*: 10,000 threads means 10,000 stacks and a scheduler run
queue that thrashes. A bounded pool fixes that — fixed workers, back-pressure, no explosion. But it
introduces a ceiling nobody mentions: **with N workers, exactly N connections can be in flight.** A
worker blocked on `recv` from a slow client is unavailable, doing no work, holding a seat.

So 64 slow clients kill a 64-worker proxy while it uses ~0% CPU. That is not a throughput problem,
and no throughput benchmark will ever show it. It is a **concurrency ceiling**, and it's why real
proxies (nginx, HAProxy, Envoy) are event-driven.

An event loop decouples connections from threads: a connection is a few hundred bytes of state, not
a thread, and the loop only touches it when the kernel says it's ready. 10,000 idle-ish connections
cost memory, not scheduling.

**What the event loop costs:** every operation becomes a state machine. There is no call stack to
hold your place across a blocking call, so partial reads, partial writes, and half-open connections
all become explicit states you manage by hand. Plus one non-obvious trap — §4.4.

---

## 2. The two résumé bullets

> **Built an HTTP forward proxy in C++20 implementing three interchangeable I/O architectures —
> thread-per-connection, bounded thread pool, and a kqueue event loop — and measured each one's
> failure mode: at **N** concurrent connections the event loop sustained **X req/s** on **A MB** RSS
> against the pool's **Y req/s** on **B MB**.**

> **Isolated the bounded pool's real limit rather than quoting throughput: with **K** slow clients —
> one per worker — the thread-pool proxy stopped serving entirely at near-zero CPU, while the event
> loop held **P%** of baseline. The pool's constraint is connections in flight, not requests/sec.**

The second bullet is the one that separates this from every other proxy repo. Throughput numbers are
common; a measured concurrency ceiling with the mechanism named is not.

### 2.1 On lineage

Write this from scratch. Consult your own notes and design docs freely — that's your prior work —
but no copy-paste, and this is a different architecture, not a translation.

The bullets above describe what the software does and what you measured, all of which is true. A
résumé doesn't narrate development history, so there's nothing to disclose there. If an interviewer
asks whether you've built a proxy before, the answer is yes and it's a good answer — it's why you
knew the thread pool's ceiling was worth measuring in the first place.

---

## 3. Architecture

```
                     ┌──────────────────────────────────────┐
   clients ──────▶   │  IoBackend  (one of three, at build  │
                     │             or config time)          │
                     │  ├─ ThreadPerConn                    │
                     │  ├─ ThreadPool(N)                    │
                     │  └─ EventLoop(kqueue)                │
                     └───────────────┬──────────────────────┘
                                     │  drives
                     ┌───────────────▼──────────────────────┐
                     │  Connection — an explicit state       │
                     │  machine, identical in all three      │
                     │  backends                             │
                     └───────────────┬──────────────────────┘
                                     │
                     Resolver (§4.5) ─┴─▶ origin server
```

**The critical design constraint: `Connection` must be backend-agnostic.** All three backends drive
the *same* state machine over the *same* parser and buffers. If the blocking backends share code
with the event loop, then the benchmark isolates the I/O architecture rather than comparing three
different programs. Get this wrong and the whole comparison is meaningless.

Practically: `Connection::on_readable()` / `on_writable()` never block and never own a thread. The
blocking backends call them in a loop after their own `recv`; the event loop calls them when kqueue
says so.

---

## 4. Design

### 4.1 Connection state machine

```
READING_REQUEST → RESOLVING → CONNECTING → SENDING_UPSTREAM → RELAYING → CLOSING
```

Each connection owns: client fd, upstream fd, a read buffer, a write buffer, an offset into each,
and a state. Non-blocking I/O means **every read is partial and every write is partial** — a `send`
returning less than you gave it is normal, not an error, and the remainder must be buffered and
retried on the next writable event.

That single fact is the main source of bugs in this project. Write the buffer handling once,
carefully, and use it in all three backends.

### 4.2 HTTP parsing

Absolute-form request lines (`GET http://host/path HTTP/1.1`), `Host` header, `Content-Length`.
Reject what you don't support with 400/501 rather than guessing. Relay responses byte-for-byte —
this is a proxy, not a rewriter.

### 4.6 LRU response cache — and why it makes the comparison *better*

An O(1) LRU cache (hash map + intrusive doubly-linked list) in the shared layer, checked before the
upstream path, storing only complete `200 OK` responses.

The obvious objection is that cached requests skip the upstream path and therefore blur the I/O
comparison. That's solved by a flag, not by omission: **the three-arm I/O benchmark runs with the
cache disabled**, exactly the isolation discipline used elsewhere in this portfolio. Production
config is cache-on; the comparison runs cache-off.

And the cache pays for itself, because it exposes a difference between the architectures that a
cacheless proxy cannot:

| Backend | What the shared cache costs |
|---|---|
| thread-per-connection | a mutex, contended by every live connection |
| bounded thread pool | a mutex, contended by N workers |
| event loop | **nothing — single-threaded, no lock at all** |

That is one of the actual reasons production servers went event-driven or shard-per-core: shared
mutable state stops needing synchronisation when only one thread touches it. With a hot cache at
high concurrency, the mutex becomes a measurable contention point for the threaded backends and a
no-op for the event loop.

So the cache adds a **third measurement** (§7) rather than muddying the first two. Implement it once
in the shared layer with the lock behind a policy type — `std::mutex` for the threaded backends, a
no-op lock for the event loop — so the two configurations are the same code and the comparison is
honest.

### 4.7 Graceful shutdown

`SIGINT`/`SIGTERM` stops accepting, drains in-flight connections, then exits 0. Worth doing because
it is a *different problem* in each backend: the threaded backends signal and join workers; the event
loop has no threads to join, so it must close the listener, keep servicing existing connections until
their state machines reach `CLOSING`, and then break the loop. Assert in tests that an in-flight
request completes rather than being dropped.

### 4.3 kqueue

- `kqueue()` once; `kevent()` to register and to wait.
- `EVFILT_READ` for readability, `EVFILT_WRITE` for writability.
- **Never leave `EVFILT_WRITE` armed permanently.** A socket with room in its send buffer is
  *always* writable, so a level-triggered write filter that's always registered spins the loop at
  100% CPU doing nothing. Register the write filter only when you have buffered data to flush, and
  delete it when the buffer drains. (`EV_CLEAR` for edge-triggering is the alternative, but then you
  must drain until `EAGAIN` on every wakeup or you'll hang.) Document which you chose and why.
- Every socket non-blocking via `fcntl(fd, F_SETFL, O_NONBLOCK)` — accepted sockets **and** upstream
  sockets. An accepted socket does not inherit the listener's flags.

### 4.4 Non-blocking connect — the classic trap

`connect()` on a non-blocking socket returns `-1` with `errno == EINPROGRESS`. That is success-so-far,
not failure. Completion arrives as a **writability** event — at which point you must call
`getsockopt(fd, SOL_SOCKET, SO_ERROR, ...)` to find out whether it actually connected. A socket whose
connect *failed* also becomes writable, so skipping the `SO_ERROR` check gives you a proxy that
cheerfully relays requests into a dead connection.

### 4.5 DNS — the thing that quietly breaks event loops

**`getaddrinfo` blocks.** One lookup against a slow resolver stalls the entire event loop — all
10,000 connections — for the duration. It is the single most common way a hand-written event loop
turns out not to be an event loop.

Phase 4 is built around this, deliberately, as a **measure-then-fix**:

1. Ship the naive version — `getaddrinfo` called inline from the loop.
2. Measure the stall: point some requests at a deliberately slow resolver and record the latency
   inflicted on *unrelated* in-flight connections. That number is the finding.
3. Fix it: a small dedicated resolver thread pool plus a hostname→addr cache with a TTL. The loop
   posts a resolve request and moves on; the connection sits in `RESOLVING` until the answer lands.

Same "build it wrong, measure, then fix" shape as the rest of your portfolio, and here the wrong
version is what almost everyone actually ships.

---

## 5. Phases

`docs/phaseN.md` written before each phase's code.

| # | Work | Days |
|---|---|---|
| 1 | Sockets, HTTP parse, upstream relay, `Connection` state machine, **thread-per-connection** backend | 3 |
| 2 | **Bounded thread pool** backend — fixed workers, bounded queue, back-pressure | 1.5 |
| 3 | **Event loop** backend — non-blocking everything, kqueue, partial I/O, `SO_ERROR` handling | 4 |
| 4 | Resolver: measure the blocking-DNS stall, then fix with resolver threads + cache | 2 |
| 5 | O(1) LRU response cache with a policy-typed lock + graceful shutdown in all three backends | 1.5 |
| 6 | Load generator, three-arm benchmark, cache-contention measurement, `RESULTS.md`, README | 3 |

**~2.5 weeks total.** Phases 1–4 and 6 are the spine; phase 5 is what makes it a complete proxy
rather than a benchmark harness, and it's cheap because the LRU design is already familiar ground.

**Optional phase 7 (+1.5d):** the hybrid every production proxy actually uses — an event loop for
I/O plus a small worker pool for anything CPU-bound. Adds a fourth arm and the honest conclusion
that the answer is "both", not "event loops win".

---

## 6. Correctness before benchmarking

The three backends must be **behaviourally identical**. Benchmarking three programs that don't agree
on what they serve is measuring noise.

- **Differential test:** the same 200-request script through all three backends produces
  byte-identical responses. Run it after every change to any backend.
- **Partial-I/O torture:** an origin that dribbles its response a few bytes at a time with delays,
  and a client that reads slowly. This is what breaks buffer handling, and it breaks it silently —
  the response looks fine until it's truncated at exactly the wrong byte.
- **fd-leak assertion:** after N requests and all connections closed, the open-fd count must return
  to its starting value. An event loop leaks descriptors mercilessly if any error path forgets to
  close, and it presents as a mysterious failure at connection ~250, not as a crash.
- **Sanitizers:** ASan/UBSan on all three, TSan on the two threaded backends.

---

## 7. Benchmark design

Three arms, one workload, everything else held constant.

| Measurement | Why |
|---|---|
| Throughput and p50/p99 latency at concurrency 1, 10, 100, 1 000, 5 000, 10 000 | The main curve — and the crossover where the pool loses |
| RSS at N established connections | Memory per connection, the resource story |
| **Slow-client test:** K slow clients (one per worker), then measure whether a normal request is still served | **Bullet 2.** The pool should collapse at K = worker count while nearly idle |
| Thread-per-conn ceiling: raise N until it degrades, record where and why | Rung 1's failure mode |
| DNS stall, before and after phase 4's fix | The event-loop-specific finding |
| **Cache-lock contention:** hot cache, rising concurrency, threaded backends vs event loop | §4.6 — the cost of shared mutable state, which the event loop simply doesn't pay |
| Cache hit vs miss latency (origin fetch vs served copy) | The cache's own value, measured once |

**All three-arm I/O measurements run cache-off.** The cache is a separate, controlled experiment —
never a variable inside the architecture comparison.

**Report the crossover honestly.** The thread pool should *win* below a few hundred connections —
lower per-request overhead, no state-machine bookkeeping. A result that shows the event loop winning
everywhere means the benchmark is wrong.

### 7.1 Practical blockers to handle on day one

- **`ulimit -n` defaults to 256 on macOS.** You need ~10 000 on both proxy and load generator.
  `ulimit -n 10240`, and raise `kern.maxfiles`/`kern.maxfilesperproc` if needed. Call `setrlimit`
  in-process too, and **log the effective limit at startup** — silently hitting the fd ceiling looks
  exactly like a proxy bug.
- **Ephemeral port exhaustion.** Client and proxy on one machine share ~16k ephemeral ports, and
  `TIME_WAIT` holds them. Cap the test at ~10 000 connections, or bind the generator across several
  loopback aliases.
- **`ab` can't do this.** It won't hold 10 000 connections and can't model slow clients. Write the
  generator yourself — Python `asyncio` is fine and it's not the part being measured — with knobs for
  connection count, request rate, and per-connection slowness.
- **The generator must not be the bottleneck.** Verify by running it against a trivial static
  responder first and confirming it can saturate that. Report the generator's own ceiling in
  `RESULTS.md`.

---

## 8. Repo layout

```
evproxy/
  BLUEPRINT.md  README.md  RESULTS.md
  CMakeLists.txt  evproxy.conf
  src/
    main.cpp
    config.{hpp,cpp}
    socket.{hpp,cpp}        # RAII fd wrapper, non-blocking helpers, send_all/recv semantics
    http.{hpp,cpp}          # request line + headers, absolute-form parsing
    connection.{hpp,cpp}    # the backend-agnostic state machine
    cache.{hpp,cpp}         # O(1) LRU, lock as a policy type (mutex | no-op)
    resolver.{hpp,cpp}      # resolver threads + TTL cache
    backend/
      backend.hpp           # the interface all three implement
      thread_per_conn.cpp
      thread_pool.cpp
      event_loop.cpp        # kqueue
  bench/
    loadgen.py  slow_client.py  slow_origin.py  analyse.py
  tests/
    differential.sh  partial_io.sh  fd_leak.sh
  docs/ phase1..5.md
```

An **RAII fd wrapper is not optional** here. Three backends and a dozen error paths across a state
machine is exactly where raw `close()` calls get missed, and the fd-leak test will catch it after
you've wasted an afternoon. This is also the clearest place C++ earns its keep over C.

---

## 9. Scope boundaries

**Out:** HTTPS/`CONNECT` tunnelling (would need TLS or blind tunnelling; orthogonal to the I/O
question) · HTTP/2 · `io_uring`/`epoll` (Linux-only; kqueue is the macOS primitive and portability
isn't the point) · load balancing across origins · auth · a config-reload story · cache
revalidation and `Cache-Control` semantics (a simple LRU over complete 200s is the scope; real HTTP
caching is a project of its own) · Docker and k8s (HookRelay owns those).

**Keep the feature set minimal on purpose.** Three architectures is already three implementations to
keep behaviourally identical; every proxy feature added multiplies that.

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| Event-loop bugs are silent and timing-dependent | §6's differential + partial-I/O tests, written in phase 3, not after |
| The three backends drift and stop being comparable | Shared `Connection`/parser by construction (§3); differential test in CI-like local script |
| Benchmark measures the load generator | Calibrate against a trivial responder first; report the generator's ceiling |
| fd limits / port exhaustion masquerading as proxy bugs | §7.1, handled on day one, effective limits logged at startup |
| Scope drift into a full proxy feature set | §9 |
| Phase 3 overruns — it's the hard one | It's budgeted at 4 days for a reason; if it slips, cut optional phase 6, not the tests |

---

## 11. Definition of done

- [ ] Three backends, selectable by flag, driving one shared `Connection` state machine.
- [ ] Differential test: byte-identical responses across all three on a 200-request script.
- [ ] Partial-I/O torture and fd-leak assertions passing on all three.
- [ ] ASan/UBSan clean on all three; TSan clean on both threaded backends.
- [ ] Throughput/latency curve across concurrency 1 → 10 000 for each backend.
- [ ] Slow-client experiment showing the pool's concurrency ceiling at worker count.
- [ ] DNS stall measured before the fix and after.
- [ ] `RESULTS.md` naming hardware, fd limits, generator ceiling, ≥3 runs, median and spread.
- [ ] README states the crossover honestly — including where the event loop *loses*.

---

## 12. Questions this project must answer

1. Why doesn't a bounded thread pool solve C10K? *(§1.1 — it fixes resource explosion, not the
   concurrency ceiling. With your slow-client number attached.)*
2. When is an event loop the wrong choice? *(Below the crossover, and for anything CPU-bound — one
   slow handler stalls every connection.)*
3. What does a connection cost in each architecture? *(A thread + stack; a queue slot + a worker; a
   few hundred bytes of state.)*
4. Why did DNS break your event loop, and how did you find it? *(§4.5 — and you measured the stall
   inflicted on unrelated connections.)*
5. `connect()` returned EINPROGRESS. Now what? *(§4.4 — wait for writable, then check `SO_ERROR`.)*
6. Why not leave `EVFILT_WRITE` registered? *(§4.3 — always-writable sockets spin the loop.)*
7. How do you know the three backends are comparable? *(§6 differential test — and if they weren't,
   the benchmark would be meaningless.)*
8. Edge-triggered or level-triggered, and why? *(Whichever you chose — the answer is the trade
   between spinning and having to drain to EAGAIN.)*
9. Your cache needs a mutex in two backends and none in the third — why? *(§4.6. Single-threaded
   means shared mutable state needs no synchronisation, which is a real reason production servers
   went event-driven or shard-per-core. With your measured contention number attached.)*
10. Why does graceful shutdown look different in an event loop? *(§4.7 — no threads to join; you
   drain state machines instead.)*
