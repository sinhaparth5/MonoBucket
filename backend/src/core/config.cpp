#include "core/config.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <thread>

// A leaf parser with no dependency of its own beyond ConfigError. Validating
// the Redis URL here rather than where the connection is made is what lets
// `--print-config` reject a typo, instead of the container starting and then
// quietly running without the cache the operator asked for.
#include "cache/redis_url.hpp"
#include "core/env.hpp"
#include "monobucket/constants.hpp"

namespace monobucket {
namespace {

// Defaults live in the header as in-class initialisers so that a
// default-constructed Config is already valid; fromEnvironment() overrides
// them rather than re-declaring them.
constexpr unsigned kMaxWorkerThreads = 256;
constexpr unsigned kMaxIoThreads     = 256;

/// RocksDB needs room for a write buffer and a block cache; below this it
/// thrashes rather than saving memory.
constexpr std::uint64_t kMinMetadataMemoryBytes = 4ull * 1024 * 1024;

/// Below this the per-entry bookkeeping dominates the budget, so the cache
/// would hold almost nothing while still costing what a cache costs.
constexpr std::uint64_t kMinCacheBytes = 1ull * 1024 * 1024;

std::string redact(const std::string& secret) {
    if (secret.empty()) return "<unset>";
    if (secret.size() <= 4) return "****";
    return secret.substr(0, 2) + std::string(secret.size() - 4, '*') +
           secret.substr(secret.size() - 2);
}

std::uint16_t port(std::string_view name, std::uint16_t fallback) {
    const std::uint64_t value = env::number(name, fallback);
    if (value > 65535) {
        throw ConfigError(std::string(name) + " must be between 0 and 65535, got " +
                          std::to_string(value));
    }
    return static_cast<std::uint16_t>(value);
}

}  // namespace

std::string_view toString(CacheBackend backend) {
    switch (backend) {
        case CacheBackend::Memory: return "memory";
        case CacheBackend::Redis:  return "redis";
    }
    return "memory";
}

Config Config::fromEnvironment() {
    Config cfg;

    cfg.host           = env::string("MONOBUCKET_HOST", cfg.host);
    cfg.s3Port         = port("MONOBUCKET_PORT", cfg.s3Port);
    cfg.consolePort    = port("MONOBUCKET_CONSOLE_PORT", cfg.consolePort);
    cfg.consoleEnabled = env::boolean("MONOBUCKET_CONSOLE_ENABLED", cfg.consoleEnabled);

    cfg.dataDir = env::string("MONOBUCKET_DATA_DIR", cfg.dataDir);
    cfg.region  = env::string("MONOBUCKET_REGION", cfg.region);
    cfg.s3Domain = env::string("MONOBUCKET_S3_DOMAIN", cfg.s3Domain);

    const std::string durability =
        env::string("MONOBUCKET_DURABILITY", std::string(toString(cfg.durability)));
    if (const auto parsed = durabilityFromString(durability)) {
        cfg.durability = *parsed;
    } else {
        throw ConfigError("MONOBUCKET_DURABILITY must be one of none/relaxed/strict, got '" +
                          durability + "'");
    }

    cfg.metadataMemoryBytes =
        env::bytes("MONOBUCKET_METADATA_MEMORY_BYTES", cfg.metadataMemoryBytes);
    cfg.metadataMaxOpenFiles = static_cast<std::uint32_t>(
        env::number("MONOBUCKET_METADATA_MAX_OPEN_FILES", cfg.metadataMaxOpenFiles));
    cfg.reclaimGraceSeconds = static_cast<std::uint32_t>(
        env::number("MONOBUCKET_RECLAIM_GRACE_SECONDS", cfg.reclaimGraceSeconds));
    cfg.reclaimIntervalSeconds = static_cast<std::uint32_t>(
        env::number("MONOBUCKET_RECLAIM_INTERVAL_SECONDS", cfg.reclaimIntervalSeconds));

    cfg.ioThreads    = static_cast<unsigned>(env::number("MONOBUCKET_IO_THREADS", cfg.ioThreads));
    cfg.ioQueueLimit = static_cast<std::uint32_t>(
        env::number("MONOBUCKET_IO_QUEUE_LIMIT", cfg.ioQueueLimit));

    cfg.rootAccessKey = env::string("MONOBUCKET_ROOT_ACCESS_KEY", "");
    cfg.rootSecretKey = env::string("MONOBUCKET_ROOT_SECRET_KEY", "");

    cfg.workerThreads = static_cast<unsigned>(env::number("MONOBUCKET_WORKER_THREADS", 0));
    cfg.maxBodyBytes  = env::bytes("MONOBUCKET_MAX_BODY_BYTES", cfg.maxBodyBytes);
    cfg.maxMemoryBodyBytes =
        env::bytes("MONOBUCKET_MAX_MEMORY_BODY_BYTES", cfg.maxMemoryBodyBytes);
    cfg.streamChunkBytes = env::bytes("MONOBUCKET_STREAM_CHUNK_BYTES", cfg.streamChunkBytes);
    cfg.idleTimeoutSeconds = static_cast<std::uint32_t>(
        env::number("MONOBUCKET_IDLE_TIMEOUT_SECONDS", cfg.idleTimeoutSeconds));

    const std::string backend = env::string("MONOBUCKET_CACHE_BACKEND", "memory");
    if (backend == "memory") {
        cfg.cacheBackend = CacheBackend::Memory;
    } else if (backend == "redis") {
        cfg.cacheBackend = CacheBackend::Redis;
    } else {
        throw ConfigError("MONOBUCKET_CACHE_BACKEND must be 'memory' or 'redis', got '" + backend +
                          "'");
    }
    cfg.cacheMaxBytes = env::bytes("MONOBUCKET_CACHE_MAX_BYTES", cfg.cacheMaxBytes);
    cfg.cacheTtlSeconds = static_cast<std::uint32_t>(
        env::number("MONOBUCKET_CACHE_TTL_SECONDS", cfg.cacheTtlSeconds));
    cfg.cacheLocalTtlSeconds = static_cast<std::uint32_t>(
        env::number("MONOBUCKET_CACHE_LOCAL_TTL_SECONDS", cfg.cacheLocalTtlSeconds));
    cfg.redisUrl      = env::string("MONOBUCKET_REDIS_URL", "");
    cfg.redisPoolSize = static_cast<std::uint32_t>(
        env::number("MONOBUCKET_REDIS_POOL_SIZE", cfg.redisPoolSize));

    cfg.logLevel       = env::string("MONOBUCKET_LOG_LEVEL", "info");
    cfg.metricsEnabled = env::boolean("MONOBUCKET_METRICS_ENABLED", true);

    cfg.resolveDerivedValues();
    cfg.validate();
    return cfg;
}

void Config::resolveDerivedValues() {
    if (workerThreads == 0) {
        const unsigned detected = std::thread::hardware_concurrency();
        // hardware_concurrency() is allowed to return 0 when it cannot tell.
        workerThreads = detected == 0 ? 4u : detected;
    }
    workerThreads = std::min(workerThreads, kMaxWorkerThreads);

    // Storage work is blocking, so the pool is sized like the event loop rather
    // than from a fixed guess — a machine with more cores has more requests in
    // flight and therefore more concurrent reads to satisfy.
    if (ioThreads == 0) ioThreads = workerThreads;
    ioThreads = std::min(ioThreads, kMaxIoThreads);

    if (rootAccessKey.empty()) rootAccessKey = kDefaultAccessKey;
    if (rootSecretKey.empty()) rootSecretKey = kDefaultSecretKey;

    if (cacheBackend == CacheBackend::Redis && redisUrl.empty()) {
        redisUrl = "redis://127.0.0.1:6379";
    }
}

void Config::validate() const {
    if (s3Port == 0) throw ConfigError("MONOBUCKET_PORT must be a non-zero port");

    if (consoleEnabled && consolePort == s3Port) {
        throw ConfigError(
            "MONOBUCKET_CONSOLE_PORT must differ from MONOBUCKET_PORT; the dashboard cannot share "
            "a listener with the S3 API because bucket names would shadow console routes");
    }

    if (region.empty()) throw ConfigError("MONOBUCKET_REGION must not be empty");

    // A domain with a scheme or a path in it would never match a Host header,
    // and the failure would look like virtual-host addressing quietly not
    // working rather than like a typo.
    if (!s3Domain.empty() &&
        (s3Domain.find('/') != std::string::npos || s3Domain.find(':') != std::string::npos ||
         s3Domain.front() == '.')) {
        throw ConfigError(
            "MONOBUCKET_S3_DOMAIN must be a bare host name such as 's3.example.com', got '" +
            s3Domain + "'");
    }

    if (dataDir.empty()) throw ConfigError("MONOBUCKET_DATA_DIR must not be empty");
    if (!std::filesystem::path(dataDir).is_absolute()) {
        throw ConfigError("MONOBUCKET_DATA_DIR must be an absolute path, got '" + dataDir + "'");
    }

    if (rootSecretKey.size() < 8) {
        throw ConfigError("MONOBUCKET_ROOT_SECRET_KEY must be at least 8 characters");
    }
    if (rootAccessKey.size() < 3) {
        throw ConfigError("MONOBUCKET_ROOT_ACCESS_KEY must be at least 3 characters");
    }

    if (streamChunkBytes < 4096) {
        throw ConfigError("MONOBUCKET_STREAM_CHUNK_BYTES must be at least 4096");
    }
    if (maxMemoryBodyBytes > maxBodyBytes) {
        throw ConfigError(
            "MONOBUCKET_MAX_MEMORY_BODY_BYTES must not exceed MONOBUCKET_MAX_BODY_BYTES");
    }

    if (cacheBackend == CacheBackend::Redis) {
        if (redisUrl.empty()) {
            throw ConfigError("MONOBUCKET_REDIS_URL is required when the cache backend is 'redis'");
        }
        parseRedisUrl(redisUrl);  // throws ConfigError with the reason
        if (redisPoolSize == 0) {
            throw ConfigError("MONOBUCKET_REDIS_POOL_SIZE must be at least 1");
        }
        if (redisPoolSize > 256) {
            throw ConfigError("MONOBUCKET_REDIS_POOL_SIZE must not exceed 256");
        }
    }

    // Zero is a valid budget — it turns the cache off. Anything between zero
    // and a megabyte is not: per-entry overhead would dominate it, and an
    // operator who wrote `1024` meaning a kilobyte should be told so rather
    // than handed a cache that rejects every value.
    if (cacheMaxBytes != 0 && cacheMaxBytes < kMinCacheBytes) {
        throw ConfigError("MONOBUCKET_CACHE_MAX_BYTES must be 0 (disabled) or at least " +
                          env::formatBytes(kMinCacheBytes));
    }

    if (cacheLocalTtlSeconds == 0 && cacheBackend == CacheBackend::Redis) {
        throw ConfigError(
            "MONOBUCKET_CACHE_LOCAL_TTL_SECONDS must be at least 1; an unbounded local copy of a "
            "value held in a shared Redis can never be found to be stale");
    }

    if (metadataMemoryBytes < kMinMetadataMemoryBytes) {
        throw ConfigError("MONOBUCKET_METADATA_MEMORY_BYTES must be at least " +
                          env::formatBytes(kMinMetadataMemoryBytes));
    }
    if (metadataMaxOpenFiles < 16) {
        throw ConfigError("MONOBUCKET_METADATA_MAX_OPEN_FILES must be at least 16");
    }
    if (ioQueueLimit == 0) {
        throw ConfigError("MONOBUCKET_IO_QUEUE_LIMIT must be at least 1");
    }

    // A grace shorter than the time a large upload takes would let the sweeper
    // reclaim a payload that is still being written to. See
    // MetadataStore::listOrphans for why that is unrecoverable rather than
    // merely wasteful.
    if (reclaimGraceSeconds < 60) {
        throw ConfigError(
            "MONOBUCKET_RECLAIM_GRACE_SECONDS must be at least 60; a shorter grace can reclaim a "
            "payload that an upload is still writing to");
    }

    static constexpr std::string_view kLevels[] = {"trace", "debug", "info", "warn", "error"};
    if (std::find(std::begin(kLevels), std::end(kLevels), logLevel) == std::end(kLevels)) {
        throw ConfigError("MONOBUCKET_LOG_LEVEL must be one of trace/debug/info/warn/error, got '" +
                          logLevel + "'");
    }
}

bool Config::usingDefaultCredentials() const noexcept {
    return rootAccessKey == kDefaultAccessKey && rootSecretKey == kDefaultSecretKey;
}

std::string Config::summary() const {
    // The URL can carry a password, so only its shape is logged. Parsed
    // defensively: a summary that can throw would turn a bad setting into a
    // crash at exactly the moment the operator needs to read the setting.
    std::string redisSummary = "<unused>";
    if (cacheBackend == CacheBackend::Redis) {
        try {
            redisSummary = describe(parseRedisUrl(redisUrl)) + " pool " +
                           std::to_string(redisPoolSize);
        } catch (const ConfigError&) {
            redisSummary = "<unparseable>";
        }
    }

    std::ostringstream os;
    os << "configuration:\n"
       << "  s3 listener      : " << host << ':' << s3Port << '\n'
       << "  console listener : "
       << (consoleEnabled ? host + ':' + std::to_string(consolePort) : std::string("disabled"))
       << '\n'
       << "  data dir         : " << dataDir << '\n'
       << "  region           : " << region << '\n'
       << "  s3 domain        : "
       << (s3Domain.empty() ? std::string("path style only") : s3Domain) << '\n'
       << "  durability       : " << toString(durability) << '\n'
       << "  metadata memory  : " << env::formatBytes(metadataMemoryBytes) << '\n'
       << "  worker threads   : " << workerThreads << '\n'
       << "  io threads       : " << ioThreads << " (queue " << ioQueueLimit << ")\n"
       << "  max body         : " << env::formatBytes(maxBodyBytes) << '\n'
       << "  in-memory body   : " << env::formatBytes(maxMemoryBodyBytes) << '\n'
       << "  stream chunk     : " << env::formatBytes(streamChunkBytes) << '\n'
       << "  cache backend    : " << toString(cacheBackend) << '\n'
       << "  cache budget     : "
       << (cacheMaxBytes == 0 ? std::string("disabled") : env::formatBytes(cacheMaxBytes)) << '\n'
       << "  cache ttl        : " << cacheTtlSeconds << "s (local " << cacheLocalTtlSeconds
       << "s)\n"
       << "  redis            : " << redisSummary << '\n'
       << "  root access key  : " << rootAccessKey << '\n'
       << "  root secret key  : " << redact(rootSecretKey) << '\n'
       << "  log level        : " << logLevel;
    return os.str();
}

nlohmann::json Config::toJson() const {
    return nlohmann::json{
        {"host", host},
        {"s3Port", s3Port},
        {"consolePort", consolePort},
        {"consoleEnabled", consoleEnabled},
        {"dataDir", dataDir},
        {"region", region},
        {"s3Domain", s3Domain},
        {"durability", std::string(toString(durability))},
        {"metadataMemoryBytes", metadataMemoryBytes},
        {"metadataMaxOpenFiles", metadataMaxOpenFiles},
        {"reclaimGraceSeconds", reclaimGraceSeconds},
        {"reclaimIntervalSeconds", reclaimIntervalSeconds},
        {"ioThreads", ioThreads},
        {"ioQueueLimit", ioQueueLimit},
        {"rootAccessKey", rootAccessKey},
        {"rootSecretKey", redact(rootSecretKey)},
        {"workerThreads", workerThreads},
        {"maxBodyBytes", maxBodyBytes},
        {"maxMemoryBodyBytes", maxMemoryBodyBytes},
        {"streamChunkBytes", streamChunkBytes},
        {"idleTimeoutSeconds", idleTimeoutSeconds},
        {"cacheBackend", std::string(toString(cacheBackend))},
        {"cacheMaxBytes", cacheMaxBytes},
        {"cacheTtlSeconds", cacheTtlSeconds},
        {"cacheLocalTtlSeconds", cacheLocalTtlSeconds},
        {"redisConfigured", !redisUrl.empty()},
        {"redisPoolSize", redisPoolSize},
        {"logLevel", logLevel},
        {"metricsEnabled", metricsEnabled},
    };
}

}  // namespace monobucket
