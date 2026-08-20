#include <algorithm>
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
#include "storage/storage_engine.hpp"

namespace {

constexpr int kExitConfigError = 78;  // EX_CONFIG
constexpr int kExitFailure     = 1;

/// A completed check that found problems. Distinct from kExitFailure so a
/// caller can tell "the store is damaged" from "the check could not run" —
/// the first is a maintenance decision, the second is a bug or a bad path.
constexpr int kExitUnclean = 2;

void printUsage() {
    std::cout
        << "MonoBucket " << monobucket::version::kVersion << "\n"
        << "Single-binary S3-compatible object storage.\n\n"
        << "Usage: monobucket [--version | --help | --print-config]\n"
        << "       monobucket --fsck [--deep]\n"
        << "       monobucket --checkpoint <dir>\n\n"
        << "  --fsck    check the metadata against the payload tree and report\n"
        << "            every disagreement. Reports; never repairs. Exits 0 when\n"
        << "            clean, 2 when it found something.\n"
        << "  --deep    additionally re-hash every payload and compare it with\n"
        << "            the digest recorded when it was written. Reads the whole\n"
        << "            store.\n"
        << "  --checkpoint <dir>\n"
        << "            write a consistent, startable copy of the store into\n"
        << "            <dir>, which must not exist or must be empty. Payloads\n"
        << "            are hard-linked, so on one filesystem this duplicates no\n"
        << "            bytes. Requires a STOPPED server -- one process may open\n"
        << "            the metadata store at a time. To back up a running one,\n"
        << "            use the console: it is the process holding the store.\n"
        << "            Verify a copy with MONOBUCKET_DATA_DIR=<dir> monobucket\n"
        << "            --fsck --deep, and restore by stopping the server and\n"
        << "            pointing MONOBUCKET_DATA_DIR at it.\n\n"
        << "MonoBucket is configured entirely through the environment:\n\n"
        << "  MONOBUCKET_HOST                    bind address           (0.0.0.0)\n"
        << "  MONOBUCKET_PORT                    S3 API port            (9000)\n"
        << "  MONOBUCKET_CONSOLE_PORT            dashboard port         (9001)\n"
        << "  MONOBUCKET_CONSOLE_ENABLED         serve the dashboard    (true)\n"
        << "  MONOBUCKET_DATA_DIR                storage root           (/data)\n"
        << "  MONOBUCKET_BACKUP_DIR              where the console may write backups\n"
        << "                                     (unset disables that action)\n"
        << "  MONOBUCKET_REGION                  reported S3 region     (us-east-1)\n"
        << "  MONOBUCKET_S3_PUBLIC_URL           public S3 origin, e.g. https://s3.example.com\n"
        << "  MONOBUCKET_S3_DOMAIN               virtual-host domain    (path style only)\n"
        << "  MONOBUCKET_ADMIN_USERNAME          console login name     (admin)\n"
        << "  MONOBUCKET_ADMIN_PASSWORD          console password, >= 12 chars\n"
        << "  MONOBUCKET_ADMIN_PASSWORD_FILE     read it from a file instead\n"
        << "  MONOBUCKET_CONSOLE_COOKIE_SECURE   auto | true | false    (auto)\n"
        << "  MONOBUCKET_ROOT_ACCESS_KEY         root S3 access key\n"
        << "  MONOBUCKET_ROOT_SECRET_KEY         root S3 secret key\n"
        << "  MONOBUCKET_WORKER_THREADS          0 = hardware_concurrency\n"
        << "  MONOBUCKET_ALLOCATABLE_BYTES       capacity to allocate   (disk - 10%)\n"
        << "  MONOBUCKET_CAPACITY_RESERVE_PERCENT  reserve when derived (10)\n"
        << "  MONOBUCKET_DEFAULT_BUCKET_QUOTA_BYTES  S3 CreateBucket    (0 = unlimited)\n"
        << "  MONOBUCKET_MAX_BODY_BYTES          single-PUT ceiling     (5GiB)\n"
        << "  MONOBUCKET_MAX_MEMORY_BODY_BYTES   spill-to-disk cutoff   (1MiB)\n"
        << "  MONOBUCKET_MAX_UPLOAD_BYTES        initial object limit   (5GiB)\n"
        << "  MONOBUCKET_MULTIPART_EXPIRY_HOURS  abandon idle uploads   (168)\n"
        << "  MONOBUCKET_MAX_UPLOAD_CEILING_BYTES  most it may be raised to (5TiB)\n"
        << "  MONOBUCKET_STREAM_CHUNK_BYTES      streaming chunk size   (1MiB)\n"
        << "  MONOBUCKET_IDLE_TIMEOUT_SECONDS    idle connection reap   (60)\n"
        << "  MONOBUCKET_CACHE_BACKEND           memory | redis         (memory)\n"
        << "  MONOBUCKET_CACHE_MAX_BYTES         cache budget           (128MiB)\n"
        << "  MONOBUCKET_REDIS_URL               redis://host:port\n"
        << "  MONOBUCKET_LOG_LEVEL               trace|debug|info|warn|error\n"
        << "  MONOBUCKET_METRICS_ENABLED         expose /metrics        (true)\n\n"
        << "Sizes accept unit suffixes: binary (KiB/MiB/GiB) and SI (KB/MB/GB).\n";
}

/// Opens the store, checks it and prints what it found.
///
/// Deliberately does not start a listener or the I/O pool: fsck is a
/// maintenance command run against a stopped server, and the synchronous
/// storage API is directly callable precisely because the threading rule lives
/// above it rather than inside it.
int runFsck(const monobucket::Config& config, bool deep) {
    monobucket::StorageEngine::FsckOptions options;
    options.verifyDigests = deep;
    // Nothing else is running against this directory — that is the premise of
    // the command — so a file linked into the tree is either referenced or
    // genuinely orphaned, with no in-flight window to allow for.
    options.unreferencedGraceMs = 0;

    try {
        monobucket::StorageEngine        storage(monobucket::StorageEngine::optionsFrom(config));
        const auto                       report = storage.fsck(options);

        std::cout << "scanned " << report.bucketsScanned << " buckets, " << report.objectsScanned
                  << " objects, " << report.partsScanned << " parts, " << report.filesScanned
                  << " payload files\n";
        if (deep) {
            // Scaled rather than always MiB: a small store would otherwise be
            // told it verified "0 MiB", which reads as "did nothing".
            std::cout << "verified ";
            if (report.bytesRead >= 1024 * 1024) {
                std::cout << report.bytesRead / (1024 * 1024) << " MiB";
            } else if (report.bytesRead >= 1024) {
                std::cout << report.bytesRead / 1024 << " KiB";
            } else {
                std::cout << report.bytesRead << " bytes";
            }
            std::cout << " of payload\n";
        }

        if (report.clean()) {
            std::cout << "clean\n";
            return 0;
        }

        std::cout << report.findings.size() << " finding(s)";
        if (report.leakedBytes > 0) {
            std::cout << ", " << report.leakedBytes / 1024 << " KiB unreferenced";
        }
        std::cout << ":\n";
        for (const auto& finding : report.findings) {
            std::cout << "  " << monobucket::toString(finding.kind) << ' ' << finding.blobId;
            if (!finding.reference.empty()) std::cout << " <- " << finding.reference;
            std::cout << " (" << finding.detail << ")\n";
        }
        return kExitUnclean;
    } catch (const std::exception& ex) {
        std::cerr << "fsck failed: " << ex.what() << '\n';
        return kExitFailure;
    }
}

/// Writes a checkpoint of a stopped store.
///
/// Stopped, and there is no way around it: RocksDB allows one process to open a
/// directory at a time, so a second binary cannot checkpoint a store the server
/// is holding. Backing up a *running* instance is the console's job, because
/// the running process is the only thing that can do it — which is why this
/// command and that endpoint exist side by side rather than one wrapping the
/// other.
int runCheckpoint(const monobucket::Config& config, const std::string& destination) {
    try {
        monobucket::StorageEngine storage(monobucket::StorageEngine::optionsFrom(config));
        const auto                report = storage.checkpoint(destination);

        std::cout << "checkpoint written to " << report.destination.string() << '\n'
                  << "  " << report.payloadsLinked << " payloads hard-linked\n";
        if (report.payloadsCopied > 0) {
            std::cout << "  " << report.payloadsCopied << " payloads copied ("
                      << report.bytesCopied / (1024 * 1024)
                      << " MiB) -- the destination is on another filesystem, so this "
                         "duplicated the bytes\n";
        }
        std::cout << "  took " << report.elapsedMs << " ms\n\n"
                  << "Verify it before relying on it:\n"
                  << "  MONOBUCKET_DATA_DIR=" << report.destination.string()
                  << " monobucket --fsck --deep\n";
        return 0;
    } catch (const monobucket::StorageError& ex) {
        std::cerr << "checkpoint failed: " << ex.what() << '\n';
        return kExitFailure;
    } catch (const std::exception& ex) {
        std::cerr << "checkpoint failed: " << ex.what() << '\n';
        return kExitFailure;
    }
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

    const bool fsckRequested = std::find(args.begin(), args.end(), "--fsck") != args.end();
    if (fsckRequested) {
        return runFsck(config, std::find(args.begin(), args.end(), "--deep") != args.end());
    }

    if (const auto flag = std::find(args.begin(), args.end(), "--checkpoint"); flag != args.end()) {
        if (flag + 1 == args.end()) {
            std::cerr << "--checkpoint needs a destination directory\n";
            return kExitConfigError;
        }
        return runCheckpoint(config, *(flag + 1));
    }

    monobucket::log::info("MonoBucket ", monobucket::version::kVersion, " starting");
    monobucket::log::info(config.summary());
    if (config.usingDefaultCredentials()) {
        monobucket::log::warn(
            "running with the built-in demo S3 credentials; set "
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
