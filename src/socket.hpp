#pragma once

#include <cstddef>
#include <string>
#include <sys/socket.h>
#include <vector>

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

// Both ends non-blocking. Used as a wakeup channel so an async result can be waited on as an
// ordinary readable descriptor -- which is what lets all three backends handle it unchanged.
bool make_pipe(Fd& read_end, Fd& write_end);

// One non-blocking read into buf / write from buf. EINTR is retried internally.
IoResult read_some(int fd, char* buf, size_t len);
IoResult write_some(int fd, const char* buf, size_t len);

// Bind and listen. Returns an invalid Fd on failure, with err set.
Fd listen_on(const std::string& host, int port, int backlog, std::string& err);

// One candidate address for an origin. resolve() returns every candidate so a failed connect can
// fall through to the next -- that is how a host with an AAAA record but no working IPv6 route
// still gets proxied.
struct Endpoint {
    sockaddr_storage addr{};
    socklen_t        len      = 0;
    int              family   = 0;
    int              socktype = 0;
    int              protocol = 0;
};

// STILL BLOCKING, deliberately. getaddrinfo has no non-blocking form, and inside an event loop one
// slow lookup stalls every other connection. Phase 4's experiment is to measure that stall and then
// move resolution to its own threads.
bool resolve(const std::string& host, const std::string& port, std::vector<Endpoint>& out,
             std::string& err);

enum class ConnectStatus { Connected, InProgress, Failed };

// Creates a non-blocking socket and starts connecting. InProgress means connect() returned
// EINPROGRESS, which is success-so-far: completion arrives as a WRITABILITY event.
ConnectStatus start_connect(const Endpoint& ep, Fd& out, std::string& err);

// Call this once the connecting socket becomes writable. A socket whose connect FAILED also becomes
// writable, so this check is the only thing standing between you and writing a request into a dead
// connection.
bool connect_succeeded(int fd, std::string& err);

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
