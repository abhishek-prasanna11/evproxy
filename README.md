# evproxy

An HTTP forward proxy in C++20 that implements **three I/O architectures behind one flag** and
measures where each one breaks.

```
thread-per-connection  →  dies of resources
bounded thread pool    →  dies of a concurrency ceiling
kqueue event loop      →  pays multiplexing overhead at low concurrency
```

The proxy is not the point. The **ladder** is: each rung fixes the previous rung's limit and
introduces a new one, and the last rung is not universally best.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/evproxy -p 8888 -b event_loop
curl -x http://127.0.0.1:8888 http://example.com/
```

`-b thread_per_conn | thread_pool | event_loop` · `-w` workers · `-C 0|1` cache · `-R 0|1` async
resolve · `-D ms` injected resolver delay

---

## The claim this project exists to test

**A bounded thread pool does not solve C10K. It solves the wrong half.**

Thread-per-connection dies of *resources* — ten thousand connections means ten thousand stacks and a
scheduler thrashing between threads that are almost all blocked on a socket. A bounded pool fixes
that: fixed workers, back-pressure, no explosion.

But it introduces a ceiling nobody mentions. **With N workers, exactly N connections can be in
flight.** A worker blocked on `recv` from a slow client is not doing work — it is holding a seat. So
64 slow clients kill a 64-worker proxy while it uses almost no CPU.

That is not a throughput problem, and **no throughput benchmark will ever show it**, because a
throughput benchmark closes each connection before opening the next. It is a concurrency ceiling,
and it is why real proxies are event-driven.

What the event loop costs: everything becomes an explicit state machine. There is no call stack to
hold your place across a blocking call, so partial reads, partial writes and half-open connections
all become states you manage by hand. And below the crossover it is *slower* than the pool.

## Results

See **[RESULTS.md](RESULTS.md)** for the full tables, the methodology, and the limits of both.

**The ceiling.** 64 slow clients, one per worker, then an ordinary request timed against them:

| backend | ordinary request | proxy CPU |
|---|---|---|
| thread pool (64 workers) | **2.14 s** | **0.0 %** |
| event loop | **0.01 s** | 3.1 % |

The pool cannot serve the request while using no CPU at all — 64 workers are each blocked on a slow
client, holding a seat. Reproduced across two independent runs.

**Memory per established connection**, 500 idle connections: **16.22 KB** (thread-per-conn) vs
**0.58 KB** (event loop), ~28×.

**What is NOT measured:** the throughput crossover. The Python origin and the asyncio load generator
saturate at ~2.4k req/s, so all three arms flatline there and the sweep cannot distinguish them —
the proxied arms measured *faster than the origin they proxy*, which is the signature of a shared
bottleneck rather than a real result. Reported as inconclusive in RESULTS.md §3 rather than dressed
up. The cache-contention measurement is likewise discarded: its sign flipped between runs.

## How the comparison is kept honest

**One shared `Connection`.** All three backends drive the same state machine over the same parser
and buffers. They differ only in *who waits and where*. Without that, the benchmark would be
comparing three different programs.

**A differential test.** The same 13 assertions run against all three backends and must pass
identically. If they disagree about what they serve, the performance numbers mean nothing.

**The cache is off for every I/O measurement.** It has its own experiment. Phase 5's isolation test
asserts that `cache_enabled=0` really produces zero hits.

**The generator is calibrated against the origin first**, and that ceiling is reported next to every
result. A benchmark that measures its own load generator is a very convincing way to be wrong.

**Median of three runs with the spread**, never a single number.

## Design notes worth reading

- **`Connection` never blocks and never owns a thread.** It states an intent — `ReadClient`,
  `WriteClient`, `ReadUpstream`, `WriteUpstream`, `ReadResolver` — and a backend decides when that is
  satisfiable. This decision in phase 1 is what makes phase 3 possible at all.
- **Exactly one kqueue registration per connection.** This makes the `EVFILT_WRITE` trap
  structurally impossible: a socket with room in its send buffer is *always* writable, so a
  permanently armed level-triggered write filter spins at 100% CPU while passing every correctness
  test. Here a write filter exists only while there are bytes to flush.
- **The event loop compares an epoch, not `(fd, direction)`.** Trying the next candidate address
  closes one socket and opens another, which often reuses the same fd number — the pair compares
  equal while the kernel has already dropped the registration.
- **`getaddrinfo` blocks and has no non-blocking form.** Moved to resolver threads with a TTL cache;
  completion is signalled over a pipe so it is just another readable descriptor and no backend needs
  to know resolution is special.
- **The cache's lock is a policy type.** `LruCacheImpl<std::mutex>` for the threaded backends,
  `LruCacheImpl<NullMutex>` for the event loop — the same code, one type parameter apart, so the
  measurement is of synchronisation cost rather than of two different implementations.

## Limitations

- **Single machine.** Client, proxy and origin share a CPU and the loopback interface. The
  comparison between arms is valid — they contend identically — but the absolute figures are not
  capacity numbers.
- **Connection-per-request.** The proxy forces `Connection: close` upstream and closes the client
  connection when the response completes, so every request pays a full TCP setup. Uniform across
  arms, but it means req/s here is dominated by connection setup and is not comparable to a
  keep-alive proxy.
- **macOS/kqueue only.** No `epoll` or `io_uring` port; portability is not the point.
- **No HTTPS/`CONNECT`, no HTTP/2, no keep-alive.**
- **Caching is a bounded LRU over complete 200s.** No `Cache-Control`, `ETag`, `Vary` or
  revalidation — real HTTP caching is a project of its own.
- **A loopback `connect()` completes synchronously**, so the automated suite never reaches
  `EINPROGRESS` and does not exercise the `SO_ERROR` check. Verified separately against a real
  remote origin; see `docs/phase3.md §9`.
- **Ephemeral-port pressure.** Connection-per-request at high concurrency burns through the
  ephemeral range, which is why the sweep tops out where it does and why runs are spaced.

## Layout

```
src/        connection.cpp is the shared state machine; backend/ holds the three architectures
docs/       phase1..6.md, written BEFORE each phase's code
tests/      phase1..5.sh — phase1.sh is the differential test, run against all three backends
bench/      loadgen.py (asyncio, no deps), run.sh, analyse.py
```

Each phase's document follows the same shape: concepts → the problem → options considered → the
chosen design → the experiment → **what actually went wrong** → lessons. The "what went wrong"
sections are the honest part; `docs/phase4.md §7` in particular records a case where the
verification method itself was broken and earlier "sanitizer clean" claims were vacuous.
