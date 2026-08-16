#include "core/lifecycle.hpp"

#include <csignal>
#include <atomic>

#include "core/logging.hpp"

namespace monobucket {

Lifecycle& Lifecycle::instance() {
    static Lifecycle instance;
    return instance;
}

void Lifecycle::installSignalHandlers() {
#if !defined(_WIN32)
    // A client that vanishes mid-upload must not take the server with it.
    // SIGTERM/SIGINT are Drogon's to install — see the note in lifecycle.hpp.
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    ignore.sa_flags = 0;
    sigaction(SIGPIPE, &ignore, nullptr);
#endif
}

void Lifecycle::onShutdown(std::string name, std::function<void()> hook) {
    std::lock_guard<std::mutex> guard(mutex_);
    hooks_.push_back(Hook{std::move(name), std::move(hook)});
}

void Lifecycle::runShutdownHooks() {
    std::vector<Hook> pending;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (hooksRan_) return;
        hooksRan_ = true;
        pending = std::move(hooks_);
        hooks_.clear();
    }

    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
        log::debug("shutdown hook: ", it->name);
        try {
            it->fn();
        } catch (const std::exception& ex) {
            log::error("shutdown hook '", it->name, "' failed: ", ex.what());
        } catch (...) {
            log::error("shutdown hook '", it->name, "' failed with an unknown exception");
        }
    }
}


}  // namespace monobucket
