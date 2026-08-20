# Phase 4 — the blocking resolver, measured and then fixed

*Written before the code, per project convention.*

---

## 1. The problem

Phase 3 made every socket operation non-blocking. One call was left alone on purpose:

```cpp
::getaddrinfo(host, port, &hints, &res);
```

**`getaddrinfo` blocks, and there is no non-blocking form of it.** It talks to the network, reads
`/etc/hosts`, may consult mDNS, and on a bad day waits out a DNS timeout of several seconds.

In a thread-per-connection or pool backend that costs one thread. In a single-threaded event loop it
costs *everything*: while that one call is inside the resolver, the loop is not calling `kevent`, so
ten thousand connections that have nothing to do with this hostname make no progress at all.

This is the most common way a hand-written event loop turns out not to be an event loop. It is also
close to invisible: the symptom is **random p99 spikes across every connection**, uncorrelated with
the request that caused them, so the natural suspects are the scheduler, the load generator, or the
kernel — never a DNS lookup on some unrelated request.

## 2. Build it wrong first

Phases 1–3 shipped the naive version deliberately. This phase measures it before fixing it, because
"we moved DNS off the loop" is an assertion and "one 2-second lookup added 2 seconds to the p99 of
every unrelated in-flight request, and here it is at zero afterwards" is a result.

Both arms live behind `async_resolve`, one flag, same binary — the same discipline as the three
backends.

### Making it measurable

A real slow resolver is not reproducible on demand, so the delay is **injected**: `resolve_delay_ms`
sleeps inside the resolve path before `getaddrinfo`. That is fault injection, stated plainly rather
than dressed up as a natural measurement. It models exactly the thing being studied — a lookup that
takes a long time — and it applies identically in both arms, so the comparison is fair.

### The experiment

1. Warm the cache for host A, so requests to A need no lookup at all.
2. Fire one request to host B, whose lookup is injected with a 2s delay.
3. 100ms later, fire eight requests to host A — cached, no lookup, nothing to wait for.
4. Measure how long the **A** requests take. They have nothing to do with B.

Naive arm: the loop is inside `getaddrinfo` for B, so the A requests sit untouched until it returns.
Fixed arm: the loop never blocks, so the A requests complete immediately while B waits.

## 3. The fix

```
   Connection                Resolver                     resolver threads
       │                        │                                │
       ├── start(host, port) ──▶│                                │
       │                        ├── cache hit? ──▶ return Hit ───┤ (no thread, no fds)
       │                        │                                │
       │◀── Pending + job ──────┤── enqueue ────────────────────▶│ getaddrinfo (blocking, here)
       │                        │                                │
   want() == ReadResolver       │                                │
   waits on job's pipe fd ◀─────────────── 1 byte written ───────┤
       │
       └── take endpoints, begin_connect()
```

### Why a pipe per job, rather than kqueue's EVFILT_USER

`EVFILT_USER` would be the natural kqueue mechanism, but it exists only in the event loop — and the
whole project depends on all three backends driving the *same* `Connection`. A pipe is a file
descriptor, so `Want::ReadResolver` is just another "wait until this fd is readable", which the
blocking backends already handle with `poll` and the event loop already handles with `EVFILT_READ`.
No backend needs to know that resolution is special.

The cost is two descriptors per *in-flight lookup*. Cache hits allocate nothing at all, which is
what keeps it cheap in the common case.

### The cache

`host:port → (endpoints, expiry)`, TTL-bounded, mutex-protected. This is not just an optimisation:
without it, every request pays a lookup, and the resolver thread pool becomes the new ceiling — N
resolver threads means N concurrent lookups, and we would have rebuilt phase 2's problem one layer
down.

TTL exists because DNS records change. Caching forever is a correctness bug that shows up as "the
proxy kept sending traffic to the old server for hours".

## 4. What this does *not* fix

The resolver threads still block — that is the point of putting them somewhere that can afford it.
With `resolver_threads = 4`, five simultaneous uncached slow lookups mean the fifth waits. That is a
real, bounded limitation, and the honest framing is that the loop is now insulated from DNS rather
than DNS having been made fast.

## 5. Files

| File | Holds |
|---|---|
| `resolver.{hpp,cpp}` | thread pool, request queue, TTL cache, `ResolveJob` |
| `connection.{hpp,cpp}` | new `Resolving` state and `Want::ReadResolver` |
| `socket.{hpp,cpp}` | `make_pipe` helper |

