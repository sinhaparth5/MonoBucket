#include "server/console_api.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include "cache/cache_provider.hpp"
#include "core/config.hpp"
#include "core/io_executor.hpp"
#include "core/logging.hpp"
#include "monobucket/version.hpp"
#include "s3/cors.hpp"
#include "s3/handlers.hpp"
#include "s3/metrics.hpp"
#include "s3/s3_error.hpp"
#include "s3/sigv4.hpp"
#include "s3/uri.hpp"
#include "server/metrics_history.hpp"
#include "server/server.hpp"
#include "server/system_routes.hpp"
#include "storage/records.hpp"
#include "storage/storage_engine.hpp"

namespace monobucket {
namespace {

using drogon::HttpRequestPtr;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;
using ResponseCallback = std::function<void(const HttpResponsePtr&)>;

constexpr const char* kSessionCookie = "mb_session";

/// Twelve hours: long enough to survive a working day with the tab open, short
/// enough that a forgotten session on a shared machine expires the same day.
/// Not an environment knob — a console session is not part of the deployment
/// shape, and every extra variable is one more thing to get wrong.
constexpr std::int64_t kSessionTtlSeconds = 12 * 60 * 60;

/// Sampling cadence and retention for the console graphs: 5s × 240 = 20 minutes
/// of history for roughly 30 KB of ring. Longer windows are Prometheus's job.
constexpr double      kSampleIntervalSeconds = 5.0;
constexpr std::size_t kSampleCapacity        = 240;

/// A wrong password is cheap to try, so cap how often it can be tried. The
/// window is global rather than per-IP on purpose: there is exactly one account,
/// so a per-IP bucket would only tell an attacker to change source addresses.
constexpr int          kMaxFailedLogins    = 10;
constexpr std::int64_t kLoginWindowSeconds = 60;

/// S3's own ceiling on a presigned URL's lifetime. Repeated here rather than
/// reached for through sigv4 because the console rejects an over-long request
/// before it reaches an I/O thread.
constexpr std::int64_t kMaxPresignSeconds = 604800;

/// The console tells us which name the browser reached the deployment by,
/// because `config.host` is normally 0.0.0.0 and knows no better. That string is
/// signed and handed back inside a URL, so it is held to what a host and port
/// can actually contain — not to stop an attack (the caller already holds the
/// root credentials) but so a mistake surfaces as a 400 here instead of as a
/// link that fails somewhere else.
bool plausibleHost(const std::string& host) {
    if (host.empty() || host.size() > 255) return false;
    return std::all_of(host.begin(), host.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '.' || ch == '-' || ch == ':' || ch == '[' ||
               ch == ']';
    });
}

std::int64_t nowSeconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

HttpResponsePtr jsonResponse(const nlohmann::json& body,
                             drogon::HttpStatusCode status = drogon::k200OK) {
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(status);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(body.dump());
    // The console is an application, not a document. A cached 200 here is a
    // stale bucket list after a delete.
    resp->addHeader("Cache-Control", "no-store");
    return resp;
}

HttpResponsePtr errorJson(const std::string& message, drogon::HttpStatusCode status) {
    return jsonResponse({{"error", message}}, status);
}

/// Equal-length, timing-independent comparison. A byte-by-byte `==` on a secret
/// leaks its prefix to anyone willing to measure.
bool secretsMatch(const std::string& a, const std::string& b) noexcept {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string randomToken() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        throw std::runtime_error("the system random source is unavailable");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string           out;
    out.reserve(sizeof(bytes) * 2);
    for (unsigned char byte : bytes) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

/// Session tokens held in memory only. A restart logs everyone out, which is
/// the correct trade for a single-binary server: persisting them would mean a
/// stolen data directory is also a stolen login, and re-authenticating against
/// the root credentials costs one round trip.
class SessionStore {
public:
    std::string open(std::string accessKey) {
        std::string                       token = randomToken();
        const std::lock_guard<std::mutex> guard(mutex_);
        sweep();
        sessions_.emplace(token, Session{std::move(accessKey), nowSeconds() + kSessionTtlSeconds});
        return token;
    }

    /// Returns the access key the session was opened with, or empty.
    std::string resolve(const std::string& token) const {
        if (token.empty()) return {};
        const std::lock_guard<std::mutex> guard(mutex_);
        const auto                        it = sessions_.find(token);
        if (it == sessions_.end() || it->second.expiresAt <= nowSeconds()) return {};
        return it->second.accessKey;
    }

    void close(const std::string& token) {
        const std::lock_guard<std::mutex> guard(mutex_);
        sessions_.erase(token);
    }

private:
    struct Session {
        std::string  accessKey;
        std::int64_t expiresAt = 0;
    };

    /// Called on every login rather than on a timer: expired entries are only
    /// a leak if logins keep happening, and if they do this collects them.
    void sweep() {
        const std::int64_t now = nowSeconds();
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            it = it->second.expiresAt <= now ? sessions_.erase(it) : std::next(it);
        }
    }

    mutable std::mutex                           mutex_;
    std::unordered_map<std::string, Session>     sessions_;
};

class LoginThrottle {
public:
    bool blocked() {
        const std::lock_guard<std::mutex> guard(mutex_);
        roll();
        return failures_ >= kMaxFailedLogins;
    }

    void recordFailure() {
        const std::lock_guard<std::mutex> guard(mutex_);
        roll();
        ++failures_;
    }

    void recordSuccess() {
        const std::lock_guard<std::mutex> guard(mutex_);
        failures_ = 0;
    }

private:
    void roll() {
        const std::int64_t now = nowSeconds();
        if (now - windowStart_ >= kLoginWindowSeconds) {
            windowStart_ = now;
            failures_    = 0;
        }
    }

