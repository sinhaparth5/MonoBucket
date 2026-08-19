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

    /// The endpoint domain, when there is one — `s3.example.com`. Setting it
    /// enables virtual-host style addressing, where `bucket.s3.example.com`
    /// names the bucket. Empty means path style only: without a configured
    /// domain there is no way to tell `bucket.example.com` from a host that
    /// simply is not us, and guessing would make `Host: localhost` address a
    /// bucket called "localhost".
    std::string s3Domain;

    /// The origin S3 clients reach this deployment at — `https://s3.example.com`.
    ///
    /// Only the console reads it, and only to build a URL: it is what the object
    /// links are written against and what gets *signed* as the host of a
    /// presigned URL. Behind a reverse proxy that is not something the browser
    /// can work out — the console is loaded from a different hostname, on a
    /// different port, and `MONOBUCKET_HOST` is 0.0.0.0 — so it has to be
    /// stated. Empty means "the console's own hostname and MONOBUCKET_PORT",
    /// which is right for a direct deployment and wrong the moment there is a
    /// proxy in front.
    ///
    /// Scheme included, because a presigned URL's signature covers the host and
    /// the link has to name the scheme the browser will actually use.
    std::string s3PublicUrl;

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

    // --- Storage allocations -----------------------------------------------

    /// What may be allocated across every bucket. Zero derives it from the
    /// filesystem holding the data directory, less `capacityReservePercent`.
    ///
    /// Set it when the data directory shares a filesystem with anything else,
    /// or when the volume is larger than this instance is meant to fill: the
    /// derived figure assumes the disk is MonoBucket's.
    std::uint64_t allocatableBytes = 0;

    /// The share of a derived capacity held back for the storage engine —
    /// RocksDB's files, streaming temporaries, and the second copy a multipart
    /// completion makes. Ignored when `allocatableBytes` is set explicitly.
    std::uint32_t capacityReservePercent = 10;

    /// The allocation a bucket receives when it is created by something that
    /// cannot name one: plain S3 CreateBucket, which has no field for it.
    ///
    /// Zero leaves such buckets unlimited. That is the default because it is
    /// what every bucket was before allocations existed, and an upgrade that
    /// silently started refusing writes to buckets an operator never sized
    /// would be indistinguishable from a bug.
    std::uint64_t defaultBucketQuotaBytes = 0;

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
    ///
    /// This pair signs S3 requests and nothing else. It stopped being a console
    /// login when the administrator account arrived, and it is kept because
    /// every deployment that predates that account is configured with it — see
    /// the migration note in README.md.
    std::string rootAccessKey;
    std::string rootSecretKey;

    /// The console administrator's name. Only ever compared, never used as a
    /// key: there is one account, stored at a fixed place.
    std::string adminUsername = "admin";

    /// The password to provision the administrator with, in plaintext, held
    /// only until startup has hashed it into the store.
    ///
    /// Empty means "leave whatever is already provisioned alone", which is the
    /// normal state of a running deployment — the variable exists to establish
    /// or reset the account, not to be present forever. Startup refuses to open
    /// a listener when this is empty *and* no administrator has ever been
    /// provisioned, because a console nobody can sign in to is not a safe
    /// default, and a shipped default password is worse.
    ///
    /// Never logged, never in summary(), never in toJson().
    std::string adminPassword;

    /// Whether the session cookie carries `Secure`.
    ///
    /// Auto is the default and the only setting most deployments should use: it
    /// marks the cookie Secure when the request that produced it arrived over
    /// TLS, directly or through a proxy that said so. Forcing it on behind a
    /// plain-HTTP hop makes the browser drop a cookie it was just given, which
    /// presents as a login that silently does nothing.
    enum class CookieSecurity { Auto, Always, Never };
    CookieSecurity consoleCookieSecure = CookieSecurity::Auto;

    // --- Runtime -----------------------------------------------------------
    /// 0 means "derive from hardware_concurrency"; after
    /// resolveDerivedValues() this always holds a concrete count.
    unsigned workerThreads = 0;

    /// Ceiling for a single-part upload — 5 GiB, matching S3's own limit.
    std::uint64_t maxBodyBytes = 5ull * 1024 * 1024 * 1024;

    /// The largest object this instance accepts, until an administrator sets a
    /// different one from the console.
    ///
    /// The one setting here that is a starting value rather than the value:
    /// the effective limit is persisted in the metadata store, seeded from
    /// this on a store that has never carried one, and thereafter owned by the
    /// console. That is the deliberate exception to "configuration is
    /// environment only", for the same reason a bucket's allocation is: it is
    /// a policy figure an operator changes while the server is running, not a
    /// wiring parameter that decides how the process is built.
    ///
    /// 5 GiB, matching what S3 accepts in a single PUT — which is already the
    /// most a body can be here. It therefore constrains only multipart, and
    /// deliberately: an object assembled out of parts is still an object
    /// somebody has to store, read back and pay for.
    std::uint64_t maxUploadBytes = 5ull * 1024 * 1024 * 1024;

    /// The most `maxUploadBytes` may ever be raised to from the console.
    ///
    /// Environment-only and never writable, which is what makes it a ceiling
    /// rather than a second copy of the limit: an administrator who can raise
    /// the limit without bound is not being held to anything. 5 TiB is S3's
    /// own maximum object size, so the default constrains nothing S3 would
    /// have accepted.
    std::uint64_t maxUploadCeilingBytes = 5ull * 1024 * 1024 * 1024 * 1024;

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
