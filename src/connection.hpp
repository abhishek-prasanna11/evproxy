#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cache.hpp"
#include "config.hpp"
#include "http.hpp"
#include "resolver.hpp"
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
enum class Want { ReadClient, WriteClient, ReadUpstream, WriteUpstream, ReadResolver, None };

class Connection {
public:
    Connection(Fd client, const Config& cfg, Resolver& resolver, ResponseCache* cache,
               uint64_t id);

    Want want() const { return want_; }
    int  want_fd() const;
    bool want_write() const {
        return want_ == Want::WriteClient || want_ == Want::WriteUpstream;
    }

    // Called when want_fd() is ready in the want() direction. Performs at most one non-blocking
    // read or write and advances the state machine.
    void on_ready();

    // Bumped every time the connection changes what it is waiting on.
    //
    // A backend cannot decide "the registration is unchanged" from (fd, direction) alone: closing a
    // socket and immediately opening another -- which is exactly what trying the next candidate
    // address does -- frequently REUSES the same fd number. The pair looks identical while the
    // kernel has already dropped the old registration along with the old descriptor, so the new
    // socket ends up registered nowhere and the connection hangs forever.
    uint64_t epoch() const { return epoch_; }

    bool done() const { return state_ == State::Done; }
    uint64_t id() const { return id_; }

private:
    enum class State { ReadingRequest, Resolving, Connecting, SendingUpstream, Relaying, Failing, Done };

    void do_read_client();
    void do_write_client();
    void do_read_upstream();
    void do_write_upstream();

    // Kicks off resolution: a cache hit goes straight to connecting, otherwise the connection waits
    // on the job's pipe fd like any other readable descriptor -- which is why no backend needs to
    // know that resolution is special.
    // True only for requests whose response may be shared between clients: GET/HEAD, and nothing
    // carrying credentials. Serving one user's response to another is the classic proxy-cache hole.
    bool cacheable_request() const;
    void store_in_cache();

    void begin_resolve();
    void finish_resolve();

    // Starts a non-blocking connect to endpoints_[endpoint_idx_], advancing through the candidate
    // list until one is InProgress/Connected or all have failed.
    void begin_connect();
    void finish_connect();

    // Abandon the exchange and send `body` to the client instead. The upstream fd (if any) is
    // dropped immediately: nothing further can be relayed.
    void fail_with(std::string response);
    void finish();

    // Every want_ assignment goes through here so no path can forget to bump the epoch.
    void set_want(Want w) { want_ = w; ++epoch_; }

    Fd            client_;
    Fd            upstream_;
    const Config& cfg_;
    Resolver&      resolver_;
    ResponseCache* cache_ = nullptr;
    uint64_t      id_;

    State    state_ = State::ReadingRequest;
    Want     want_  = Want::ReadClient;
    uint64_t epoch_ = 0;

    Request     req_;
    bool        parsed_ = false;

    std::shared_ptr<ResolveJob> resolve_job_;
    std::vector<Endpoint> endpoints_;
    size_t                endpoint_idx_ = 0;

    std::string in_client_;        // raw request bytes as they arrive
    std::string out_upstream_;     // rebuilt request awaiting flush to origin
    size_t      out_upstream_off_ = 0;
    std::string out_client_;       // response bytes awaiting flush to client
    size_t      out_client_off_ = 0;

    bool upstream_eof_ = false;

    std::string cache_key_;      // empty when this exchange is not cacheable
    std::string cache_accum_;    // response bytes accumulated for storage
    bool        cache_too_big_ = false;
};

}  // namespace evp
