#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <deque>
#include <mutex>
#include <sys/event.h>
#include <thread>
#include <unistd.h>
#include <sys/time.h>
#include <unordered_map>
#include <vector>

#include "backend/backend.hpp"
#include "connection.hpp"
#include "resolver.hpp"
#include "log.hpp"

namespace evp {
namespace {

// udata value marking the shard's hand-off pipe. Never a valid Connection*.
void* const INBOX_TAG = reinterpret_cast<void*>(1);

// One shard's hand-off queue. macOS SO_REUSEPORT does NOT load-balance TCP accepts (measured: with
// 4 listeners, shard 3 took all 19,215 connections and shards 0-2 took none) -- unlike Linux, where
// SO_REUSEPORT hashes the 4-tuple across listeners. So a single acceptor distributes descriptors
// instead, which costs exactly the shared state the SO_REUSEPORT design was chosen to avoid.
struct Inbox {
    std::mutex     mutex;
    std::deque<Fd> queue;
    Fd             read_end;
    Fd             write_end;

    bool init() { return make_pipe(read_end, write_end); }

    void push(Fd fd) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push_back(std::move(fd));
        }
        const char b = 1;
        ssize_t n = ::write(write_end.get(), &b, 1);
        (void)n;
    }

    std::deque<Fd> drain() {
        std::lock_guard<std::mutex> lock(mutex);
        std::deque<Fd> out;
        out.swap(queue);
        return out;
    }
};

// Rung 3: one thread, kqueue, non-blocking everything.
//
// A connection is no longer paired with a thread. It is a few hundred bytes of state that the loop
// touches only when the kernel says the relevant descriptor can make progress, so idle connections
// cost memory rather than scheduling -- which is precisely the ceiling rung 2 could not escape.
//
// What it costs: below the crossover this backend is SLOWER than the pool. There is real
// per-event bookkeeping here and no call stack to remember where a connection was. If the phase 6
// benchmark shows the event loop winning everywhere, the benchmark is wrong.
class EventLoop final : public Backend {
public:
    EventLoop(const Config& cfg, Resolver& resolver, ResponseCache* cache, int shard = -1)
        : cfg_(cfg), resolver_(resolver), cache_(cache), shard_(shard) {}

    // Shard mode: no listener of its own; connections arrive from the acceptor via `inbox`.
    void set_inbox(Inbox* inbox) { inbox_ = inbox; }

    const char* name() const override { return "event_loop"; }

    // Signal-handler context: atomics only. Phase 2's lesson -- taking a lock here deadlocks
    // against whichever thread the signal interrupted.
    void stop() override { stop_.store(true, std::memory_order_relaxed); }

    void run(Fd listener) override {
        listener_ = std::move(listener);
        set_nonblocking(listener_.get());

        kq_.reset(::kqueue());
        if (!kq_.valid()) {
            EVP_LOG_ERROR("kqueue: %s", std::strerror(errno));
            return;
        }

        if (inbox_) {
            if (!arm(inbox_->read_end.get(), /*write=*/false, INBOX_TAG)) {
                EVP_LOG_ERROR("kevent(inbox): %s", std::strerror(errno));
                return;
            }
        } else if (!arm(listener_.get(), /*write=*/false, /*udata=*/nullptr)) {
            EVP_LOG_ERROR("kevent(listener): %s", std::strerror(errno));
            return;
        }

        if (shard_ < 0)
            EVP_LOG_INFO("event_loop: single-threaded, no connections-in-flight ceiling");
        else
            EVP_LOG_DEBUG("event_loop shard %d: started", shard_);

        std::vector<struct kevent> events(256);

        while (!stop_.load(std::memory_order_relaxed)) {
            // Short timeout so the stop flag is noticed promptly on an idle proxy.
            timespec ts{0, 200 * 1000 * 1000};
            int n = ::kevent(kq_.get(), nullptr, 0, events.data(),
                             static_cast<int>(events.size()), &ts);

            if (n < 0) {
                if (errno == EINTR) continue;
                EVP_LOG_ERROR("kevent: %s", std::strerror(errno));
                break;
            }

            for (int i = 0; i < n; ++i) {
                if (events[i].udata == INBOX_TAG)      take_from_inbox();
                else if (events[i].udata == nullptr)   do_accept();
                else service(static_cast<Connection*>(events[i].udata));
            }
        }

        // Stop accepting, then let what is already in flight finish. No threads to join -- the same
        // graceful-shutdown requirement as the other two backends, by a completely different route.
        if (inbox_) {
            disarm(inbox_->read_end.get(), false);
        } else {
            disarm(listener_.get(), false);
            listener_.reset();
        }
        drain();

        if (shard_ < 0)
            EVP_LOG_INFO("event_loop: stopped after %llu connections", (unsigned long long)accepted_);
        else
            EVP_LOG_INFO("event_loop shard %d: stopped after %llu connections", shard_,
                         (unsigned long long)accepted_);
    }

private:
    bool arm(int fd, bool write, void* udata) {
        struct kevent ev;
        EV_SET(&ev, fd, write ? EVFILT_WRITE : EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, udata);
        return ::kevent(kq_.get(), &ev, 1, nullptr, 0, nullptr) == 0;
    }

