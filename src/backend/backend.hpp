#pragma once

#include <memory>

#include "config.hpp"
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

std::unique_ptr<Backend> make_backend(const Config& cfg);

}  // namespace evp
