#include "config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace evp {

bool parse_backend(const std::string& name, BackendKind& out) {
    if (name == "thread_per_conn") { out = BackendKind::ThreadPerConn; return true; }
    if (name == "thread_pool")     { out = BackendKind::ThreadPool;    return true; }
    if (name == "event_loop")      { out = BackendKind::EventLoop;     return true; }
    return false;
}

const char* backend_name(BackendKind kind) {
    switch (kind) {
        case BackendKind::ThreadPerConn: return "thread_per_conn";
        case BackendKind::ThreadPool:    return "thread_pool";
        case BackendKind::EventLoop:     return "event_loop";
    }
    return "unknown";
}

bool load_config(const std::string& path, Config& cfg, std::string& err) {
    std::ifstream in(path);
    if (!in) {
        err = "cannot open config: " + path;
        return false;
    }

    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (size_t hash = line.find('#'); hash != std::string::npos) line.erase(hash);

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            err = "line " + std::to_string(lineno) + ": expected key = value";
            return false;
        }

        auto trim = [](std::string s) {
            size_t b = s.find_first_not_of(" \t\r");
            if (b == std::string::npos) return std::string{};
            size_t e = s.find_last_not_of(" \t\r");
            return s.substr(b, e - b + 1);
        };

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key.empty()) continue;

        if      (key == "listen_host")      cfg.listen_host = val;
        else if (key == "listen_port")      cfg.listen_port = std::atoi(val.c_str());
        else if (key == "backlog")          cfg.backlog = std::atoi(val.c_str());
        else if (key == "max_header_bytes") cfg.max_header_bytes = std::strtoul(val.c_str(), nullptr, 10);
        else if (key == "max_body_bytes")   cfg.max_body_bytes = std::strtoul(val.c_str(), nullptr, 10);
        else if (key == "io_chunk_bytes")   cfg.io_chunk_bytes = std::strtoul(val.c_str(), nullptr, 10);
        else if (key == "desired_fd_limit") cfg.desired_fd_limit = std::strtol(val.c_str(), nullptr, 10);
        else if (key == "poll_timeout_ms")  cfg.poll_timeout_ms = std::atoi(val.c_str());
        else if (key == "async_resolve")      cfg.async_resolve = (std::atoi(val.c_str()) != 0);
        else if (key == "resolver_threads")   cfg.resolver_threads = std::strtoul(val.c_str(), nullptr, 10);
        else if (key == "dns_cache_ttl_s")    cfg.dns_cache_ttl_s = std::atoi(val.c_str());
        else if (key == "resolve_delay_ms")   cfg.resolve_delay_ms = std::atoi(val.c_str());
        else if (key == "thread_pool_size")   cfg.thread_pool_size = std::strtoul(val.c_str(), nullptr, 10);
        else if (key == "job_queue_capacity") cfg.job_queue_capacity = std::strtoul(val.c_str(), nullptr, 10);
        else if (key == "log_level")        cfg.log_level = std::atoi(val.c_str());
        else if (key == "backend") {
            if (!parse_backend(val, cfg.backend)) {
                err = "line " + std::to_string(lineno) + ": unknown backend '" + val + "'";
                return false;
            }
        } else {
            // Loud rather than silent: a typo in a benchmark config must not quietly run defaults.
            err = "line " + std::to_string(lineno) + ": unknown key '" + key + "'";
            return false;
        }
    }

    return true;
}

}  // namespace evp
