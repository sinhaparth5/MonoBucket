#include "s3/request.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>

// strptime() and timegm() are POSIX rather than ISO C, and <ctime> is not
// obliged to declare them.
#include <time.h>

#include "monobucket/constants.hpp"
#include "s3/s3_error.hpp"

namespace monobucket::s3 {
namespace {

/// Paths the server serves itself on the S3 listener. Drogon matches an exact
/// path before the catch-all, so a bucket with one of these names would be
/// unreachable over HTTP even though it existed in the metadata store.
constexpr std::array<std::string_view, 4> kReservedNames{"healthz", "readyz", "metrics", "_mb"};

bool looksLikeIpv4(std::string_view name) {
    int  octets = 0;
    int  digits = 0;
    int  value  = 0;

    for (std::size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '.') {
            if (digits == 0 || digits > 3 || value > 255) return false;
            ++octets;
            digits = 0;
            value  = 0;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(name[i])) == 0) return false;
        value = value * 10 + (name[i] - '0');
        ++digits;
    }
    return octets == 4;
}

std::string toLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

S3Request parseRequest(std::string_view method, std::string_view decodedPath,
                       std::string_view rawQuery, std::string_view virtualHostBucket) {
    S3Request request;
    request.method    = std::string(method);
    request.query     = parseQuery(rawQuery);
    request.resource  = std::string(decodedPath);
    request.requestId = newRequestId();

    if (!decodedPath.empty() && decodedPath.front() == '/') decodedPath.remove_prefix(1);

    if (!virtualHostBucket.empty()) {
        request.bucket = std::string(virtualHostBucket);
        request.key    = std::string(decodedPath);
        return request;
    }

    const std::size_t slash = decodedPath.find('/');
    if (slash == std::string_view::npos) {
        request.bucket = std::string(decodedPath);
    } else {
        request.bucket = std::string(decodedPath.substr(0, slash));
        // Everything after the first slash, separators included: an object key
        // is an opaque string that happens to be allowed to contain them.
        request.key = std::string(decodedPath.substr(slash + 1));
    }

    return request;
}

