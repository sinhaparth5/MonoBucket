#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace monobucket {

/// Where the Redis backend connects, and as what.
struct RedisEndpoint {
    std::string   host = "127.0.0.1";
    std::uint16_t port = 6379;

    /// Redis 6 ACL user. Empty means the legacy single-password AUTH.
    std::string username;
    std::string password;

    int db = 0;
};

/// Parses `redis://[user[:password]@]host[:port][/db]`.
///
/// Percent escapes in the userinfo are decoded, because a generated password
/// containing `@` or `:` is otherwise unrepresentable and would fail in a way
/// that looks like a wrong password rather than a mangled URL.
///
/// Throws ConfigError with an actionable message. Startup treats that as fatal:
/// an unparseable URL means the operator asked for something specific and did
/// not get it, and silently falling back to localhost would hide that.
RedisEndpoint parseRedisUrl(std::string_view url);

/// Renders an endpoint without its password, for logs.
std::string describe(const RedisEndpoint& endpoint);

}  // namespace monobucket
