#include "storage/records.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace monobucket {

TimestampMs nowMs() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string toIso8601(TimestampMs ms) {
    const auto seconds = static_cast<std::time_t>(ms / 1000);

    std::tm utc{};
    // gmtime_r rather than gmtime: request handlers format timestamps
    // concurrently and the non-reentrant form returns shared storage.
    if (::gmtime_r(&seconds, &utc) == nullptr) return "1970-01-01T00:00:00.000Z";

    std::array<char, 32> buffer{};
    const std::size_t    written =
        std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S", &utc);
    if (written == 0) return "1970-01-01T00:00:00.000Z";

    // S3 renders millisecond precision with a literal trailing Z.
    const auto millis = static_cast<int>(((ms % 1000) + 1000) % 1000);

    std::array<char, 8> fraction{};
    std::snprintf(fraction.data(), fraction.size(), ".%03dZ", millis);

    return std::string(buffer.data(), written) + fraction.data();
}

}  // namespace monobucket
