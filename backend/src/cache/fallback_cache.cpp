#include "cache/fallback_cache.hpp"

#include <algorithm>
#include <utility>

#include "core/logging.hpp"

namespace monobucket {
namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

FallbackCache::FallbackCache(std::unique_ptr<CacheProvider> shared,
                             std::unique_ptr<CacheProvider> local,
                             Options                        options)
    : shared_(std::move(shared)),
      local_(std::move(local)),
      options_(options),
      cooldown_(options.initialCooldown) {
    name_ = std::string(shared_->name()) + "+" + std::string(local_->name());
}

FallbackCache::~FallbackCache() = default;

std::chrono::seconds FallbackCache::localTtl(std::chrono::seconds requested) const {
    // "Never expires" becomes "expires at the cap": a local copy of a value
    // another instance owns cannot be held indefinitely. An already-expired ttl
    // stays already-expired.
    if (requested == kNoExpiry) return options_.localTtlCap;
    return std::min(requested, options_.localTtlCap);
}

bool FallbackCache::allowShared() {
    std::lock_guard lock(breaker_);

    if (consecutiveFailures_ < options_.failureThreshold) return true;  // closed
    if (Clock::now() < openUntil_) return false;                        // open
    if (probeInFlight_) return false;                                   // someone else is probing

    probeInFlight_ = true;  // half-open: this caller carries the probe
    return true;
}

void FallbackCache::recordSuccess() {
    std::lock_guard lock(breaker_);
    if (consecutiveFailures_ >= options_.failureThreshold) {
        log::info("cache: shared tier is answering again");
    }
    consecutiveFailures_ = 0;
    probeInFlight_       = false;
    cooldown_            = options_.initialCooldown;
}

void FallbackCache::recordFailure(const char* operation, const std::exception& error) {
    errors_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard lock(breaker_);
    const bool wasHealthy = consecutiveFailures_ < options_.failureThreshold;
    ++consecutiveFailures_;
    probeInFlight_ = false;

    if (consecutiveFailures_ < options_.failureThreshold) {
        log::debug("cache: shared tier ", operation, " failed: ", error.what());
        return;
    }

    if (wasHealthy) {
        cooldown_ = options_.initialCooldown;
        log::warn("cache: shared tier unavailable (", operation, ": ", error.what(),
                  "); serving from the local tier");
    } else {
        // A failed probe means it is still down; back off rather than
        // reconnecting on every request.
        cooldown_ = std::min(cooldown_ * 2, options_.maxCooldown);
    }
    openUntil_ = Clock::now() + cooldown_;
}

bool FallbackCache::sharedHealthy() const {
    std::lock_guard lock(breaker_);
    return consecutiveFailures_ < options_.failureThreshold;
}

CacheValuePtr FallbackCache::get(std::string_view key) {
    if (auto value = local_->get(key)) {
        hits_.fetch_add(1, std::memory_order_relaxed);
        return value;
    }

    if (!allowShared()) {
        bypassed_.fetch_add(1, std::memory_order_relaxed);
        misses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    try {
        auto value = shared_->get(key);
        recordSuccess();
        if (value != nullptr) {
            local_->set(key, value, options_.localTtlCap);
            hits_.fetch_add(1, std::memory_order_relaxed);
            return value;
        }
    } catch (const std::exception& ex) {
        // Reported as a miss on purpose. The caller's recourse — read it from
        // storage — is the same whether the key was absent or unreachable.
        recordFailure("get", ex);
    }

    misses_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void FallbackCache::set(std::string_view key, CacheValuePtr value, std::chrono::seconds ttl) {
    // The local tier is written first and unconditionally: it is the one that
    // has to be warm when the shared tier goes away.
    local_->set(key, value, localTtl(ttl));

    if (!allowShared()) {
        bypassed_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    try {
        shared_->set(key, std::move(value), ttl);
        recordSuccess();
    } catch (const std::exception& ex) {
        recordFailure("set", ex);
    }
}

bool FallbackCache::del(std::string_view key) {
    const bool localHad = local_->del(key);

    if (!allowShared()) {
        bypassed_.fetch_add(1, std::memory_order_relaxed);
        return localHad;
    }

    try {
        const bool sharedHad = shared_->del(key);
        recordSuccess();
        return localHad || sharedHad;
    } catch (const std::exception& ex) {
        // The shared copy outlives its deletion here. Bounded by its own TTL,
        // and the alternative — failing the caller's delete — is worse.
        recordFailure("del", ex);
        return localHad;
    }
}

std::size_t FallbackCache::evict(std::uint64_t budgetBytes) {
    // Only the local tier has a budget this process owns; the shared one is
    // sized by whoever runs it.
    return local_->evict(budgetBytes);
}

void FallbackCache::clear() {
    local_->clear();

    if (!allowShared()) return;
    try {
        shared_->clear();
        recordSuccess();
    } catch (const std::exception& ex) {
        recordFailure("clear", ex);
    }
}

CacheStats FallbackCache::stats() const {
    // Counted here rather than summed from the tiers: a shared-tier hit is
    // preceded by a local-tier miss, so adding the two would report a hit ratio
    // that no request actually experienced.
    CacheStats out = local_->stats();
    out.hits       = hits_.load(std::memory_order_relaxed);
    out.misses     = misses_.load(std::memory_order_relaxed);
    out.errors     = errors_.load(std::memory_order_relaxed);
    out.healthy    = sharedHealthy();
    return out;
}

}  // namespace monobucket
