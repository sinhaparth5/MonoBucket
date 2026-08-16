#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace monobucket {

/// Process lifecycle: ordered shutdown.
///
/// SIGTERM and SIGINT are deliberately *not* handled here. Drogon installs its
/// own handlers inside run() and would overwrite ours, so the shutdown
/// callbacks are registered with Drogon instead (see Server::watchForShutdown).
/// This class owns the part Drogon has no opinion about: running teardown in a
/// defined order once the event loop has stopped.
class Lifecycle {
public:
    static Lifecycle& instance();

    /// Ignores SIGPIPE so that a peer disconnecting mid-transfer surfaces as a
    /// write error instead of killing the process. Drogon does not do this for
    /// us, and it must happen before the first byte is written.
    void installSignalHandlers();

    /// Hooks run in reverse registration order, so a component can rely on the
    /// things it was built on top of still being alive.
    void onShutdown(std::string name, std::function<void()> hook);

    /// Runs every registered hook exactly once. Exceptions from a hook are
    /// logged and swallowed: one failing flush must not skip the others.
    void runShutdownHooks();

    Lifecycle(const Lifecycle&) = delete;
    Lifecycle& operator=(const Lifecycle&) = delete;

private:
    Lifecycle() = default;

    struct Hook {
        std::string           name;
        std::function<void()> fn;
    };

    mutable std::mutex mutex_;
    std::vector<Hook>  hooks_;
    bool               hooksRan_ = false;
};

}  // namespace monobucket
