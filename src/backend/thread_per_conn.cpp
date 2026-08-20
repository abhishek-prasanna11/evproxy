#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include "backend/backend.hpp"
#include "connection.hpp"
#include "resolver.hpp"
#include "log.hpp"

namespace evp {
namespace {

// Rung 1 of the ladder: one thread per connection.
//
// The failure mode this arm exists to demonstrate is RESOURCE exhaustion -- each connection costs a
// thread, a stack, and a slot in the scheduler's run queue. It is the honest baseline that the
// bounded pool (rung 2) fixes, and the reason the C10K problem has a name.
class ThreadPerConn final : public Backend {
public:
    ThreadPerConn(const Config& cfg, Resolver& resolver, ResponseCache* cache)
        : cfg_(cfg), resolver_(resolver), cache_(cache) {}

    const char* name() const override { return "thread_per_conn"; }
    void        stop() override { stop_.store(true, std::memory_order_relaxed); }

    void run(Fd listener) override {
        listener_ = std::move(listener);
        set_nonblocking(listener_.get());

        uint64_t next_id = 0;

        while (!stop_.load(std::memory_order_relaxed)) {
            // Wait on the listener rather than spinning; the timeout is what lets us notice stop_.
            if (!wait_ready(listener_.get(), /*for_write=*/false, 200)) continue;

            AcceptResult a = accept_one(listener_.get());
            if (a.status == AcceptStatus::WouldBlock) continue;
            if (a.status == AcceptStatus::OutOfFds) {
                // The fd ceiling, not a bug in the proxy. Say so explicitly -- this is the failure
                // most often misdiagnosed as a broken accept loop.
                EVP_LOG_ERROR("accept: out of file descriptors (limit %ld) -- raise ulimit -n",
                              fd_limit_current());
                continue;
            }
            if (a.status != AcceptStatus::Ok) {
                EVP_LOG_ERROR("accept: %s", std::strerror(errno));
                continue;
            }

            live_.fetch_add(1, std::memory_order_relaxed);
            std::thread(&ThreadPerConn::serve, this, std::move(a.conn), next_id++).detach();
        }

        // Detached threads cannot be joined, so run() must not return until every one of them has
        // finished touching shared state -- otherwise main destroys the Resolver out from under a
        // connection still using it. TSan caught exactly that: a detached thread inside
        // Resolver::cache_get() while ~Resolver ran on the main thread.
        //
        // This is a real cost of the thread-per-connection design, not an incidental bug: with no
        // handles to join, the only shutdown barrier available is a counter, and the counter has to
        // be decremented AFTER the Connection is destroyed (see serve()).
        const int grace_ms = 30000;
        int waited = 0;
        while (live_.load(std::memory_order_acquire) > 0 && waited < grace_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            waited += 20;
        }

        long remaining = live_.load(std::memory_order_acquire);
        if (remaining > 0)
            EVP_LOG_ERROR("thread_per_conn: %ld connections still live after %dms -- shutting down anyway",
                          remaining, grace_ms);
        EVP_LOG_INFO("thread_per_conn: stopped (%ld still live)", remaining);
    }

private:
    // One connection, start to finish, on this thread. The state machine itself never blocks: we
    // poll the single fd it asks for and hand control back. That keeps this backend and the event
    // loop driving identical logic.
    void serve(Fd client, uint64_t id) {
        // Scoped so the Connection is fully destroyed BEFORE the counter drops. Decrementing while
        // `conn` is still alive would let run() return, and main destroy the Resolver, while this
        // thread is still unwinding through connection state.
        {
            Connection conn(std::move(client), cfg_, resolver_, cache_, id);

            while (!conn.done()) {
                int fd = conn.want_fd();
                if (fd < 0) break;

                if (!wait_ready(fd, conn.want_write(), cfg_.poll_timeout_ms)) {
                    EVP_LOG_DEBUG("conn %llu: idle timeout", (unsigned long long)id);
                    break;
                }
                conn.on_ready();
            }
        }

        // Release, paired with the acquire load in run()'s drain loop: everything this thread did
        // is visible to whoever observes the counter reach zero.
        live_.fetch_sub(1, std::memory_order_release);
    }

    const Config&         cfg_;
    Resolver&             resolver_;
    ResponseCache*        cache_ = nullptr;
    Fd                    listener_;
    std::atomic<bool>     stop_{false};
    std::atomic<long>     live_{0};
};

}  // namespace

std::unique_ptr<Backend> make_thread_per_conn(const Config& cfg, Resolver& resolver, ResponseCache* cache) {
    return std::make_unique<ThreadPerConn>(cfg, resolver, cache);
}

}  // namespace evp
