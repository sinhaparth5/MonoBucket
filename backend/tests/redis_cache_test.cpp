// Integration tests for the Redis backend.
//
// They need a real server, so they are opt-in: set MONOBUCKET_TEST_REDIS_URL to
// run them and they are skipped otherwise. Mocking hiredis would test the mock;
// what is actually in question here is whether a real connection behaves the
// way FallbackCache assumes — in particular that a dead server produces a
// prompt CacheError rather than a hang.
//
//   docker run -d --rm -p 63790:6379 redis:7-alpine
//   MONOBUCKET_TEST_REDIS_URL=redis://127.0.0.1:63790/0 ctest --preset dev-redis

#include <catch2/catch_test_macros.hpp>

#if defined(MONOBUCKET_WITH_REDIS)

#include <unistd.h>

#include <cstdlib>
#include <string>

#include "cache/fallback_cache.hpp"
#include "cache/memory_cache.hpp"
#include "cache/redis_cache.hpp"
#include "cache/redis_url.hpp"

using monobucket::CacheError;
using monobucket::FallbackCache;
using monobucket::kNoExpiry;
using monobucket::MemoryCache;
using monobucket::RedisCache;

namespace {

/// A per-run prefix, so a shared Redis is not disturbed and two runs cannot
/// collide.
std::string uniquePrefix() {
    return "mbtest:" + std::to_string(::getpid()) + ":";
}

RedisCache::Options optionsFor(const char* url) {
    RedisCache::Options options;
    options.endpoint  = monobucket::parseRedisUrl(url);
    options.poolSize  = 2;
    options.keyPrefix = uniquePrefix();
    return options;
}

}  // namespace

TEST_CASE("the redis backend round-trips against a real server", "[cache][redis][integration]") {
    const char* url = std::getenv("MONOBUCKET_TEST_REDIS_URL");
    if (url == nullptr) {
        SKIP("MONOBUCKET_TEST_REDIS_URL is not set");
    }

    RedisCache cache(optionsFor(url));
    REQUIRE_FALSE(cache.probe().has_value());

    SECTION("a value survives the round trip") {
        cache.put("key", "value", kNoExpiry);
        const auto value = cache.get("key");
        REQUIRE(value != nullptr);
        CHECK(*value == "value");
    }

    SECTION("a binary value is not mangled") {
        // Commands go over redisCommandArgv precisely so that embedded NULs
        // and '%' are data rather than syntax.
        const std::string payload("a\0b%sc\r\n", 8);
        cache.put("binary", payload, kNoExpiry);
        REQUIRE(*cache.get("binary") == payload);
    }

    SECTION("a key containing a format specifier is a key, not a command") {
        cache.put("%s%d", "value", kNoExpiry);
        REQUIRE(*cache.get("%s%d") == "value");
    }

    SECTION("an absent key is a miss, not an error") {
        CHECK(cache.get("never-written") == nullptr);
    }

    SECTION("a lapsed ttl deletes rather than storing forever") {
        cache.put("brief", "value", kNoExpiry);
        cache.put("brief", "value", std::chrono::seconds(-1));
        CHECK(cache.get("brief") == nullptr);
    }

    SECTION("deleting reports whether the key was there") {
        cache.put("doomed", "value", kNoExpiry);
        CHECK(cache.del("doomed"));
        CHECK_FALSE(cache.del("doomed"));
    }

    SECTION("clear only removes our own keys") {
        cache.put("a", "1", kNoExpiry);
        cache.put("b", "2", kNoExpiry);

        // A second cache stands in for another tenant sharing the database.
        // Its prefix has to be disjoint from ours, not nested under it: SCAN
        // matches by prefix, so "ours" + "other:" would be inside our own
        // namespace and would be swept, correctly.
        auto foreign      = optionsFor(url);
        foreign.keyPrefix = "mbother:" + std::to_string(::getpid()) + ":";
        RedisCache neighbour(foreign);
        neighbour.put("keep", "mine", kNoExpiry);

        cache.clear();

        CHECK(cache.get("a") == nullptr);
        CHECK(cache.get("b") == nullptr);
        REQUIRE(neighbour.get("keep") != nullptr);
        neighbour.clear();
    }

    cache.clear();
}

TEST_CASE("an unreachable redis fails promptly rather than hanging",
          "[cache][redis][integration]") {
    RedisCache::Options options;
    // Port 1 is not listening, so the connection is refused rather than timing
    // out — this asserts the error path, not the timeout path.
    options.endpoint       = monobucket::parseRedisUrl("redis://127.0.0.1:1");
    options.connectTimeout = std::chrono::milliseconds(250);

    RedisCache cache(options);

    // The contract FallbackCache depends on: failures are thrown, not
    // swallowed, because the breaker cannot count what it is not told about.
    CHECK_THROWS_AS(cache.get("k"), CacheError);
    CHECK_THROWS_AS(cache.put("k", "v", kNoExpiry), CacheError);
    CHECK_THROWS_AS(cache.del("k"), CacheError);

    CHECK(cache.probe().has_value());
    CHECK_FALSE(cache.stats().healthy);
    CHECK(cache.stats().errors >= 3);
}

// The two halves are tested separately above and in fallback_cache_test.cpp:
// that a real hiredis connection throws on failure, and that the breaker turns
// a throw into a bypass. This is the join, with nothing mocked.
TEST_CASE("a dead redis behind the fallback is invisible to the caller",
          "[cache][redis][integration]") {
    RedisCache::Options redisOptions;
    redisOptions.endpoint       = monobucket::parseRedisUrl("redis://127.0.0.1:1");
    redisOptions.connectTimeout = std::chrono::milliseconds(250);

    MemoryCache::Options localOptions;
    localOptions.maxBytes = 4 * 1024 * 1024;

    FallbackCache::Options fallbackOptions;
    fallbackOptions.failureThreshold = 2;
    fallbackOptions.initialCooldown  = std::chrono::milliseconds(100);
    fallbackOptions.maxCooldown      = std::chrono::milliseconds(400);
    fallbackOptions.localTtlCap      = std::chrono::seconds(60);

    FallbackCache cache(std::make_unique<RedisCache>(redisOptions),
                        std::make_unique<MemoryCache>(localOptions), fallbackOptions);

    for (int i = 0; i < 20; ++i) {
        const std::string key = "key-" + std::to_string(i);
        CHECK_NOTHROW(cache.put(key, "value", kNoExpiry));
        const auto value = cache.get(key);
        REQUIRE(value != nullptr);
        CHECK(*value == "value");
    }

    // Degraded, and saying so — but every call succeeded.
    CHECK_FALSE(cache.sharedHealthy());
    CHECK(cache.bypassCount() > 0);
}

#endif  // MONOBUCKET_WITH_REDIS
