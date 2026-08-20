# Phase 5 — the response cache, and why it sharpens the comparison

*Written before the code, per project convention.*

---

## 1. Why a cache is in an I/O-architecture project at all

The obvious objection: cached requests skip the upstream path entirely, so putting a cache in front
of a three-way architecture comparison muddies it.

That is solved by a flag, not by omission. **The three-arm benchmark runs `cache_enabled = 0`.**
Production config is cache-on. Same discipline as everything else here: one binary, one switch, and
the comparison never has a hidden variable in it.

And the cache earns its place, because it exposes a difference between the architectures that a
cacheless proxy cannot:

| Backend | What the shared cache costs |
|---|---|
| thread-per-connection | a mutex, contended by every live connection |
| bounded thread pool | a mutex, contended by N workers |
| **event loop** | **nothing — one thread, so no lock at all** |

That is one of the real reasons production servers went event-driven or shard-per-core: **shared
mutable state stops needing synchronisation when only one thread can touch it.** Without a piece of
shared mutable state in the proxy, the comparison has no way to show it.

## 2. The lock as a policy type

One implementation, two instantiations:

```cpp
template <typename LockT> class LruCacheImpl;
using LockedCache   = LruCacheImpl<std::mutex>;   // threaded backends
using UnlockedCache = LruCacheImpl<NullMutex>;    // event loop
```

`NullMutex` has `lock()`/`unlock()` that do nothing, so `std::lock_guard<NullMutex>` compiles away.

This matters for honesty: the two configurations run **the same code**, differing only in the lock
type. Writing a separate lock-free cache for the event loop would be comparing two implementations
rather than measuring the cost of synchronisation.

Both go through the same abstract `ResponseCache` interface, so the virtual-dispatch overhead is
identical on both sides and cannot bias the result either way.

**The event loop gets `UnlockedCache` because it is single-threaded — that is a safety-critical
assumption, not an optimisation.** If the event loop ever grows a second thread, this becomes a data
race. Asserted at construction, and stated in the header.

## 3. What gets cached

Deliberately conservative — this is a demonstration of shared state, not an HTTP caching
implementation:

- **`GET` and `HEAD` only.** A cached `POST` response is a correctness bug.
- **Complete `200 OK` responses only.** "Complete" means the upstream reached EOF cleanly; an error
  or a timeout mid-relay must not be stored.
- **Under a per-entry size cap**, so one large response cannot evict everything.
- **Requests carrying `Authorization` or `Cookie` are never cached**, and neither are responses
  carrying `Set-Cookie`. Serving one user's response to another is the classic proxy-cache
  vulnerability, and the cheap defence is to refuse to store anything user-specific.

Key = `method + host + ":" + port + path`. Not the raw request line: two clients can spell the same
target differently, and the request also carries per-client headers that must not be part of the
identity.

**Explicitly out of scope:** `Cache-Control`, `Expires`, `ETag`, revalidation, `Vary`. Real HTTP
caching is a project of its own; this is a bounded LRU over complete 200s and says so.

## 4. O(1) LRU

`std::list<Entry>` for recency order plus `unordered_map<key, list::iterator>` for lookup. A hit
splices its node to the front in constant time; eviction pops the back. Bounded by both entry count
and total bytes, because either alone lets the other run away.

## 5. Graceful shutdown

Already implemented and asserted in all three backends by earlier phases — SIGTERM exits 0 with
in-flight requests completed (phase 1 test 10, phase 2 test 3). Worth restating that it is a
*different problem* in each: the pool signals and joins, thread-per-connection has only a counter
because its threads are detached (phase 4 §7.4), and the event loop has no threads to join at all
and drains state machines instead.

## 6. Experiment for this phase

1. Same request twice: second is a hit, and **byte-identical** to the first.
2. `POST` is never cached; a non-200 is never cached.
3. LRU eviction: overflow the capacity, confirm the oldest entry is gone and the newest is present.
4. `Authorization` on the request is not cached.
5. `cache_enabled = 0` produces zero hits — the benchmark's isolation actually works.
6. Differential: all three backends agree, cache on and cache off.

The lock-contention measurement is **phase 6**, alongside the rest of the benchmark.

## 7. Results

8/8 on all three backends, and 13/13 on the phase 1 differential with the cache in place. Clean
under ASan+UBSan and TSan.

Test 6 is the one the rest of the project depends on: **`cache_enabled = 0` produces zero hits.**
If that isolation leaked, every number in the phase 6 I/O comparison would be suspect.

### A harness bug worth naming, because it is the third of its family

`cache_hits()` was `grep -c "cache HIT" log || echo 0`. **`grep -c` prints `0` *and* exits 1 when
there are no matches**, so the fallback fired on top of a perfectly good count and the function
emitted two zeros. Every numeric comparison downstream then failed with a *syntax* error, which
reads like a broken test rather than a broken helper — and two assertions reported FAIL while the
cache was behaving correctly.

Same family as phase 2's false pass and phase 4's vacuous sanitizer grep: **the assertion machinery
was wrong, not the thing under test.** A test that fails for the wrong reason costs the same as one
that passes for the wrong reason; only the direction of the lie differs.

### A regression the tests caught after the fact

Shipping the cache with `cache_enabled` defaulting to **on** silently broke phases 2 and 3. Both
experiments issue the *same* URL repeatedly, so every slow request after the first was served from
cache: the 2-worker pool stopped serialising, and both ceiling assertions failed with
"expected the narrow pool to serialise".

Nothing was wrong with the proxy. The cache had become a hidden variable inside a *timing*
experiment — which is precisely what §1 says must never happen, applied to the wrong file. The
benchmark had `-C 0` from the start; the phase 2 and 3 scripts did not, because they predate the
cache.

Both now pass `-C 0` with a comment saying why. Generalisable: **when a feature ships with a default,
audit every existing experiment for whether that default changes what it measures.**

## 8. Expected failures

- Caching a truncated response because the relay ended in error rather than EOF.
- Keying on the raw request line, so per-client headers leak into cache identity.
- Splicing on hit but forgetting to update the map iterator, or evicting from the map but not the
  list — the two structures must stay in step or the cache silently grows.
- Storing the response *and* relaying from the same buffer, so a partial write corrupts what gets
  stored.
