#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "socket.hpp"

namespace evp {

// One outstanding lookup. The waiting Connection holds the read end of the pipe and treats it as an
// ordinary "wait until readable" -- which is why no backend needs to know that resolution is
// special.
struct ResolveJob {
    std::vector<Endpoint> endpoints;
    std::string           err;
    bool                  ok = false;

    // The publication barrier. Writing the pipe byte is a syscall and in practice orders these
    // fields, but the C++ memory model promises nothing about it -- so reading them without this
    // is a data race, which is UB regardless of what the hardware happens to do. TSan is right.
    //
    // Store with release AFTER filling the fields; load with acquire BEFORE reading them.
    std::atomic<bool> ready{false};

    Fd read_end;   // the Connection waits on this
    Fd write_end;  // a resolver thread writes one byte, then closes

    std::string host;
    std::string port;
};

enum class ResolveStatus {
    Hit,      // served from cache: `out` is filled, no job, no fds, no thread
    Pending,  // `job` is set; wait for its read_end to become readable
    Failed,
};

class Resolver {
public:
    Resolver(size_t threads, int cache_ttl_s, int inject_delay_ms);
    ~Resolver();

    Resolver(const Resolver&) = delete;
    Resolver& operator=(const Resolver&) = delete;

    // Async path. A cache hit costs nothing -- no descriptors and no thread hop -- which is what
    // keeps the fix cheap in the common case.
    ResolveStatus start(const std::string& host, const std::string& port,
                        std::vector<Endpoint>& out, std::shared_ptr<ResolveJob>& job,
                        std::string& err);

    // Naive arm: blocks the calling thread, which inside a single-threaded event loop means
    // blocking every other connection. Kept so both arms can be measured from one binary.
    bool resolve_blocking(const std::string& host, const std::string& port,
                          std::vector<Endpoint>& out, std::string& err);

    void shutdown();

    uint64_t cache_hits() const { return cache_hits_.load(std::memory_order_relaxed); }
    uint64_t lookups() const { return lookups_.load(std::memory_order_relaxed); }

private:
    struct CacheEntry {
        std::vector<Endpoint> endpoints;
        std::chrono::steady_clock::time_point expires;
    };

    void worker_loop();

    // Performs the actual getaddrinfo, honouring the injected delay. Only ever called from a
    // resolver thread (async arm) or the caller's thread (naive arm).
    bool do_lookup(const std::string& host, const std::string& port,
                   std::vector<Endpoint>& out, std::string& err);

    bool cache_get(const std::string& key, std::vector<Endpoint>& out);
    void cache_put(const std::string& key, const std::vector<Endpoint>& eps);

    std::vector<std::thread>                       threads_;
    std::deque<std::shared_ptr<ResolveJob>>        queue_;
    std::mutex                                     queue_mutex_;
    std::condition_variable                        queue_cv_;
    bool                                           closed_ = false;

    std::mutex                                     cache_mutex_;
    std::unordered_map<std::string, CacheEntry>    cache_;
    int                                            cache_ttl_s_;
    int                                            inject_delay_ms_;

    std::atomic<uint64_t> cache_hits_{0};
    std::atomic<uint64_t> lookups_{0};
};

}  // namespace evp
