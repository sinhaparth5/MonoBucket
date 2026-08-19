#include "server/policy_reconcile.hpp"
#include "server/server.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <drogon/drogon.h>

#include "cache/cache_factory.hpp"
#include "core/identity.hpp"
#include "core/identity_migration.hpp"
#include "core/lifecycle.hpp"
#include "core/logging.hpp"
#include "core/password.hpp"
#include "monobucket/version.hpp"
#include "s3/router.hpp"
#include "server/console_api.hpp"
#include "server/system_routes.hpp"

namespace monobucket {
namespace {

const std::chrono::steady_clock::time_point kStartedAt = std::chrono::steady_clock::now();

/// Bounded so one sweep cannot monopolise an I/O thread; the next tick picks up
/// whatever is left.
constexpr std::size_t kReclaimBatch = 512;

/// Bounded for the same reason, and smaller: aborting an upload reads its parts
/// and writes a batch per upload, where reclaiming a payload is one unlink.
constexpr std::size_t kUploadSweepBatch = 64;

/// How often expired cache entries are swept. Not configurable: the budget is
/// held on insert regardless, so this only decides how long an entry nobody
/// reads again keeps occupying space it is no longer entitled to.
constexpr double kCacheSweepSeconds = 30.0;

trantor::Logger::LogLevel toTrantorLevel(const std::string& level) {
    if (level == "trace") return trantor::Logger::kTrace;
    if (level == "debug") return trantor::Logger::kDebug;
    if (level == "warn")  return trantor::Logger::kWarn;
    if (level == "error") return trantor::Logger::kError;
    return trantor::Logger::kInfo;
}

}  // namespace

double uptimeSeconds() noexcept {
    const auto elapsed = std::chrono::steady_clock::now() - kStartedAt;
    return std::chrono::duration<double>(elapsed).count();
}

Server::Server(Config config) : config_(std::move(config)) {}

Server::~Server() = default;

void Server::prepareDataDirectory() {
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::create_directories(config_.dataDir, ec);
    if (ec) {
        throw ConfigError("cannot create data directory '" + config_.dataDir + "': " + ec.message());
    }

    // Fail fast at startup rather than on the first PUT.
    const fs::path probe = fs::path(config_.dataDir) / ".monobucket-write-probe";
    {
        std::ofstream out(probe);
        if (!out) throw ConfigError("data directory '" + config_.dataDir + "' is not writable");
    }
    fs::remove(probe, ec);

    // The storage engine creates objects/ and tmp/ itself and RocksDB owns
    // meta/, but creating them here keeps the permission failure at startup
    // rather than on the first PUT.
    for (const char* sub : {"objects", "meta", "tmp"}) {
        fs::create_directories(fs::path(config_.dataDir) / sub, ec);
        if (ec) {
            throw ConfigError("cannot create '" + std::string(sub) + "' under the data directory: " +
                              ec.message());
        }
    }
}

void Server::openStorage() {
    storage_ = std::make_unique<StorageEngine>(StorageEngine::optionsFrom(config_));

    // Reconcile before the listeners open, so no request can observe a tree
    // that still contains the residue of an unclean stop.
    storage_->recover();

    io_ = std::make_unique<IoExecutor>(config_.ioThreads, config_.ioQueueLimit);

    // Reverse registration order: the executor stops first so no task is still
    // touching the engine when it flushes.
    Lifecycle::instance().onShutdown("storage", [this] {
        storage_->flush();
        log::debug("metadata flushed");
    });
    Lifecycle::instance().onShutdown("io-executor", [this] { io_->stop(); });
}

void Server::provisionAdministrator() {
    // An S3-only deployment has nothing to sign in to, so it is not held to
    // having an account. A password supplied anyway is still honoured — it
    // provisions the account ahead of the day the console is turned on.
    if (!config_.consoleEnabled && config_.adminPassword.empty()) return;

    migrateLegacyIdentities(*storage_);

    const std::size_t administrators = storage_->countEnabledAdministrators();

    if (config_.adminPassword.empty()) {
        if (administrators > 0) {
            const auto users = storage_->listUsers();
            log::info("console users loaded from the store: ", users.size(), " account",
                      users.size() == 1 ? "" : "s", ", ", administrators, " enabled administrator",
                      administrators == 1 ? "" : "s");
            return;
        }
        // Refusing to start is the point. The alternatives are a console with a
        // documented default password — which is a published credential on
        // every deployment that skipped the docs — or a console that runs with
        // no way in, which an operator discovers at the moment they need it.
        //
        // "No enabled administrator" and "no accounts at all" are the same
        // refusal, because they are the same problem: a console that cannot be
        // administered. Disabling the last administrator is refused at the
        // console, so reaching this with accounts present means the store was
        // edited from outside.
        throw ConfigError(
            "no enabled console administrator exists for this data directory. Set "
            "MONOBUCKET_ADMIN_PASSWORD_FILE (preferred) or MONOBUCKET_ADMIN_PASSWORD to at least " +
            std::to_string(password::kMinimumLength) +
            " characters and start again; MONOBUCKET_ADMIN_USERNAME chooses the name and defaults "
            "to 'admin'. Set MONOBUCKET_CONSOLE_ENABLED=false to run the S3 API without a console "
            "instead.");
    }

    if (!isValidUsername(config_.adminUsername)) {
        throw ConfigError("MONOBUCKET_ADMIN_USERNAME must be 1-64 characters of letters, digits, "
                          "dot, underscore or hyphen, starting with a letter or digit");
    }

    auto       existing = storage_->getUser(config_.adminUsername);
    UserRecord admin;
    admin.username     = config_.adminUsername;
    admin.passwordHash = password::hash(config_.adminPassword);
    // The environment always restores the account to a usable administrator.
    // This variable is the recovery path — a reset that left the account
    // disabled, or left it an operator, would recover nothing.
    admin.role              = Role::Administrator;
    admin.disabled          = false;
    admin.createdAt         = existing ? existing->createdAt : nowMs();
    admin.updatedAt         = nowMs();
    admin.passwordChangedAt = admin.updatedAt;
    storage_->putUser(admin);

    // The plaintext is dropped here rather than kept for the process lifetime.
    // Nothing after this point has a use for it, and a copy that outlives its
    // use is a copy that can be read out of a core dump.
    std::fill(config_.adminPassword.begin(), config_.adminPassword.end(), '\0');
    config_.adminPassword.clear();
    config_.adminPassword.shrink_to_fit();

    adoptOwnerlessAccessKeys(*storage_, admin.username);

    if (existing) {
        log::warn("console administrator reset to '", admin.username,
                  "' from the environment; unset the password variable once the new credentials "
                  "are confirmed so a restart cannot reset it again");
    } else {
        log::info("console administrator '", admin.username, "' provisioned");
    }
}

void Server::openCache() {
    cache_ = makeCache(config_);

    // Dropped before the storage engine flushes. Nothing in the cache is
    // authoritative — every entry is a copy of something already durable — so
    // shutdown discards it rather than trying to persist it.
    Lifecycle::instance().onShutdown("cache", [this] { cache_->clear(); });
}

void Server::configureFramework() {
    auto& app = drogon::app();

    app.setLogLevel(toTrantorLevel(config_.logLevel))
        .setLogPath("")  // stderr; container logs are the transport
        .setThreadNum(config_.workerThreads)
        .setIdleConnectionTimeout(config_.idleTimeoutSeconds)
        .setServerHeaderField(version::kServerHeader)
        .enableServerHeader(true)
        .enableDateHeader(true)
        .disableSession();

    // Response compression is off on purpose. Drogon would gzip a small object
    // body whose content type looks textual, which changes the bytes a client
    // receives from the bytes it stored — S3 never does that, and an SDK that
    // checks Content-Length or a checksum against the object would be right to
    // complain. Pre-compressed dashboard assets are the Phase 5 answer for the
    // console side.
    app.enableGzip(false).enableBrotli(false);

    // The two limits below are what keep resident memory flat: bodies larger
    // than maxMemoryBodyBytes are spooled to disk instead of buffered, and
    // anything past maxBodyBytes is rejected outright.
    app.setClientMaxBodySize(config_.maxBodyBytes)
        .setClientMaxMemoryBodySize(config_.maxMemoryBodyBytes)
        .setUploadPath((std::filesystem::path(config_.dataDir) / "tmp").string());

    app.addListener(config_.host, config_.s3Port);
    log::info("S3 API listening on ", config_.host, ':', config_.s3Port);

    if (config_.consoleEnabled) {
        app.addListener(config_.host, config_.consolePort);
        log::info("console listening on ", config_.host, ':', config_.consolePort);
    }
}

void Server::registerRoutes() {
    // Exact-path handlers first: Drogon matches those before any regex route,
    // so these stay reachable underneath the S3 catch-all below.
    registerSystemRoutes(config_, *storage_, *io_, *cache_, s3Metrics_);
    registerConsoleApi(config_, *storage_, *io_, *cache_, s3Metrics_);

    // One catch-all serves both listeners — the console assets included. Two
    // regex routes would shadow each other by registration order.
    s3::registerS3Routes(config_, *storage_, *io_, *cache_, s3Metrics_);
}

void Server::scheduleMaintenance() {
    // The cache holds its own budget on every insert, so this pass exists only
    // to collect entries that expired without anyone asking for them again.
    // It runs on the event loop rather than the I/O pool: it touches no file
    // descriptors, and it must not queue behind a slow disk.
    drogon::app().getLoop()->runEvery(kCacheSweepSeconds, [this] {
        const std::size_t dropped = cache_->evict(config_.cacheMaxBytes);
        if (dropped > 0) log::debug("cache: swept ", dropped, " expired entries");
    });

    if (config_.reclaimIntervalSeconds == 0) {
        log::info("background reclamation disabled");
        return;
    }

    // Deletions reclaim their own payload inline, so this only picks up what a
    // crash left behind. It runs on the I/O pool rather than the loop because
    // unlinking files blocks.
    //
    // Expiring abandoned multipart uploads rides the same tick rather than
    // taking a timer of its own: both are janitorial, both must run on the I/O
    // pool, and a second timer would only add a way for the two to overlap.
    // The upload sweep goes first — it turns parts into unreferenced payloads,
    // which the reclamation pass behind it is then free to collect on its own
    // schedule.
    drogon::app().getLoop()->runEvery(
        static_cast<double>(config_.reclaimIntervalSeconds), [this] {
            if (!io_->post([this] {
                    storage_->sweepExpiredUploads(kUploadSweepBatch);
                    storage_->reclaim(kReclaimBatch);
                })) {
                log::debug("skipped a reclamation pass: the io queue is saturated");
            }
        });
}

void Server::watchForShutdown() {
    // Drogon installs its own SIGTERM/SIGINT handlers inside run(), which would
    // overwrite anything we registered beforehand. Rather than race it, we hand
    // it the callbacks to invoke — quit() then unwinds run() and the shutdown
    // hooks flush state and close descriptors on the way out.
    drogon::app()
        .setTermSignalHandler([] {
            log::info("received SIGTERM, shutting down gracefully");
            drogon::app().quit();
        })
        .setIntSignalHandler([] {
            log::info("received SIGINT, shutting down gracefully");
            drogon::app().quit();
        });
}

int Server::run() {
    prepareDataDirectory();
    openStorage();
    provisionAdministrator();
    reconcileBucketPolicies(*storage_);
    openCache();
    configureFramework();
    registerRoutes();
    scheduleMaintenance();
    watchForShutdown();

    Lifecycle::instance().onShutdown("event-loop", [] {
        log::debug("event loop stopped");
    });

    log::info("MonoBucket ", version::kVersion, " ready");
    drogon::app().run();

    Lifecycle::instance().runShutdownHooks();
    log::info("shutdown complete");
    return 0;
}

}  // namespace monobucket
