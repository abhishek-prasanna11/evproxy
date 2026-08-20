# Phase 1 — sockets, HTTP, and a backend-agnostic connection

*Written before the code, per project convention.*

---

## 1. Concepts

**A socket is a file descriptor.** `socket()` returns an integer indexing a kernel table; `read`/
`write` work on it like a file. What makes it different is that the other end is a machine that may
be slow, absent, or lying.

**A forward proxy** is a server to the client and a client to the origin. The client connects to
*us* and sends `GET http://example.com/path HTTP/1.1` — an **absolute-form** request line, which
normal origin servers never see. We parse out the host, open our own connection to it, forward the
request in **origin form** (`GET /path HTTP/1.1`), and relay the response back.

**Non-blocking I/O** means `recv` and `send` return immediately with whatever they can do right now.
`recv` returning `-1` with `EAGAIN` is not an error — it means "nothing available yet". `send`
returning less than you asked is normal. Every read is partial and every write is partial.

## 2. The problem this phase solves

We need one connection-handling core that all three backends (§3 of the blueprint) will drive, so
the final benchmark compares **I/O architectures** rather than three different programs.

That forces a decision now, before any of the backends exist: **`Connection` must never block and
must never own a thread.** It is a state machine over buffers. It says what it wants to do next
(read or write, on which fd); somebody else decides when that's possible.

If we wrote phase 1 the easy way — a thread per connection making blocking calls, with the logic
living in the call stack — then phase 3 would throw all of it away, and the comparison would be
between two unrelated implementations.

## 3. Design options considered

**(a) Blocking logic now, rewrite for the event loop later.** Simplest for phase 1. Rejected: it
destroys the comparison, which is the entire project.

**(b) Callback-per-state.** Flexible, and unreadable at this size.

**(c) Explicit state machine with an intent enum.** `Connection` exposes `want()` — one of
`ReadClient / WriteUpstream / ReadUpstream / WriteClient / Done` — plus `on_readable()` and
`on_writable()`. **Chosen.** The state is data we can inspect and log, and every backend drives it
identically:

- thread-per-connection: `poll()` the single fd it wants, then call the handler
- thread pool: same, from a worker
- event loop: register that fd with kqueue and call the handler on the event

The backends differ only in *who waits and where*, which is exactly the variable under study.

## 4. Chosen design

```
                    ┌──────────────────────────────────────────────┐
  client fd ───────▶│ ReadingRequest                               │
                    │   accumulate until \r\n\r\n, parse, then read │
                    │   Content-Length bytes of body               │
                    └───────────────┬──────────────────────────────┘
                                    │  resolve + connect (BLOCKING in phase 1 — see §7)
                    ┌───────────────▼──────────────────────────────┐
                    │ SendingUpstream   flush the rewritten request │
                    └───────────────┬──────────────────────────────┘
                    ┌───────────────▼──────────────────────────────┐
                    │ Relaying   upstream → buffer → client,        │
                    │            alternating Read/Write as needed   │
                    └───────────────┬──────────────────────────────┘
                                    ▼   upstream EOF and buffer drained
                                  Done
```

### Deliberate simplifications, each with its reason

- **`Connection: close` is forced on the upstream request, and we relay until upstream EOF.** This
  avoids needing to interpret `Content-Length` or chunked encoding on the *response* path to know
  when it ends. Keep-alive is a real feature but it is orthogonal to the I/O-architecture question
  and it would double the state machine. Documented as a limitation, not hidden.
- **Blocking `getaddrinfo` and blocking `connect` in phase 1.** This is deliberate: phase 4's whole
  experiment is measuring the damage a blocking resolver does to an event loop and then fixing it.
  Building the naive version first is the point.
- **`GET`, `HEAD`, `POST` only**; anything else gets `501`. Malformed requests get `400`,
  unreachable origins `502`.
- **Request body must use `Content-Length`.** Chunked request bodies are rejected with `501`.

### Files

| File | Holds |
|---|---|
| `socket.{hpp,cpp}` | `Fd` (RAII), non-blocking helper, listener setup, fd-limit handling |
| `http.{hpp,cpp}` | absolute-form request parsing, upstream request rebuilding, error responses |
| `connection.{hpp,cpp}` | the state machine — the shared core |
| `backend/backend.hpp` | the interface the three backends implement |
| `backend/thread_per_conn.cpp` | this phase's backend |
| `config.{hpp,cpp}` | listen port, buffer sizes, backend selection |

## 5. Why `Fd` is RAII from line one

Three backends × a dozen error paths through a state machine is precisely where a bare `close()`
gets missed. A leaked descriptor doesn't crash — it accumulates silently and then `accept` fails at
whatever the fd limit is, which looks like a concurrency bug and isn't. So descriptors are owned by
a move-only type whose destructor closes, and the leak test (phase 6) asserts the open-fd count
returns to baseline.

This is the clearest place C++ earns its keep over C in this project.

## 6. The fd limit, handled on day one

macOS defaults `ulimit -n` to **256**. A proxy that dies at ~250 connections looks broken; it isn't.
So at startup we call `setrlimit` to raise `RLIMIT_NOFILE` toward the hard limit and **log the
effective value**. Any benchmark run that doesn't state its fd limit is not a result — the number
would silently cap.

## 7. Experiment for this phase

Not a benchmark yet — a correctness baseline:

1. Proxy a real request end to end, compare the body byte-for-byte against fetching it directly.
2. A slow origin (dribbles its response with delays) proves the partial-read path works.
3. Two concurrent clients through the thread-per-connection backend prove independence.
4. Startup log states the effective fd limit.
5. ASan/UBSan clean.

## 8. Expected failures

- Partial `send` mishandled → truncated responses that look fine at small sizes and break on large
  ones. The slow-origin test exists specifically to find this.
- Forgetting that `recv` returning `0` means orderly close while `-1`/`EAGAIN` means "not yet" —
  conflating them produces either a hang or a premature close.
- Absolute-form parsing that assumes a path is present (`GET http://host HTTP/1.1` has none).
