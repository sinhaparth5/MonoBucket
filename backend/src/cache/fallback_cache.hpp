#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "cache/cache_provider.hpp"

namespace monobucket {

/// Two tiers and a circuit breaker.
///
/// The roadmap's requirement is that a Redis outage must never become a storage
/// outage. Retrying a dead socket on every request satisfies that only on
/// paper: the requests still stall, they just stall on the way to a fallback.
/// So this keeps a local cache in front of the shared one and stops calling the
/// shared tier at all once it starts failing, probing occasionally to find out
/// when it comes back.
///
/// Arranging it as two tiers rather than a plain either/or is what makes the
/// failover warm — the local tier is already populated when the shared one
/// disappears, instead of starting empty at the worst possible moment.
///
/// The cost is coherence: another instance can change a shared value that this
/// process has already copied locally. `Options::localTtlCap` bounds how long
/// that can go unnoticed, and applies even to entries stored with no expiry —
/// an unbounded local copy of a shared value is a correctness bug, not a
/// performance win.
class FallbackCache final : public CacheProvider {
public:
    struct Options {
        /// Consecutive failures before the shared tier is bypassed.
        unsigned failureThreshold = 3;

        /// First bypass window. Doubles on each failed probe, up to maxCooldown.
        std::chrono::milliseconds initialCooldown{500};
        std::chrono::milliseconds maxCooldown{30'000};

        /// Ceiling on the lifetime of any locally held copy.
        std::chrono::seconds localTtlCap{5};
    };

    /// Options are passed explicitly rather than defaulted: a nested type's
    /// member initialisers are not yet usable inside its own enclosing class,
    /// and `Options{}` at the call site reads no worse.
    FallbackCache(std::unique_ptr<CacheProvider> shared,
                  std::unique_ptr<CacheProvider> local,
                  Options                        options);
    ~FallbackCache() override;

    std::string_view name() const noexcept override { return name_; }

    CacheValuePtr get(std::string_view key) override;
    void          set(std::string_view key, CacheValuePtr value,
                      std::chrono::seconds ttl) override;
    bool          del(std::string_view key) override;
    std::size_t   evict(std::uint64_t budgetBytes) override;
    void          clear() override;
    CacheStats    stats() const override;

    /// False while the shared tier is being bypassed. Reported by /readyz and
    /// /metrics, because "the cache is up but degraded" is a state an operator
    /// needs to be able to see.
    bool sharedHealthy() const;

    /// Operations that skipped the shared tier because the breaker was open.
    /// A rising count with `sharedHealthy()` true again is what tells you the
    /// outage happened and ended, rather than that it never happened.
    std::uint64_t bypassCount() const noexcept {
        return bypassed_.load(std::memory_order_relaxed);
    }

private:
    /// Decides whether this call may touch the shared tier. Also performs the
    /// half-open transition, so exactly one probe goes out per cooldown.
    bool allowShared();
    void recordSuccess();
    void recordFailure(const char* operation, const std::exception& error);

    std::chrono::seconds localTtl(std::chrono::seconds requested) const;

    std::unique_ptr<CacheProvider> shared_;
    std::unique_ptr<CacheProvider> local_;
    Options                        options_;
    std::string                    name_;

    mutable std::mutex                    breaker_;
    unsigned                              consecutiveFailures_ = 0;
    bool                                  probeInFlight_       = false;
    std::chrono::milliseconds             cooldown_{0};
    std::chrono::steady_clock::time_point openUntil_{};

    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<std::uint64_t> bypassed_{0};
};

}  // namespace monobucket
