#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <thread>

#include "core/config.hpp"

using monobucket::CacheBackend;
using monobucket::Config;
using monobucket::ConfigError;

namespace {

/// A config that passes validate(), so each test can perturb exactly one field.
Config validConfig() {
    Config cfg;
    cfg.dataDir       = "/tmp/monobucket-test";
    cfg.rootAccessKey = "monobucket";
    cfg.rootSecretKey = "monobucket-secret";
    cfg.resolveDerivedValues();
    return cfg;
}

}  // namespace

TEST_CASE("the baseline configuration validates", "[config]") {
    CHECK_NOTHROW(validConfig().validate());
}

// Regression: the byte limits once defaulted to 0 while their real defaults
// lived only inside fromEnvironment(), so a default-constructed Config could
// never pass validate(). Defaults belong in exactly one place.
TEST_CASE("a default-constructed configuration is already valid", "[config]") {
    Config cfg;
    cfg.resolveDerivedValues();

    CHECK_NOTHROW(cfg.validate());
    CHECK(cfg.streamChunkBytes >= 4096);
    CHECK(cfg.maxBodyBytes > 0);
    CHECK(cfg.maxMemoryBodyBytes > 0);
    CHECK(cfg.maxMemoryBodyBytes <= cfg.maxBodyBytes);
    CHECK(cfg.cacheMaxBytes > 0);
    CHECK_FALSE(cfg.rootSecretKey.empty());
}

TEST_CASE("the built-in demo credentials are detectable", "[config]") {
    Config cfg;
    cfg.resolveDerivedValues();
    CHECK(cfg.usingDefaultCredentials());

    cfg.rootSecretKey = "something-else-entirely";
    CHECK_FALSE(cfg.usingDefaultCredentials());
}

TEST_CASE("worker threads default to hardware concurrency", "[config]") {
    Config cfg;
    cfg.workerThreads = 0;
    cfg.resolveDerivedValues();

    const unsigned detected = std::thread::hardware_concurrency();
    CHECK(cfg.workerThreads == (detected == 0 ? 4u : detected));
    CHECK(cfg.workerThreads > 0);
}

TEST_CASE("an explicit worker thread count is respected but capped", "[config]") {
    Config cfg;
    cfg.workerThreads = 3;
    cfg.resolveDerivedValues();
    CHECK(cfg.workerThreads == 3);

    Config huge;
    huge.workerThreads = 100000;
    huge.resolveDerivedValues();
    CHECK(huge.workerThreads == 256);
}

TEST_CASE("the console cannot share a port with the S3 API", "[config]") {
    Config cfg = validConfig();
    cfg.consoleEnabled = true;
    cfg.consolePort = cfg.s3Port;
    CHECK_THROWS_AS(cfg.validate(), ConfigError);

    // ...unless the console is switched off entirely.
    cfg.consoleEnabled = false;
    CHECK_NOTHROW(cfg.validate());
}

TEST_CASE("the data directory must be an absolute path", "[config]") {
    Config cfg = validConfig();
    cfg.dataDir = "relative/path";
    CHECK_THROWS_AS(cfg.validate(), ConfigError);

    cfg.dataDir = "";
    CHECK_THROWS_AS(cfg.validate(), ConfigError);
}

TEST_CASE("weak root credentials are rejected", "[config]") {
    Config cfg = validConfig();
    cfg.rootSecretKey = "short";
    CHECK_THROWS_AS(cfg.validate(), ConfigError);

    Config other = validConfig();
    other.rootAccessKey = "ab";
    CHECK_THROWS_AS(other.validate(), ConfigError);
}

TEST_CASE("streaming limits must be internally consistent", "[config]") {
    Config cfg = validConfig();
    cfg.streamChunkBytes = 128;
    CHECK_THROWS_AS(cfg.validate(), ConfigError);

    Config other = validConfig();
    other.maxBodyBytes = 1024;
    other.maxMemoryBodyBytes = 4096;
    CHECK_THROWS_AS(other.validate(), ConfigError);
}

TEST_CASE("the upload limit must sit inside its ceiling", "[config]") {
    Config cfg = validConfig();
    cfg.maxUploadBytes        = 2048;
    cfg.maxUploadCeilingBytes = 1024;
    CHECK_THROWS_AS(cfg.validate(), ConfigError);

    // Exactly at the ceiling is fine; a ceiling that cannot be reached is a
    // ceiling one byte lower than it says.
    Config equal = validConfig();
    equal.maxUploadBytes        = 1024;
    equal.maxUploadCeilingBytes = 1024;
    CHECK_NOTHROW(equal.validate());
}

TEST_CASE("a zero upload limit is refused rather than read as unlimited", "[config]") {
    // Every other byte figure here treats zero as "no limit". A *maximum*
    // upload size that silently meant the opposite is the setting somebody
    // reaches for in a hurry and gets backwards.
    Config cfg = validConfig();
    cfg.maxUploadBytes = 0;
    CHECK_THROWS_AS(cfg.validate(), ConfigError);

    Config ceiling = validConfig();
    ceiling.maxUploadCeilingBytes = 0;
    CHECK_THROWS_AS(ceiling.validate(), ConfigError);
}

