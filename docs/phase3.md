# Phase 3 — the kqueue event loop

*Written before the code, per project convention.*

---

## 1. The problem with rung 2

A bounded pool of N workers can have exactly N connections in flight. Phase 2 measured it: the same
four slow requests took 1.09s with eight workers and 2.15s with two. The workers were not busy —
they were *blocked*, holding seats.

The fix is to stop pairing a connection with a thread. A connection becomes a few hundred bytes of
state; one thread asks the kernel "which of these thousands of descriptors can make progress right
now?" and services only those. Idle connections cost memory, not scheduling.

## 2. What this costs

Everything becomes explicit. A thread has a call stack that remembers where you were across a
blocking call; an event loop has no such thing, so every "wait here" becomes a state you store and
return to. Phase 1 already paid most of this price by making `Connection` a state machine that never
blocks — this phase is where that decision earns its keep, because the event loop drives exactly the
same machine the other two backends drive.

## 3. Design

```
   kqueue
     ├── listener fd, EVFILT_READ ────▶ accept until EAGAIN, create Connection
     └── one registration per connection, and exactly one
             │
             └── the fd/filter that Connection::want() currently names
```

**One registration per connection at a time.** `Connection::want()` returns exactly one intent, so
the loop keeps exactly one `(fd, filter)` registered for it. After `on_ready()`, if the intent
changed, delete the old registration and add the new one.

This falls out of the phase 1 design rather than being an extra mechanism, and it has a useful
side effect: it makes the `EVFILT_WRITE` trap (§4) structurally impossible, because a write filter
only exists while the connection is actually trying to write.

`udata` carries the `Connection*`; `ident` carries the fd, so a handler knows which of the two
sockets fired.

## 4. The EVFILT_WRITE trap

A socket with room in its send buffer is *always* writable. So a level-triggered `EVFILT_WRITE` that
stays registered fires every single time round the loop, forever, and the process spins at 100% CPU
doing nothing. It does not look like a bug — it looks like the event loop is "busy".

Two ways out:

- **`EV_CLEAR`** (edge-triggered): only fires on a transition. Requires draining every ready fd until
  `EAGAIN` on each wakeup, or events are lost and connections hang.
- **Register the write filter only while there is buffered data to flush**, and remove it when the
  buffer drains.

**Chosen: the second**, because the one-registration-per-connection design already does it for free.
The connection only says `WriteClient`/`WriteUpstream` when it has bytes to push, so a write filter
only exists during that window. Level-triggered is also more forgiving of a partial drain, which
matters when the whole project is about partial I/O.

## 5. Non-blocking connect, and the SO_ERROR trap

Phase 1 called `connect()` blocking. Inside an event loop that stalls every other connection, so
this phase splits it:

```
resolve (still blocking -- phase 4)
   │
   ├─▶ socket(), set O_NONBLOCK, connect()
   │      └── returns -1/EINPROGRESS, which is success-so-far, not failure
   │
   ├─▶ wait for WRITABILITY
   │
   └─▶ getsockopt(fd, SOL_SOCKET, SO_ERROR) ◀── the part everyone skips
```

**A socket whose connect *failed* also becomes writable.** Skip the `SO_ERROR` check and the proxy
happily writes a request into a dead connection, then reports a confusing failure later — or hangs.
The symptom is intermittent, load-dependent, and looks like a buffer bug, which is the most
reasonable thing to suspect in a hand-written event loop and the wrong place to look.

This also gives address fallback honestly: `resolve` returns every candidate address, and a failed
connect advances to the next one before giving up with 502. That is how a host with an AAAA record
but no working IPv6 route still gets proxied.

**Deliberately still blocking: `getaddrinfo`.** That is phase 4's experiment — measure the stall it
inflicts on unrelated connections, then fix it. Building the naive version first is the point.

## 6. Lifetime and registration removal

Closing a descriptor removes its kevent registrations automatically. So after a connection finishes
and closes its fds, an `EV_DELETE` on those fds returns `ENOENT` — that is expected, not an error to
report. The loop tries the delete and ignores that specific failure.

The ordering that matters: capture the current `(fd, filter)` *before* calling `on_ready()`, because
`on_ready()` may close that very fd. Comparing intents afterwards without having saved the old fd
means deleting a registration for a descriptor number that may already have been reused.

## 7. Shutdown

`stop()` sets an atomic and nothing else — phase 2's lesson, learned the hard way: a signal handler
that takes a mutex deadlocks against whichever thread it interrupted. `kevent` is called with a
short timeout so the flag is noticed promptly.

Then the loop stops accepting, keeps servicing existing connections until each reaches `Done`, and
returns. There are no threads to join — which is the same graceful-shutdown requirement as the other
two backends, solved by an entirely different mechanism.

## 8. Experiment for this phase

1. **Differential:** `tests/phase1.sh event_loop` must pass 12/12, and `tests/phase2.sh`'s slow
   requests must behave. If the three backends disagree, phase 6 is meaningless.
2. **No ceiling:** the phase 2 comparison, rerun against the event loop. Four slow requests should
   take ~1s regardless of any worker-count analogue, because there isn't one.
3. **CPU sanity:** hold idle connections open and confirm the loop is not spinning. A busy loop
   passes every correctness test while being completely broken.
4. ASan/UBSan clean. TSan is not meaningful here — one thread — but must not regress the others.

## 9. Results, and one honest gap in the coverage

**Differential:** 12/12 on all three backends. They agree on every assertion, which is the
precondition for phase 6 meaning anything.

**No ceiling.** 16 slow requests: **1.10s through the event loop, 8.64s through a 2-worker pool
(7.9×)**. The pool serialised into eight rounds of two; the event loop did not serialise at all,
because there is no worker to run out of.

**Not spinning.** 200 established-but-idle connections held for 3 seconds cost **0.00s of CPU**. A
permanently-armed `EVFILT_WRITE` would have cost ~3s here while passing every correctness test in
the repo. Under load, 16 concurrent slow relays cost **0.01s of CPU over 1.10s of wall time** — the
loop is waiting, not burning.

**Clean** under ASan+UBSan, and TSan reports 0 warnings (single-threaded, so this only confirms no
regression in the other arms).

### The gap: the local suite cannot reach the InProgress path

**A loopback `connect()` completes synchronously.** It returns 0 immediately, so every test in this
repo takes the `Connected` branch and `EINPROGRESS` never happens. The `SO_ERROR` check — the thing
§5 is about — is therefore **not covered by the automated suite**, and reporting 12/12 as if it were
would be a false claim.

Verified separately against a real remote origin (`http://example.com/` → 200, 559 bytes). That
proves the path because the `-> fd N` debug line is emitted *only* from `finish_connect()`, which is
reachable only via `InProgress`. So the path works; it just isn't exercised by anything hermetic.

Making it hermetic would need an origin that is routable but slow to complete a handshake — a
blackholed address takes ~75s to time out, which is not a test. Left as a documented gap rather than
pretended away.

## 10. Expected failures

- Registering `EVFILT_WRITE` permanently → 100% CPU that looks like throughput.
- Missing the `SO_ERROR` check → requests written into dead sockets, intermittently.
- Deleting a registration using an fd captured *after* `on_ready()` closed it.
- Forgetting that `accept` on a ready listener must loop until `EAGAIN`: with level-triggering you
  get away with it, with `EV_CLEAR` you would silently stop accepting.
