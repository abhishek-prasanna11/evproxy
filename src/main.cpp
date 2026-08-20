#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "backend/backend.hpp"
#include "cache.hpp"
#include "config.hpp"
#include "log.hpp"
#include "resolver.hpp"
#include "socket.hpp"

namespace {

evp::Backend* g_backend = nullptr;

void on_signal(int) {
    // Signal-handler safe: touch nothing but the backend's atomic stop flag.
    if (g_backend) g_backend->stop();
}

void usage() {
    std::fprintf(stderr,
                 "usage: evproxy [-c config] [-p port] [-b backend] [-v level] [-w workers] [-q queuecap]\n"
                 "  backends: thread_per_conn | thread_pool | event_loop\n"
                 "  -w/-q apply to thread_pool only; -w IS the connections-in-flight ceiling\n"
                 "  -R 0|1 blocking|async resolve   -D ms injected resolver delay\n"
                 "  -C 0|1 response cache off|on (the I/O benchmark runs with -C 0)\n");
}

}  // namespace

int main(int argc, char** argv) {
    evp::Config cfg;
    std::string config_path;

    bool port_set = false, level_set = false, backend_set = false;
    bool workers_set = false, queue_set = false;

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
        else if (arg == "-w") { cfg.thread_pool_size = std::strtoul(next("-w"), nullptr, 10); workers_set = true; }
        else if (arg == "-C") { cfg.cache_enabled = (std::atoi(next("-C")) != 0); }
        else if (arg == "-R") { cfg.async_resolve = (std::atoi(next("-R")) != 0); }
        else if (arg == "-D") { cfg.resolve_delay_ms = std::atoi(next("-D")); }
        else if (arg == "-q") { cfg.job_queue_capacity = std::strtoul(next("-q"), nullptr, 10); queue_set = true; }
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
        const size_t           flag_workers = cfg.thread_pool_size;
        const size_t           flag_queue   = cfg.job_queue_capacity;

        cfg = from_file;
        if (port_set)    cfg.listen_port        = flag_port;
        if (level_set)   cfg.log_level          = flag_level;
        if (backend_set) cfg.backend            = flag_backend;
        if (workers_set) cfg.thread_pool_size   = flag_workers;
        if (queue_set)   cfg.job_queue_capacity = flag_queue;
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

    // Owns the resolver threads. Declared before the backend and destroyed after it, so no
    // Connection can still be holding a ResolveJob when the resolver threads are joined.
    evp::Resolver resolver(cfg.async_resolve ? cfg.resolver_threads : 0,
                           cfg.dns_cache_ttl_s, cfg.resolve_delay_ms);

    // Lock policy follows the backend: the event loop is single-threaded, so its cache needs no
    // mutex at all. Same code, one type parameter apart -- see docs/phase5.md.
    std::unique_ptr<evp::ResponseCache> cache;
    if (cfg.cache_enabled) cache = evp::make_cache(cfg);

    auto backend = evp::make_backend(cfg, resolver, cache.get());
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

    EVP_LOG_INFO("evproxy listening on %s:%d backend=%s resolve=%s",
                 cfg.listen_host.c_str(), cfg.listen_port, backend->name(),
                 cfg.async_resolve ? "async" : "blocking");
    if (cache)
        EVP_LOG_INFO("cache: on, %zu entries / %zu bytes max, lock=%s",
                     cfg.cache_max_entries, cfg.cache_max_bytes, cache->locked() ? "mutex" : "none");
    else
        EVP_LOG_INFO("cache: off");
    if (cfg.resolve_delay_ms > 0)
        EVP_LOG_INFO("resolve_delay_ms=%d -- FAULT INJECTION is active, not a natural measurement",
                     cfg.resolve_delay_ms);

    backend->run(std::move(listener));
    resolver.shutdown();

    if (cache)
        EVP_LOG_INFO("cache: %llu hits, %llu misses, %zu entries, %zu bytes",
                     (unsigned long long)cache->hits(), (unsigned long long)cache->misses(),
                     cache->entries(), cache->bytes());
    EVP_LOG_INFO("resolver: %llu lookups, %llu cache hits",
                 (unsigned long long)resolver.lookups(), (unsigned long long)resolver.cache_hits());
    EVP_LOG_INFO("clean shutdown");
    return 0;
}
