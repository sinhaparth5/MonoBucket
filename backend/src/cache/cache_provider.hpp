#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace monobucket {

/// A cached payload, handed out by shared pointer.
///
/// Two properties follow from that, and both are load-bearing. Readers get a
/// value without copying its bytes, which is what makes a shared lock on the
/// read path worth having at all; and a value outlives its cache entry for as
/// long as someone holds it, so eviction on another thread cannot pull the
/// ground out from under a response that is still being written.
using CacheValuePtr = std::shared_ptr<const std::string>;

/// Wraps bytes for storage. The buffer is never mutated afterwards.
CacheValuePtr makeCacheValue(std::string value);

/// A time-to-live of zero means "until evicted".
///
/// A negative one means "already expired", which falls out of storing a
/// deadline rather than a duration. That is not an accident to be guarded
/// against: it gives callers — and tests — a way to store something that is
/// immediately stale without waiting for wall-clock time to pass.
inline constexpr std::chrono::seconds kNoExpiry{0};

/// Raised by backends that talk to something outside this process.
///
/// The in-memory backend never throws it. Callers that want that guarantee
/// regardless of backend should say so by wrapping in FallbackCache rather than
/// by hoping — a cache outage must never become a storage outage.
class CacheError : public std::runtime_error {
public:
    explicit CacheError(const std::string& what) : std::runtime_error(what) {}
};

struct CacheStats {
    std::uint64_t hits        = 0;
    std::uint64_t misses      = 0;
    std::uint64_t evictions   = 0;
    std::uint64_t expirations = 0;

    /// Entries refused because a single value exceeded the whole budget.
    std::uint64_t rejections = 0;

    /// Backend failures. Always 0 for in-memory backends.
    std::uint64_t errors = 0;

    std::uint64_t entries = 0;
    std::uint64_t bytes   = 0;  ///< 0 when the backend cannot know (Redis)
    std::uint64_t limitBytes = 0;

    /// False when a remote backend is currently being bypassed.
    bool healthy = true;

    /// Hits over lookups. Returns 0 before the first lookup rather than NaN,
    /// because this goes straight into a Prometheus gauge.
    double hitRatio() const noexcept;
};

/// The cache seam.
///
/// Everything above this interface — object metadata in Phase 4, listing pages,
/// the dashboard's counters — is written against it, so the backend really is
/// swappable by environment variable alone.
class CacheProvider {
public:
    virtual ~CacheProvider() = default;

    CacheProvider(const CacheProvider&)            = delete;
    CacheProvider& operator=(const CacheProvider&) = delete;

    /// Backend name as it appears in logs and metrics.
    virtual std::string_view name() const noexcept = 0;

    /// Null on a miss. Never throws: a backend failure is reported as a miss,
    /// because the caller's recourse is the same either way.
    virtual CacheValuePtr get(std::string_view key) = 0;

    /// Stores a value. A null value deletes the key. Never throws.
    virtual void set(std::string_view key, CacheValuePtr value, std::chrono::seconds ttl) = 0;

    /// True when the key was present.
    virtual bool del(std::string_view key) = 0;

    /// Shrinks the cache to at most `budgetBytes`, dropping expired entries
    /// first and then the coldest. Returns how many entries were removed.
    ///
    /// This does not change the configured budget — that is enforced on every
    /// insert. This is the periodic pass that collects what expiry left behind.
    virtual std::size_t evict(std::uint64_t budgetBytes) = 0;

    virtual void clear() = 0;

    virtual CacheStats stats() const = 0;

    /// Convenience for callers holding bytes rather than a shared buffer.
    /// Deliberately a different name from set(): an overload would be hidden in
    /// every derived class that overrides the virtual.
    void put(std::string_view key, std::string_view value,
             std::chrono::seconds ttl = kNoExpiry);

protected:
    CacheProvider() = default;
};

}  // namespace monobucket
