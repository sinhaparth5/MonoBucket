#pragma once

#include "core/config.hpp"

namespace monobucket {

/// Owns the process' event loop and listeners.
///
/// Phase 1 wires up the loop, the worker pool and the system endpoints. The S3
/// protocol handlers (Phase 4) and the console asset routes (Phase 5) attach to
/// the same framework instance from registerRoutes().
class Server {
public:
    explicit Server(Config config);

    /// Blocks until a shutdown signal arrives. Returns a process exit code.
    int run();

    const Config& config() const noexcept { return config_; }

private:
    void prepareDataDirectory();
    void configureFramework();
    void registerRoutes();
    void watchForShutdown();

    Config config_;
};

/// Seconds since the process started serving. Used by /metrics.
double uptimeSeconds() noexcept;

}  // namespace monobucket
