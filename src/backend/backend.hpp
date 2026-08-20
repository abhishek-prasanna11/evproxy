#pragma once

#include <memory>

#include "config.hpp"
#include "resolver.hpp"
#include "socket.hpp"

namespace evp {

// The one thing the three architectures disagree about: who waits, and where.
//
// Everything else -- parsing, buffering, the connection state machine -- is shared, so the phase 6
// benchmark measures the I/O architecture rather than three different programs.
class Backend {
public:
    virtual ~Backend() = default;

    // Takes ownership of the listening socket and serves until stop() is observed.
    virtual void run(Fd listener) = 0;

    // Called from a signal handler context in main: must only touch atomics.
    virtual void stop() = 0;

    virtual const char* name() const = 0;
};

// One factory per architecture; make_backend dispatches on cfg.backend. Kept separate so adding a
// backend does not touch another backend's file.
std::unique_ptr<Backend> make_thread_per_conn(const Config& cfg, Resolver& resolver);
std::unique_ptr<Backend> make_thread_pool(const Config& cfg, Resolver& resolver);
std::unique_ptr<Backend> make_event_loop(const Config& cfg, Resolver& resolver);

std::unique_ptr<Backend> make_backend(const Config& cfg, Resolver& resolver);

}  // namespace evp
