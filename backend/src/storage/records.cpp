#include "storage/records.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <utility>

#include "monobucket/constants.hpp"

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

namespace {

/// Field ids for the content headers. Written into the record, so they are
/// fixed forever: a new header takes the next free number and never reuses one.
enum class ContentHeaderId : std::uint8_t {
    CacheControl       = 1,
    ContentDisposition = 2,
    ContentEncoding    = 3,
    ContentLanguage    = 4,
    Expires            = 5,
};

}  // namespace

bool isStorableHeaderValue(std::string_view value) {
    if (value.size() > limits::kMaxContentHeaderLength) return false;

    for (const char ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

bool areStorableHeaderValues(const ContentHeaders& headers) {
    return isStorableHeaderValue(headers.cacheControl) &&
           isStorableHeaderValue(headers.contentDisposition) &&
           isStorableHeaderValue(headers.contentEncoding) &&
           isStorableHeaderValue(headers.contentLanguage) &&
           isStorableHeaderValue(headers.expires);
}

void encodeContentHeaders(codec::Writer& writer, const ContentHeaders& headers) {
    const std::pair<ContentHeaderId, const std::string&> fields[] = {
        {ContentHeaderId::CacheControl,       headers.cacheControl},
        {ContentHeaderId::ContentDisposition, headers.contentDisposition},
        {ContentHeaderId::ContentEncoding,    headers.contentEncoding},
        {ContentHeaderId::ContentLanguage,    headers.contentLanguage},
        {ContentHeaderId::Expires,            headers.expires},
    };

    std::uint64_t present = 0;
    for (const auto& [id, value] : fields) present += value.empty() ? 0 : 1;

    writer.varint(present);
    for (const auto& [id, value] : fields) {
        if (value.empty()) continue;
        writer.u8(static_cast<std::uint8_t>(id));
        writer.string(value);
    }
}

ContentHeaders decodeContentHeaders(codec::Reader& reader) {
    ContentHeaders headers;

    const std::uint64_t count = reader.varint();
    // There are five of them and a newer build can only have added more of the
    // same kind. A length past that is not a record we wrote.
    if (count > 64) throw codec::DecodeError("content header list is implausibly long");

    for (std::uint64_t i = 0; i < count; ++i) {
        const auto  id    = static_cast<ContentHeaderId>(reader.u8());
        std::string value = reader.string();
        switch (id) {
            case ContentHeaderId::CacheControl:
                headers.cacheControl = std::move(value);
                break;
            case ContentHeaderId::ContentDisposition:
                headers.contentDisposition = std::move(value);
                break;
            case ContentHeaderId::ContentEncoding:
                headers.contentEncoding = std::move(value);
                break;
            case ContentHeaderId::ContentLanguage:
                headers.contentLanguage = std::move(value);
                break;
            case ContentHeaderId::Expires:
                headers.expires = std::move(value);
                break;
            // A header a newer build added. Its value has already been
            // consumed, so the rest of the record still decodes — which is the
            // whole point of tagging the fields rather than writing them
            // positionally.
            default:
                break;
        }
    }
    return headers;
}

}  // namespace monobucket
