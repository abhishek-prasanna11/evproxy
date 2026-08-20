#pragma once

#include <cstddef>
#include <string>

namespace evp {

// Owning, move-only file descriptor. Closes on destruction.
//
// Every fd in this program lives in one of these. Three backends times a dozen error paths through
// a state machine is exactly where a bare close() gets missed, and a leaked descriptor does not
// crash -- it accumulates until accept() fails at the fd limit, which looks like a concurrency bug.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : fd_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    int  get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    int release() {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset(int fd = -1);

private:
    int fd_ = -1;
};

// Result of a single non-blocking read or write. WouldBlock is not an error: it means the kernel
// has nothing for us right now. Closed only ever comes from a read (recv returning 0).
enum class IoStatus { Ok, WouldBlock, Closed, Error };

struct IoResult {
    IoStatus status = IoStatus::Error;
    size_t   bytes  = 0;
};

bool set_nonblocking(int fd);

// One non-blocking read into buf / write from buf. EINTR is retried internally.
IoResult read_some(int fd, char* buf, size_t len);
IoResult write_some(int fd, const char* buf, size_t len);

// Bind and listen. Returns an invalid Fd on failure, with err set.
Fd listen_on(const std::string& host, int port, int backlog, std::string& err);

// Blocking resolve + connect. Deliberately blocking in phase 1 -- phase 4's experiment is to
// measure the damage this does inside an event loop and then fix it.
Fd connect_to(const std::string& host, const std::string& port, std::string& err);

// Accept one pending connection from a non-blocking listener. Shared by all three backends so they
// handle the fd ceiling identically -- EMFILE is a resource limit, not a proxy bug, and it is the
// failure most often misdiagnosed as a broken accept loop.
enum class AcceptStatus { Ok, WouldBlock, OutOfFds, Error };

struct AcceptResult {
    AcceptStatus status = AcceptStatus::Error;
    Fd           conn;
};

AcceptResult accept_one(int listener_fd);

// Wait for one fd to become readable or writable. Used only by the blocking backends; the event
// loop backend never calls this. Returns true if ready, false on timeout or error.
bool wait_ready(int fd, bool for_write, int timeout_ms);

// RLIMIT_NOFILE handling. macOS defaults to 256, which makes a healthy proxy look broken at ~250
// connections, and silently caps any benchmark that does not check.
long fd_limit_current();
long fd_limit_raise(long desired);

// Number of currently open descriptors in this process. Used by the leak assertions.
long open_fd_count();

}  // namespace evp
