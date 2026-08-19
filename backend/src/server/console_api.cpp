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

#include "cache/cache_provider.hpp"
#include "core/config.hpp"
#include "core/identity.hpp"
#include "core/io_executor.hpp"
#include "core/credentials.hpp"
#include "core/logging.hpp"
#include "core/password.hpp"
#include "monobucket/version.hpp"
#include "s3/cors.hpp"
#include "s3/bucket_policy.hpp"
#include "s3/handlers.hpp"
#include "s3/metrics.hpp"
#include "s3/s3_error.hpp"
#include "s3/sigv4.hpp"
#include "s3/uri.hpp"
#include "server/console_session.hpp"
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

/// Sampling cadence and retention for the console graphs: 5s × 240 = 20 minutes
/// of history for roughly 30 KB of ring. Longer windows are Prometheus's job.
constexpr double      kSampleIntervalSeconds = 5.0;
constexpr std::size_t kSampleCapacity        = 240;

/// S3's own ceiling on a presigned URL's lifetime. Repeated here rather than
/// reached for through sigv4 because the console rejects an over-long request
/// before it reaches an I/O thread.
constexpr std::int64_t kMaxPresignSeconds = 604800;

/// A label, not a document. Bounded so a description cannot be used to store
/// arbitrary bulk in a record the S3 hot path reads.
constexpr std::size_t kMaxDescriptionLength = 200;

/// The console tells us which name the browser reached the deployment by,
/// because `config.host` is normally 0.0.0.0 and knows no better. That string is
/// signed and handed back inside a URL, so it is held to what a host and port
/// can actually contain — not to stop an attack (the caller already holds a
/// console session) but so a mistake surfaces as a 400 here instead of as a
/// link that fails somewhere else.
bool plausibleHost(const std::string& host) {
    if (host.empty() || host.size() > 255) return false;
    return std::all_of(host.begin(), host.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '.' || ch == '-' || ch == ':' || ch == '[' ||
               ch == ']';
    });
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

/// Decides whether the session cookie carries `Secure`.
///
/// `Secure` tells the browser never to send the cookie over plain HTTP, which
/// is right whenever there is TLS anywhere in front of the console and wrong
/// the moment there is not: a cookie the browser accepts and then refuses to
/// send back presents as a login that succeeds and lands on the login page
/// again. Auto reads the request rather than guessing.
bool secureCookieFor(const Config& config, const HttpRequestPtr& req) {
    switch (config.consoleCookieSecure) {
        case Config::CookieSecurity::Always: return true;
        case Config::CookieSecurity::Never:  return false;
        case Config::CookieSecurity::Auto:   break;
    }

    if (req->isOnSecureConnection()) return true;

    // Set by a terminating proxy. Trusting it unverified is safe in this one
    // direction: a forged header can only ask the browser to be stricter with
    // a cookie it is already being handed.
    std::string_view forwarded = req->getHeader("x-forwarded-proto");
    const std::size_t comma    = forwarded.find(',');
    if (comma != std::string_view::npos) forwarded = forwarded.substr(0, comma);
    return forwarded == "https";
}

/// One place that builds the session cookie, so a flag cannot be set on the
/// login path and forgotten on the logout path — where dropping `Secure` would
/// leave the browser holding a cookie the clear was never applied to.
drogon::Cookie sessionCookie(std::string value, std::int64_t maxAge, bool secure) {
    drogon::Cookie cookie(kSessionCookie, std::move(value));
    cookie.setHttpOnly(true);
    cookie.setPath("/");
    cookie.setMaxAge(maxAge);
    cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
    cookie.setSecure(secure);
    return cookie;
}

/// Everything the handlers share, kept alive by the closures Drogon holds.
struct ConsoleState {
    SessionStore    sessions;
    LoginThrottle   throttle;
    MetricsHistory  history{kSampleCapacity, static_cast<std::uint32_t>(kSampleIntervalSeconds)};
};

bool onConsoleListener(const HttpRequestPtr& req, const Config& config) {
    return config.consoleEnabled && req->getLocalAddr().toPort() == config.consolePort;
}

nlohmann::json toJson(const BucketRecord& bucket, const BucketCapacity& capacity) {
    return {{"name", bucket.name},
            {"createdAt", toIso8601(bucket.createdAt)},
            {"createdAtMs", bucket.createdAt},
            {"publicRead", bucket.publicRead},
            {"publicList", bucket.publicList},
            {"hasPolicy", !bucket.policy.empty()},
            {"corsRules", bucket.cors.size()},
            // null rather than the level it currently resolves to: the bucket
            // is following the server, and rendering today's answer would make
            // a form that saves it back pin the bucket to it by accident.
            {"durability", bucket.durability
                               ? nlohmann::json(std::string(toString(*bucket.durability)))
                               : nlohmann::json(nullptr)},
            // Zero means unlimited, and the console renders it as such. A
            // separate boolean would be a second thing to keep in step with the
            // number it describes.
            {"quotaBytes", bucket.quotaBytes},
            {"usedBytes", capacity.usedBytes},
            {"pendingBytes", capacity.pendingBytes},
            // null for an unlimited bucket rather than a large number: there is
            // no remainder to report, and any figure invented here would be
            // rendered as a progress bar that means nothing.
            {"remainingBytes", capacity.remainingBytes()
                                   ? nlohmann::json(*capacity.remainingBytes())
                                   : nlohmann::json(nullptr)}};
}

nlohmann::json toJson(const InstanceCapacity& capacity) {
    return {{"allocatableBytes", capacity.allocatableBytes},
            {"allocatedBytes", capacity.allocatedBytes},
            {"remainingBytes", capacity.remainingBytes()},
            {"usedBytes", capacity.usedBytes},
            // Named so the console can say that the remainder is an upper
            // bound: a bucket with no allocation can consume capacity that
            // nothing has reserved against it.
            {"unlimitedBuckets", capacity.unlimitedBuckets}};
}

nlohmann::json toJson(const UserRecord& user) {
    // No password field of any kind — not the verifier, not its parameters.
    // The console never has a reason to see one, and a field that is only ever
    // ignored is a field a later handler can start rendering by accident.
    return {{"username", user.username},
            {"role", std::string(toString(user.role))},
            {"disabled", user.disabled},
            {"createdAt", toIso8601(user.createdAt)},
            {"createdAtMs", user.createdAt},
            {"updatedAt", toIso8601(user.updatedAt)},
            {"updatedAtMs", user.updatedAt},
            {"passwordChangedAt", user.passwordChangedAt > 0
                                      ? nlohmann::json(toIso8601(user.passwordChangedAt))
                                      : nlohmann::json(nullptr)},
            {"passwordChangedAtMs", user.passwordChangedAt}};
}

