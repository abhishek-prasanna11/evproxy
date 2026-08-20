#include "resolver.hpp"

#include <chrono>
#include <unistd.h>

#include "log.hpp"

namespace evp {

Resolver::Resolver(size_t threads, int cache_ttl_s, int inject_delay_ms)
    : cache_ttl_s_(cache_ttl_s), inject_delay_ms_(inject_delay_ms) {
    threads_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) threads_.emplace_back(&Resolver::worker_loop, this);
}

Resolver::~Resolver() { shutdown(); }

void Resolver::shutdown() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (closed_) return;
        closed_ = true;
    }
    queue_cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

bool Resolver::do_lookup(const std::string& host, const std::string& port,
                         std::vector<Endpoint>& out, std::string& err) {
    // Fault injection, stated plainly: a real slow resolver is not reproducible on demand, so the
    // delay is simulated. It applies identically in both arms, so the comparison stays fair.
    if (inject_delay_ms_ > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(inject_delay_ms_));

    lookups_.fetch_add(1, std::memory_order_relaxed);
    return resolve(host, port, out, err);
}

bool Resolver::cache_get(const std::string& key, std::vector<Endpoint>& out) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(key);
    if (it == cache_.end()) return false;

    // TTL exists because DNS records change. Caching forever is a correctness bug that presents as
    // "the proxy kept sending traffic to the old server for hours".
    if (std::chrono::steady_clock::now() >= it->second.expires) {
        cache_.erase(it);
        return false;
    }

    out = it->second.endpoints;
    return true;
}

void Resolver::cache_put(const std::string& key, const std::vector<Endpoint>& eps) {
    if (cache_ttl_s_ <= 0) return;
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_[key] = CacheEntry{eps, std::chrono::steady_clock::now() +
                                      std::chrono::seconds(cache_ttl_s_)};
}

bool Resolver::resolve_blocking(const std::string& host, const std::string& port,
                                std::vector<Endpoint>& out, std::string& err) {
    const std::string key = host + ":" + port;
    if (cache_get(key, out)) {
        cache_hits_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (!do_lookup(host, port, out, err)) return false;

    // Only successes are cached. Caching a failure would let one transient outage poison a hostname
    // for the whole TTL.
    cache_put(key, out);
    return true;
}

ResolveStatus Resolver::start(const std::string& host, const std::string& port,
                              std::vector<Endpoint>& out, std::shared_ptr<ResolveJob>& job,
                              std::string& err) {
    const std::string key = host + ":" + port;
    if (cache_get(key, out)) {
        cache_hits_.fetch_add(1, std::memory_order_relaxed);
        return ResolveStatus::Hit;
    }

    auto j = std::make_shared<ResolveJob>();
    j->host = host;
    j->port = port;
    if (!make_pipe(j->read_end, j->write_end)) {
        err = "resolver: cannot create wakeup pipe";
        return ResolveStatus::Failed;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (closed_) {
            err = "resolver: shutting down";
            return ResolveStatus::Failed;
        }
        queue_.push_back(j);
    }
    queue_cv_.notify_one();

    job = std::move(j);
    return ResolveStatus::Pending;
}

void Resolver::worker_loop() {
    for (;;) {
        std::shared_ptr<ResolveJob> job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return closed_ || !queue_.empty(); });
            if (queue_.empty()) return;  // closed and drained
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        std::vector<Endpoint> eps;
        std::string           err;
        bool ok = do_lookup(job->host, job->port, eps, err);

        // Publish the result BEFORE signalling. Waking the connection first would let it read an
        // empty endpoint list and 502 intermittently -- a race that only shows under load.
        job->endpoints = std::move(eps);
        job->err       = std::move(err);
        job->ok        = ok;

        if (ok) cache_put(job->host + ":" + job->port, job->endpoints);

        // Release: everything written above is visible to whoever acquires `ready`.
        job->ready.store(true, std::memory_order_release);

        // One byte, then close. Closing alone would also wake the reader, but an explicit byte
        // distinguishes "resolution finished" from "the resolver died".
        const char b = 1;
        ssize_t n = ::write(job->write_end.get(), &b, 1);
        (void)n;
        job->write_end.reset();
    }
}

}  // namespace evp
