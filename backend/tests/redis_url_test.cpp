#include <catch2/catch_test_macros.hpp>

#include "cache/redis_url.hpp"
#include "core/config.hpp"

using monobucket::ConfigError;
using monobucket::parseRedisUrl;

TEST_CASE("a bare redis url yields the conventional defaults", "[cache][redis]") {
    const auto endpoint = parseRedisUrl("redis://127.0.0.1:6379");

    CHECK(endpoint.host == "127.0.0.1");
    CHECK(endpoint.port == 6379);
    CHECK(endpoint.db == 0);
    CHECK(endpoint.username.empty());
    CHECK(endpoint.password.empty());
}

TEST_CASE("the port and database are optional", "[cache][redis]") {
    const auto bare = parseRedisUrl("redis://cache.internal");
    CHECK(bare.host == "cache.internal");
    CHECK(bare.port == 6379);
    CHECK(bare.db == 0);

    const auto withDb = parseRedisUrl("redis://cache.internal/3");
    CHECK(withDb.host == "cache.internal");
    CHECK(withDb.port == 6379);
    CHECK(withDb.db == 3);

    // A trailing slash is the same as no database at all.
    CHECK(parseRedisUrl("redis://cache.internal/").db == 0);
}

TEST_CASE("a password with no user is the documented shorthand", "[cache][redis]") {
    const auto endpoint = parseRedisUrl("redis://s3cr3t@cache:6380/1");

    CHECK(endpoint.username.empty());
    CHECK(endpoint.password == "s3cr3t");
    CHECK(endpoint.host == "cache");
    CHECK(endpoint.port == 6380);
    CHECK(endpoint.db == 1);
}

TEST_CASE("an ACL user and password are both read", "[cache][redis]") {
    const auto endpoint = parseRedisUrl("redis://monobucket:s3cr3t@cache:6379");

    CHECK(endpoint.username == "monobucket");
    CHECK(endpoint.password == "s3cr3t");
    CHECK(endpoint.host == "cache");
}

// Generated passwords contain these characters routinely, and getting it wrong
// surfaces as an authentication failure that looks like the wrong password
// rather than like a mangled URL.
TEST_CASE("percent escapes in the credentials are decoded", "[cache][redis]") {
    const auto endpoint = parseRedisUrl("redis://user:p%40ss%3Aword@cache:6379");

    CHECK(endpoint.username == "user");
    CHECK(endpoint.password == "p@ss:word");
    CHECK(endpoint.host == "cache");
    CHECK(endpoint.port == 6379);
}

TEST_CASE("a literal @ in the password does not truncate the host", "[cache][redis]") {
    // The userinfo separator is found from the right for exactly this reason.
    const auto endpoint = parseRedisUrl("redis://user:p@ss@cache:6379");

    CHECK(endpoint.username == "user");
    CHECK(endpoint.password == "p@ss");
    CHECK(endpoint.host == "cache");
}

TEST_CASE("a lone percent sign is kept rather than rejected", "[cache][redis]") {
    const auto endpoint = parseRedisUrl("redis://user:100%pure@cache");
    CHECK(endpoint.password == "100%pure");
}

TEST_CASE("a bracketed IPv6 literal keeps its colons", "[cache][redis]") {
    const auto withPort = parseRedisUrl("redis://[fd00::1]:6380/2");
    CHECK(withPort.host == "fd00::1");
    CHECK(withPort.port == 6380);
    CHECK(withPort.db == 2);

    const auto bare = parseRedisUrl("redis://[::1]");
    CHECK(bare.host == "::1");
    CHECK(bare.port == 6379);

    CHECK_THROWS_AS(parseRedisUrl("redis://[::1"), ConfigError);
    CHECK_THROWS_AS(parseRedisUrl("redis://[::1]6379"), ConfigError);
}

TEST_CASE("tls is refused with the reason, not a connection error", "[cache][redis]") {
    // Failing here beats failing at connect time: hiredis is built without SSL,
    // so the alternative is a timeout that looks like a network problem.
    CHECK_THROWS_AS(parseRedisUrl("rediss://cache:6379"), ConfigError);
}

TEST_CASE("a malformed url aborts startup", "[cache][redis]") {
    CHECK_THROWS_AS(parseRedisUrl(""), ConfigError);
    CHECK_THROWS_AS(parseRedisUrl("cache:6379"), ConfigError);
    CHECK_THROWS_AS(parseRedisUrl("http://cache:6379"), ConfigError);
    CHECK_THROWS_AS(parseRedisUrl("redis://"), ConfigError);
    CHECK_THROWS_AS(parseRedisUrl("redis://cache:0"), ConfigError);
    CHECK_THROWS_AS(parseRedisUrl("redis://cache:99999"), ConfigError);
    CHECK_THROWS_AS(parseRedisUrl("redis://cache:http"), ConfigError);
    CHECK_THROWS_AS(parseRedisUrl("redis://cache/99"), ConfigError);
    CHECK_THROWS_AS(parseRedisUrl("redis://cache/two"), ConfigError);
}

TEST_CASE("the description never contains the password", "[cache][redis]") {
    const auto endpoint = parseRedisUrl("redis://user:s3cr3t@cache:6379/2");
    const auto rendered = monobucket::describe(endpoint);

    CHECK(rendered.find("s3cr3t") == std::string::npos);
    CHECK(rendered.find("cache:6379") != std::string::npos);
    CHECK(rendered.find("/2") != std::string::npos);
}
