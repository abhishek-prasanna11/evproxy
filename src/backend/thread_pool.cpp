#include <atomic>
#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

#include "backend/backend.hpp"
#include "conn_queue.hpp"
#include "connection.hpp"
#include "resolver.hpp"
#include "log.hpp"

namespace evp {
namespace {

// Rung 2 of the ladder: a fixed pool of workers draining a bounded queue.
//
// This fixes rung 1's resource explosion -- threads are a fixed cost now, chosen once, independent
// of load -- and introduces the limit this whole project exists to measure:
//
//   with N workers, exactly N connections can be in flight.
//
// A worker blocked reading from a slow client is not doing work; it is holding a seat. So the pool's
// ceiling is connections in flight, not requests/sec, and the two only look alike when requests are
// fast. Every closed-loop throughput benchmark will report this backend as healthy right up to the
// point where it is completely dead at ~0% CPU.
//
// Deliberately NOT done: handing a connection back to the queue whenever it would block. That is a
// work-queue-driven event loop in a costume, and it would erase the ceiling this arm exists to show.
class ThreadPool final : public Backend {
public:
    ThreadPool(const Config& cfg, Resolver& resolver)
        : cfg_(cfg), resolver_(resolver), queue_(cfg.job_queue_capacity) {}

    const char* name() const override { return "thread_pool"; }

    // Runs in a signal handler. Nothing here may take a lock: pthread_mutex_lock is not
    // async-signal-safe, so closing the queue from here can deadlock against whichever thread the
    // signal happened to interrupt while it held the queue mutex. Setting the flag is enough --
    // accept_loop() polls it, and run() closes the queue from normal context afterwards.
    void stop() override { stop_.store(true, std::memory_order_relaxed); }

    void run(Fd listener) override {
        listener_ = std::move(listener);
        set_nonblocking(listener_.get());

        size_t nworkers = cfg_.thread_pool_size ? cfg_.thread_pool_size : 1;
        EVP_LOG_INFO("thread_pool: %zu workers, queue capacity %zu (ceiling = %zu connections in flight)",
                     nworkers, cfg_.job_queue_capacity, nworkers);

        std::vector<std::thread> workers;
        workers.reserve(nworkers);
        for (size_t i = 0; i < nworkers; ++i)
            workers.emplace_back(&ThreadPool::worker_loop, this);

        accept_loop();

        // Producer is finished; let the workers drain what was already accepted, then join.
        queue_.close();
        for (auto& t : workers) t.join();

        EVP_LOG_INFO("thread_pool: stopped after %llu connections",
                     (unsigned long long)accepted_.load(std::memory_order_relaxed));
    }

private:
    void accept_loop() {
        while (!stop_.load(std::memory_order_relaxed)) {
            // The timeout is what lets us notice stop_ on an idle listener.
            if (!wait_ready(listener_.get(), /*for_write=*/false, 200)) continue;

            AcceptResult a = accept_one(listener_.get());
            if (a.status == AcceptStatus::WouldBlock) continue;
            if (a.status == AcceptStatus::OutOfFds) {
                EVP_LOG_ERROR("accept: out of file descriptors (limit %ld) -- raise ulimit -n",
                              fd_limit_current());
                continue;
            }
            if (a.status != AcceptStatus::Ok) {
                EVP_LOG_ERROR("accept: %s", std::strerror(errno));
                continue;
            }

            accepted_.fetch_add(1, std::memory_order_relaxed);

            // Waits when the queue is full. That is the point: we stop calling accept(), pending
            // connections back up into the kernel's listen backlog, and back-pressure reaches the
            // client instead of turning into unbounded memory here.
            //
            // The retry loop is what makes shutdown work while blocked on a full queue -- the stop
            // flag is re-checked every 200ms rather than requiring the signal handler to reach in
            // and close the queue under a lock.
            for (;;) {
                auto st = queue_.push_for(a.conn, 200);
                if (st == ConnQueue::PushStatus::Pushed) break;
                if (st == ConnQueue::PushStatus::Closed) return;
                if (stop_.load(std::memory_order_relaxed)) return;  // drop this one; we are exiting
            }
        }
    }

    void worker_loop() {
        Fd client;
        while (queue_.pop(client)) {
            Connection conn(std::move(client), cfg_, resolver_,
                            next_id_.fetch_add(1, std::memory_order_relaxed));

            // Serve to completion, including all the waiting. This is exactly where the seat is held.
            while (!conn.done()) {
                int fd = conn.want_fd();
                if (fd < 0) break;
                if (!wait_ready(fd, conn.want_write(), cfg_.poll_timeout_ms)) {
                    EVP_LOG_DEBUG("conn %llu: idle timeout", (unsigned long long)conn.id());
                    break;
                }
                conn.on_ready();
            }
        }
    }

    const Config&         cfg_;
    Resolver&             resolver_;
    ConnQueue             queue_;
    Fd                    listener_;
    std::atomic<bool>     stop_{false};
    std::atomic<uint64_t> next_id_{0};
    std::atomic<uint64_t> accepted_{0};
};

}  // namespace

std::unique_ptr<Backend> make_thread_pool(const Config& cfg, Resolver& resolver) {
    return std::make_unique<ThreadPool>(cfg, resolver);
}

}  // namespace evp
