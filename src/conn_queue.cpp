#include "conn_queue.hpp"

#include <chrono>

namespace evp {

ConnQueue::PushStatus ConnQueue::push_for(Fd& fd, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    // wait_for with a predicate, not a bare wait: a condition variable can wake spuriously, and with
    // several producers the space we were woken for may already be taken by the time we hold the
    // lock. The predicate is re-checked on every wake and once more at timeout.
    bool ready = not_full_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                    [this] { return closed_ || queue_.size() < capacity_; });

    if (closed_) return PushStatus::Closed;
    if (!ready)  return PushStatus::TimedOut;

    queue_.push_back(std::move(fd));
    lock.unlock();
    not_empty_.notify_one();
    return PushStatus::Pushed;
}

bool ConnQueue::pop(Fd& out) {
    std::unique_lock<std::mutex> lock(mutex_);

    not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });

    // Closed but not yet drained: keep serving what was already accepted.
    if (queue_.empty()) return false;

    out = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    not_full_.notify_one();
    return true;
}

void ConnQueue::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        closed_ = true;
    }
    // Wake everyone: blocked consumers so they can exit, and blocked producers so the accept loop
    // does not sit in push() forever while we try to join it.
    not_empty_.notify_all();
    not_full_.notify_all();
}

size_t ConnQueue::depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

}  // namespace evp
