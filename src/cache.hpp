#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>

#include "config.hpp"

namespace evp {

// A lock that isn't. std::lock_guard<NullMutex> compiles to nothing, which lets the single-threaded
// event loop run the SAME cache code as the threaded backends with the synchronisation removed --
// rather than a separately written lock-free cache, which would compare two implementations instead
// of measuring the cost of a lock.
struct NullMutex {
    void lock() noexcept {}
    void unlock() noexcept {}
    bool try_lock() noexcept { return true; }
};

class ResponseCache {
public:
    virtual ~ResponseCache() = default;

    virtual bool get(const std::string& key, std::string& out) = 0;
    virtual void put(const std::string& key, const std::string& response) = 0;

    virtual uint64_t hits() const = 0;
    virtual uint64_t misses() const = 0;
    virtual size_t   entries() const = 0;
    virtual size_t   bytes() const = 0;
    virtual bool     locked() const = 0;
};

// O(1) LRU: a list for recency, a map from key to that list node. A hit splices its node to the
// front; eviction pops the back. Bounded by BOTH entry count and total bytes -- either bound alone
// lets the other run away.
template <typename LockT>
class LruCacheImpl final : public ResponseCache {
public:
    LruCacheImpl(size_t max_entries, size_t max_bytes)
        : max_entries_(max_entries), max_bytes_(max_bytes) {}

    bool get(const std::string& key, std::string& out) override {
        std::lock_guard<LockT> lock(mutex_);
        auto it = index_.find(key);
        if (it == index_.end()) {
            ++misses_;
            return false;
        }
        // splice is O(1) and keeps the iterator valid, so the map needs no update.
        order_.splice(order_.begin(), order_, it->second);
        out = it->second->response;
        ++hits_;
        return true;
    }

    void put(const std::string& key, const std::string& response) override {
        std::lock_guard<LockT> lock(mutex_);

        if (auto it = index_.find(key); it != index_.end()) {
            bytes_ -= it->second->response.size();
            it->second->response = response;
            bytes_ += response.size();
            order_.splice(order_.begin(), order_, it->second);
            return;
        }

        order_.push_front(Entry{key, response});
        index_[key] = order_.begin();
        bytes_ += response.size();

        // The two structures must stay in step; erasing from one and not the other is how a cache
        // silently grows forever.
        while ((max_entries_ && index_.size() > max_entries_) ||
               (max_bytes_ && bytes_ > max_bytes_)) {
            if (order_.empty()) break;
            const Entry& victim = order_.back();
            bytes_ -= victim.response.size();
            index_.erase(victim.key);
            order_.pop_back();
        }
    }

    uint64_t hits() const override { return hits_; }
    uint64_t misses() const override { return misses_; }
    size_t   entries() const override { return index_.size(); }
    size_t   bytes() const override { return bytes_; }
    bool     locked() const override { return !std::is_same<LockT, NullMutex>::value; }

private:
    struct Entry {
        std::string key;
        std::string response;
    };

    mutable LockT                                              mutex_;
    std::list<Entry>                                           order_;
    std::unordered_map<std::string, typename std::list<Entry>::iterator> index_;
    size_t   max_entries_ = 0;
    size_t   max_bytes_   = 0;
    size_t   bytes_       = 0;
    uint64_t hits_        = 0;
    uint64_t misses_      = 0;
};

using LockedCache   = LruCacheImpl<std::mutex>;
using UnlockedCache = LruCacheImpl<NullMutex>;

// Picks the lock policy from the backend. The event loop gets the unlocked cache because it is
// SINGLE-THREADED -- a safety-critical assumption, not an optimisation. If the event loop ever grows
// a second thread, this becomes a data race.
std::unique_ptr<ResponseCache> make_cache(const Config& cfg);

// Cache identity. Deliberately not the raw request line: two clients can spell the same target
// differently, and the request line's neighbours carry per-client headers that must never be part
// of the key.
std::string cache_key(const std::string& method, const std::string& host, const std::string& port,
                      const std::string& path);

}  // namespace evp
