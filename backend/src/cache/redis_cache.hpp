#pragma once

#if defined(MONOBUCKET_WITH_REDIS)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cache/cache_provider.hpp"
#include "cache/redis_url.hpp"

namespace monobucket {

/// Optional shared cache backend, on hiredis.
///
/// It is meant to sit behind FallbackCache rather than to be used directly, and
/// its methods therefore throw `CacheError` instead of swallowing failures —
/// the breaker upstream is what turns a failure into a bypass, and it can only
/// do that if it is told.
///
/// Connections are made lazily. A Redis that is down when the container starts
/// must not stop the container from starting, so the constructor allocates no
/// sockets and the first request pays for the connection.
///
/// hiredis is synchronous, which is why every call runs on the I/O pool rather
/// than the event loop, exactly like a disk read.
class RedisCache final : public CacheProvider {
public:
    struct Options {
        RedisEndpoint endpoint;

        /// Bounded on purpose: an unbounded pool converts a stalled Redis into
        /// unbounded file descriptors and threads blocked on them.
        std::size_t poolSize = 4;

        std::chrono::milliseconds connectTimeout{1000};
        std::chrono::milliseconds commandTimeout{1000};

        /// How long a caller waits for a free connection before being told no.
        std::chrono::milliseconds acquireTimeout{2000};

        /// Namespaces our keys so a shared Redis stays shared. `clear()` only
        /// ever touches keys carrying it.
        std::string keyPrefix = "mb:";
    };

    explicit RedisCache(Options options);
    ~RedisCache() override;

    std::string_view name() const noexcept override { return "redis"; }

    CacheValuePtr get(std::string_view key) override;
    void          set(std::string_view key, CacheValuePtr value,
                      std::chrono::seconds ttl) override;
    bool          del(std::string_view key) override;

    /// Always 0. Redis enforces its own budget through `maxmemory` and an
    /// eviction policy; a client that also evicted would be fighting it.
    std::size_t evict(std::uint64_t budgetBytes) override;

    /// Removes only the keys carrying our prefix, via SCAN. Never FLUSHDB —
    /// the database may not be ours alone.
    void clear() override;

    CacheStats stats() const override;

    /// Best-effort connectivity check. Returns an error message rather than
    /// throwing, so startup can report it and carry on degraded.
    std::optional<std::string> probe();

private:
    class Connection;

    /// RAII checkout. Returns its connection to the pool on destruction,
    /// including when the command threw.
    class Lease {
    public:
        Lease(RedisCache& owner, std::unique_ptr<Connection> connection);
        ~Lease();

        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;

        Connection* operator->() const noexcept { return connection_.get(); }

    private:
        RedisCache&                 owner_;
        std::unique_ptr<Connection> connection_;
    };

    Lease acquire();
    void  release(std::unique_ptr<Connection> connection);

    std::string qualify(std::string_view key) const;

    Options options_;

    mutable std::mutex                       poolMutex_;
    std::condition_variable                  poolAvailable_;
    std::vector<std::unique_ptr<Connection>> idle_;
    std::size_t                              created_ = 0;

    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<bool>          healthy_{true};
};

}  // namespace monobucket

#endif  // MONOBUCKET_WITH_REDIS
