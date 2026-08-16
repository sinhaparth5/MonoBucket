#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#include "cache/cache_factory.hpp"
#include "core/config.hpp"

using monobucket::CacheBackend;
using monobucket::Config;
using monobucket::ConfigError;
using monobucket::kNoExpiry;
using monobucket::makeCache;

namespace {

Config baseConfig() {
    Config cfg;
    cfg.dataDir       = "/tmp/monobucket-cache-test";
    cfg.rootAccessKey = "monobucket";
    cfg.rootSecretKey = "monobucket-secret";
    cfg.workerThreads = 4;
    cfg.resolveDerivedValues();
    return cfg;
}

}  // namespace

TEST_CASE("the default configuration produces the in-memory backend", "[cache][factory]") {
    const Config cfg   = baseConfig();
    const auto   cache = makeCache(cfg);

    REQUIRE(cache != nullptr);
    CHECK(cache->name() == "memory");
    CHECK(cache->stats().limitBytes == cfg.cacheMaxBytes);
}

// The Phase 3 exit criterion: nothing but an environment variable changes.
TEST_CASE("the backend is selected by configuration alone", "[cache][factory]") {
    ::setenv("MONOBUCKET_DATA_DIR", "/tmp/monobucket-cache-test", 1);
    ::setenv("MONOBUCKET_ROOT_SECRET_KEY", "monobucket-secret", 1);
    ::setenv("MONOBUCKET_CACHE_BACKEND", "memory", 1);

    const auto memory = makeCache(Config::fromEnvironment());
    CHECK(memory->name() == "memory");

    ::setenv("MONOBUCKET_CACHE_BACKEND", "redis", 1);
    ::setenv("MONOBUCKET_REDIS_URL", "redis://127.0.0.1:6379", 1);

    // With the Redis backend compiled in this is the two-tier arrangement;
    // without it the factory degrades to the in-memory one and says so. Either
    // way it returns a usable cache rather than failing.
    const auto selected = makeCache(Config::fromEnvironment());
    REQUIRE(selected != nullptr);
#if defined(MONOBUCKET_WITH_REDIS)
    CHECK(selected->name() == "redis+memory");
#else
    CHECK(selected->name() == "memory");
#endif

    ::unsetenv("MONOBUCKET_CACHE_BACKEND");
    ::unsetenv("MONOBUCKET_REDIS_URL");
    ::unsetenv("MONOBUCKET_DATA_DIR");
    ::unsetenv("MONOBUCKET_ROOT_SECRET_KEY");
}

// A Redis that is down when the container starts must not stop it starting.
TEST_CASE("an unreachable redis still yields a working cache", "[cache][factory]") {
    Config cfg        = baseConfig();
    cfg.cacheBackend  = CacheBackend::Redis;
    // Port 1 refuses connections immediately, so this does not wait on a
    // timeout.
    cfg.redisUrl      = "redis://127.0.0.1:1";
    cfg.redisPoolSize = 2;
    cfg.validate();

    const auto cache = makeCache(cfg);
    REQUIRE(cache != nullptr);

    CHECK_NOTHROW(cache->put("k", "value", kNoExpiry));
    CHECK(cache->get("k") != nullptr);
    CHECK(*cache->get("k") == "value");
}

TEST_CASE("a zero budget disables the cache rather than shrinking it",
          "[cache][factory]") {
    Config cfg        = baseConfig();
    cfg.cacheMaxBytes = 0;
    cfg.validate();

    const auto cache = makeCache(cfg);
    CHECK(cache->name() == "disabled");

    // Every call still works; nothing is ever retained.
    cache->put("k", "value", kNoExpiry);
    CHECK(cache->get("k") == nullptr);
    CHECK_FALSE(cache->del("k"));
    CHECK(cache->evict(0) == 0);
    CHECK_NOTHROW(cache->clear());
    CHECK(cache->stats().entries == 0);
}

TEST_CASE("a budget too small to be a cache is rejected", "[cache][factory]") {
    // Someone writing 1024 meaning a kilobyte should be told, not handed a
    // cache whose per-entry overhead exceeds its entire budget.
    Config cfg        = baseConfig();
    cfg.cacheMaxBytes = 1024;
    CHECK_THROWS_AS(cfg.validate(), ConfigError);
}

TEST_CASE("a malformed redis url is caught before anything connects",
          "[cache][factory]") {
    Config cfg       = baseConfig();
    cfg.cacheBackend = CacheBackend::Redis;

    cfg.redisUrl = "localhost:6379";
    CHECK_THROWS_AS(cfg.validate(), ConfigError);

    cfg.redisUrl = "rediss://cache:6379";
    CHECK_THROWS_AS(cfg.validate(), ConfigError);

    cfg.redisUrl      = "redis://cache:6379";
    cfg.redisPoolSize = 0;
    CHECK_THROWS_AS(cfg.validate(), ConfigError);
}

TEST_CASE("the summary redacts the redis password", "[cache][factory]") {
    Config cfg       = baseConfig();
    cfg.cacheBackend = CacheBackend::Redis;
    cfg.redisUrl     = "redis://user:s3cr3t@cache:6379/1";
    cfg.validate();

    const auto summary = cfg.summary();
    CHECK(summary.find("s3cr3t") == std::string::npos);
    CHECK(summary.find("cache:6379") != std::string::npos);

    // The JSON view crosses an HTTP boundary, so it carries no URL at all.
    const auto json = cfg.toJson();
    CHECK(json.at("redisConfigured") == true);
    CHECK(json.dump().find("s3cr3t") == std::string::npos);
}

TEST_CASE("a summary survives a url it cannot parse", "[cache][factory]") {
    // summary() is what an operator reads when a setting is wrong, so it must
    // not be the thing that throws.
    Config cfg       = baseConfig();
    cfg.cacheBackend = CacheBackend::Redis;
    cfg.redisUrl     = "not-a-url";

    CHECK_NOTHROW(cfg.summary());
    CHECK(cfg.summary().find("unparseable") != std::string::npos);
}
