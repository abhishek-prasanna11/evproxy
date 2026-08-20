# Phase 2 — the bounded thread pool, and the ceiling it introduces

*Written before the code, per project convention.*

---

## 1. The problem with rung 1

Phase 1 spawns a thread per connection. Each one costs a kernel thread, a stack, and a slot in the
scheduler's run queue. At a few hundred connections this is fine. At ten thousand it is not: the
memory is real, and the scheduler spends its time moving between threads that are almost all blocked
on a socket rather than doing work.

There is also no back-pressure. The accept loop will happily create a thread for every connection
that arrives, so a burst translates directly into resource exhaustion. Nothing says "no".

This is the resource half of the C10K problem, and the bounded thread pool is its standard fix.

## 2. The design

A fixed number of worker threads drain a **bounded** queue of accepted connections. The accept loop
is the producer; the workers are the consumers.

```
  accept loop ──push(fd)──▶ ConnQueue (bounded, mutex + 2 condvars) ──pop()──▶ worker × N
       │                          │                                              │
       └── blocks when full ──────┘                                              └── serves one
           (back-pressure)                                                            connection to
                                                                                      completion
```

- **`not_empty_`** wakes a worker when work arrives.
- **`not_full_`** wakes the accept loop when a worker takes something.
- Both waits are `while`-guarded, not `if`-guarded: a condition variable may wake spuriously, and
  with several waiters the condition can be stolen between the notify and the wake.

### What happens when the queue is full

`push` blocks, so the accept loop stops calling `accept`. Pending connections then sit in the
kernel's listen backlog, and once *that* fills the kernel refuses new ones. That is real
back-pressure travelling all the way out to the client, and it is the correct behaviour: a proxy
that keeps accepting work it cannot start is just moving the queue somewhere less visible.

The alternative — accept everything and let the queue grow — trades a clear rejection for unbounded
memory and latency that rises without limit. Rejected.

## 3. What this fixes, and what it does not

**Fixes:** the resource explosion. Threads are now a fixed cost, chosen once, independent of load.
Memory is bounded, and the scheduler has N runnable threads instead of ten thousand.

**Introduces — and this is the point of the whole project:** with N workers, **exactly N connections
can be in flight.** A worker that is blocked reading from a slow client is not doing work. It is
holding a seat.

So the pool's limit is not requests per second. It is *connections in flight*, and the two are only
related when requests are fast. A proxy with 64 workers and 64 slow clients is completely dead while
using approximately no CPU — there is nothing to profile, nothing in the logs, and every throughput
benchmark you run against it will look healthy, because a throughput benchmark closes each
connection before opening the next.

Phase 6 measures this properly. This phase just has to make it true and demonstrate the mechanism at
small scale.

**The honest framing for the README:** a bounded thread pool does not solve C10K. It solves the
resource half and leaves the concurrency half untouched. That is not a criticism of thread pools —
it's the reason event-driven servers exist.

## 4. Design options considered

**(a) Unbounded queue.** Simpler, no `not_full_` condvar. Rejected: unbounded memory, and latency
that grows without any signal to the client.

**(b) Worker-per-connection handoff with work stealing.** Overkill at this scale and it would muddy
the comparison — the point is to measure the *standard* pool, not an optimised one.

**(c) Fixed workers, bounded queue, blocking push. Chosen.** The textbook producer-consumer, which
is exactly what should be on the other side of the comparison from an event loop.

**A note on what is deliberately NOT done:** a worker serves its connection to completion, including
all the waiting. It would be possible to have workers hand a connection back to the queue whenever
it would block — but that is a work-queue-driven event loop wearing a costume, and it would erase
the very ceiling this arm exists to demonstrate. The naive-but-standard version is the honest
comparison.

## 5. Files

| File | Holds |
|---|---|
| `conn_queue.{hpp,cpp}` | the bounded producer-consumer queue |
| `backend/thread_pool.cpp` | this phase's backend |
| `backend/factory.cpp` | `make_backend` moved out of `thread_per_conn.cpp` |

New config: `thread_pool_size` (default 64), `job_queue_capacity` (default 256).

## 6. Shutdown

Harder than phase 1. On the stop signal: close the queue, which wakes every blocked `pop` and
`push`; workers finish the connection they hold, then keep draining whatever was already accepted
before observing the closed-and-empty queue and returning; the accept loop joins them all.

`pop` returns false only when the queue is **closed and empty**, not merely closed. That one
distinction is what turns shutdown from "drop everything queued" into "serve what we already
accepted" — a connection we accepted is a promise, and dropping it silently after `accept` succeeded
would be indistinguishable to the client from the proxy crashing.

## 7. Experiment for this phase

1. **Differential:** `tests/phase1.sh thread_pool` must pass 12/12, unchanged. If the two backends
   disagree about anything, the phase 6 comparison is meaningless.
2. **The ceiling, in miniature:** 2 workers, 4 concurrent slow requests. The first two should be
   served together and the second two should wait, producing roughly two batches of latency rather
   than one. That is the mechanism that phase 6 scales up.
3. **Back-pressure:** queue capacity 1 with 2 workers, then a burst — nothing crashes, nothing is
   silently dropped, and every request that is accepted is answered.
4. TSan clean under concurrent load, since this arm has real shared state.

## 8. What actually went wrong

### The signal handler deadlocked against the queue

First version had `ThreadPool::stop()` — which runs *in the signal handler* — call `queue_.close()`.
That takes the queue mutex, and **`pthread_mutex_lock` is not async-signal-safe.** If the signal is
delivered on a thread that already holds that mutex, the handler blocks forever waiting for a lock
its own interrupted thread will never release.

The symptom was not a crash or a hang in an obvious place: the proxy simply ignored `SIGTERM` and
kept listening, so the test script's cleanup left a process behind and the *next* test bound to the
wrong listener. A deadlock in a handler looks exactly like "the signal wasn't delivered".

**Fix:** the handler sets an atomic and nothing else. `accept_loop` polls it; `run()` closes the
queue afterwards from normal context. That required `push()` to become `push_for(fd, timeout_ms)` —
otherwise an accept loop blocked on a full queue would never re-check the flag, which was the whole
reason the first version reached into the queue from the handler.

The blueprint already said `stop()` "must only touch atomics". Writing it down did not prevent it.

### A false pass in the test harness

Test 3 passed for the wrong reason. The script never stopped the proxy after test 2, so test 3's
proxy died on `EADDRINUSE` — but `wait_port` only proves *something* is listening, and the old
process was. So test 3 ran against the previous proxy's settings and sent `SIGTERM` to an
already-dead PID, then reported success.

**Fix:** `start_proxy` now asserts the child is still alive after `wait_port`. Checking that a port
is open is not the same as checking that *your* process opened it, and the difference is invisible
until it silently invalidates a result.

## 9. Expected failures

- `if`-guarded condvar waits instead of `while` — works until it doesn't, and then it is a
  once-a-week mystery.
- Forgetting to wake blocked pushers on shutdown, so the accept loop never returns and join hangs.
- Moving a move-only `Fd` out of the queue while holding a reference to it — use-after-move on the
  descriptor, which shows up as a close of fd -1 or, worse, a double close.
