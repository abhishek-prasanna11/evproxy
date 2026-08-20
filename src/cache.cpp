#include "cache.hpp"

namespace evp {

std::unique_ptr<ResponseCache> make_cache(const Config& cfg) {
    if (cfg.backend == BackendKind::EventLoop)
        return std::make_unique<UnlockedCache>(cfg.cache_max_entries, cfg.cache_max_bytes);
    return std::make_unique<LockedCache>(cfg.cache_max_entries, cfg.cache_max_bytes);
}

std::string cache_key(const std::string& method, const std::string& host, const std::string& port,
                      const std::string& path) {
    std::string key;
    key.reserve(method.size() + host.size() + port.size() + path.size() + 3);
    key += method;
    key += ' ';
    key += host;
    key += ':';
    key += port;
    key += path;
    return key;
}

}  // namespace evp
