#pragma once

#include <memory>

#include "cache/cache_provider.hpp"
#include "core/config.hpp"
#include "core/io_executor.hpp"
#include "s3/metrics.hpp"
#include "storage/storage_engine.hpp"

namespace monobucket {

/// Owns the process' event loop, listeners, storage engine and cache.
///
/// The S3 protocol handlers attach to the same framework instance from
/// registerRoutes() and reach storage through storage().
class Server {
public:
    explicit Server(Config config);
    ~Server();

    /// Blocks until a shutdown signal arrives. Returns a process exit code.
    int run();

    const Config& config() const noexcept { return config_; }

    /// Valid once run() has opened the data directory.
    StorageEngine& storage() noexcept { return *storage_; }
    IoExecutor&    io() noexcept { return *io_; }
    CacheProvider& cache() noexcept { return *cache_; }

private:
    void prepareDataDirectory();
    void openStorage();
    void openCache();
    void configureFramework();
    void registerRoutes();
    void scheduleMaintenance();
    void watchForShutdown();

    Config                         config_;
    std::unique_ptr<StorageEngine> storage_;
    std::unique_ptr<IoExecutor>    io_;
    std::unique_ptr<CacheProvider> cache_;

    /// Lives as long as the server: the S3 router holds a reference to it and
    /// /metrics reads it from another thread.
    s3::S3Metrics                  s3Metrics_;
};

/// Seconds since the process started serving. Used by /metrics.
double uptimeSeconds() noexcept;

}  // namespace monobucket
