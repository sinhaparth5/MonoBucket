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

    // Phase 2 adds the sharded object tree and the metadata store underneath.
    for (const char* sub : {"objects", "meta", "tmp"}) {
        fs::create_directories(fs::path(config_.dataDir) / sub, ec);
        if (ec) {
            throw ConfigError("cannot create '" + std::string(sub) + "' under the data directory: " +
                              ec.message());
        }
    }
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
    registerSystemRoutes(config_);

    if (config_.consoleEnabled) {
        registerConsoleRoutes(config_);
    }

    // Phase 4 registers the S3 service/bucket/object routes here.
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
    configureFramework();
    registerRoutes();
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
