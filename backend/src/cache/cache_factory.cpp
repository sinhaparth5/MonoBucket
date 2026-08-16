#include "cache/cache_factory.hpp"

#include <algorithm>
#include <atomic>

#include "cache/fallback_cache.hpp"
#include "cache/memory_cache.hpp"
#include "cache/redis_url.hpp"
#include "core/config.hpp"
#include "core/env.hpp"
#include "core/logging.hpp"

#if defined(MONOBUCKET_WITH_REDIS)
#include "cache/redis_cache.hpp"
#endif

namespace monobucket {
namespace {

/// The backend for `MONOBUCKET_CACHE_MAX_BYTES=0`.
///
/// Worth having rather than sprinkling null checks through the callers: every
/// layer above can assume a cache exists, and a benchmark that wants to measure
/// the storage engine without one gets an honest zero hit rate instead of a
/// different code path.
class NullCache final : public CacheProvider {
public:
    std::string_view name() const noexcept override { return "disabled"; }

    CacheValuePtr get(std::string_view) override {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    void        set(std::string_view, CacheValuePtr, std::chrono::seconds) override {}
    bool        del(std::string_view) override { return false; }
    std::size_t evict(std::uint64_t) override { return 0; }
    void        clear() override {}

    CacheStats stats() const override {
        CacheStats out;
        out.misses = misses_.load(std::memory_order_relaxed);
        return out;
    }

private:
    std::atomic<std::uint64_t> misses_{0};
};

/// One shard per worker, within reason. Sizing it from the thread count rather
/// than exposing a knob: the contention this defends against is created by the
/// event loop threads, so that is the number that matters.
unsigned shardsFor(const Config& config) {
    return std::clamp(config.workerThreads * 2, 4u, 32u);
}

std::unique_ptr<MemoryCache> makeMemoryCache(const Config& config, std::uint64_t budget) {
    MemoryCache::Options options;
    options.maxBytes = budget;
    options.shards   = shardsFor(config);
    return std::make_unique<MemoryCache>(options);
}

}  // namespace

std::unique_ptr<CacheProvider> makeCache(const Config& config) {
    if (config.cacheMaxBytes == 0) {
        log::info("cache: disabled (MONOBUCKET_CACHE_MAX_BYTES=0)");
        return std::make_unique<NullCache>();
    }

    if (config.cacheBackend == CacheBackend::Memory) {
        auto cache = makeMemoryCache(config, config.cacheMaxBytes);
        log::info("cache: in-memory LRU, ", env::formatBytes(config.cacheMaxBytes), " across ",
                  cache->shardCount(), " shards");
        return cache;
    }

#if defined(MONOBUCKET_WITH_REDIS)
    // The local tier is deliberately smaller than the configured budget. It is
    // an L1 in front of a shared cache, not the cache itself, and its whole
    // reason for existing is to be warm when Redis goes away.
    const std::uint64_t localBudget = std::max<std::uint64_t>(
        1024 * 1024, config.cacheMaxBytes / 4);

    RedisCache::Options redisOptions;
    redisOptions.endpoint = parseRedisUrl(config.redisUrl);
    redisOptions.poolSize = config.redisPoolSize;

    auto redis = std::make_unique<RedisCache>(redisOptions);
    if (const auto failure = redis->probe()) {
        // Not fatal. The breaker starts closed, discovers the same failure on
        // the first real request, and starts probing for recovery.
        log::warn("cache: redis is not answering yet (", *failure,
                  "); starting on the local tier");
    } else {
        log::info("cache: redis at ", describe(redisOptions.endpoint), ", pool ",
                  redisOptions.poolSize);
    }

    auto local = makeMemoryCache(config, localBudget);
    log::info("cache: local tier ", env::formatBytes(localBudget), " across ",
              local->shardCount(), " shards");

    FallbackCache::Options fallbackOptions;
    fallbackOptions.localTtlCap = std::chrono::seconds(config.cacheLocalTtlSeconds);
    return std::make_unique<FallbackCache>(std::move(redis), std::move(local), fallbackOptions);
#else
    log::warn("cache: MONOBUCKET_CACHE_BACKEND=redis, but this binary was built without the "
              "Redis backend (configure with -DMONOBUCKET_ENABLE_REDIS=ON, or use the "
              "release-redis preset); falling back to the in-memory cache");
    return makeMemoryCache(config, config.cacheMaxBytes);
#endif
}

}  // namespace monobucket
