#include "cache/redis_url.hpp"

#include <cctype>
#include <charconv>

#include "core/config.hpp"

namespace monobucket {
namespace {

constexpr std::string_view kScheme = "redis://";

[[noreturn]] void reject(std::string_view url, std::string_view why) {
    throw ConfigError("MONOBUCKET_REDIS_URL is not usable ('" + std::string(url) + "'): " +
                      std::string(why) +
                      "\nExpected redis://[user[:password]@]host[:port][/db]");
}

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Decodes %XX. A stray `%` is left alone rather than treated as an error —
/// passwords legitimately contain one, and refusing them would be worse than
/// accepting a literal.
std::string percentDecode(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());

    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size()) {
            const int hi = hexValue(raw[i + 1]);
            const int lo = hexValue(raw[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(raw[i]);
    }
    return out;
}

bool parseUnsigned(std::string_view text, unsigned long& out) {
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last  = text.data() + text.size();
    const auto  result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

}  // namespace

RedisEndpoint parseRedisUrl(std::string_view url) {
    if (url.empty()) reject(url, "it is empty");

    if (url.rfind("rediss://", 0) == 0) {
        reject(url,
               "TLS (rediss://) is not supported; hiredis is built without SSL. Terminate TLS "
               "in front of Redis, or connect over a private network");
    }
    if (url.rfind(kScheme, 0) != 0) {
        reject(url, "it must begin with redis://");
    }

    std::string_view rest = url.substr(kScheme.size());
    RedisEndpoint    endpoint;

    // --- optional /db ------------------------------------------------------
    if (const auto slash = rest.find('/'); slash != std::string_view::npos) {
        const std::string_view dbText = rest.substr(slash + 1);
        rest                          = rest.substr(0, slash);

        if (!dbText.empty()) {
            unsigned long db = 0;
            if (!parseUnsigned(dbText, db) || db > 15) {
                reject(url, "the database index must be a number between 0 and 15");
            }
            endpoint.db = static_cast<int>(db);
        }
    }

    // --- optional userinfo -------------------------------------------------
    // Searched from the right: a password may itself contain '@'.
    if (const auto at = rest.rfind('@'); at != std::string_view::npos) {
        const std::string_view userinfo = rest.substr(0, at);
        rest                            = rest.substr(at + 1);

        if (const auto colon = userinfo.find(':'); colon != std::string_view::npos) {
            endpoint.username = percentDecode(userinfo.substr(0, colon));
            endpoint.password = percentDecode(userinfo.substr(colon + 1));
        } else {
            // `redis://secret@host` is the common shorthand for a password with
            // no ACL user, which is how Redis itself documents it.
            endpoint.password = percentDecode(userinfo);
        }
    }

    // --- host[:port] -------------------------------------------------------
    if (rest.empty()) reject(url, "no host was given");

    // A bracketed IPv6 literal is handled before the port split, because the
    // address is full of colons and rfind would land inside it.
    if (rest.front() == '[') {
        const auto close = rest.find(']');
        if (close == std::string_view::npos) reject(url, "the IPv6 address is missing its ']'");

        endpoint.host                = std::string(rest.substr(1, close - 1));
        const std::string_view after = rest.substr(close + 1);

        if (after.empty()) return endpoint;
        if (after.front() != ':') reject(url, "expected ':port' after the IPv6 address");

        unsigned long port = 0;
        if (!parseUnsigned(after.substr(1), port) || port == 0 || port > 65535) {
            reject(url, "the port must be a number between 1 and 65535");
        }
        endpoint.port = static_cast<std::uint16_t>(port);
        return endpoint;
    }

    if (const auto colon = rest.rfind(':'); colon != std::string_view::npos) {
        const std::string_view portText = rest.substr(colon + 1);
        unsigned long          port     = 0;
        if (!parseUnsigned(portText, port) || port == 0 || port > 65535) {
            reject(url, "the port must be a number between 1 and 65535");
        }
        endpoint.port = static_cast<std::uint16_t>(port);
        rest          = rest.substr(0, colon);
    }

    if (rest.empty()) reject(url, "no host was given");
    endpoint.host = std::string(rest);
    return endpoint;
}

std::string describe(const RedisEndpoint& endpoint) {
    std::string out = "redis://";
    if (!endpoint.username.empty()) out += endpoint.username + "@";
    else if (!endpoint.password.empty()) out += "<auth>@";
    out += endpoint.host + ":" + std::to_string(endpoint.port);
    out += "/" + std::to_string(endpoint.db);
    return out;
}

}  // namespace monobucket