nlohmann::json toJson(const AuditRecord& entry) {
    return {{"sequence", entry.sequence},
            {"at", toIso8601(entry.atMs)},
            {"atMs", entry.atMs},
            {"actor", entry.actor},
            {"action", entry.action},
            {"target", entry.target},
            {"allowed", entry.allowed},
            {"detail", entry.detail}};
}

/// The permission list a role holds, as the console renders it.
nlohmann::json permissionsJson(Role role) {
    nlohmann::json out = nlohmann::json::array();
    for (const Permission permission : permissionsFor(role)) {
        out.push_back(std::string(toString(permission)));
    }
    return out;
}

/// The one refusal shape for "signed in, but not allowed".
///
/// Identical for every route and every role, and it names the permission rather
/// than the role: an operator told "you need user:write" knows what to ask for,
/// whereas one told "administrators only" has learned something about the role
/// list instead of about their own request.
HttpResponsePtr forbidden(Permission permission) {
    return jsonResponse({{"error", "this account is not permitted to " +
                                       std::string(toString(permission))},
                         {"requiredPermission", std::string(toString(permission))}},
                        drogon::k403Forbidden);
}

/// A credential as the console may see it — which is everything except the
/// secret. The secret is written into the create and rotate responses by hand,
/// at the one moment it is allowed to travel, so that no later caller can
/// produce it by accident through this function.
nlohmann::json toJson(const AccessKeyRecord& key) {
    return {{"accessKeyId", key.accessKeyId},
            {"description", key.description},
            {"owner", key.owner},
            {"createdAt", toIso8601(key.createdAt)},
            {"createdAtMs", key.createdAt},
            {"rotatedAt", key.rotatedAt > 0 ? nlohmann::json(toIso8601(key.rotatedAt))
                                            : nlohmann::json(nullptr)},
            {"rotatedAtMs", key.rotatedAt}};
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
        {"s3Port", "MONOBUCKET_PORT"},
        {"consolePort", "MONOBUCKET_CONSOLE_PORT"},
        {"consoleEnabled", "MONOBUCKET_CONSOLE_ENABLED"},
        {"dataDir", "MONOBUCKET_DATA_DIR"},
        {"region", "MONOBUCKET_REGION"},
        {"s3Domain", "MONOBUCKET_S3_DOMAIN"},
        {"s3PublicUrl", "MONOBUCKET_S3_PUBLIC_URL"},
        {"allocatableBytes", "MONOBUCKET_ALLOCATABLE_BYTES"},
        {"capacityReservePercent", "MONOBUCKET_CAPACITY_RESERVE_PERCENT"},
        {"defaultBucketQuotaBytes", "MONOBUCKET_DEFAULT_BUCKET_QUOTA_BYTES"},
        {"durability", "MONOBUCKET_DURABILITY"},
        {"metadataMemoryBytes", "MONOBUCKET_METADATA_MEMORY_BYTES"},
        {"metadataMaxOpenFiles", "MONOBUCKET_METADATA_MAX_OPEN_FILES"},
        {"reclaimGraceSeconds", "MONOBUCKET_RECLAIM_GRACE_SECONDS"},
        {"reclaimIntervalSeconds", "MONOBUCKET_RECLAIM_INTERVAL_SECONDS"},
        {"multipartExpiryHours", "MONOBUCKET_MULTIPART_EXPIRY_HOURS"},
        {"ioThreads", "MONOBUCKET_IO_THREADS"},
        {"ioQueueLimit", "MONOBUCKET_IO_QUEUE_LIMIT"},
        {"rootAccessKey", "MONOBUCKET_ROOT_ACCESS_KEY"},
        {"rootSecretKey", "MONOBUCKET_ROOT_SECRET_KEY"},
        {"adminUsername", "MONOBUCKET_ADMIN_USERNAME"},
        {"workerThreads", "MONOBUCKET_WORKER_THREADS"},
        {"maxBodyBytes", "MONOBUCKET_MAX_BODY_BYTES"},
        {"maxMemoryBodyBytes", "MONOBUCKET_MAX_MEMORY_BODY_BYTES"},
        {"maxUploadCeilingBytes", "MONOBUCKET_MAX_UPLOAD_CEILING_BYTES"},
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
            {"cacheControl", object.content.cacheControl},
            {"contentDisposition", object.content.contentDisposition},
            {"contentEncoding", object.content.contentEncoding},
            {"contentLanguage", object.content.contentLanguage},
            {"expires", object.content.expires},
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
        case StorageErrorCode::QuotaExceeded:
        case StorageErrorCode::QuotaBelowUsage:
            // 409, not 507: the request conflicts with what the bucket already
            // holds, and the operator's fix is to change the number or delete
            // objects — neither of which is "come back later".
            return drogon::k409Conflict;
        case StorageErrorCode::InsufficientCapacity:
            return drogon::k507InsufficientStorage;
        case StorageErrorCode::ObjectTooLarge:
            return drogon::k413RequestEntityTooLarge;
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

    // Writes one security event.
    //
    // Posted to an I/O thread because the log is a RocksDB record and the
    // callers are on the event loop. A saturated queue drops the entry rather
    // than blocking or failing the request it describes: the log is a record of
    // what happened, and refusing a legitimate sign-in because the log was busy
    // would make it a participant instead. Losses are bounded by the same queue
    // limit that sheds S3 load, and the log says so by being a ring.
    const auto audit = [&storage, &io](std::string actor, std::string action, std::string target,
                                       bool allowed, std::string detail) {
        AuditRecord entry;
        entry.atMs    = nowMs();
        entry.actor   = std::move(actor);
        entry.action  = std::move(action);
        entry.target  = std::move(target);
        entry.allowed = allowed;
        entry.detail  = std::move(detail);

        const auto recorded = entry.action;
        const bool accepted = io.post([&storage, entry = std::move(entry)]() {
            try {
                storage.appendAudit(entry);
            } catch (const std::exception& error) {
                log::warn("could not record the audit entry '", entry.action, "': ", error.what());
            }
        });
        // Debug, not warn: a full queue means the process is shedding load, and
        // a line per dropped entry would add to what saturated it.
        if (!accepted) log::debug("audit entry '", recorded, "' dropped: I/O queue full");
    };

    // Every console route shares the same four gates, in this order: it must
    // have arrived on the console listener, it must carry a live session, that
    // session's role must hold the permission the route names, and whatever it
    // does to storage must happen on an I/O thread.
    //
    // The permission is a parameter rather than something the handler checks
    // for itself, so that "which routes are protected" is answerable by reading
    // the registrations rather than by reading every handler body. A route that
    // forgets it does not compile.
    const auto guard = [&config, state, audit](
                           const HttpRequestPtr& req, const ResponseCallback& callback,
                           Permission                                      permission,
                           const std::function<void(const Principal&)>& handler) {
        if (!onConsoleListener(req, config)) {
            callback(errorJson("not found", drogon::k404NotFound));
            return;
        }
        const auto principal = state->sessions.resolve(req->getCookie(kSessionCookie));
        if (!principal) {
            callback(errorJson("not signed in", drogon::k401Unauthorized));
            return;
        }
        if (!allows(principal->role, permission)) {
            // Recorded, because a refusal is the half of the log worth having:
            // a successful action by someone entitled to it is ordinary, and an
            // attempt by someone who is not is the thing an operator is looking
            // for when they open this page.
            audit(principal->username, "authz.denied", req->getPath(), false,
                  std::string(toString(permission)) + " required, " +
                      std::string(toString(principal->role)) + " has it not");
            callback(forbidden(permission));
            return;
        }
        handler(*principal);
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

    // Username and password, never an S3 access key. The two were the same
    // thing until credentials became separable; they are not the same thing,
    // and the console accepting a storage credential meant the only way to lock
    // a person out was to break every program at the same time.
    app.registerHandler(
        "/_mb/api/login",
        [&config, &io, &storage, state, audit](const HttpRequestPtr& req,
                                               ResponseCallback&& callback) {
            if (!onConsoleListener(req, config)) {
                callback(errorJson("not found", drogon::k404NotFound));
                return;
            }
            if (state->throttle.blocked()) {
                auto resp = errorJson("too many failed attempts, try again shortly",
                                      drogon::k429TooManyRequests);
                resp->addHeader("Retry-After", std::to_string(kLoginWindowSeconds));
                callback(resp);
                return;
            }

            const auto body = nlohmann::json::parse(req->getBody(), nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
                callback(errorJson("expected a JSON object", drogon::k400BadRequest));
                return;
            }

            const auto username = body.value("username", std::string{});
            const auto pass     = body.value("password", std::string{});
            const bool secure   = secureCookieFor(config, req);

            // Posted rather than run here for two reasons that happen to
            // coincide: it reads the store, and the verifier is deliberately
            // expensive — several hundred milliseconds of PBKDF2 on the event
            // loop would stall every other console request behind one login.
            const bool accepted =
                io.post([&storage, state, audit, callback, username, pass, secure]() {
                    try {
                        // A name that could never have been stored is not
                        // looked up — but it is still verified against the
                        // dummy below, so refusing it costs exactly what
                        // refusing a real name does.
                        const auto user = isValidUsername(username)
                                              ? storage.getUser(username)
                                              : std::optional<UserRecord>{};

                        // The verifier runs even when the name is wrong,
                        // against a throwaway record that costs the same.
                        // Returning early would make response time answer the
                        // question the error text refuses to: whether that
                        // account exists.
                        const bool passOk = password::verify(
                            pass, user ? user->passwordHash : password::dummyHash());

                        // A disabled account fails here rather than earlier, so
                        // that "disabled" and "wrong password" are the same
                        // answer at the same cost. It is still recorded
                        // separately, because the log is for the operator and
                        // the response is for whoever is knocking.
                        if (!user || !passOk || user->disabled) {
                            state->throttle.recordFailure();
                            audit(username, "session.denied", username, false,
                                  !user      ? "no such user"
                                  : !passOk  ? "wrong password"
                                             : "account disabled");
                            // One message for every way this can fail. "No such
                            // user" and "wrong password" are two answers to a
                            // question nobody signing in legitimately has to
                            // ask.
                            callback(errorJson("invalid username or password",
                                               drogon::k401Unauthorized));
                            return;
                        }

                        state->throttle.recordSuccess();
                        audit(user->username, "session.open", user->username, true,
                              std::string(toString(user->role)));

                        auto resp = jsonResponse({{"username", user->username},
                                                  {"role", std::string(toString(user->role))},
                                                  {"permissions", permissionsJson(user->role)},
                                                  {"expiresInSeconds", kSessionTtlSeconds}});
                        resp->addCookie(
                            sessionCookie(state->sessions.open(user->username, user->role),
                                          kSessionTtlSeconds, secure));
                        callback(resp);
                    } catch (const std::exception& error) {
                        log::error("console login: ", error.what());
                        callback(errorJson("internal error", drogon::k500InternalServerError));
                    }
                });
            if (!accepted) {
                callback(errorJson("the storage queue is saturated",
                                   drogon::k503ServiceUnavailable));
            }
        },
        {drogon::Post});

    app.registerHandler(
        "/_mb/api/logout",
        [&config, state, audit](const HttpRequestPtr& req, ResponseCallback&& callback) {
            if (!onConsoleListener(req, config)) {
                callback(errorJson("not found", drogon::k404NotFound));
                return;
            }
            // The token is dropped server-side first. Clearing the cookie is
            // the browser's half and cannot be relied on: a copy of the token
            // taken before sign-out has to stop working regardless of what the
            // client does with its jar.
            const auto principal = state->sessions.resolve(req->getCookie(kSessionCookie));
            state->sessions.close(req->getCookie(kSessionCookie));
            if (principal) {
                audit(principal->username, "session.close", principal->username, true, "");
            }

            auto resp = jsonResponse({{"signedOut", true}});
            resp->addCookie(sessionCookie("", 0, secureCookieFor(config, req)));
            callback(resp);
        },
        {drogon::Post});

    // The one route that answers 200 when signed out: the SPA asks it on boot
    // to decide between the dashboard and the login form, and a 401 there is an
    // answer rather than an error.
    app.registerHandler(
        "/_mb/api/session",
        [&config, &storage, state](const HttpRequestPtr& req, ResponseCallback&& callback) {
            if (!onConsoleListener(req, config)) {
                callback(errorJson("not found", drogon::k404NotFound));
                return;
            }
            const auto principal = state->sessions.resolve(req->getCookie(kSessionCookie));
            // The S3 port and domain travel with the session because the console
            // is served from a different port and cannot infer them. The host is
            // deliberately not included: `config.host` is usually 0.0.0.0, and
            // the name the browser used to reach us is the only one known to
            // work — so the client supplies that half.
            //
            // The permission list travels too, so the console can leave out
            // what it must not offer. That is presentation, never enforcement:
            // every route re-decides for itself, and a browser that lies about
            // this list gets a 403 rather than a page it was not entitled to.
            callback(jsonResponse(
                {{"authenticated", principal.has_value()},
                 {"username", principal ? principal->username : std::string{}},
                 {"role", std::string(toString(principal ? principal->role : Role::ReadOnly))},
                 {"permissions",
                  principal ? permissionsJson(principal->role) : nlohmann::json::array()},
                 {"usingDefaultCredentials", config.usingDefaultCredentials()},
                 {"s3Port", config.s3Port},
                 {"s3Domain", config.s3Domain},
                 // Empty unless an operator stated it. The console falls back to
                 // its own hostname and the S3 port, which is right for a direct
                 // deployment and wrong behind any proxy.
                 {"s3PublicUrl", config.s3PublicUrl},
                 // The effective upload limit, so the file picker can refuse an
                 // oversized selection without a round trip. An atomic read, not
                 // a store lookup. A tab left open across a change is held to
                 // the figure it was given until it reloads, which is why the
                 // backend checks again and is the one that decides.
                 {"maxUploadBytes", storage.maxUploadBytes()},
                 {"version", version::kVersion}}));
        },
        {drogon::Get});

    // --- S3 credentials ----------------------------------------------------

    // Issuing and revoking the keys the S3 listener verifies. A session is
    // required to reach any of it, and none of it grants console access in
    // return: the two directions stay independent, which is the point.

    app.registerHandler(
        "/_mb/api/credentials",
        [&storage, guard, offload, audit](const HttpRequestPtr& req, ResponseCallback&& callback) {
            // Reading the list and changing it are different permissions, so
            // the method decides which one this request is held to rather than
            // the route being pinned to the weaker of the two.
            //
            // `method()` rather than `getMethodString()`: the latter answers
            // with a `const char*`, so comparing it to a literal compares two
            // addresses and is quietly always false.
            const Permission needed = req->method() == drogon::Get
                                          ? Permission::CredentialRead
                                          : Permission::CredentialWrite;
            guard(req, callback, needed,
                  [&storage, req, callback, offload, audit](const Principal& principal) {
                const std::string method = req->getMethodString();

                if (method == "GET") {
                    // An administrator sees every key, because managing other
                    // people's credentials is what the role is for. Everyone
                    // else sees their own: a key list is a list of what exists
                    // to be attacked, and an operator has no use for the shape
                    // of somebody else's.
                    const bool all = allows(principal.role, Permission::UserRead);
                    offload(callback, [&storage, all, owner = principal.username]() -> HttpResponsePtr {
                        nlohmann::json keys = nlohmann::json::array();
                        for (const auto& key : storage.listAccessKeys()) {
                            if (all || key.owner == owner) keys.push_back(toJson(key));
                        }
                        return jsonResponse({{"credentials", std::move(keys)}});
                    });
                    return;
                }

                if (method == "POST") {
                    const auto body = nlohmann::json::parse(req->getBody(), nullptr, false);
                    // A description is optional; a body is not required at all.
                    if (!body.is_discarded() && !body.is_null() && !body.is_object()) {
                        callback(errorJson("expected a JSON object", drogon::k400BadRequest));
                        return;
                    }
                    std::string description =
                        body.is_object() ? body.value("description", std::string{}) : std::string{};
                    if (description.size() > kMaxDescriptionLength) {
                        callback(errorJson("the description is too long", drogon::k400BadRequest));
                        return;
                    }

                    offload(callback, [&storage, audit, description,
                                       owner = principal.username]() -> HttpResponsePtr {
                        AccessKeyRecord key;
                        key.accessKeyId = credentials::generateAccessKeyId();
                        key.secretKey   = credentials::generateSecretKey();
                        key.description = description;
                        key.createdAt   = nowMs();
                        // The issuer owns it, always — there is no field on the
                        // request for this. A key that could be minted in
                        // someone else's name would be a way to act as them
                        // while the log said otherwise.
                        key.owner       = owner;
                        storage.putAccessKey(key);

                        log::info("issued S3 access key ", key.accessKeyId, " for ", owner);
                        audit(owner, "credential.create", key.accessKeyId, true, description);

                        // The only response that carries a secret, and only
                        // because it is the only moment it can be carried: the
                        // console shows it once and the store is the sole other
                        // copy. Every later read of this record omits it.
                        nlohmann::json out = toJson(key);
                        out["secretKey"]   = key.secretKey;
                        return jsonResponse(out, drogon::k201Created);
                    });
                    return;
                }

                const std::string accessKeyId = req->getParameter("accessKeyId");
                if (accessKeyId.empty()) {
                    callback(errorJson("an access key id is required", drogon::k400BadRequest));
                    return;
                }
                const bool any = allows(principal.role, Permission::UserWrite);
                offload(callback, [&storage, audit, accessKeyId, any,
                                   actor = principal.username]() -> HttpResponsePtr {
                    const auto key = storage.getAccessKey(accessKeyId);
                    // The same answer whether it does not exist or belongs to
                    // somebody else. Telling an operator that a key id is real
                    // but not theirs turns this route into a way to confirm
                    // which ids exist.
                    if (!key || (!any && key->owner != actor)) {
                        return errorJson("no such access key", drogon::k404NotFound);
                    }
                    if (!storage.deleteAccessKey(accessKeyId)) {
                        return errorJson("no such access key", drogon::k404NotFound);
                    }
                    log::info("revoked S3 access key ", accessKeyId);
                    audit(actor, "credential.revoke", accessKeyId, true,
                          key->owner == actor ? "" : "owned by " + key->owner);
                    // Nothing to invalidate elsewhere: the router resolves the
                    // secret from the store on every signed request and holds
                    // no copy, so the next one already fails.
                    return jsonResponse({{"revoked", accessKeyId}});
                });
            });
        },
        {drogon::Get, drogon::Post, drogon::Delete});

    app.registerHandler(
        "/_mb/api/credentials/rotate",
        [&storage, guard, offload, audit](const HttpRequestPtr& req, ResponseCallback&& callback) {
            guard(req, callback, Permission::CredentialWrite,
                  [&storage, req, callback, offload, audit](const Principal& principal) {
                const auto body = nlohmann::json::parse(req->getBody(), nullptr, false);
                if (body.is_discarded() || !body.is_object()) {
                    callback(errorJson("expected a JSON object", drogon::k400BadRequest));
                    return;
                }
                const auto accessKeyId = body.value("accessKeyId", std::string{});
                if (accessKeyId.empty()) {
                    callback(errorJson("an access key id is required", drogon::k400BadRequest));
                    return;
                }

                const bool any = allows(principal.role, Permission::UserWrite);
                offload(callback, [&storage, audit, accessKeyId, any,
                                   actor = principal.username]() -> HttpResponsePtr {
                    auto key = storage.getAccessKey(accessKeyId);
                    if (!key || (!any && key->owner != actor)) {
                        return errorJson("no such access key", drogon::k404NotFound);
                    }

                    // The id survives and the secret does not. Rotation exists
                    // to make a leaked secret stop working, so it takes effect
                    // immediately and breaks every client still holding the old
                    // one — that is the operation, not a side effect of it.
                    key->secretKey = credentials::generateSecretKey();
                    key->rotatedAt = nowMs();
                    storage.putAccessKey(*key);

                    log::info("rotated the secret for S3 access key ", accessKeyId);
                    audit(actor, "credential.rotate", accessKeyId, true,
                          key->owner == actor ? "" : "owned by " + key->owner);

                    nlohmann::json out = toJson(*key);
                    out["secretKey"]   = key->secretKey;
                    return jsonResponse(out);
                });
            });
        },
        {drogon::Post});

    // --- Users -------------------------------------------------------------

    // People, as opposed to the programs the credentials above are for. Every
    // route here needs `user:read` or `user:write`, which only the
    // administrator role holds — an operator can manage buckets and their own
    // keys and cannot reach any of this.

    app.registerHandler(
        "/_mb/api/users",
        [state, &storage, guard, offload, audit](const HttpRequestPtr& req,
                                                 ResponseCallback&&    callback) {
            const Permission needed = req->method() == drogon::Get ? Permission::UserRead
                                                                      : Permission::UserWrite;
            guard(req, callback, needed,
                  [state, &storage, req, callback, offload, audit](const Principal& principal) {
                const std::string method = req->getMethodString();

                if (method == "GET") {
                    offload(callback, [&storage]() -> HttpResponsePtr {
                        nlohmann::json users = nlohmann::json::array();
                        for (const auto& user : storage.listUsers()) users.push_back(toJson(user));

                        // The role catalogue rides along with the list. The
                        // console needs it to render a picker, and shipping it
                        // from here means the picker cannot offer a role this
                        // build does not have.
                        nlohmann::json roles = nlohmann::json::array();
                        for (const Role role :
                             {Role::Administrator, Role::Operator, Role::ReadOnly}) {
                            roles.push_back({{"name", std::string(toString(role))},
                                             {"description", std::string(describe(role))},
                                             {"permissions", permissionsJson(role)}});
                        }
                        return jsonResponse(
                            {{"users", std::move(users)}, {"roles", std::move(roles)}});
                    });
                    return;
                }

                if (method == "DELETE") {
                    const std::string username = req->getParameter("username");
                    if (username.empty()) {
                        callback(errorJson("a username is required", drogon::k400BadRequest));
                        return;
                    }
                    if (username == principal.username) {
                        // Not a lockout rule — the last-administrator check
                        // below would catch that case anyway. It is that
                        // deleting the account you are signed in as leaves a
                        // live session naming a user that no longer exists,
                        // and that is a worse thing to have to reason about
                        // than a refusal.
                        callback(errorJson("an account cannot delete itself",
                                           drogon::k409Conflict));
                        return;
                    }

                    offload(callback, [state, &storage, audit, username,
                                       actor = principal.username]() -> HttpResponsePtr {
                        const auto user = storage.getUser(username);
                        if (!user) return errorJson("no such user", drogon::k404NotFound);

                        if (user->role == Role::Administrator && !user->disabled &&
                            storage.countEnabledAdministrators() <= 1) {
                            return errorJson("this is the last enabled administrator",
                                             drogon::k409Conflict);
                        }

                        // The keys go with the person. Leaving them behind
                        // would leave credentials that authorise as an identity
                        // the store can no longer describe — which the S3 path
                        // would then have to have an opinion about.
                        std::size_t revoked = 0;
                        for (const auto& key : storage.listAccessKeys()) {
                            if (key.owner != username) continue;
                            if (storage.deleteAccessKey(key.accessKeyId)) ++revoked;
                            audit(actor, "credential.revoke", key.accessKeyId, true,
                                  "owner '" + username + "' deleted");
                        }

                        storage.deleteUser(username);
                        const std::size_t closed = state->sessions.closeUser(username);

                        log::info("deleted console user '", username, "' (", revoked,
                                  " access keys revoked, ", closed, " sessions ended)");
                        audit(actor, "user.delete", username, true,
                              std::to_string(revoked) + " keys revoked");

                        return jsonResponse({{"deleted", username},
                                             {"revokedCredentials", revoked},
                                             {"endedSessions", closed}});
                    });
                    return;
                }

                const auto body = nlohmann::json::parse(req->getBody(), nullptr, false);
                if (body.is_discarded() || !body.is_object()) {
                    callback(errorJson("expected a JSON object", drogon::k400BadRequest));
                    return;
                }
                const auto username = body.value("username", std::string{});
                if (!isValidUsername(username)) {
                    callback(errorJson("a username must be 1-64 characters of letters, digits, "
                                       "dot, underscore or hyphen, starting with a letter or "
                                       "digit",
                                       drogon::k400BadRequest));
                    return;
                }

                if (method == "POST") {
                    const auto secret = body.value("password", std::string{});
                    if (secret.size() < password::kMinimumLength) {
                        callback(errorJson("a password must be at least " +
                                               std::to_string(password::kMinimumLength) +
                                               " characters",
                                           drogon::k400BadRequest));
                        return;
                    }
                    const auto role = parseRole(body.value("role", std::string{}));
                    if (!role) {
                        callback(errorJson("a role is required and must be one of administrator, "
                                           "operator, readonly",
                                           drogon::k400BadRequest));
                        return;
                    }

                    offload(callback, [&storage, audit, username, secret, role = *role,
                                       actor = principal.username]() -> HttpResponsePtr {
                        if (storage.getUser(username)) {
                            return errorJson("that username is taken", drogon::k409Conflict);
                        }

                        UserRecord user;
                        user.username          = username;
                        user.passwordHash      = password::hash(secret);
                        user.role              = role;
                        user.createdAt         = nowMs();
                        user.updatedAt         = user.createdAt;
                        user.passwordChangedAt = user.createdAt;
                        storage.putUser(user);

                        log::info("created console user '", username, "' as ", toString(role));
                        audit(actor, "user.create", username, true, std::string(toString(role)));
                        return jsonResponse(toJson(user), drogon::k201Created);
                    });
                    return;
                }

                // PATCH: role and status, and nothing else. A password is
                // changed through its own route, because the two have different
                // rules about who may do it and what has to be presented first.
                const bool wantsRole     = body.contains("role") && !body.at("role").is_null();
                const bool wantsDisabled = body.contains("disabled") &&
                                           !body.at("disabled").is_null();
                if (!wantsRole && !wantsDisabled) {
                    callback(errorJson("nothing to change", drogon::k400BadRequest));
                    return;
                }

                std::optional<Role> role;
                if (wantsRole) {
                    if (!body.at("role").is_string()) {
                        callback(errorJson("role must be a string", drogon::k400BadRequest));
                        return;
                    }
                    role = parseRole(body.at("role").get<std::string>());
                    if (!role) {
                        callback(errorJson("a role must be one of administrator, operator, "
                                           "readonly",
                                           drogon::k400BadRequest));
                        return;
                    }
                }

                std::optional<bool> disabled;
                if (wantsDisabled) {
                    if (!body.at("disabled").is_boolean()) {
                        callback(errorJson("disabled must be true or false",
                                           drogon::k400BadRequest));
                        return;
                    }
                    disabled = body.at("disabled").get<bool>();
                }

                offload(callback, [state, &storage, audit, username, role, disabled,
                                   actor = principal.username]() -> HttpResponsePtr {
                    auto user = storage.getUser(username);
                    if (!user) return errorJson("no such user", drogon::k404NotFound);

                    const bool wasLastAdmin = user->role == Role::Administrator &&
                                              !user->disabled &&
                                              storage.countEnabledAdministrators() <= 1;
                    const bool losesAdmin =
                        (role && *role != Role::Administrator) || (disabled && *disabled);
                    if (wasLastAdmin && losesAdmin) {
                        // The check that keeps a console reachable. Demoting or
                        // disabling the only administrator left is not an
                        // operation anybody wants the result of, and it cannot
                        // be undone from a console nobody can now sign in to.
                        return errorJson("this is the last enabled administrator",
                                         drogon::k409Conflict);
                    }

                    std::string changes;
                    if (role && *role != user->role) {
                        changes = "role " + std::string(toString(user->role)) + " -> " +
                                  std::string(toString(*role));
                        user->role = *role;
                    }
                    if (disabled && *disabled != user->disabled) {
                        if (!changes.empty()) changes += ", ";
                        changes += *disabled ? "disabled" : "enabled";
                        user->disabled = *disabled;
                    }
                    if (changes.empty()) return jsonResponse(toJson(*user));

                    user->updatedAt = nowMs();
                    storage.putUser(*user);

                    // A session carries a copy of the role, so the copy has to
                    // go. Disabling takes effect on the next S3 request by
                    // itself — the router reads the owner's record every time —
                    // but an open console tab would otherwise keep the
                    // authority it was handed at sign-in until the tab was
                    // closed.
                    const std::size_t closed = state->sessions.closeUser(username);

                    log::info("updated console user '", username, "': ", changes, " (", closed,
                              " sessions ended)");
                    audit(actor, "user.update", username, true, changes);

                    nlohmann::json out = toJson(*user);
                    out["endedSessions"] = closed;
                    return jsonResponse(out);
                });
            });
        },
        {drogon::Get, drogon::Post, drogon::Patch, drogon::Delete});

    // Two operations wearing one route, told apart by whose password it is.
    // Changing your own needs the current one and no permission; resetting
    // somebody else's needs `user:write` and not their current password —
    // an administrator who had to know it could not do the one thing a reset
    // exists for.
    app.registerHandler(
        "/_mb/api/users/password",
        [&config, state, &storage, &io, audit](const HttpRequestPtr& req,
                                               ResponseCallback&&    callback) {
            if (!onConsoleListener(req, config)) {
                callback(errorJson("not found", drogon::k404NotFound));
                return;
            }
            const auto principal = state->sessions.resolve(req->getCookie(kSessionCookie));
            if (!principal) {
                callback(errorJson("not signed in", drogon::k401Unauthorized));
                return;
            }

            const auto body = nlohmann::json::parse(req->getBody(), nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
                callback(errorJson("expected a JSON object", drogon::k400BadRequest));
                return;
            }

            const auto target =
                body.value("username", std::string{}).empty()
                    ? principal->username
                    : body.value("username", std::string{});
            const bool self       = target == principal->username;
            const auto current    = body.value("currentPassword", std::string{});
            const auto next       = body.value("newPassword", std::string{});
            const bool secureFlag = secureCookieFor(config, req);

            if (!self && !allows(principal->role, Permission::UserWrite)) {
                audit(principal->username, "authz.denied", req->getPath(), false,
                      "user:write required to reset another account's password");
                callback(forbidden(Permission::UserWrite));
                return;
            }
            if (next.size() < password::kMinimumLength) {
                callback(errorJson("a password must be at least " +
                                       std::to_string(password::kMinimumLength) + " characters",
                                   drogon::k400BadRequest));
                return;
            }
            if (self && current.empty()) {
                callback(errorJson("the current password is required", drogon::k400BadRequest));
                return;
            }

            // Posted for the same reason login is: two PBKDF2 rounds on the
            // event loop would stall every other console request behind them.
            const bool accepted = io.post([state, &storage, audit, callback, target, self, current,
                                           next, secureFlag, actor = principal->username]() {
                try {
                    auto user = storage.getUser(target);
                    if (!user) {
                        callback(errorJson("no such user", drogon::k404NotFound));
                        return;
                    }
                    if (self && !password::verify(current, user->passwordHash)) {
                        audit(actor, "user.password", target, false, "wrong current password");
                        callback(errorJson("the current password is wrong",
                                           drogon::k401Unauthorized));
                        return;
                    }

                    user->passwordHash      = password::hash(next);
                    user->updatedAt         = nowMs();
                    user->passwordChangedAt = user->updatedAt;
                    storage.putUser(*user);

                    // Every session for that account ends, including this one.
                    // A password change whose point is that a copy of the old
                    // one is loose has to invalidate whatever that copy was
                    // used to open.
                    const std::size_t closed = state->sessions.closeUser(target);
                    audit(actor, self ? "user.password" : "user.password.reset", target, true,
                          std::to_string(closed) + " sessions ended");
                    log::info("password changed for console user '", target, "' by '", actor,
                              "' (", closed, " sessions ended)");

                    auto resp = jsonResponse({{"username", target}, {"endedSessions", closed}});
                    if (self) {
                        // The caller just invalidated their own cookie. Handing
                        // back a fresh one keeps a password change from
                        // presenting as being thrown out of the console.
                        resp->addCookie(
                            sessionCookie(state->sessions.open(user->username, user->role),
                                          kSessionTtlSeconds, secureFlag));
                    }
                    callback(resp);
                } catch (const std::exception& error) {
                    log::error("console password change: ", error.what());
                    callback(errorJson("internal error", drogon::k500InternalServerError));
                }
            });
            if (!accepted) {
                callback(errorJson("the storage queue is saturated",
                                   drogon::k503ServiceUnavailable));
            }
        },
        {drogon::Post});

    // --- Audit log ---------------------------------------------------------

    app.registerHandler(
        "/_mb/api/audit",
        [&storage, guard, offload](const HttpRequestPtr& req, ResponseCallback&& callback) {
            guard(req, callback, Permission::AuditRead,
                  [&storage, req, callback, offload](const Principal&) {
                // Clamped rather than refused: a caller asking for more than
                // the ring holds wants all of it, and the ring is the bound.
                std::size_t limit = 200;
                if (const std::string requested = req->getParameter("limit");
                    !requested.empty()) {
                    try {
                        limit = std::min<std::size_t>(std::stoul(requested), kAuditCapacity);
                    } catch (const std::exception&) {
                        callback(errorJson("limit must be a number", drogon::k400BadRequest));
                        return;
                    }
                }

                offload(callback, [&storage, limit]() -> HttpResponsePtr {
                    nlohmann::json entries = nlohmann::json::array();
                    for (const auto& entry : storage.listAudit(limit)) {
                        entries.push_back(toJson(entry));
                    }
                    return jsonResponse(
                        {{"entries", std::move(entries)}, {"capacity", kAuditCapacity}});
                });
            });
        },
        {drogon::Get});

    // --- Overview ----------------------------------------------------------

    app.registerHandler(
        "/_mb/api/overview",
        [&config, &storage, &io, &cache, &s3Metrics, guard, offload](
            const HttpRequestPtr& req, ResponseCallback&& callback) {
            guard(req, callback, Permission::SettingsRead,
                  [&config, &storage, &io, &cache, &s3Metrics, callback,
                   offload](const Principal&) {
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
                        {"capacity", toJson(storage.capacity())},
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
            guard(req, callback, Permission::SettingsRead,
                  [state, callback](const Principal&) {
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
            // Reading the bucket list and changing it are different
            // permissions, decided by the method for the same reason the
            // credentials route decides it that way.
            const Permission needed = req->method() == drogon::Get ? Permission::BucketRead
                                                                      : Permission::BucketWrite;
            guard(req, callback, needed,
                  [&storage, req, callback, offload](const Principal&) {
                const std::string method = req->getMethodString();

                if (method == "GET") {
                    offload(callback, [&storage]() -> HttpResponsePtr {
                        nlohmann::json buckets = nlohmann::json::array();
                        for (const auto& bucket : storage.listBuckets()) {
                            buckets.push_back(
                                toJson(bucket, storage.bucketCapacity(bucket.name)));
                        }
                        // The instance figures travel with the list because the
                        // page that renders one renders the other: a bucket's
                        // allocation only means something beside what is left
                        // to allocate.
                        return jsonResponse({{"buckets", std::move(buckets)},
                                             {"capacity", toJson(storage.capacity())}});
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
                    if (!body.contains("quotaBytes") || !body["quotaBytes"].is_number_unsigned()) {
                        callback(errorJson(
                            "a storage allocation is required; send quotaBytes as a whole number "
                            "of bytes",
                            drogon::k400BadRequest));
                        return;
                    }
                    const auto quotaBytes = body["quotaBytes"].get<std::uint64_t>();
                    // Zero is unlimited over S3, where nothing can name a
                    // figure. Here something can, so zero is a mistake rather
                    // than a request — and a console that quietly created an
                    // unlimited bucket would defeat the point of asking.
                    if (quotaBytes == 0) {
                        callback(errorJson(
                            "a storage allocation must be greater than zero",
                            drogon::k400BadRequest));
                        return;
                    }
                    offload(callback, [&storage, name, quotaBytes]() -> HttpResponsePtr {
                        storage.createBucket(name, quotaBytes);
                        const auto record = storage.getBucket(name);
                        return jsonResponse(record ? toJson(*record, storage.bucketCapacity(name))
                                                   : nlohmann::json{{"name", name}},
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
            guard(req, callback, Permission::BucketWrite,
                  [&storage, &cache, req, callback, offload](const Principal&) {
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
                    return jsonResponse(toJson(*record, storage.bucketCapacity(name)));
                });
            });
        },
        {drogon::Post});

    // Changing an allocation is an administrator's decision, not a bucket
    // owner's: it moves capacity between buckets that other people are using,
    // and the instance total is the thing being divided up.
    app.registerHandler(
        "/_mb/api/buckets/quota",
        [&storage, guard, offload, audit](const HttpRequestPtr& req,
                                          ResponseCallback&& callback) {
            // Reading how capacity is divided is part of reading the server;
            // moving it between buckets is not.
            const Permission needed = req->method() == drogon::Get ? Permission::SettingsRead
                                                                   : Permission::CapacityWrite;
            guard(req, callback, needed,
                  [&storage, req, callback, offload, audit](const Principal& principal) {
                if (req->method() == drogon::Get) {
                    offload(callback, [&storage]() -> HttpResponsePtr {
                        nlohmann::json buckets = nlohmann::json::array();
                        for (const auto& [name, capacity] : storage.bucketCapacities()) {
                            buckets.push_back(
                                {{"name", name},
                                 {"quotaBytes", capacity.quotaBytes},
                                 {"usedBytes", capacity.usedBytes},
                                 {"pendingBytes", capacity.pendingBytes},
                                 {"remainingBytes", capacity.remainingBytes()
                                                        ? nlohmann::json(*capacity.remainingBytes())
                                                        : nlohmann::json(nullptr)}});
                        }
                        return jsonResponse(
                            {{"capacity", toJson(storage.capacity())},
                             {"defaultBucketQuotaBytes", storage.defaultBucketQuotaBytes()},
                             {"buckets", std::move(buckets)}});
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
                if (!body.contains("quotaBytes") || !body["quotaBytes"].is_number_unsigned()) {
                    callback(errorJson("quotaBytes must be a whole number of bytes",
                                       drogon::k400BadRequest));
                    return;
                }
                const auto quotaBytes = body["quotaBytes"].get<std::uint64_t>();

                offload(callback, [&storage, audit, name, quotaBytes,
                                   actor = principal.username]() -> HttpResponsePtr {
                    // Both refusals — below what is stored, or over what the
                    // instance has left — come back from the ledger as a
                    // StorageError and are turned into a status by statusFor.
                    // Duplicating the arithmetic here to phrase the message
                    // better would be a second answer that could disagree.
                    storage.setBucketQuota(name, quotaBytes);
                    audit(actor, "bucket.quota", name, true,
                          quotaBytes == 0 ? std::string("unlimited")
                                          : std::to_string(quotaBytes) + " bytes");

                    const auto record = storage.getBucket(name);
                    if (!record) return errorJson("no such bucket", drogon::k404NotFound);
                    return jsonResponse({{"bucket", toJson(*record, storage.bucketCapacity(name))},
                                         {"capacity", toJson(storage.capacity())}});
                });
            });
        },
        {drogon::Get, drogon::Post});

    // GET reads the rules, POST replaces them, DELETE turns CORS off. The same
    // three verbs the S3 API uses on ?cors, and the same storage call behind
    // them — the console is a second front door onto one feature, not a second
    // implementation of it.
    app.registerHandler(
        "/_mb/api/buckets/cors",
        [&storage, &cache, guard, offload](const HttpRequestPtr& req,
                                           ResponseCallback&& callback) {
            const Permission needed = req->method() == drogon::Get ? Permission::BucketRead
                                                                      : Permission::BucketWrite;
            guard(req, callback, needed,
                  [&storage, &cache, req, callback, offload](const Principal&) {
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
            const Permission needed = req->method() == drogon::Get ? Permission::BucketRead
                                                                      : Permission::BucketWrite;
            guard(req, callback, needed,
                  [&storage, &cache, req, callback, offload](const Principal&) {
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
                        s3::validateBucketPolicy(document, name);
                    } catch (const s3::S3Exception& error) {
                        callback(errorJson(error.what(), drogon::k400BadRequest));
                        return;
                    }
                }

                // Deleting the policy removes the access it granted; leaving
                // the flag set would keep a bucket public with nothing left in
                // the record to say why.
                const s3::AnonymousGrants grants =
                    document.empty() ? s3::AnonymousGrants{}
                                     : s3::analyseBucketPolicy(document, name).grants;

                offload(callback, [&storage, &cache, name, document, grants]()
                                      -> HttpResponsePtr {
                    storage.setBucketPolicy(name, document, grants.readObjects, grants.listBucket);
                    cache.del(s3::bucketCacheKey(name));

                    const auto record = storage.getBucket(name);
                    if (!record) return errorJson("no such bucket", drogon::k404NotFound);
                    return jsonResponse({{"bucket", name},
                                         {"policy", record->policy},
                                         {"publicRead", record->publicRead},
                                         {"publicList", record->publicList}});
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
            guard(req, callback, Permission::SettingsRead,
                  [&config, &cache, callback](const Principal&) {
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

    // The one figure on the settings panel that is stored rather than
    // configured, and therefore the one route that writes a setting.
    //
    // It is here and not under /_mb/api/config because that route is read-only
    // by construction — configuration is environment-only, and giving it a
    // POST arm would invite the next writable-looking value to be added to it
    // instead of being thought about.
    app.registerHandler(
        "/_mb/api/upload-limit",
        [&storage, guard, offload, audit](const HttpRequestPtr& req,
                                          ResponseCallback&& callback) {
            const Permission needed = req->method() == drogon::Get ? Permission::SettingsRead
                                                                   : Permission::SettingsWrite;
            guard(req, callback, needed,
                  [&storage, req, callback, offload, audit](const Principal& principal) {
                if (req->method() == drogon::Get) {
                    // Two atomic reads and a copy of an integer. No offload:
                    // this touches neither RocksDB nor the filesystem, and
                    // posting it to the I/O pool would put a load-shedding
                    // queue in front of a field read.
                    callback(jsonResponse({{"maxUploadBytes", storage.maxUploadBytes()},
                                           {"ceilingBytes", storage.maxUploadCeilingBytes()}}));
                    return;
                }

                const auto body = nlohmann::json::parse(req->getBody(), nullptr, false);
                if (body.is_discarded() || !body.is_object()) {
                    callback(errorJson("expected a JSON object", drogon::k400BadRequest));
                    return;
                }
                if (!body.contains("maxUploadBytes") ||
                    !body["maxUploadBytes"].is_number_unsigned()) {
                    callback(errorJson("maxUploadBytes must be a whole number of bytes",
                                       drogon::k400BadRequest));
                    return;
                }
                const auto bytes = body["maxUploadBytes"].get<std::uint64_t>();
                if (bytes == 0) {
                    // Refused here as well as in the engine so the console
                    // hears the reason rather than a 500: zero would read as
                    // "unlimited" everywhere else in this server, and it is
                    // not what a maximum upload size may mean.
                    callback(errorJson("the maximum upload size must be at least 1 byte",
                                       drogon::k400BadRequest));
                    return;
                }

                offload(callback, [&storage, audit, bytes,
                                   actor = principal.username]() -> HttpResponsePtr {
                    // Above the ceiling comes back from the engine as a
                    // StorageError and is turned into 413 by statusFor.
                    storage.setMaxUploadBytes(bytes);
                    audit(actor, "settings.upload-limit", "", true,
                          std::to_string(bytes) + " bytes");
                    return jsonResponse({{"maxUploadBytes", storage.maxUploadBytes()},
                                         {"ceilingBytes", storage.maxUploadCeilingBytes()}});
                });
            });
        },
        {drogon::Get, drogon::Post});

    // --- Objects -----------------------------------------------------------

    // Bucket and key travel as query parameters rather than path segments: an
    // object key may contain slashes, question marks and percent signs, and a
    // routing pattern that tries to carve one out of a path gets it wrong for
    // exactly the keys a file browser needs to show.
    app.registerHandler(
        "/_mb/api/objects",
        [&storage, &cache, guard, offload](const HttpRequestPtr& req,
                                           ResponseCallback&& callback) {
            // A listing reads; the DELETE this route also serves does not.
            const Permission needed = req->method() == drogon::Get ? Permission::ObjectRead
                                                                      : Permission::ObjectWrite;
            guard(req, callback, needed,
                  [&storage, &cache, req, callback, offload](const Principal&) {
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
            guard(req, callback, Permission::ObjectWrite,
                  [&config, &storage, &cache, req, callback, offload](const Principal&) {
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

                // The whole body is already resident (as a mapping, above the
                // in-memory threshold), so its size is exact and the refusal
                // costs the browser one response instead of a write it would
                // have thrown away. finishWrite checks the same figure again.
                if (req->getBody().size() > storage.maxUploadBytes()) {
                    callback(errorJson("that file is " + std::to_string(req->getBody().size()) +
                                           " bytes, over this instance's maximum upload size of " +
                                           std::to_string(storage.maxUploadBytes()) + " bytes",
                                       drogon::k413RequestEntityTooLarge));
                    return;
                }

                std::string contentType = std::string(req->getHeader("content-type"));
                if (contentType.empty()) contentType = "application/octet-stream";

                // The same five headers PutObject stores, read the same way, so
                // an object uploaded from the console is not a second-class one
                // that a CDN has to be told about separately. Validated here
                // rather than at the write, because the browser gets a message
                // it can show instead of a 500.
                ContentHeaders content;
                try {
                    content = s3::collectContentHeaders(req);
                } catch (const s3::S3Exception&) {
                    callback(errorJson("a Cache-Control, Content-Disposition, Content-Encoding, "
                                       "Content-Language or Expires header contains characters "
                                       "that cannot be stored",
                                       drogon::k400BadRequest));
                    return;
                }

                offload(callback, [&config, &storage, &cache, req, bucket, key, contentType,
                                   content]() -> HttpResponsePtr {
                    if (!storage.getBucket(bucket)) {
                        return errorJson("no such bucket", drogon::k404NotFound);
                    }

                    StorageEngine::PutRequest put;
                    put.bucket      = bucket;
                    put.key         = key;
                    put.contentType = contentType;
                    put.content     = content;

                    // `req` is kept alive by this closure, which is what keeps
                    // the body's mapping valid for the whole write.
                    const std::string_view payload = req->getBody();
                    const std::size_t chunk = std::max<std::size_t>(config.streamChunkBytes, 1);

                    // The whole body is already here, so the claim is exact and
                    // a bucket that is full refuses before anything is written
                    // into the payload tree.
                    auto reservation = storage.reserveSpace(bucket, payload.size());

                    BlobWriter writer = storage.beginWrite();
                    for (std::size_t at = 0; at < payload.size(); at += chunk) {
                        writer.write(payload.substr(at, chunk));
                    }

                    const ObjectRecord record =
                        storage.finishWrite(put, std::move(writer), std::move(reservation));

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
            guard(req, callback, Permission::ObjectRead,
                  [&storage, req, callback, offload](const Principal&) {
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
            guard(req, callback, Permission::ObjectRead,
                  [&config, &storage, req, callback, offload](const Principal&) {
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