    void disarm(int fd, bool write) {
        if (fd < 0) return;
        struct kevent ev;
        EV_SET(&ev, fd, write ? EVFILT_WRITE : EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        // Closing a descriptor already removes its registrations, so ENOENT/EBADF here is the
        // normal case after a connection finishes -- not an error worth reporting.
        ::kevent(kq_.get(), &ev, 1, nullptr, 0, nullptr);
    }

    // Adopt whatever the acceptor handed us. Same path as do_accept() from the Connection's point
    // of view -- the descriptor just arrived by a different route.
    void take_from_inbox() {
        char buf[256];
        while (::read(inbox_->read_end.get(), buf, sizeof(buf)) > 0) {}  // drain the wakeups

        for (auto& fd : inbox_->drain()) {
            ++accepted_;
            auto conn = std::make_unique<Connection>(std::move(fd), cfg_, resolver_, cache_,
                                                     next_id_++);
            Connection* raw = conn.get();
            int  cfd   = raw->want_fd();
            bool write = raw->want_write();
            if (cfd < 0 || !arm(cfd, write, raw)) continue;
            live_[raw] = {std::move(conn), cfd, write, raw->epoch()};
        }
    }

    void do_accept() {
        // Loop until EAGAIN: one readability event can cover several pending connections, and with
        // edge-triggering (if this ever changes) stopping early would silently wedge the listener.
        for (;;) {
            AcceptResult a = accept_one(listener_.get());
            if (a.status == AcceptStatus::WouldBlock) return;
            if (a.status == AcceptStatus::OutOfFds) {
                EVP_LOG_ERROR("accept: out of file descriptors (limit %ld) -- raise ulimit -n",
                              fd_limit_current());
                return;
            }
            if (a.status != AcceptStatus::Ok) {
                EVP_LOG_ERROR("accept: %s", std::strerror(errno));
                return;
            }

            ++accepted_;
            auto conn = std::make_unique<Connection>(std::move(a.conn), cfg_, resolver_, cache_, next_id_++);
            Connection* raw = conn.get();

            int  fd    = raw->want_fd();
            bool write = raw->want_write();
            if (fd < 0 || !arm(fd, write, raw)) continue;  // conn destroyed here

            live_[raw] = {std::move(conn), fd, write, raw->epoch()};
        }
    }

    void service(Connection* conn) {
        auto it = live_.find(conn);
        if (it == live_.end()) return;  // already finished earlier in this event batch

        // Capture the registration BEFORE on_ready(), which may close this very descriptor. Reading
        // want_fd() afterwards could hand us a number that has already been reused.
        const int      old_fd    = it->second.fd;
        const bool     old_write = it->second.write;
        const uint64_t old_epoch = it->second.epoch;

        conn->on_ready();

        if (conn->done()) {
            disarm(old_fd, old_write);
            live_.erase(it);
            return;
        }

        const int  new_fd    = conn->want_fd();
        const bool new_write = conn->want_write();

        if (new_fd < 0) {  // nothing to wait on: treat as finished
            disarm(old_fd, old_write);
            live_.erase(it);
            return;
        }

        // Compare the EPOCH, not just (fd, direction). Trying the next candidate address closes
        // one socket and opens another, which very often reuses the same fd number -- so the pair
        // can be identical while the kernel has already dropped the registration along with the
        // closed descriptor. Trusting (fd, direction) leaves the new socket registered nowhere and
        // the connection hangs forever.
        if (conn->epoch() != old_epoch) {
            // Exactly one registration per connection. That is what makes the EVFILT_WRITE trap
            // structurally impossible: a write filter exists only while there are bytes to flush,
            // so an always-writable socket cannot spin the loop.
            disarm(old_fd, old_write);
            if (!arm(new_fd, new_write, conn)) {
                live_.erase(it);
                return;
            }
            it->second.fd    = new_fd;
            it->second.write = new_write;
            it->second.epoch = conn->epoch();
        }
    }

    void drain() {
        const int grace_ms = 5000;
        int waited = 0;
        std::vector<struct kevent> events(256);

        while (!live_.empty() && waited < grace_ms) {
            timespec ts{0, 100 * 1000 * 1000};
            int n = ::kevent(kq_.get(), nullptr, 0, events.data(),
                             static_cast<int>(events.size()), &ts);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) waited += 100;
            for (int i = 0; i < n; ++i) {
                if (events[i].udata != nullptr) service(static_cast<Connection*>(events[i].udata));
            }
        }

        if (!live_.empty())
            EVP_LOG_INFO("event_loop: %zu connections dropped at shutdown grace", live_.size());
        live_.clear();
    }

