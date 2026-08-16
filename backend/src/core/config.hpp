#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "monobucket/constants.hpp"
#include "storage/durability.hpp"

namespace monobucket {

/// Credentials used when none are supplied. Startup warns when they are in
/// effect — they exist so `docker run` works out of the box, not for real use.
inline constexpr const char* kDefaultAccessKey = "monobucket";
inline constexpr const char* kDefaultSecretKey = "monobucket";


/// Raised when the resolved configuration is internally inconsistent.
class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& what) : std::runtime_error(what) {}
};

enum class CacheBackend { Memory, Redis };

std::string_view toString(CacheBackend backend);

/// Whole-process configuration. Populated once at startup from the
/// environment, then treated as immutable — anything an operator can change at
/// runtime belongs in the settings store (Phase 5), not here.
struct Config {
    // --- Network -----------------------------------------------------------
    std::string   host          = "0.0.0.0";
    std::uint16_t s3Port        = 9000;   ///< S3 REST API listener
    std::uint16_t consolePort   = 9001;   ///< Embedded dashboard listener
    bool          consoleEnabled = true;

    // --- Storage -----------------------------------------------------------
    std::string dataDir  = "/data";      ///< Object payloads + metadata store
    std::string region   = "us-east-1";  ///< Reported in S3 responses

    /// How much of a write must reach stable storage before it is acknowledged.
    /// `relaxed` survives a process crash; `strict` survives a power cut and
    /// costs an fsync per commit.
    Durability durability = Durability::Relaxed;

    /// Combined ceiling for the metadata store's block cache and its write
    /// buffers. RocksDB sizes those independently by default and the container
    /// only sees the sum, so this is charged against both.
    std::uint64_t metadataMemoryBytes = 32ull * 1024 * 1024;

    /// Bounds the table-reader memory that open SST files pin. Unbounded, this
    /// grows with the object count for the lifetime of the process.
    std::uint32_t metadataMaxOpenFiles = 256;

    /// How long an unreferenced payload is left alone before the sweeper may
    /// reclaim it. Must exceed the longest upload still in flight — see
    /// MetadataStore::listOrphans.
    std::uint32_t reclaimGraceSeconds = 3600;

    /// How often the background sweeper runs. Zero disables it, leaving
    /// reclamation to the deletion path and to startup recovery.
    std::uint32_t reclaimIntervalSeconds = 300;

    // --- I/O ---------------------------------------------------------------
    /// 0 means "match the worker thread count". Blocking filesystem work runs
    /// here so it never occupies an event loop thread.
    unsigned ioThreads = 0;

    /// Queue depth before storage work is rejected outright. Bounded on
    /// purpose: an unbounded queue turns a slow disk into unbounded memory.
    std::uint32_t ioQueueLimit = 1024;

    // --- Identity ----------------------------------------------------------
    /// Empty means "use kDefaultAccessKey/kDefaultSecretKey"; resolveDerived-
    /// Values() substitutes them and startup warns that they are in use.
    std::string rootAccessKey;
    std::string rootSecretKey;

    // --- Runtime -----------------------------------------------------------
    /// 0 means "derive from hardware_concurrency"; after
    /// resolveDerivedValues() this always holds a concrete count.
    unsigned workerThreads = 0;

    /// Ceiling for a single-part upload — 5 GiB, matching S3's own limit.
    std::uint64_t maxBodyBytes = 5ull * 1024 * 1024 * 1024;

    /// Above this, request bodies spill to disk instead of being buffered.
    std::uint64_t maxMemoryBodyBytes = 1ull * 1024 * 1024;

    /// Streaming read/write granularity.
    std::uint64_t streamChunkBytes = limits::kDefaultChunkSize;

    std::uint32_t idleTimeoutSeconds = 60;

    // --- Cache -------------------------------------------------------------
    CacheBackend cacheBackend = CacheBackend::Memory;

    /// Total budget for the cache, counted as stored bytes plus per-entry
    /// overhead. Zero disables caching entirely.
    std::uint64_t cacheMaxBytes = 128ull * 1024 * 1024;

    /// Default lifetime for cached entries. Zero means "until evicted", which
    /// is safe for a single instance and not for a shared Redis.
    std::uint32_t cacheTtlSeconds = 300;

    /// Ceiling on how long the local tier may hold a copy of a shared value.
    /// Only meaningful with the Redis backend, where another instance can
    /// change a value this process has already copied.
    std::uint32_t cacheLocalTtlSeconds = 5;

    std::string   redisUrl;
    std::uint32_t redisPoolSize = 4;

    // --- Observability -----------------------------------------------------
    std::string logLevel = "info";
    bool        metricsEnabled = true;

    /// Reads every MONOBUCKET_* variable and resolves derived values.
    /// Throws env::ParseError for malformed input and ConfigError for
    /// combinations that cannot work.
    static Config fromEnvironment();

    /// Cross-field checks. Called by fromEnvironment(); exposed for tests.
    /// A default-constructed Config, once resolved, always passes.
    void validate() const;

    /// True when the built-in demo credentials are in effect.
    bool usingDefaultCredentials() const noexcept;

    /// Fills in anything left at its "auto" sentinel (thread count, cache size).
    void resolveDerivedValues();

    /// Multi-line, human-readable dump written to the log at startup.
    /// Secrets are redacted.
    std::string summary() const;

    /// Machine-readable form for the dashboard's settings panel. Secrets are
    /// redacted here too — this crosses an HTTP boundary.
    nlohmann::json toJson() const;
};

}  // namespace monobucket