std::string virtualHostBucket(std::string_view host, std::string_view domain) {
    if (domain.empty() || host.empty()) return {};

    // Strip the port; Host carries one whenever it is not the scheme default.
    if (const std::size_t colon = host.rfind(':'); colon != std::string_view::npos &&
                                                   host.find(']') == std::string_view::npos) {
        host = host.substr(0, colon);
    }

    const std::string loweredHost   = toLower(host);
    const std::string loweredDomain = toLower(domain);

    if (loweredHost == loweredDomain) return {};  // path style against the endpoint itself

    const std::string suffix = "." + loweredDomain;
    if (loweredHost.size() <= suffix.size()) return {};
    if (loweredHost.compare(loweredHost.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return {};
    }

    std::string bucket = loweredHost.substr(0, loweredHost.size() - suffix.size());

    // A bucket containing dots is legal and addressable this way, but a label
    // that is not a valid bucket name is more likely a different service on the
    // same domain than a bucket we should invent.
    return isValidBucketName(bucket) ? bucket : std::string();
}

bool isValidBucketName(std::string_view name) {
    if (name.size() < limits::kMinBucketNameLength || name.size() > limits::kMaxBucketNameLength) {
        return false;
    }

    if (std::isalnum(static_cast<unsigned char>(name.front())) == 0) return false;
    if (std::isalnum(static_cast<unsigned char>(name.back())) == 0) return false;

    char previous = '\0';
    for (const char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool          allowed =
            (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!allowed) return false;
        if (ch == '.' && previous == '.') return false;
        previous = ch;
    }

    // A name shaped like an address would be ambiguous with the endpoint itself
    // in a virtual-host URL.
    return !looksLikeIpv4(name);
}

bool isReservedBucketName(std::string_view name) {
    return std::find(kReservedNames.begin(), kReservedNames.end(), name) != kReservedNames.end();
}

bool isValidObjectKey(std::string_view key) {
    if (key.empty() || key.size() > limits::kMaxKeyLength) return false;

    // A key containing a path traversal segment never reaches the filesystem —
    // payloads are named by blob id, not by key — but it would still be a key
    // no client could address unambiguously.
    if (key == "." || key == ".." || key.find("/../") != std::string_view::npos ||
        key.rfind("../", 0) == 0) {
        return false;
    }

    for (const char ch : key) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

// --- Header parsing --------------------------------------------------------

RangeResult parseRange(std::string_view header, std::uint64_t objectSize, ByteRange& out) {
    constexpr std::string_view kUnit = "bytes=";
    if (header.compare(0, kUnit.size(), kUnit) != 0) return RangeResult::Absent;

    std::string_view spec = header.substr(kUnit.size());
    if (spec.find(',') != std::string_view::npos) return RangeResult::Absent;  // multi-range

    const std::size_t dash = spec.find('-');
    if (dash == std::string_view::npos) return RangeResult::Absent;

    const std::string_view firstText = spec.substr(0, dash);
    const std::string_view lastText  = spec.substr(dash + 1);
    if (firstText.empty() && lastText.empty()) return RangeResult::Absent;

    const auto number = [](std::string_view text) -> std::optional<std::uint64_t> {
        if (text.empty() || text.size() > 19) return std::nullopt;
        std::uint64_t value = 0;
        for (const char ch : text) {
            if (std::isdigit(static_cast<unsigned char>(ch)) == 0) return std::nullopt;
            value = value * 10 + static_cast<std::uint64_t>(ch - '0');
        }
        return value;
    };

    if (firstText.empty()) {
        // `bytes=-N` — the last N bytes. Larger than the object means the whole
        // object, which is satisfiable rather than an error.
        const auto count = number(lastText);
        if (!count) return RangeResult::Absent;
        if (*count == 0) return RangeResult::Unsatisfiable;
        const std::uint64_t length = std::min(*count, objectSize);
        out.offset = objectSize - length;
        out.length = length;
        return length == 0 ? RangeResult::Unsatisfiable : RangeResult::Satisfiable;
    }

    const auto first = number(firstText);
    if (!first) return RangeResult::Absent;
    if (*first >= objectSize) return RangeResult::Unsatisfiable;

    std::uint64_t last = objectSize - 1;
    if (!lastText.empty()) {
        const auto explicitLast = number(lastText);
        if (!explicitLast) return RangeResult::Absent;
        last = std::min(*explicitLast, objectSize - 1);
        if (last < *first) return RangeResult::Unsatisfiable;
    }

    out.offset = *first;
    out.length = last - *first + 1;
    return RangeResult::Satisfiable;
}

std::string toHttpDate(std::int64_t epochMs) {
    const std::time_t stamp = static_cast<std::time_t>(epochMs / 1000);
    std::tm           parts{};
#if defined(_WIN32)
    ::gmtime_s(&parts, &stamp);
#else
    ::gmtime_r(&stamp, &parts);
#endif

    // Written out rather than taken from strftime: %a and %b are locale
    // dependent, and HTTP dates are not.
    static constexpr const char* kDays[]   = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                  kDays[parts.tm_wday % 7], parts.tm_mday, kMonths[parts.tm_mon % 12],
                  parts.tm_year + 1900, parts.tm_hour, parts.tm_min, parts.tm_sec);
    return std::string(buffer);
}

std::optional<std::int64_t> parseHttpDate(std::string_view text) {
    const std::string value(text);
    static constexpr const char* kFormats[] = {
        "%a, %d %b %Y %H:%M:%S",  // IMF-fixdate, what everything sends
        "%A, %d-%b-%y %H:%M:%S",  // RFC 850
        "%a %b %d %H:%M:%S %Y",   // asctime
    };

    for (const char* format : kFormats) {
        std::tm parts{};
        if (::strptime(value.c_str(), format, &parts) != nullptr) {
#if defined(_WIN32)
            const std::time_t stamp = ::_mkgmtime(&parts);
#else
            const std::time_t stamp = ::timegm(&parts);
#endif
            if (stamp != static_cast<std::time_t>(-1)) {
                return static_cast<std::int64_t>(stamp) * 1000;
            }
        }
    }
    return std::nullopt;
}

std::string_view unquoteETag(std::string_view etag) {
    // A conditional header may carry the weak-comparison marker.
    if (etag.rfind("W/", 0) == 0) etag.remove_prefix(2);
    if (etag.size() >= 2 && etag.front() == '"' && etag.back() == '"') {
        etag.remove_prefix(1);
        etag.remove_suffix(1);
    }
    return etag;
}

std::string quoteETag(std::string_view etag) {
    std::string out;
    out.reserve(etag.size() + 2);
    out.push_back('"');
    out.append(etag);
    out.push_back('"');
    return out;
}

std::string userMetadataKey(std::string_view headerName) {
    constexpr std::string_view kPrefix = "x-amz-meta-";
    const std::string          lowered = toLower(headerName);
    if (lowered.compare(0, kPrefix.size(), kPrefix) != 0) return {};
    return lowered.substr(kPrefix.size());
}

std::string encodeListingValue(std::string_view value, bool urlEncode) {
    return urlEncode ? uriEncode(value, true) : std::string(value);
}

}  // namespace monobucket::s3
