#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/env.hpp"
#include "core/lifecycle.hpp"
#include "core/logging.hpp"
#include "monobucket/version.hpp"
#include "server/asset_store.hpp"
#include "server/server.hpp"

namespace {

constexpr int kExitConfigError = 78;  // EX_CONFIG
constexpr int kExitFailure     = 1;

void printUsage() {
    std::cout
        << "MonoBucket " << monobucket::version::kVersion << "\n"
        << "Single-binary S3-compatible object storage.\n\n"
        << "Usage: monobucket [--version | --help | --print-config]\n\n"
        << "MonoBucket is configured entirely through the environment:\n\n"
        << "  MONOBUCKET_HOST                    bind address           (0.0.0.0)\n"
        << "  MONOBUCKET_PORT                    S3 API port            (9000)\n"
        << "  MONOBUCKET_CONSOLE_PORT            dashboard port         (9001)\n"
        << "  MONOBUCKET_CONSOLE_ENABLED         serve the dashboard    (true)\n"
        << "  MONOBUCKET_DATA_DIR                storage root           (/data)\n"
        << "  MONOBUCKET_REGION                  reported S3 region     (us-east-1)\n"
        << "  MONOBUCKET_ROOT_ACCESS_KEY         root access key\n"
        << "  MONOBUCKET_ROOT_SECRET_KEY         root secret key\n"
        << "  MONOBUCKET_WORKER_THREADS          0 = hardware_concurrency\n"
        << "  MONOBUCKET_MAX_BODY_BYTES          single-PUT ceiling     (5GiB)\n"
        << "  MONOBUCKET_MAX_MEMORY_BODY_BYTES   spill-to-disk cutoff   (1MiB)\n"
        << "  MONOBUCKET_STREAM_CHUNK_BYTES      streaming chunk size   (1MiB)\n"
        << "  MONOBUCKET_IDLE_TIMEOUT_SECONDS    idle connection reap   (60)\n"
        << "  MONOBUCKET_CACHE_BACKEND           memory | redis         (memory)\n"
        << "  MONOBUCKET_CACHE_MAX_BYTES         cache budget           (128MiB)\n"
        << "  MONOBUCKET_REDIS_URL               redis://host:port\n"
        << "  MONOBUCKET_LOG_LEVEL               trace|debug|info|warn|error\n"
        << "  MONOBUCKET_METRICS_ENABLED         expose /metrics        (true)\n\n"
        << "Sizes accept unit suffixes: binary (KiB/MiB/GiB) and SI (KB/MB/GB).\n";
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    for (const auto& arg : args) {
        if (arg == "--version" || arg == "-v") {
            std::cout << monobucket::version::kVersion << '\n';
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
    }

    monobucket::Config config;
    try {
        config = monobucket::Config::fromEnvironment();
    } catch (const monobucket::env::ParseError& ex) {
        std::cerr << "configuration error: " << ex.what() << '\n';
        return kExitConfigError;
    } catch (const monobucket::ConfigError& ex) {
        std::cerr << "configuration error: " << ex.what() << '\n';
        return kExitConfigError;
    }

    if (const auto level = monobucket::log::fromString(config.logLevel)) {
        monobucket::log::setLevel(*level);
    }

    for (const auto& arg : args) {
        if (arg == "--print-config") {
            std::cout << config.toJson().dump(2) << '\n';
            return 0;
        }
    }

    monobucket::log::info("MonoBucket ", monobucket::version::kVersion, " starting");
    monobucket::log::info(config.summary());
    if (config.usingDefaultCredentials()) {
        monobucket::log::warn(
            "running with the built-in demo credentials; set "
            "MONOBUCKET_ROOT_ACCESS_KEY and MONOBUCKET_ROOT_SECRET_KEY before "
            "exposing this server");
    }
    if (!monobucket::assets::embedded()) {
        monobucket::log::warn(
            "built without an embedded dashboard; the console will return 404 "
            "(rebuild with -DMONOBUCKET_EMBED_FRONTEND=ON)");
    }

    monobucket::Lifecycle::instance().installSignalHandlers();

    try {
        monobucket::Server server(std::move(config));
        return server.run();
    } catch (const monobucket::ConfigError& ex) {
        monobucket::log::error("startup failed: ", ex.what());
        return kExitConfigError;
    } catch (const std::exception& ex) {
        monobucket::log::error("fatal: ", ex.what());
        return kExitFailure;
    }
}