TEST_CASE("selecting redis implies a connection URL", "[config]") {
    Config cfg = validConfig();
    cfg.cacheBackend = CacheBackend::Redis;
    cfg.redisUrl.clear();
    CHECK_THROWS_AS(cfg.validate(), ConfigError);

    // resolveDerivedValues() supplies the conventional local default.
    cfg.resolveDerivedValues();
    CHECK(cfg.redisUrl == "redis://127.0.0.1:6379");
    CHECK_NOTHROW(cfg.validate());
}

TEST_CASE("io threads default to the worker count", "[config]") {
    Config cfg;
    cfg.workerThreads = 6;
    cfg.ioThreads     = 0;
    cfg.resolveDerivedValues();
    CHECK(cfg.ioThreads == 6);

    Config explicitCount;
    explicitCount.ioThreads = 2;
    explicitCount.resolveDerivedValues();
    CHECK(explicitCount.ioThreads == 2);
}

TEST_CASE("the durability setting round-trips through its name", "[config]") {
    using monobucket::Durability;
    using monobucket::durabilityFromString;

    CHECK(durabilityFromString("none") == Durability::None);
    CHECK(durabilityFromString("relaxed") == Durability::Relaxed);
    CHECK(durabilityFromString("strict") == Durability::Strict);

    // A misspelling must not quietly weaken durability.
    CHECK_FALSE(durabilityFromString("Strict").has_value());
    CHECK_FALSE(durabilityFromString("fsync").has_value());

    for (auto level : {Durability::None, Durability::Relaxed, Durability::Strict}) {
        CHECK(durabilityFromString(monobucket::toString(level)) == level);
    }
}

TEST_CASE("storage limits are checked for sanity", "[config]") {
    Config cfg = validConfig();
    cfg.metadataMemoryBytes = 1024;  // below RocksDB's usable floor
    CHECK_THROWS_AS(cfg.validate(), ConfigError);

    Config files = validConfig();
    files.metadataMaxOpenFiles = 4;
    CHECK_THROWS_AS(files.validate(), ConfigError);

    Config queue = validConfig();
    queue.ioQueueLimit = 0;
    CHECK_THROWS_AS(queue.validate(), ConfigError);
}

TEST_CASE("a too-short reclaim grace is rejected", "[config]") {
    // A grace shorter than an upload can reclaim the payload that upload is
    // still writing to, which loses data rather than merely wasting space.
    Config cfg = validConfig();
    cfg.reclaimGraceSeconds = 5;
    CHECK_THROWS_AS(cfg.validate(), ConfigError);

    cfg.reclaimGraceSeconds = 3600;
    CHECK_NOTHROW(cfg.validate());
}

TEST_CASE("an unknown durability setting aborts startup", "[config]") {
    ::setenv("MONOBUCKET_DURABILITY", "fsync-please", 1);
    CHECK_THROWS_AS(Config::fromEnvironment(), ConfigError);
    ::unsetenv("MONOBUCKET_DURABILITY");
}

TEST_CASE("an unknown log level is rejected", "[config]") {
    Config cfg = validConfig();
    cfg.logLevel = "verbose";
    CHECK_THROWS_AS(cfg.validate(), ConfigError);
}

TEST_CASE("the JSON view redacts the root secret", "[config]") {
    const Config cfg = validConfig();
    const auto json = cfg.toJson();

    CHECK(json.at("rootAccessKey") == "monobucket");
    CHECK(json.at("rootSecretKey") != cfg.rootSecretKey);
    CHECK(json.at("rootSecretKey").get<std::string>().find('*') != std::string::npos);
    CHECK(json.at("workerThreads").get<unsigned>() > 0);

    // The summary is what lands in the startup log — it must not leak either.
    CHECK(cfg.summary().find(cfg.rootSecretKey) == std::string::npos);
}

TEST_CASE("fromEnvironment reads MONOBUCKET_* variables", "[config]") {
    ::setenv("MONOBUCKET_DATA_DIR", "/tmp/monobucket-env-test", 1);
    ::setenv("MONOBUCKET_PORT", "19000", 1);
    ::setenv("MONOBUCKET_CONSOLE_PORT", "19001", 1);
    ::setenv("MONOBUCKET_CACHE_MAX_BYTES", "64MiB", 1);
    ::setenv("MONOBUCKET_ROOT_SECRET_KEY", "supersecret", 1);

    const Config cfg = Config::fromEnvironment();
    CHECK(cfg.dataDir == "/tmp/monobucket-env-test");
    CHECK(cfg.s3Port == 19000);
    CHECK(cfg.consolePort == 19001);
    CHECK(cfg.cacheMaxBytes == 64ull * 1024 * 1024);
    CHECK(cfg.cacheBackend == CacheBackend::Memory);

    ::setenv("MONOBUCKET_CACHE_BACKEND", "memcached", 1);
    CHECK_THROWS_AS(Config::fromEnvironment(), ConfigError);

    ::unsetenv("MONOBUCKET_CACHE_BACKEND");
    ::unsetenv("MONOBUCKET_DATA_DIR");
    ::unsetenv("MONOBUCKET_PORT");
    ::unsetenv("MONOBUCKET_CONSOLE_PORT");
    ::unsetenv("MONOBUCKET_CACHE_MAX_BYTES");
    ::unsetenv("MONOBUCKET_ROOT_SECRET_KEY");
}
