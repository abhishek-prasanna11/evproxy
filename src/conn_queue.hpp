#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

#include "socket.hpp"

namespace evp {

// Bounded producer-consumer queue of accepted connections.
//
// Bounded on purpose. When it fills, push() blocks, so the accept loop stops calling accept() and
// pending connections back up into the kernel's listen backlog -- real back-pressure that reaches
// the client. An unbounded queue would trade that clear signal for unbounded memory and latency
// that grows without limit.
class ConnQueue {
public:
    explicit ConnQueue(size_t capacity) : capacity_(capacity ? capacity : 1) {}

    enum class PushStatus { Pushed, TimedOut, Closed };

    // Waits up to timeout_ms for space. `fd` is moved from only on Pushed; on TimedOut or Closed it
    // is left intact so the caller still owns the connection.
    //
    // The timeout exists so the accept loop can re-check its stop flag while blocked on a full
    // queue. Without it, shutdown would have to reach in and close the queue from a signal handler
    // -- which takes a mutex, and pthread_mutex_lock is not async-signal-safe. See docs/phase2.md.
    PushStatus push_for(Fd& fd, int timeout_ms);

    // Blocks while empty. Returns false once the queue is closed AND drained, which is how workers
    // learn to exit.
    bool pop(Fd& out);

    // Wakes every blocked producer and consumer. Idempotent.
    void close();

    size_t depth() const;

private:
    mutable std::mutex      mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<Fd>          queue_;
    size_t                  capacity_;
    bool                    closed_ = false;
};

}  // namespace evp
