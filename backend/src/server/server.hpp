#pragma once

#include <memory>

#include "core/config.hpp"
#include "core/io_executor.hpp"
#include "storage/storage_engine.hpp"

namespace monobucket {

/// Owns the process' event loop, listeners and storage engine.
///
/// The S3 protocol handlers (Phase 4) attach to the same framework instance
/// from registerRoutes() and reach storage through storage().
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

private:
    void prepareDataDirectory();
    void openStorage();
    void configureFramework();
    void registerRoutes();
    void scheduleMaintenance();
    void watchForShutdown();

    Config                         config_;
    std::unique_ptr<StorageEngine> storage_;
    std::unique_ptr<IoExecutor>    io_;
};

/// Seconds since the process started serving. Used by /metrics.
double uptimeSeconds() noexcept;

}  // namespace monobucket
