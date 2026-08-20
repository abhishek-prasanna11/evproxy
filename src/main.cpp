#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "backend/backend.hpp"
#include "config.hpp"
#include "log.hpp"
#include "socket.hpp"

namespace {

evp::Backend* g_backend = nullptr;

void on_signal(int) {
    // Signal-handler safe: touch nothing but the backend's atomic stop flag.
    if (g_backend) g_backend->stop();
}

void usage() {
    std::fprintf(stderr,
                 "usage: evproxy [-c config] [-p port] [-b backend] [-v level]\n"
                 "  backends: thread_per_conn | thread_pool | event_loop\n");
}

}  // namespace

int main(int argc, char** argv) {
    evp::Config cfg;
    std::string config_path;

    bool port_set = false, level_set = false, backend_set = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };

        if      (arg == "-c") config_path = next("-c");
        else if (arg == "-p") { cfg.listen_port = std::atoi(next("-p")); port_set = true; }
        else if (arg == "-v") { cfg.log_level = std::atoi(next("-v")); level_set = true; }
        else if (arg == "-b") {
            const char* name = next("-b");
            if (!evp::parse_backend(name, cfg.backend)) {
                std::fprintf(stderr, "unknown backend '%s'\n", name);
                return 2;
            }
            backend_set = true;
        } else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument '%s'\n", arg.c_str());
            usage();
            return 2;
        }
    }

    if (!config_path.empty()) {
        // Config first, then re-apply flags so the command line wins.
        evp::Config from_file;
        std::string err;
        if (!evp::load_config(config_path, from_file, err)) {
            std::fprintf(stderr, "config: %s\n", err.c_str());
            return 2;
        }
        const evp::BackendKind flag_backend = cfg.backend;
        const int              flag_port    = cfg.listen_port;
        const int              flag_level   = cfg.log_level;

        cfg = from_file;
        if (port_set)    cfg.listen_port = flag_port;
        if (level_set)   cfg.log_level   = flag_level;
        if (backend_set) cfg.backend     = flag_backend;
    }

    evp::log_level() = static_cast<evp::LogLevel>(cfg.log_level);

    // Raise RLIMIT_NOFILE before binding, and state the effective value. macOS defaults to 256:
    // a proxy that stalls at ~250 connections looks broken and is not, and a benchmark run that
    // silently hits this ceiling reports an artifact rather than a result.
    long limit = evp::fd_limit_raise(cfg.desired_fd_limit);
    EVP_LOG_INFO("fd limit: %ld (requested %ld)", limit, cfg.desired_fd_limit);
    if (limit < cfg.desired_fd_limit)
        EVP_LOG_ERROR("fd limit below requested -- high-concurrency runs will cap here, not in the proxy");

    std::string err;
    evp::Fd listener = evp::listen_on(cfg.listen_host, cfg.listen_port, cfg.backlog, err);
    if (!listener.valid()) {
        std::fprintf(stderr, "listen: %s\n", err.c_str());
        return 1;
    }

    auto backend = evp::make_backend(cfg);
    if (!backend) {
        std::fprintf(stderr, "backend '%s' is not implemented yet\n", evp::backend_name(cfg.backend));
        return 2;
    }
    g_backend = backend.get();

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: we want blocking calls interrupted
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    EVP_LOG_INFO("evproxy listening on %s:%d backend=%s",
                 cfg.listen_host.c_str(), cfg.listen_port, backend->name());

    backend->run(std::move(listener));

    EVP_LOG_INFO("clean shutdown");
    return 0;
}
