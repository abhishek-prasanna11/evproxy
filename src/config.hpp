#pragma once

#include <cstddef>
#include <string>

namespace evp {

enum class BackendKind { ThreadPerConn, ThreadPool, EventLoop };

struct Config {
    std::string listen_host = "127.0.0.1";
    int         listen_port = 8888;
    int         backlog     = 512;

    BackendKind backend = BackendKind::ThreadPerConn;

    // thread_pool only. The pool's real limit is connections in flight, not requests/sec: a worker
    // blocked on a slow client is holding a seat, not doing work. thread_pool_size IS that ceiling.
    size_t thread_pool_size   = 64;
    size_t job_queue_capacity = 256;

    // Bounded so a client cannot make us buffer without limit. A request whose headers exceed this
    // gets 400 rather than growing the buffer forever.
    size_t max_header_bytes = 16 * 1024;
    size_t max_body_bytes   = 8 * 1024 * 1024;
    size_t io_chunk_bytes   = 32 * 1024;

    // Raised at startup via setrlimit; the effective value is logged. macOS defaults to 256, which
    // makes a working proxy look broken at ~250 connections and silently caps any benchmark.
    long desired_fd_limit = 10240;

    int poll_timeout_ms = 30000;

    // Resolver. getaddrinfo blocks and has no non-blocking form, so in the event loop one slow
    // lookup stalls every other connection. async_resolve=0 is the naive arm, kept so both can be
    // measured from one binary.
    bool   async_resolve    = true;
    size_t resolver_threads = 4;
    int    dns_cache_ttl_s  = 60;

    // Fault injection only: sleeps inside the resolve path to model a slow resolver. Stated as
    // injection rather than dressed up as a natural measurement.
    int resolve_delay_ms = 0;

    int log_level = 2;
};

// Parses `key = value` lines; `#` begins a comment. Unknown keys are an error, so a typo in a
// benchmark config fails loudly instead of silently running the default.
bool load_config(const std::string& path, Config& cfg, std::string& err);

bool parse_backend(const std::string& name, BackendKind& out);
const char* backend_name(BackendKind kind);

}  // namespace evp