New config: `async_resolve` (default 1), `resolver_threads` (4), `dns_cache_ttl_s` (60),
`resolve_delay_ms` (0, injection only).

## 6. Results

**The stall, measured on the collateral damage:** with a 2000ms lookup injected, the worst of eight
requests that needed **no lookup at all** took **1.89s** on the naive arm and **0.02s** after the
fix — **94.5×**. The slow request itself still succeeded in both arms: the wait was moved, not
dropped.

All three backends pass 13/13; phase 2 6/6, phase 3 5/5, phase 4 5/5. Clean under ASan+UBSan and
TSan on every backend.

## 7. Three bugs, and one broken verification method

### 7.1 An unhandled enum case became a silent hang — and I had hidden the warning

Adding `Want::ReadResolver` left `Connection::want_fd()`'s switch without a case for it, so it fell
through and returned `-1`. Every backend reads that as "nothing to wait on" and drops the
connection: **every cache MISS failed, every cache HIT worked.**

The compiler had said so exactly — `warning: enumeration value 'ReadResolver' not handled in switch
[-Wswitch]` — but the build output was being filtered with `grep -E "error|Built target"`, so the
warning scrolled past unseen. `-Werror` is now on. A warning you filter out is a warning you do not
have.

### 7.2 fd-number reuse made a changed registration look unchanged

The event loop decided whether to re-register by comparing `(fd, direction)` before and after
`on_ready()`. Trying the next candidate address closes one socket and opens another — which very
often **reuses the same fd number**. The pair compared equal, so the loop skipped re-arming, while
the kernel had already dropped the registration along with the closed descriptor. The new socket was
registered nowhere and the connection hung forever.

Phase 3 never hit this because a loopback connect succeeds on the first candidate. `localhost`
resolving to `::1` *and* `127.0.0.1` is what exposed it.

Fixed with an **epoch counter** bumped on every `want_` change; the loop compares epochs, not
descriptors.

### 7.3 The result publication was a real data race

`finish_resolve()` read `job->ok`, `job->endpoints` and `job->err` as plain fields, relying on the
pipe write to order them. **It does not.** A syscall is not a synchronisation edge in the C++ memory
model, so those reads were UB no matter how reliably the hardware behaved. TSan flagged it
immediately. Now published with `ready.store(release)` and consumed with `ready.load(acquire)`.

Related, found by the same run: `read_some()` uses `recv()`, which fails with `ENOTSOCK` on a pipe.
The wakeup byte is drained with `::read()`.

### 7.4 The detached threads outlived the Resolver

`thread_per_conn` detaches its threads and waited only 5s for `live_` to reach zero before
returning. Under TSan that timed out, `main` destroyed the `Resolver`, and a still-running detached
thread was inside `Resolver::cache_get()` — destroying the cache under a live reader.

The fix is a longer, loudly-logged drain barrier plus decrementing `live_` **after** the
`Connection` is destroyed, with release/acquire on the counter. Worth naming as a property of the
architecture rather than a slip: **with no handles to join, a counter is the only shutdown barrier
available**, and it has to be placed with care. The pool joins its workers; the event loop has one
thread. Only rung 1 has this problem.

### 7.5 The verification method itself was broken

Phases 1–3 reported "TSan clean". **That check was vacuous.** The proxy's stderr goes to
`$WORK/proxy.log`, which the test script deletes on exit — and the grep for `WARNING:
ThreadSanitizer` was run against the *script's* output, which never contains it. Sanitizer reports
were being thrown away, and a clean grep meant nothing.

Every test script now asserts on the proxy's own log and copies it to
`/tmp/evp_sanitizer_report.log` on failure. Re-run afterwards, phases 1–3 are genuinely clean — but
they were not verified before, and the earlier claims should be read as unproven rather than false.

## 8. Expected failures

- Writing the wakeup byte before the result is visible to the reader — the connection wakes, reads
  `endpoints` that are still empty, and 502s intermittently. Publish the result *then* signal.
- Leaking the pipe fds when a connection is cancelled mid-lookup.
- A resolver thread still running while `Connection` objects are being destroyed at shutdown.
- Caching failures as though they were results, so one transient outage poisons a hostname for the
  whole TTL.