    struct Slot {
        std::unique_ptr<Connection> conn;
        int                         fd    = -1;
        bool                        write = false;
        uint64_t                    epoch = 0;
    };

    const Config&                            cfg_;
    Resolver&                                resolver_;
    ResponseCache*                           cache_ = nullptr;
    Fd                                       kq_;
    Fd                                       listener_;
    std::unordered_map<Connection*, Slot>    live_;
    std::atomic<bool>                        stop_{false};
    Inbox*                                   inbox_    = nullptr;
    int                                      shard_    = -1;
    uint64_t                                 next_id_  = 0;
    uint64_t                                 accepted_ = 0;
};

// Phase 7: N independent loops, one per core, each with its own kqueue, its OWN listening socket
// bound with SO_REUSEPORT, and its OWN cache. There is no shared state between shards at all --
// which is the point, and the reason SO_REUSEPORT is used instead of one acceptor thread feeding a
// shared queue.
//
// Sharding invalidates phase 5's NullMutex cache, whose header says plainly that a second thread
// makes it a data race. Resolved by giving each shard its own cache rather than by adding a lock:
// that preserves the no-lock property, at the cost of N x the memory and a lower effective hit rate
// because each shard only ever sees its own share of the traffic.
class ShardedEventLoop final : public Backend {
public:
    ShardedEventLoop(const Config& cfg, Resolver& resolver)
        : cfg_(cfg), resolver_(resolver), shards_(cfg.event_loops ? cfg.event_loops : 1) {}

    const char* name() const override { return "event_loop"; }

    // Signal-handler context: atomics only, N times over.
    void stop() override {
        stop_.store(true, std::memory_order_relaxed);
        for (auto& l : loops_)
            if (l) l->stop();
    }

    void run(Fd listener) override {
        EVP_LOG_INFO("event_loop: %zu shards, one kqueue + cache each; 1 acceptor distributing",
                     shards_);

        listener_ = std::move(listener);
        set_nonblocking(listener_.get());

        for (size_t i = 0; i < shards_; ++i) inboxes_.emplace_back();
        for (size_t i = 0; i < shards_; ++i) {
            if (!inboxes_[i].init()) {
                EVP_LOG_ERROR("shard %zu: cannot create hand-off pipe", i);
                return;
            }
            caches_.push_back(cfg_.cache_enabled ? make_cache(cfg_) : nullptr);
            loops_.push_back(std::make_unique<EventLoop>(cfg_, resolver_, caches_.back().get(),
                                                         static_cast<int>(i)));
            loops_.back()->set_inbox(&inboxes_[i]);
        }

        std::vector<std::thread> threads;
        threads.reserve(shards_);
        for (size_t i = 0; i < shards_; ++i)
            threads.emplace_back([this, i] { loops_[i]->run(Fd{}); });

        accept_loop();

        for (auto& l : loops_) l->stop();
        for (auto& t : threads) t.join();
        EVP_LOG_INFO("event_loop: all %zu shards stopped", shards_);
    }

private:
    void accept_loop() {
        size_t next = 0;
        while (!stop_.load(std::memory_order_relaxed)) {
            if (!wait_ready(listener_.get(), /*for_write=*/false, 200)) continue;

            for (;;) {
                AcceptResult a = accept_one(listener_.get());
                if (a.status == AcceptStatus::WouldBlock) break;
                if (a.status == AcceptStatus::OutOfFds) {
                    EVP_LOG_ERROR("accept: out of file descriptors (limit %ld) -- raise ulimit -n",
                                  fd_limit_current());
                    break;
                }
                if (a.status != AcceptStatus::Ok) break;

                // Round-robin. The kernel balances by connection, not by work, and so does this --
                // one shard can still end up holding the slow clients.
                inboxes_[next].push(std::move(a.conn));
                next = (next + 1) % shards_;
            }
        }
    }

private:
    const Config&                                cfg_;
    Resolver&                                    resolver_;
    size_t                                       shards_;
    Fd                                           listener_;
    std::atomic<bool>                            stop_{false};
    std::deque<Inbox>                            inboxes_;
    std::vector<std::unique_ptr<ResponseCache>>  caches_;
    std::vector<std::unique_ptr<EventLoop>>      loops_;
};

}  // namespace

std::unique_ptr<Backend> make_event_loop(const Config& cfg, Resolver& resolver, ResponseCache* cache) {
    if (cfg.event_loops > 1) return std::make_unique<ShardedEventLoop>(cfg, resolver);
    return std::make_unique<EventLoop>(cfg, resolver, cache);
}

}  // namespace evp
