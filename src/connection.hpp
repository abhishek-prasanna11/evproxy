#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"
#include "http.hpp"
#include "socket.hpp"

namespace evp {

// What this connection needs the world to do for it next.
//
// This enum is the entire contract between the connection logic and the three I/O backends. The
// connection never blocks and never owns a thread; it states an intent, somebody else decides when
// that intent is satisfiable:
//
//   thread-per-connection -> poll() this one fd, then call on_ready()
//   bounded thread pool   -> same, from a worker
//   kqueue event loop     -> register this fd with the matching filter, call on_ready() on the event
//
// Because all three drive the same state machine over the same parser and buffers, the benchmark
// isolates the I/O architecture instead of comparing three different programs.
enum class Want { ReadClient, WriteClient, ReadUpstream, WriteUpstream, None };

class Connection {
public:
    Connection(Fd client, const Config& cfg, uint64_t id);

    Want want() const { return want_; }
    int  want_fd() const;
    bool want_write() const {
        return want_ == Want::WriteClient || want_ == Want::WriteUpstream;
    }

    // Called when want_fd() is ready in the want() direction. Performs at most one non-blocking
    // read or write and advances the state machine.
    void on_ready();

    bool done() const { return state_ == State::Done; }
    uint64_t id() const { return id_; }

private:
    enum class State { ReadingRequest, Connecting, SendingUpstream, Relaying, Failing, Done };

    void do_read_client();
    void do_write_client();
    void do_read_upstream();
    void do_write_upstream();

    // Starts a non-blocking connect to endpoints_[endpoint_idx_], advancing through the candidate
    // list until one is InProgress/Connected or all have failed.
    void begin_connect();
    void finish_connect();

    // Abandon the exchange and send `body` to the client instead. The upstream fd (if any) is
    // dropped immediately: nothing further can be relayed.
    void fail_with(std::string response);
    void finish();

    Fd            client_;
    Fd            upstream_;
    const Config& cfg_;
    uint64_t      id_;

    State state_ = State::ReadingRequest;
    Want  want_  = Want::ReadClient;

    Request     req_;
    bool        parsed_ = false;

    std::vector<Endpoint> endpoints_;
    size_t                endpoint_idx_ = 0;

    std::string in_client_;        // raw request bytes as they arrive
    std::string out_upstream_;     // rebuilt request awaiting flush to origin
    size_t      out_upstream_off_ = 0;
    std::string out_client_;       // response bytes awaiting flush to client
    size_t      out_client_off_ = 0;

    bool upstream_eof_ = false;
};

}  // namespace evp
