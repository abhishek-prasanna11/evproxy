#pragma once

#include <cstdio>
#include <mutex>
#include <string>

namespace evp {

// Deliberately tiny. The threaded backends need the mutex so lines don't interleave; the event loop
// backend never contends on it. Logging is off the measured path -- benchmarks run at level 0.
enum class LogLevel { Silent = 0, Error = 1, Info = 2, Debug = 3 };

inline LogLevel& log_level() {
    static LogLevel level = LogLevel::Info;
    return level;
}

inline std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

// Non-template overload for the no-argument case: passing a runtime string as fprintf's format is
// a format-string hazard, so a bare message goes through "%s".
inline void log_at(LogLevel level, const char* tag, const char* msg) {
    if (level > log_level()) return;
    std::lock_guard<std::mutex> lock(log_mutex());
    std::fprintf(stderr, "[%s] %s\n", tag, msg);
}

template <typename... Args>
void log_at(LogLevel level, const char* tag, const char* fmt, Args... args) {
    if (level > log_level()) return;
    std::lock_guard<std::mutex> lock(log_mutex());
    std::fprintf(stderr, "[%s] ", tag);
    std::fprintf(stderr, fmt, args...);
    std::fputc('\n', stderr);
}

#define EVP_LOG_INFO(...)  ::evp::log_at(::evp::LogLevel::Info, "info", __VA_ARGS__)
#define EVP_LOG_ERROR(...) ::evp::log_at(::evp::LogLevel::Error, "err ", __VA_ARGS__)
#define EVP_LOG_DEBUG(...) ::evp::log_at(::evp::LogLevel::Debug, "dbg ", __VA_ARGS__)

}  // namespace evp
