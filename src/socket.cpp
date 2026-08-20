#include "socket.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace evp {

void Fd::reset(int fd) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
}

bool set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool make_pipe(Fd& read_end, Fd& write_end) {
    int fds[2];
    if (::pipe(fds) != 0) return false;
    read_end.reset(fds[0]);
    write_end.reset(fds[1]);
    if (!set_nonblocking(read_end.get())) return false;
    return true;
}

IoResult read_some(int fd, char* buf, size_t len) {
    for (;;) {
        ssize_t n = ::recv(fd, buf, len, 0);
        if (n > 0) return {IoStatus::Ok, static_cast<size_t>(n)};
        if (n == 0) return {IoStatus::Closed, 0};  // orderly shutdown by peer
        if (errno == EINTR) continue;              // signal, not a failure
        if (errno == EAGAIN || errno == EWOULDBLOCK) return {IoStatus::WouldBlock, 0};
        return {IoStatus::Error, 0};
    }
}

IoResult write_some(int fd, const char* buf, size_t len) {
    for (;;) {
        // MSG_NOSIGNAL does not exist on macOS; SO_NOSIGPIPE is set per-socket instead, so a write
        // to a closed peer returns EPIPE rather than killing the process.
        ssize_t n = ::send(fd, buf, len, 0);
        if (n >= 0) return {IoStatus::Ok, static_cast<size_t>(n)};
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return {IoStatus::WouldBlock, 0};
        return {IoStatus::Error, 0};
    }
}

static void suppress_sigpipe(int fd) {
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
}

Fd listen_on(const std::string& host, int port, int backlog, std::string& err) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    std::string port_str = std::to_string(port);
    addrinfo*   res      = nullptr;

    int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) {
        err = std::string("getaddrinfo: ") + ::gai_strerror(rc);
        return Fd{};
    }

    Fd listener;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        Fd fd{::socket(p->ai_family, p->ai_socktype, p->ai_protocol)};
        if (!fd.valid()) continue;

        int on = 1;
        ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        suppress_sigpipe(fd.get());

        if (::bind(fd.get(), p->ai_addr, p->ai_addrlen) == 0 &&
            ::listen(fd.get(), backlog) == 0) {
            listener = std::move(fd);
            break;
        }
        err = std::string("bind/listen: ") + std::strerror(errno);
    }

    ::freeaddrinfo(res);
    if (!listener.valid() && err.empty()) err = "no usable address";
    return listener;
}

bool resolve(const std::string& host, const std::string& port, std::vector<Endpoint>& out,
             std::string& err) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    int       rc  = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        err = std::string("getaddrinfo(") + host + "): " + ::gai_strerror(rc);
        return false;
    }

    out.clear();
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        if (p->ai_addrlen > sizeof(sockaddr_storage)) continue;
        Endpoint ep;
        std::memcpy(&ep.addr, p->ai_addr, p->ai_addrlen);
        ep.len      = p->ai_addrlen;
        ep.family   = p->ai_family;
        ep.socktype = p->ai_socktype;
        ep.protocol = p->ai_protocol;
        out.push_back(ep);
    }
    ::freeaddrinfo(res);

    if (out.empty()) {
        err = "no usable address for " + host;
        return false;
    }
    return true;
}

ConnectStatus start_connect(const Endpoint& ep, Fd& out, std::string& err) {
    Fd fd{::socket(ep.family, ep.socktype, ep.protocol)};
    if (!fd.valid()) {
        err = std::string("socket: ") + std::strerror(errno);
        return ConnectStatus::Failed;
    }
    suppress_sigpipe(fd.get());

    // Non-blocking BEFORE connect, or connect() blocks regardless of what we do afterwards.
    if (!set_nonblocking(fd.get())) {
        err = std::string("set_nonblocking: ") + std::strerror(errno);
        return ConnectStatus::Failed;
    }

    int rc = ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&ep.addr), ep.len);
    if (rc == 0) {
        out = std::move(fd);
        return ConnectStatus::Connected;  // loopback usually lands here immediately
    }

    if (errno == EINPROGRESS || errno == EINTR) {
        // Success-so-far. Completion is reported as a writability event, and the socket becoming
        // writable does NOT by itself mean the connect worked -- see connect_succeeded().
        out = std::move(fd);
        return ConnectStatus::InProgress;
    }

    err = std::string("connect: ") + std::strerror(errno);
    return ConnectStatus::Failed;
}

bool connect_succeeded(int fd, std::string& err) {
    int       so_error = 0;
    socklen_t len      = sizeof(so_error);

    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) {
        err = std::string("getsockopt(SO_ERROR): ") + std::strerror(errno);
        return false;
    }
    if (so_error != 0) {
        err = std::string("connect: ") + std::strerror(so_error);
        return false;
    }
    return true;
}

AcceptResult accept_one(int listener_fd) {
    for (;;) {
        int fd = ::accept(listener_fd, nullptr, nullptr);
        if (fd >= 0) {
            suppress_sigpipe(fd);
            return {AcceptStatus::Ok, Fd{fd}};
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return {AcceptStatus::WouldBlock, Fd{}};
        if (errno == EMFILE || errno == ENFILE) return {AcceptStatus::OutOfFds, Fd{}};
        return {AcceptStatus::Error, Fd{}};
    }
}

bool wait_ready(int fd, bool for_write, int timeout_ms) {
    pollfd pfd{};
    pfd.fd     = fd;
    pfd.events = static_cast<short>(for_write ? POLLOUT : POLLIN);

    for (;;) {
        int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc > 0) return true;
        if (rc == 0) return false;              // timeout
        if (errno == EINTR) continue;
        return false;
    }
}

long fd_limit_current() {
    rlimit rl{};
    if (::getrlimit(RLIMIT_NOFILE, &rl) != 0) return -1;
    return static_cast<long>(rl.rlim_cur);
}

long fd_limit_raise(long desired) {
    rlimit rl{};
    if (::getrlimit(RLIMIT_NOFILE, &rl) != 0) return -1;

    rlim_t want = static_cast<rlim_t>(desired);
    if (rl.rlim_cur >= want) return static_cast<long>(rl.rlim_cur);

    // Never ask for more than the hard limit -- setrlimit fails outright rather than clamping.
    rlim_t target = (rl.rlim_max == RLIM_INFINITY) ? want : (want < rl.rlim_max ? want : rl.rlim_max);

    rlimit next = rl;
    next.rlim_cur = target;
    if (::setrlimit(RLIMIT_NOFILE, &next) != 0) return static_cast<long>(rl.rlim_cur);

    return fd_limit_current();
}

long open_fd_count() {
    long limit = fd_limit_current();
    if (limit < 0) return -1;

    long count = 0;
    for (int fd = 0; fd < limit; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1) ++count;
    }
    return count;
}

}  // namespace evp