    std::mutex   mutex_;
    std::int64_t windowStart_ = 0;
    int          failures_    = 0;
};

/// Everything the handlers share, kept alive by the closures Drogon holds.
struct ConsoleState {
    SessionStore    sessions;
    LoginThrottle   throttle;
    MetricsHistory  history{kSampleCapacity, static_cast<std::uint32_t>(kSampleIntervalSeconds)};
};

bool onConsoleListener(const HttpRequestPtr& req, const Config& config) {
    return config.consoleEnabled && req->getLocalAddr().toPort() == config.consolePort;
}

nlohmann::json toJson(const BucketRecord& bucket) {
    return {{"name", bucket.name},
            {"createdAt", toIso8601(bucket.createdAt)},
            {"createdAtMs", bucket.createdAt},
            {"publicRead", bucket.publicRead},
            {"hasPolicy", !bucket.policy.empty()},
            {"corsRules", bucket.cors.size()},
            // null rather than the level it currently resolves to: the bucket
            // is following the server, and rendering today's answer would make
            // a form that saves it back pin the bucket to it by accident.
            {"durability", bucket.durability
                               ? nlohmann::json(std::string(toString(*bucket.durability)))
                               : nlohmann::json(nullptr)}};
}

nlohmann::json toJson(const CorsRule& rule) {
    // maxAgeSeconds is null rather than 0 when the rule sets none: zero tells a
    // browser not to cache the preflight, which is a different instruction from
    // leaving the choice to it, and a form that could not express the
    // difference would silently change one into the other.
    return {{"id", rule.id},
            {"allowedOrigins", rule.allowedOrigins},
            {"allowedMethods", rule.allowedMethods},
            {"allowedHeaders", rule.allowedHeaders},
            {"exposeHeaders", rule.exposeHeaders},
            {"maxAgeSeconds", rule.maxAgeSeconds >= 0 ? nlohmann::json(rule.maxAgeSeconds)
                                                      : nlohmann::json(nullptr)}};
}

/// Reads the rule list a console form submits. Shape errors are reported here;
/// everything about whether the rules are *acceptable* is left to
/// s3::validateCorsRules, so the console and the S3 API cannot disagree.
std::vector<CorsRule> corsRulesFrom(const nlohmann::json& value) {
    if (!value.is_array()) throw std::invalid_argument("rules must be an array");

    const auto stringList = [](const nlohmann::json& node, const char* field) {
        std::vector<std::string> out;
        if (node.is_null()) return out;
        if (!node.is_array()) throw std::invalid_argument(std::string(field) + " must be an array");
        for (const auto& entry : node) {
            if (!entry.is_string()) {
                throw std::invalid_argument(std::string(field) + " must contain strings");
            }
            out.push_back(entry.get<std::string>());
        }
        return out;
    };

    std::vector<CorsRule> rules;
    for (const auto& node : value) {
        if (!node.is_object()) throw std::invalid_argument("each rule must be an object");

        CorsRule rule;
        rule.id             = node.value("id", std::string{});
        rule.allowedOrigins = stringList(node.value("allowedOrigins", nlohmann::json::array()),
                                         "allowedOrigins");
        rule.allowedMethods = stringList(node.value("allowedMethods", nlohmann::json::array()),
                                         "allowedMethods");
        rule.allowedHeaders = stringList(node.value("allowedHeaders", nlohmann::json::array()),
                                         "allowedHeaders");
        rule.exposeHeaders  = stringList(node.value("exposeHeaders", nlohmann::json::array()),
                                         "exposeHeaders");

        const auto& maxAge = node.contains("maxAgeSeconds") ? node.at("maxAgeSeconds")
                                                            : nlohmann::json(nullptr);
        if (!maxAge.is_null()) {
            if (!maxAge.is_number_integer()) {
                throw std::invalid_argument("maxAgeSeconds must be a whole number or null");
            }
            const auto seconds = maxAge.get<std::int64_t>();
            if (seconds < 0 || seconds > 86400) {
                throw std::invalid_argument("maxAgeSeconds must be between 0 and 86400");
            }
            rule.maxAgeSeconds = static_cast<std::int32_t>(seconds);
        }

        rules.push_back(std::move(rule));
    }
    return rules;
}

/// The environment variable behind each key of `Config::toJson()`.
///
/// The settings panel is read-only because configuration is environment-only,
/// and a read-only panel that does not say *which* variable to set is a panel
/// that sends the reader to the documentation anyway. Anything unmapped is
/// rendered without a variable rather than with a guessed one.
const std::unordered_map<std::string, std::string>& settingEnvironmentNames() {
    static const std::unordered_map<std::string, std::string> kNames{
        {"host", "MONOBUCKET_HOST"},
        {"s3Port", "MONOBUCKET_S3_PORT"},
        {"consolePort", "MONOBUCKET_CONSOLE_PORT"},
        {"consoleEnabled", "MONOBUCKET_CONSOLE_ENABLED"},
        {"dataDir", "MONOBUCKET_DATA_DIR"},
        {"region", "MONOBUCKET_REGION"},
        {"s3Domain", "MONOBUCKET_S3_DOMAIN"},
        {"durability", "MONOBUCKET_DURABILITY"},
        {"metadataMemoryBytes", "MONOBUCKET_METADATA_MEMORY_BYTES"},
        {"metadataMaxOpenFiles", "MONOBUCKET_METADATA_MAX_OPEN_FILES"},
        {"reclaimGraceSeconds", "MONOBUCKET_RECLAIM_GRACE_SECONDS"},
        {"reclaimIntervalSeconds", "MONOBUCKET_RECLAIM_INTERVAL_SECONDS"},
        {"ioThreads", "MONOBUCKET_IO_THREADS"},
        {"ioQueueLimit", "MONOBUCKET_IO_QUEUE_LIMIT"},
        {"rootAccessKey", "MONOBUCKET_ROOT_ACCESS_KEY"},
        {"rootSecretKey", "MONOBUCKET_ROOT_SECRET_KEY"},
        {"workerThreads", "MONOBUCKET_WORKER_THREADS"},
        {"maxBodyBytes", "MONOBUCKET_MAX_BODY_BYTES"},
        {"maxMemoryBodyBytes", "MONOBUCKET_MAX_MEMORY_BODY_BYTES"},
        {"streamChunkBytes", "MONOBUCKET_STREAM_CHUNK_BYTES"},
        {"idleTimeoutSeconds", "MONOBUCKET_IDLE_TIMEOUT_SECONDS"},
        {"cacheBackend", "MONOBUCKET_CACHE_BACKEND"},
        {"cacheMaxBytes", "MONOBUCKET_CACHE_MAX_BYTES"},
        {"cacheTtlSeconds", "MONOBUCKET_CACHE_TTL_SECONDS"},
        {"cacheLocalTtlSeconds", "MONOBUCKET_CACHE_LOCAL_TTL_SECONDS"},
        {"redisConfigured", "MONOBUCKET_REDIS_URL"},
        {"redisPoolSize", "MONOBUCKET_REDIS_POOL_SIZE"},
        {"logLevel", "MONOBUCKET_LOG_LEVEL"},
        {"metricsEnabled", "MONOBUCKET_METRICS_ENABLED"},
    };
    return kNames;
}

nlohmann::json toJson(const ObjectRecord& object) {
    return {{"key", object.key},
            {"size", object.size},
            {"etag", object.etag},
            {"contentType", object.contentType},
            {"lastModified", toIso8601(object.lastModified)},
            {"lastModifiedMs", object.lastModified}};
}

nlohmann::json toJson(const MetricsHistory::Sample& sample) {
    return {{"atMs", sample.atMs},
            {"spanMs", sample.spanMs},
            {"requests", sample.requests},
            {"succeeded", sample.succeeded},
            {"clientErrors", sample.clientErrors},
            {"serverErrors", sample.serverErrors},
            {"shed", sample.shed},
            {"bytesIn", sample.bytesIn},
            {"bytesOut", sample.bytesOut},
            {"cacheHits", sample.cacheHits},
            {"cacheMisses", sample.cacheMisses},
            {"objects", sample.objects},
            {"storedBytes", sample.storedBytes},
            {"residentBytes", sample.residentBytes},
            {"cacheBytes", sample.cacheBytes},
            {"ioQueued", sample.ioQueued},
            {"ioActive", sample.ioActive},
            {"connections", sample.connections}};
}

drogon::HttpStatusCode statusFor(StorageErrorCode code) {
    switch (code) {
        case StorageErrorCode::NoSuchBucket:
        case StorageErrorCode::NoSuchKey:
        case StorageErrorCode::NoSuchUpload:
            return drogon::k404NotFound;
        case StorageErrorCode::BucketAlreadyExists:
        case StorageErrorCode::BucketNotEmpty:
        case StorageErrorCode::InvalidPart:
            return drogon::k409Conflict;
        default:
            return drogon::k500InternalServerError;
    }
}

}  // namespace

