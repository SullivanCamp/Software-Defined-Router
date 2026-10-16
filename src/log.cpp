#include "sdr/log.hpp"

#include <ctime>
#include <iomanip>
#include <iostream>

namespace sdr {
namespace {

std::mutex g_mutex;
LogLevel   g_level = LogLevel::Info;

std::string_view level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Debug: return "DEBUG";
    }
    return "?????";
}

}  // namespace

void set_log_level(LogLevel level) { g_level = level; }

namespace detail {

LogLevel log_level() { return g_level; }

void log_write(LogLevel level, const std::string& message) {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    std::tm tm{};
    ::localtime_r(&ts.tv_sec, &tm);

    std::lock_guard<std::mutex> lock(g_mutex);
    std::cerr << '[' << std::put_time(&tm, "%H:%M:%S") << '.'
              << std::setfill('0') << std::setw(3) << (ts.tv_nsec / 1000000)
              << std::setfill(' ') << "] " << level_name(level) << ' '
              << message << '\n';
    std::cerr.flush();
}

}  // namespace detail
}  // namespace sdr
