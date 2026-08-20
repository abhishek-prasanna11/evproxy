#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <sys/event.h>
#include <sys/time.h>
#include <unordered_map>
#include <vector>

#include "backend/backend.hpp"
#include "connection.hpp"
#include "log.hpp"

namespace evp {
namespace {

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
    explicit EventLoop(const Config& cfg) : cfg_(cfg) {}

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

        if (!arm(listener_.get(), /*write=*/false, /*udata=*/nullptr)) {
            EVP_LOG_ERROR("kevent(listener): %s", std::strerror(errno));
            return;
        }

        EVP_LOG_INFO("event_loop: single-threaded, no connections-in-flight ceiling");

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
                if (events[i].udata == nullptr) do_accept();
                else                            service(static_cast<Connection*>(events[i].udata));
            }
        }

        // Stop accepting, then let what is already in flight finish. No threads to join -- the same
        // graceful-shutdown requirement as the other two backends, by a completely different route.
        disarm(listener_.get(), false);
        listener_.reset();
        drain();

        EVP_LOG_INFO("event_loop: stopped after %llu connections",
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
            auto conn = std::make_unique<Connection>(std::move(a.conn), cfg_, next_id_++);
            Connection* raw = conn.get();

            int  fd    = raw->want_fd();
            bool write = raw->want_write();
            if (fd < 0 || !arm(fd, write, raw)) continue;  // conn destroyed here

            live_[raw] = {std::move(conn), fd, write};
        }
    }

    void service(Connection* conn) {
        auto it = live_.find(conn);
        if (it == live_.end()) return;  // already finished earlier in this event batch

        // Capture the registration BEFORE on_ready(), which may close this very descriptor. Reading
        // want_fd() afterwards could hand us a number that has already been reused.
        const int  old_fd    = it->second.fd;
        const bool old_write = it->second.write;

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

        if (new_fd != old_fd || new_write != old_write) {
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
    };

    const Config&                            cfg_;
    Fd                                       kq_;
    Fd                                       listener_;
    std::unordered_map<Connection*, Slot>    live_;
    std::atomic<bool>                        stop_{false};
    uint64_t                                 next_id_  = 0;
    uint64_t                                 accepted_ = 0;
};

}  // namespace

std::unique_ptr<Backend> make_event_loop(const Config& cfg) {
    return std::make_unique<EventLoop>(cfg);
}

}  // namespace evp
