# Phase 6 — the benchmark

*Written before the code, per project convention.*

---

## 1. What has to come out of this

Two numbers, and the honesty around them:

1. Throughput and latency for all three backends across a concurrency sweep, **including the region
   where the event loop loses**.
2. The slow-client result: the pool's ceiling is connections in flight, not requests/sec.

Plus two supporting measurements: memory per established connection, and the cost of the shared
cache's mutex.

## 2. Methodology, and its limits stated up front

**The throughput sweep is closed-loop.** N virtual clients, each opening a connection, issuing one
request, reading to EOF, closing, and repeating for a fixed duration. This measures *capacity at a
given concurrency*, which is the right tool for finding the crossover between architectures.

It is also, deliberately, **not** the tool for showing the pool's ceiling. Under closed loop a slow
server simply receives fewer requests — queues never build, and head-of-line blocking is invisible.
That is why the ceiling has its own experiment (§4) rather than being expected to fall out of the
throughput curve. Reporting only the closed-loop sweep would understate the difference between the
architectures, and reporting only the slow-client test would overstate it.

**Connection-per-request.** The proxy forces `Connection: close` upstream and closes the client
connection when the response completes (phase 1 §4), so every request pays a full TCP setup and
teardown. This is uniform across all three backends, so it does not bias the comparison — but it
means the absolute req/s figures are dominated by connection setup and are not comparable to a
keep-alive proxy's numbers.

**The generator must not be the bottleneck.** It is calibrated against the origin directly first,
and that ceiling is reported alongside every result. A benchmark that measures its own load
generator is a very convincing way to be wrong.

**Everything runs on one machine.** Client, proxy and origin share a CPU and the loopback interface,
so absolute numbers are lower than a real deployment's and the three arms are contending with their
own load generator. The comparison between arms is still valid — they contend identically — but the
absolute figures should not be quoted as capacity.

**The cache is OFF for every I/O measurement** (`-C 0`), verified by phase 5's isolation test. It
has its own experiment in §5.

## 3. The concurrency sweep

Concurrency 1, 10, 50, 100, 250, 500. Each arm, each level, fixed duration, three runs, median
reported with the spread.

**The expected shape, and what would falsify it:** the pool should *win* at low concurrency — it has
no per-event bookkeeping and no state machine to re-enter, just a thread that runs the request
straight through. The event loop should catch up and pass it as concurrency rises. If the event loop
wins everywhere, the benchmark is measuring something other than what it claims.

## 4. The ceiling experiment

K slow clients, one per worker, then a normal request timed against them.

Phases 2 and 3 already showed the mechanism at small scale (2 workers: 2.18s vs 1.09s; 16 slow
requests: 1.09s event loop vs 8.68s 2-worker pool). This scales it to a realistic worker count and
measures the thing the bullet claims: with the pool saturated by slow clients at near-zero CPU, what
happens to an ordinary request.

## 5. The cache-contention experiment

Hot cache, rising concurrency, cache ON. The threaded backends take a mutex on every lookup; the
event loop takes nothing. This is the one measurement where the cache is a variable rather than a
disabled feature.

## 6. Files

`bench/loadgen.py` (asyncio, no dependencies, emits JSON), `bench/run.sh` (orchestrates the sweep),
`bench/analyse.py` (JSON → the tables in `RESULTS.md`).
