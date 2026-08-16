#include "server/server.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <drogon/drogon.h>

#include "core/lifecycle.hpp"
#include "core/logging.hpp"
#include "monobucket/version.hpp"
#include "server/system_routes.hpp"

namespace monobucket {
namespace {

const std::chrono::steady_clock::time_point kStartedAt = std::chrono::steady_clock::now();

/// Bounded so one sweep cannot monopolise an I/O thread; the next tick picks up
/// whatever is left.
constexpr std::size_t kReclaimBatch = 512;

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
    registerSystemRoutes(config_, *storage_, *io_);

    if (config_.consoleEnabled) {
        registerConsoleRoutes(config_);
    }

    // Phase 4 registers the S3 service/bucket/object routes here.
}

void Server::scheduleMaintenance() {
    if (config_.reclaimIntervalSeconds == 0) {
        log::info("background reclamation disabled");
        return;
    }

    // Deletions reclaim their own payload inline, so this only picks up what a
    // crash left behind. It runs on the I/O pool rather than the loop because
    // unlinking files blocks.
    drogon::app().getLoop()->runEvery(
        static_cast<double>(config_.reclaimIntervalSeconds), [this] {
            if (!io_->post([this] { storage_->reclaim(kReclaimBatch); })) {
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
