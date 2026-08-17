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

void encodeCorsRules(codec::Writer& writer, const std::vector<CorsRule>& rules) {
    writer.varint(rules.size());
    for (const CorsRule& rule : rules) {
        writer.string(rule.id);
        for (const auto* list : {&rule.allowedOrigins, &rule.allowedMethods, &rule.allowedHeaders,
                                 &rule.exposeHeaders}) {
            writer.varint(list->size());
            for (const std::string& entry : *list) writer.string(entry);
        }
        // The flag and the value are separate because "no max age" is a
        // distinct answer from zero: zero tells the browser not to cache the
        // preflight, absence lets it choose.
        writer.boolean(rule.maxAgeSeconds >= 0);
        writer.varint(rule.maxAgeSeconds >= 0 ? static_cast<std::uint64_t>(rule.maxAgeSeconds) : 0);
    }
}

std::vector<CorsRule> decodeCorsRules(codec::Reader& reader) {
    // Sized from the record before a byte of it is read, so a corrupt length is
    // refused rather than reserved: the counts are bounded by what the S3
    // schema accepts, and anything past that is not a rule set we wrote.
    const auto bounded = [](std::uint64_t count, std::uint64_t limit) {
        if (count > limit) throw codec::DecodeError("CORS rule list is implausibly long");
        return static_cast<std::size_t>(count);
    };

    std::vector<CorsRule> rules(bounded(reader.varint(), 100));
    for (CorsRule& rule : rules) {
        rule.id = reader.string();
        for (auto* list : {&rule.allowedOrigins, &rule.allowedMethods, &rule.allowedHeaders,
                           &rule.exposeHeaders}) {
            list->resize(bounded(reader.varint(), 1000));
            for (std::string& entry : *list) entry = reader.string();
        }
        const bool hasMaxAge = reader.boolean();
        const auto maxAge    = reader.varint();
        rule.maxAgeSeconds   = hasMaxAge ? static_cast<std::int32_t>(maxAge) : -1;
    }
    return rules;
}

}  // namespace monobucket