void registerConsoleApi(const Config& config, StorageEngine& storage, IoExecutor& io,
                        CacheProvider& cache, const s3::S3Metrics& s3Metrics) {
    if (!config.consoleEnabled) return;

    auto  state = std::make_shared<ConsoleState>();
    auto& app   = drogon::app();

    // --- Plumbing ----------------------------------------------------------

    // Every console route shares the same three gates, in this order: it must
    // have arrived on the console listener, it must carry a live session, and
    // whatever it does to storage must happen on an I/O thread.
    const auto guard = [&config, state](const HttpRequestPtr& req, const ResponseCallback& callback,
                                        const std::function<void(const std::string&)>& handler) {
        if (!onConsoleListener(req, config)) {
            callback(errorJson("not found", drogon::k404NotFound));
            return;
        }
        const std::string accessKey = state->sessions.resolve(req->getCookie(kSessionCookie));
        if (accessKey.empty()) {
            callback(errorJson("not signed in", drogon::k401Unauthorized));
            return;
        }
        handler(accessKey);
    };

    // Storage work is posted rather than run inline for the same reason the S3
    // router posts it: a listing that hits a cold RocksDB block must not stall
    // the loop that is serving the rest of the console.
    const auto offload = [&io](const ResponseCallback& callback,
                               std::function<HttpResponsePtr()> work) {
        const bool accepted = io.post([callback, work = std::move(work)]() mutable {
            try {
                callback(work());
            } catch (const StorageError& error) {
                callback(errorJson(error.what(), statusFor(error.code())));
            } catch (const std::exception& error) {
                log::error("console api: ", error.what());
                callback(errorJson("internal error", drogon::k500InternalServerError));
            }
        });
        if (!accepted) {
            callback(errorJson("the storage queue is saturated", drogon::k503ServiceUnavailable));
        }
    };

    // --- Session -----------------------------------------------------------

    app.registerHandler(
        "/_mb/api/login",
        [&config, state](const HttpRequestPtr& req, ResponseCallback&& callback) {
            if (!onConsoleListener(req, config)) {
                callback(errorJson("not found", drogon::k404NotFound));
                return;
            }
            if (state->throttle.blocked()) {
                auto resp = errorJson("too many failed attempts, try again shortly",
                                      drogon::k429TooManyRequests);
                resp->addHeader("Retry-After", "60");
                callback(resp);
                return;
            }

            const auto body = nlohmann::json::parse(req->getBody(), nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
                callback(errorJson("expected a JSON object", drogon::k400BadRequest));
                return;
            }

            const auto accessKey = body.value("accessKey", std::string{});
            const auto secretKey = body.value("secretKey", std::string{});

            // Both halves are compared even when the access key is already
            // wrong, so a valid key is not distinguishable by response time.
            const bool keyOk    = secretsMatch(accessKey, config.rootAccessKey);
            const bool secretOk = secretsMatch(secretKey, config.rootSecretKey);
            if (!keyOk || !secretOk) {
                state->throttle.recordFailure();
                callback(errorJson("invalid credentials", drogon::k401Unauthorized));
                return;
            }

            state->throttle.recordSuccess();
            auto resp = jsonResponse({{"accessKey", accessKey},
                                      {"expiresInSeconds", kSessionTtlSeconds}});

            // HttpOnly so a script bug cannot read the token, SameSite=Strict so
            // another origin cannot ride the session. No Secure flag: the
            // console is routinely served over plain HTTP on a private network,
            // and a cookie the browser refuses to send is not a security win.
            drogon::Cookie cookie(kSessionCookie, state->sessions.open(accessKey));
            cookie.setHttpOnly(true);
            cookie.setPath("/");
            cookie.setMaxAge(kSessionTtlSeconds);
            cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
            resp->addCookie(std::move(cookie));
            callback(resp);
        },
        {drogon::Post});

    app.registerHandler(
        "/_mb/api/logout",
        [&config, state](const HttpRequestPtr& req, ResponseCallback&& callback) {
            if (!onConsoleListener(req, config)) {
                callback(errorJson("not found", drogon::k404NotFound));
                return;
            }
            state->sessions.close(req->getCookie(kSessionCookie));

            auto           resp = jsonResponse({{"signedOut", true}});
            drogon::Cookie cookie(kSessionCookie, "");
            cookie.setHttpOnly(true);
            cookie.setPath("/");
            cookie.setMaxAge(0);
            cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
            resp->addCookie(std::move(cookie));
            callback(resp);
        },
        {drogon::Post});

    // The one route that answers 200 when signed out: the SPA asks it on boot
    // to decide between the dashboard and the login form, and a 401 there is an
    // answer rather than an error.
    app.registerHandler(
        "/_mb/api/session",
        [&config, state](const HttpRequestPtr& req, ResponseCallback&& callback) {
            if (!onConsoleListener(req, config)) {
                callback(errorJson("not found", drogon::k404NotFound));
                return;
            }
            const std::string accessKey = state->sessions.resolve(req->getCookie(kSessionCookie));
            // The S3 port and domain travel with the session because the console
            // is served from a different port and cannot infer them. The host is
            // deliberately not included: `config.host` is usually 0.0.0.0, and
            // the name the browser used to reach us is the only one known to
            // work — so the client supplies that half.
            callback(jsonResponse({{"authenticated", !accessKey.empty()},
                                   {"accessKey", accessKey},
                                   {"usingDefaultCredentials", config.usingDefaultCredentials()},
                                   {"s3Port", config.s3Port},
                                   {"s3Domain", config.s3Domain},
                                   {"version", version::kVersion}}));
        },
        {drogon::Get});

    // --- Overview ----------------------------------------------------------

    app.registerHandler(
        "/_mb/api/overview",
        [&config, &storage, &io, &cache, &s3Metrics, guard, offload](
            const HttpRequestPtr& req, ResponseCallback&& callback) {
            guard(req, callback, [&config, &storage, &io, &cache, &s3Metrics, callback,
                                  offload](const std::string&) {
                offload(callback, [&config, &storage, &io, &cache, &s3Metrics]() -> HttpResponsePtr {
                    const auto stats      = storage.stats();
                    const auto ioStats    = io.stats();
                    const auto cacheStats = cache.stats();

                    nlohmann::json gauges = nlohmann::json::object();
                    for (const auto& [name, value] : stats.engineGauges) gauges[name] = value;

                    return jsonResponse({
                        {"server",
                         nlohmann::json{{"version", version::kVersion},
                                        {"region", config.region},
                                        {"uptimeSeconds", uptimeSeconds()},
                                        {"residentBytes", residentBytes()},
                                        {"workerThreads", config.workerThreads},
                                        {"s3Port", config.s3Port},
                                        {"consolePort", config.consolePort},
                                        // Both listeners together, which is
                                        // all Drogon counts. Labelling it "S3"
                                        // would be a number that includes this
                                        // very request.
                                        {"connections", drogon::app().getConnectionCount()}}},
                        {"storage",
                         nlohmann::json{{"engine", stats.engine},
                                        {"buckets", stats.usage.buckets},
                                        {"objects", stats.usage.objects},
                                        {"bytes", stats.usage.bytes},
                                        {"uploads", stats.usage.uploads},
                                        {"orphanBlobs", stats.usage.orphanBlobs},
                                        {"diskTotalBytes", stats.space.totalBytes},
                                        {"diskAvailableBytes", stats.space.availableBytes},
                                        {"engineGauges", gauges}}},
                        {"io",
                         nlohmann::json{{"queued", ioStats.queued},
                                        {"active", ioStats.active},
                                        {"completed", ioStats.completed},
                                        {"rejected", ioStats.rejected},
                                        {"threads", ioStats.threads},
                                        {"limit", ioStats.limit}}},
                        {"cache",
                         nlohmann::json{{"backend", std::string(cache.name())},
                                        {"healthy", cacheStats.healthy},
                                        {"entries", cacheStats.entries},
                                        {"bytes", cacheStats.bytes},
                                        {"limitBytes", cacheStats.limitBytes},
                                        {"hits", cacheStats.hits},
                                        {"misses", cacheStats.misses},
                                        {"hitRatio", cacheStats.hitRatio()},
                                        {"evictions", cacheStats.evictions},
                                        {"rejections", cacheStats.rejections},
                                        {"errors", cacheStats.errors}}},
                        {"s3",
                         nlohmann::json{
                             {"requests", s3Metrics.requests.load(std::memory_order_relaxed)},
                             {"succeeded", s3Metrics.succeeded.load(std::memory_order_relaxed)},
                             {"clientErrors",
                              s3Metrics.clientErrors.load(std::memory_order_relaxed)},
                             {"serverErrors",
                              s3Metrics.serverErrors.load(std::memory_order_relaxed)},
                             {"shed", s3Metrics.shed.load(std::memory_order_relaxed)},
                             {"authFailures",
                              s3Metrics.authFailures.load(std::memory_order_relaxed)},
                             {"anonymous", s3Metrics.anonymous.load(std::memory_order_relaxed)},
                             {"bytesIn", s3Metrics.bytesIn.load(std::memory_order_relaxed)},
                             {"bytesOut", s3Metrics.bytesOut.load(std::memory_order_relaxed)}}},
                    });
                });
            });
        },
        {drogon::Get});

    // Served straight off the event loop: the ring is in memory and copying it
    // is cheaper than the round trip to an I/O thread would be.
    app.registerHandler(
        "/_mb/api/series",
        [state, guard](const HttpRequestPtr& req, ResponseCallback&& callback) {
            guard(req, callback, [state, callback](const std::string&) {
                nlohmann::json samples = nlohmann::json::array();
                for (const auto& sample : state->history.samples()) samples.push_back(toJson(sample));
                callback(jsonResponse({{"intervalSeconds", state->history.intervalSeconds()},
                                       {"capacity", state->history.capacity()},
                                       {"samples", std::move(samples)}}));
            });
        },
        {drogon::Get});

    // --- Buckets -----------------------------------------------------------

    app.registerHandler(
        "/_mb/api/buckets",
        [&storage, guard, offload](const HttpRequestPtr& req, ResponseCallback&& callback) {
            guard(req, callback, [&storage, req, callback, offload](const std::string&) {
                const std::string method = req->getMethodString();

                if (method == "GET") {
                    offload(callback, [&storage]() -> HttpResponsePtr {
                        nlohmann::json buckets = nlohmann::json::array();
                        for (const auto& bucket : storage.listBuckets()) {
                            buckets.push_back(toJson(bucket));
                        }
                        return jsonResponse({{"buckets", std::move(buckets)}});
                    });
                    return;
                }

                if (method == "POST") {
                    const auto body = nlohmann::json::parse(req->getBody(), nullptr, false);
                    if (body.is_discarded() || !body.is_object()) {
                        callback(errorJson("expected a JSON object", drogon::k400BadRequest));
                        return;
                    }
                    const auto name = body.value("name", std::string{});
                    if (name.empty()) {
                        callback(errorJson("a bucket name is required", drogon::k400BadRequest));
                        return;
                    }
                    offload(callback, [&storage, name]() -> HttpResponsePtr {
                        storage.createBucket(name);
                        const auto record = storage.getBucket(name);
                        return jsonResponse(record ? toJson(*record) : nlohmann::json{{"name", name}},
                                            drogon::k201Created);
                    });
                    return;
                }

                const std::string name = req->getParameter("name");
                if (name.empty()) {
                    callback(errorJson("a bucket name is required", drogon::k400BadRequest));
                    return;
                }
                offload(callback, [&storage, name]() -> HttpResponsePtr {
                    storage.deleteBucket(name);
                    return jsonResponse({{"deleted", name}});
                });
            });
        },
        {drogon::Get, drogon::Post, drogon::Delete});

    app.registerHandler(
        "/_mb/api/buckets/access",
        [&storage, &cache, guard, offload](const HttpRequestPtr& req, ResponseCallback&& callback) {
            guard(req, callback, [&storage, &cache, req, callback, offload](const std::string&) {
                const auto body = nlohmann::json::parse(req->getBody(), nullptr, false);
                if (body.is_discarded() || !body.is_object()) {
                    callback(errorJson("expected a JSON object", drogon::k400BadRequest));
                    return;
                }
                const auto name = body.value("name", std::string{});
                if (name.empty()) {
                    callback(errorJson("a bucket name is required", drogon::k400BadRequest));
                    return;
                }
                const bool publicRead = body.value("publicRead", false);

                // Absent means "leave the override alone", null means "clear it
                // and follow the server". A form that only edits public access
                // must not silently reset durability by omitting the field.
                std::optional<std::optional<Durability>> durability;
                if (body.contains("durability")) {
                    const auto& node = body.at("durability");
                    if (node.is_null()) {
                        durability.emplace();
                    } else if (node.is_string()) {
                        const auto parsed = durabilityFromString(node.get<std::string>());
                        if (!parsed) {
                            callback(errorJson("durability must be none, relaxed or strict",
                                               drogon::k400BadRequest));
                            return;
                        }
                        durability.emplace(*parsed);
                    } else {
                        callback(errorJson("durability must be a string or null",
                                           drogon::k400BadRequest));
                        return;
                    }
                }

                offload(callback, [&storage, &cache, name, publicRead,
                                   durability]() -> HttpResponsePtr {
                    storage.setBucketPublicRead(name, publicRead);
                    if (durability) storage.setBucketDurability(name, *durability);

                    // Without this the S3 anonymous path keeps reading the old
                    // flag until the entry ages out, so making a bucket private
                    // in the console would not take effect for a whole TTL.
                    cache.del(s3::bucketCacheKey(name));

                    const auto record = storage.getBucket(name);
                    if (!record) {
                        return errorJson("no such bucket", drogon::k404NotFound);
                    }
                    return jsonResponse(toJson(*record));
                });
            });
        },
        {drogon::Post});

    // GET reads the rules, POST replaces them, DELETE turns CORS off. The same
    // three verbs the S3 API uses on ?cors, and the same storage call behind
    // them — the console is a second front door onto one feature, not a second
    // implementation of it.
    app.registerHandler(
        "/_mb/api/buckets/cors",
        [&storage, &cache, guard, offload](const HttpRequestPtr& req,
                                           ResponseCallback&& callback) {
            guard(req, callback, [&storage, &cache, req, callback, offload](const std::string&) {
                const std::string method = std::string(req->getMethodString());

                if (method == "GET") {
                    const std::string name = req->getParameter("bucket");
                    if (name.empty()) {
                        callback(errorJson("a bucket name is required", drogon::k400BadRequest));
                        return;
                    }
                    offload(callback, [&storage, name]() -> HttpResponsePtr {
                        const auto record = storage.getBucket(name);
                        if (!record) return errorJson("no such bucket", drogon::k404NotFound);

                        auto rules = nlohmann::json::array();
                        for (const CorsRule& rule : record->cors) rules.push_back(toJson(rule));
                        return jsonResponse({{"bucket", name}, {"rules", rules}});
                    });
                    return;
                }

                const auto body = nlohmann::json::parse(req->getBody(), nullptr, false);
                if (body.is_discarded() || !body.is_object()) {
                    callback(errorJson("expected a JSON object", drogon::k400BadRequest));
                    return;
                }
                const auto name = body.value("name", std::string{});
                if (name.empty()) {
                    callback(errorJson("a bucket name is required", drogon::k400BadRequest));
                    return;
                }

                std::vector<CorsRule> rules;
                if (method == "POST") {
                    try {
                        rules = s3::validateCorsRules(
                            corsRulesFrom(body.contains("rules") ? body.at("rules")
                                                                 : nlohmann::json::array()));
                    } catch (const s3::S3Exception& error) {
                        callback(errorJson(error.what(), drogon::k400BadRequest));
                        return;
                    } catch (const std::invalid_argument& error) {
                        callback(errorJson(error.what(), drogon::k400BadRequest));
                        return;
                    }
                }

                offload(callback,
                        [&storage, &cache, name, rules = std::move(rules)]() -> HttpResponsePtr {
                    storage.setBucketCors(name, rules);

                    // The S3 read path caches the bucket record, and a stale
                    // entry would keep answering preflights from the rules the
                    // console just replaced.
                    cache.del(s3::bucketCacheKey(name));

                    auto out = nlohmann::json::array();
                    for (const CorsRule& rule : rules) out.push_back(toJson(rule));
                    return jsonResponse({{"bucket", name}, {"rules", out}});
                });
            });
        },
        {drogon::Get, drogon::Post, drogon::Delete});

    // The same three verbs, the same validation and the same derived
    // anonymous-read flag as `?policy` on the S3 listener. The console is a
    // second front door onto one feature, never a second implementation.
    app.registerHandler(
        "/_mb/api/buckets/policy",
        [&storage, &cache, guard, offload](const HttpRequestPtr& req,
                                           ResponseCallback&& callback) {
            guard(req, callback, [&storage, &cache, req, callback, offload](const std::string&) {
                const std::string method = std::string(req->getMethodString());

                if (method == "GET") {
                    const std::string name = req->getParameter("bucket");
                    if (name.empty()) {
                        callback(errorJson("a bucket name is required", drogon::k400BadRequest));
                        return;
                    }
                    offload(callback, [&storage, name]() -> HttpResponsePtr {
                        const auto record = storage.getBucket(name);
                        if (!record) return errorJson("no such bucket", drogon::k404NotFound);
                        return jsonResponse({{"bucket", name},
                                             {"policy", record->policy},
                                             {"publicRead", record->publicRead}});
                    });
                    return;
                }

                const auto body = nlohmann::json::parse(req->getBody(), nullptr, false);
                if (body.is_discarded() || !body.is_object()) {
                    callback(errorJson("expected a JSON object", drogon::k400BadRequest));
                    return;
                }
                const auto name = body.value("name", std::string{});
                if (name.empty()) {
                    callback(errorJson("a bucket name is required", drogon::k400BadRequest));
                    return;
                }

                std::string document;
                if (method == "POST") {
                    // Accepted either as a JSON string holding the document or
                    // as the document itself, because a textarea produces the
                    // first and a programmatic caller reaches for the second.
                    const auto& policy = body.contains("policy") ? body.at("policy")
                                                                 : nlohmann::json(nullptr);
                    document = policy.is_string() ? policy.get<std::string>() : policy.dump();
                    try {
                        s3::validateBucketPolicy(document);
                    } catch (const s3::S3Exception& error) {
                        callback(errorJson(error.what(), drogon::k400BadRequest));
                        return;
                    }
                }

                // Deleting the policy removes the access it granted; leaving
                // the flag set would keep a bucket public with nothing left in
                // the record to say why.
                const bool publicRead =
                    !document.empty() && s3::policyGrantsAnonymousRead(document, name);

                offload(callback, [&storage, &cache, name, document, publicRead]()
                                      -> HttpResponsePtr {
                    storage.setBucketPolicy(name, document, publicRead);
                    cache.del(s3::bucketCacheKey(name));

                    const auto record = storage.getBucket(name);
                    if (!record) return errorJson("no such bucket", drogon::k404NotFound);
                    return jsonResponse({{"bucket", name},
                                         {"policy", record->policy},
                                         {"publicRead", record->publicRead}});
                });
            });
        },
        {drogon::Get, drogon::Post, drogon::Delete});

    // --- Settings ----------------------------------------------------------

    // Read-only, and behind the session: the resolved configuration names the
    // data directory and the root access key, which is not something to hand
    // out to whoever can reach the console port.
    //
    // There is nothing to write here on purpose. Configuration is environment
    // only, validated once before the first listener opens, so an editable
    // panel would be a promise the server cannot keep.
    app.registerHandler(
        "/_mb/api/config",
        [&config, &cache, guard](const HttpRequestPtr& req, ResponseCallback&& callback) {
            guard(req, callback, [&config, &cache, callback](const std::string&) {
                const auto& names = settingEnvironmentNames();

                // Bound to a local rather than iterated in place: `items()`
                // returns a view over the object it was called on, and calling
                // it on the temporary `toJson()` returns leaves the loop walking
                // a destroyed value.
                const nlohmann::json resolved = config.toJson();

                nlohmann::json settings = nlohmann::json::array();
                for (const auto& [key, value] : resolved.items()) {
                    const auto name = names.find(key);
                    settings.push_back({{"key", key},
                                        {"value", value},
                                        {"env", name == names.end() ? "" : name->second}});
                }

                callback(jsonResponse(
                    {{"version", version::kVersion},
                     {"settings", std::move(settings)},
                     // The backend that is actually in use, which is not always
                     // the one that was asked for: a Redis that cannot be
                     // reached leaves the local tier serving alone.
                     {"cacheBackendActive", std::string(cache.name())},
                     {"usingDefaultCredentials", config.usingDefaultCredentials()}}));
            });
        },
        {drogon::Get});

    // --- Objects -----------------------------------------------------------

    // Bucket and key travel as query parameters rather than path segments: an
    // object key may contain slashes, question marks and percent signs, and a
    // routing pattern that tries to carve one out of a path gets it wrong for
    // exactly the keys a file browser needs to show.
    app.registerHandler(
        "/_mb/api/objects",
        [&storage, &cache, guard, offload](const HttpRequestPtr& req,
                                           ResponseCallback&& callback) {
            guard(req, callback, [&storage, &cache, req, callback, offload](const std::string&) {
                const std::string bucket = req->getParameter("bucket");
                if (bucket.empty()) {
                    callback(errorJson("a bucket is required", drogon::k400BadRequest));
                    return;
                }

                if (std::string(req->getMethodString()) == "DELETE") {
                    const std::string key = req->getParameter("key");
                    if (key.empty()) {
                        callback(errorJson("a key is required", drogon::k400BadRequest));
                        return;
                    }
                    offload(callback, [&storage, &cache, bucket, key]() -> HttpResponsePtr {
                        if (!storage.deleteObject(bucket, key)) {
                            return errorJson("no such key", drogon::k404NotFound);
                        }
                        // Without this the S3 read path answers HEAD from the
                        // cached metadata of an object that is no longer there.
                        cache.del(s3::objectCacheKey(bucket, key));
                        return jsonResponse({{"deleted", key}});
                    });
                    return;
                }

                ListObjectsRequest listing;
                listing.prefix     = req->getParameter("prefix");
                listing.delimiter  = req->getParameter("delimiter");
                listing.startAfter = req->getParameter("after");

                const std::string limit = req->getParameter("limit");
                if (!limit.empty()) {
                    try {
                        // Clamped rather than rejected: the browser paginates,
                        // and an unbounded page is a way to ask the server to
                        // materialise a million records into one response.
                        listing.maxKeys = static_cast<std::uint32_t>(
                            std::clamp(std::stol(limit), 1L, 1000L));
                    } catch (const std::exception&) {
                        callback(errorJson("limit must be a number", drogon::k400BadRequest));
                        return;
                    }
                }

                offload(callback, [&storage, bucket, listing]() -> HttpResponsePtr {
                    const auto result = storage.listObjects(bucket, listing);

                    nlohmann::json objects = nlohmann::json::array();
                    for (const auto& object : result.objects) objects.push_back(toJson(object));

                    // Carried on the listing so the file browser can say whether
                    // an object's URL works without credentials. It is one more
                    // metadata read on a task that has already paid for the trip
                    // to an I/O thread, against a second round trip if the
                    // browser had to ask separately.
                    const auto record = storage.getBucket(bucket);

                    return jsonResponse({{"bucket", bucket},
                                         {"publicRead", record && record->publicRead},
                                         {"prefix", listing.prefix},
                                         {"delimiter", listing.delimiter},
                                         {"objects", std::move(objects)},
                                         {"prefixes", result.commonPrefixes},
                                         {"truncated", result.truncated},
                                         {"nextAfter", result.nextStartAfter}});
                });
            });
        },
        {drogon::Get, drogon::Delete});

    // One request per file, streamed into the payload tree in fixed-size
    // chunks. The browser's own upload progress is what the drop zone draws:
    // a chunked console protocol would need a resumable upload session on this
    // side, and the S3 listener already has one — multipart — for the clients
    // that need it.
    //
    // Memory stays flat because Drogon spills anything above
    // MONOBUCKET_MAX_MEMORY_BODY_BYTES to a file and hands back a mapping of
    // it, so a five-gigabyte body is five gigabytes of page cache and one
    // chunk of heap. No signature is involved: the caller holds a console
    // session, not an S3 secret.
    app.registerHandler(
        "/_mb/api/upload",
        [&config, &storage, &cache, guard, offload](const HttpRequestPtr& req,
                                                    ResponseCallback&& callback) {
            guard(req, callback, [&config, &storage, &cache, req, callback,
                                  offload](const std::string&) {
                const std::string bucket = req->getParameter("bucket");
                const std::string key    = req->getParameter("key");
                if (bucket.empty() || key.empty()) {
                    callback(errorJson("a bucket and key are required", drogon::k400BadRequest));
                    return;
                }
                // The same rule the S3 key validator applies. Refusing here
                // means the browser is told which name is the problem instead
                // of finding out after the bytes have been sent.
                if (key.size() > 1024 || key.front() == '/' || key.find("//") != std::string::npos ||
                    key.find("..") != std::string::npos) {
                    callback(errorJson("that object key is not valid", drogon::k400BadRequest));
                    return;
                }

                std::string contentType = std::string(req->getHeader("content-type"));
                if (contentType.empty()) contentType = "application/octet-stream";

                offload(callback, [&config, &storage, &cache, req, bucket, key,
                                   contentType]() -> HttpResponsePtr {
                    if (!storage.getBucket(bucket)) {
                        return errorJson("no such bucket", drogon::k404NotFound);
                    }

                    StorageEngine::PutRequest put;
                    put.bucket      = bucket;
                    put.key         = key;
                    put.contentType = contentType;

                    // `req` is kept alive by this closure, which is what keeps
                    // the body's mapping valid for the whole write.
                    const std::string_view payload = req->getBody();
                    const std::size_t chunk = std::max<std::size_t>(config.streamChunkBytes, 1);

                    BlobWriter writer = storage.beginWrite();
                    for (std::size_t at = 0; at < payload.size(); at += chunk) {
                        writer.write(payload.substr(at, chunk));
                    }

                    const ObjectRecord record = storage.finishWrite(put, std::move(writer));

                    // After the write, never before: a failed upload must not
                    // drop a cached entry that is still the current one.
                    cache.del(s3::objectCacheKey(bucket, key));

                    return jsonResponse(toJson(record), drogon::k201Created);
                });
            });
        },
        {drogon::Put});

    app.registerHandler(
        "/_mb/api/object",
        [&storage, guard, offload](const HttpRequestPtr& req, ResponseCallback&& callback) {
            guard(req, callback, [&storage, req, callback, offload](const std::string&) {
                const std::string bucket = req->getParameter("bucket");
                const std::string key    = req->getParameter("key");
                if (bucket.empty() || key.empty()) {
                    callback(errorJson("a bucket and key are required", drogon::k400BadRequest));
                    return;
                }
                offload(callback, [&storage, bucket, key]() -> HttpResponsePtr {
                    const auto record = storage.statObject(bucket, key);
                    if (!record) return errorJson("no such key", drogon::k404NotFound);

                    nlohmann::json meta = nlohmann::json::object();
                    for (const auto& [name, value] : record->userMetadata) meta[name] = value;

                    nlohmann::json body = toJson(*record);
                    body["sha256"]      = record->sha256;
                    body["bucket"]      = bucket;
                    body["userMetadata"] = std::move(meta);
                    return jsonResponse(body);
                });
            });
        },
        {drogon::Get});

    // --- Presigned links ---------------------------------------------------

    // Signing happens here rather than in the browser because the browser holds
    // a session cookie, not an S3 secret — and giving it one to sign with would
    // undo the reason the two are separate.
    app.registerHandler(
        "/_mb/api/presign",
        [&config, &storage, guard, offload](const HttpRequestPtr& req,
                                            ResponseCallback&& callback) {
            guard(req, callback, [&config, &storage, req, callback, offload](const std::string&) {
                const auto body = nlohmann::json::parse(req->getBody(), nullptr, false);
                if (body.is_discarded() || !body.is_object()) {
                    callback(errorJson("expected a JSON object", drogon::k400BadRequest));
                    return;
                }

                const auto bucket  = body.value("bucket", std::string{});
                const auto key     = body.value("key", std::string{});
                const auto host    = body.value("host", std::string{});
                const bool secure  = body.value("secure", false);
                const auto expires = body.value("expiresSeconds", std::int64_t{3600});

                if (bucket.empty() || key.empty()) {
                    callback(errorJson("a bucket and key are required", drogon::k400BadRequest));
                    return;
                }
                if (!plausibleHost(host)) {
                    callback(errorJson("a valid host is required", drogon::k400BadRequest));
                    return;
                }
                if (expires <= 0 || expires > kMaxPresignSeconds) {
                    callback(errorJson("expiresSeconds must be between 1 and 604800",
                                       drogon::k400BadRequest));
                    return;
                }

                offload(callback, [&config, &storage, bucket, key, host, secure,
                                   expires]() -> HttpResponsePtr {
                    // A link to a key that is not there looks exactly like a link
                    // that is wrong, and the difference only shows up after it has
                    // been sent to someone. One stat is cheaper than that.
                    if (!storage.statObject(bucket, key)) {
                        return errorJson("no such key", drogon::k404NotFound);
                    }

                    // Virtual-host addressing only when the host really is this
                    // bucket's subdomain of the configured endpoint. The two
                    // forms sign different URIs, so guessing wrong produces a
                    // signature that verifies against nothing.
                    const bool virtualHost =
                        !config.s3Domain.empty() && host == bucket + '.' + config.s3Domain;

                    std::string path = "/";
                    if (!virtualHost) {
                        path += s3::uriEncode(bucket, true);
                        path += '/';
                    }
                    path += s3::uriEncode(key, false);

                    s3::PresignRequest presign;
                    presign.method         = "GET";
                    presign.host           = host;
                    presign.uri            = path;
                    presign.region         = config.region;
                    presign.nowSeconds     = nowSeconds();
                    presign.expiresSeconds = expires;

                    const std::string query =
                        s3::presignQuery(presign, {config.rootAccessKey, config.rootSecretKey});

                    return jsonResponse(
                        {{"url", (secure ? "https://" : "http://") + host + path + '?' + query},
                         {"method", presign.method},
                         {"expiresInSeconds", expires},
                         {"expiresAtMs", (presign.nowSeconds + expires) * 1000}});
                });
            });
        },
        {drogon::Post});

    // --- Sampler -----------------------------------------------------------

    // Reading storage stats touches RocksDB, so the tick posts rather than
    // reads. A saturated queue skips the sample instead of queueing behind
    // real work: a gap in a graph is cheaper than a slower PUT.
    drogon::app().getLoop()->runEvery(kSampleIntervalSeconds, [state, &storage, &io, &cache,
                                                               &s3Metrics] {
        // Read here rather than inside the posted task: by the time the task
        // runs it is one of the things being counted, and the number would
        // include the queue delay as if it were load.
        const std::int64_t connectionCount = drogon::app().getConnectionCount();

        const bool accepted = io.post([state, &storage, &io, &cache, &s3Metrics,
                                       connectionCount] {
            try {
                const auto stats      = storage.stats();
                const auto ioStats    = io.stats();
                const auto cacheStats = cache.stats();

                MetricsHistory::Reading reading;
                reading.atMs = nowMs();

                reading.requests     = s3Metrics.requests.load(std::memory_order_relaxed);
                reading.succeeded    = s3Metrics.succeeded.load(std::memory_order_relaxed);
                reading.clientErrors = s3Metrics.clientErrors.load(std::memory_order_relaxed);
                reading.serverErrors = s3Metrics.serverErrors.load(std::memory_order_relaxed);
                reading.shed         = s3Metrics.shed.load(std::memory_order_relaxed);
                reading.bytesIn      = s3Metrics.bytesIn.load(std::memory_order_relaxed);
                reading.bytesOut     = s3Metrics.bytesOut.load(std::memory_order_relaxed);
                reading.cacheHits    = cacheStats.hits;
                reading.cacheMisses  = cacheStats.misses;

                reading.objects       = stats.usage.objects;
                reading.storedBytes   = stats.usage.bytes;
                reading.residentBytes = residentBytes();
                reading.cacheBytes    = cacheStats.bytes;
                reading.ioQueued      = ioStats.queued;
                reading.ioActive      = ioStats.active;
                reading.connections =
                    static_cast<std::uint64_t>(std::max<std::int64_t>(0, connectionCount));

                state->history.record(reading);
            } catch (const std::exception& error) {
                log::debug("metrics sample skipped: ", error.what());
            }
        });
        if (!accepted) log::debug("metrics sample skipped: the io queue is saturated");
    });
}

}  // namespace monobucket
