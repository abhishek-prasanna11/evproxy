#include "backend/backend.hpp"

namespace evp {

std::unique_ptr<Backend> make_backend(const Config& cfg, Resolver& resolver) {
    switch (cfg.backend) {
        case BackendKind::ThreadPerConn: return make_thread_per_conn(cfg, resolver);
        case BackendKind::ThreadPool:    return make_thread_pool(cfg, resolver);
        case BackendKind::EventLoop:     return make_event_loop(cfg, resolver);
    }
    return nullptr;
}

}  // namespace evp
