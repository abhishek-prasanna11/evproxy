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

    // Bounded so a client cannot make us buffer without limit. A request whose headers exceed this
    // gets 400 rather than growing the buffer forever.
    size_t max_header_bytes = 16 * 1024;
    size_t max_body_bytes   = 8 * 1024 * 1024;
    size_t io_chunk_bytes   = 32 * 1024;

    // Raised at startup via setrlimit; the effective value is logged. macOS defaults to 256, which
    // makes a working proxy look broken at ~250 connections and silently caps any benchmark.
    long desired_fd_limit = 10240;

    int poll_timeout_ms = 30000;

    int log_level = 2;
};

// Parses `key = value` lines; `#` begins a comment. Unknown keys are an error, so a typo in a
// benchmark config fails loudly instead of silently running the default.
bool load_config(const std::string& path, Config& cfg, std::string& err);

bool parse_backend(const std::string& name, BackendKind& out);
const char* backend_name(BackendKind kind);

}  // namespace evp
