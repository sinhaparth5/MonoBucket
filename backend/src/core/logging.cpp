#include "core/logging.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <mutex>

namespace monobucket::log {
namespace {

std::atomic<Level> g_level{Level::Info};
std::mutex         g_writeMutex;

std::string timestamp() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto secs = clock::to_time_t(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif

    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
       << millis << 'Z';
    return os.str();
}

}  // namespace

std::string_view toString(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
        case Level::Off:   return "OFF";
    }
    return "INFO";
}

std::optional<Level> fromString(std::string_view name) {
    if (name == "trace") return Level::Trace;
    if (name == "debug") return Level::Debug;
    if (name == "info")  return Level::Info;
    if (name == "warn" || name == "warning") return Level::Warn;
    if (name == "error") return Level::Error;
    if (name == "off" || name == "none") return Level::Off;
    return std::nullopt;
}

void setLevel(Level level) noexcept { g_level.store(level, std::memory_order_relaxed); }

Level level() noexcept { return g_level.load(std::memory_order_relaxed); }

void write(Level lvl, std::string_view message) {
    const std::string line = timestamp() + " [" + std::string(toString(lvl)) + "] " +
                             std::string(message) + "\n";
    // Logs go to stderr so that stdout stays free for anything the binary may
    // need to emit in a pipeline (e.g. `monobucket --version`).
    std::lock_guard<std::mutex> guard(g_writeMutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

}  // namespace monobucket::log
