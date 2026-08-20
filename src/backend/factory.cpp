#include "backend/backend.hpp"

namespace evp {

std::unique_ptr<Backend> make_backend(const Config& cfg) {
    switch (cfg.backend) {
        case BackendKind::ThreadPerConn: return make_thread_per_conn(cfg);
        case BackendKind::ThreadPool:    return make_thread_pool(cfg);
        case BackendKind::EventLoop:     return make_event_loop(cfg);
    }
    return nullptr;
}

}  // namespace evp
