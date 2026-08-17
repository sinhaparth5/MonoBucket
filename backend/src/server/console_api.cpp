#include "server/console_api.hpp"

#include <algorithm>
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
#include "s3/metrics.hpp"
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
            {"hasPolicy", !bucket.policy.empty()}};
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
            {"ioActive", sample.ioActive}};
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
                                        {"consolePort", config.consolePort}}},
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
        [&storage, guard, offload](const HttpRequestPtr& req, ResponseCallback&& callback) {
            guard(req, callback, [&storage, req, callback, offload](const std::string&) {
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
                offload(callback, [&storage, name, publicRead]() -> HttpResponsePtr {
                    storage.setBucketPublicRead(name, publicRead);
                    const auto record = storage.getBucket(name);
                    if (!record) {
                        return errorJson("no such bucket", drogon::k404NotFound);
                    }
                    return jsonResponse(toJson(*record));
                });
            });
        },
        {drogon::Post});

    // --- Objects -----------------------------------------------------------

    // Bucket and key travel as query parameters rather than path segments: an
    // object key may contain slashes, question marks and percent signs, and a
    // routing pattern that tries to carve one out of a path gets it wrong for
    // exactly the keys a file browser needs to show.
    app.registerHandler(
        "/_mb/api/objects",
        [&storage, guard, offload](const HttpRequestPtr& req, ResponseCallback&& callback) {
            guard(req, callback, [&storage, req, callback, offload](const std::string&) {
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
                    offload(callback, [&storage, bucket, key]() -> HttpResponsePtr {
                        if (!storage.deleteObject(bucket, key)) {
                            return errorJson("no such key", drogon::k404NotFound);
                        }
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

    // --- Sampler -----------------------------------------------------------

    // Reading storage stats touches RocksDB, so the tick posts rather than
    // reads. A saturated queue skips the sample instead of queueing behind
    // real work: a gap in a graph is cheaper than a slower PUT.
    drogon::app().getLoop()->runEvery(kSampleIntervalSeconds, [state, &storage, &io, &cache,
                                                               &s3Metrics] {
        const bool accepted = io.post([state, &storage, &io, &cache, &s3Metrics] {
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

                state->history.record(reading);
            } catch (const std::exception& error) {
                log::debug("metrics sample skipped: ", error.what());
            }
        });
        if (!accepted) log::debug("metrics sample skipped: the io queue is saturated");
    });
}

}  // namespace monobucket
