// Minimal thread-safe leveled logger built on ostream insertion, so any type
// with operator<< logs without a format specifier.
#pragma once

#include <mutex>
#include <ostream>
#include <sstream>
#include <string>

namespace sdr {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

namespace detail {
void        log_write(LogLevel level, const std::string& message);
LogLevel    log_level();
}  // namespace detail

void set_log_level(LogLevel level);

template <typename... Args>
void log(LogLevel level, Args&&... args) {
    if (level > detail::log_level()) {
        return;
    }
    std::ostringstream oss;
    (oss << ... << std::forward<Args>(args));
    detail::log_write(level, oss.str());
}

template <typename... Args> void log_error(Args&&... a) { log(LogLevel::Error, std::forward<Args>(a)...); }
template <typename... Args> void log_warn (Args&&... a) { log(LogLevel::Warn,  std::forward<Args>(a)...); }
template <typename... Args> void log_info (Args&&... a) { log(LogLevel::Info,  std::forward<Args>(a)...); }
template <typename... Args> void log_debug(Args&&... a) { log(LogLevel::Debug, std::forward<Args>(a)...); }

}  // namespace sdr
