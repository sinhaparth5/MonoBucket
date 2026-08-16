#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "cache/cache_provider.hpp"

namespace monobucket {

/// The default backend: a sharded LRU with a byte budget.
///
/// Three decisions are worth stating outright, because each of them departs
/// from the textbook version on purpose.
///
/// **Sharded, rather than one lock.** Every shard is an independent map, list
/// and mutex, and carries `maxBytes / shards` of the budget. A single lock over
/// a cache this hot serialises the whole server; the cost of sharding is that
/// an unlucky key distribution can leave part of the budget unused, which is a
/// far better failure than a convoy on one mutex.
///
/// **Reads take a shared lock and do not reorder the list.** A textbook LRU
/// promotes on every read, which makes `get()` a writer and reduces a
/// `shared_mutex` to decoration. Here a hit sets an atomic flag on the entry,
/// and eviction gives a flagged entry exactly one reprieve before taking it
/// (the CLOCK approximation). Recency becomes approximate; concurrency becomes
/// real. For a cache that exists to absorb read load, that is the right trade.
///
/// **The budget is enforced on insert, not by a timer.** A budget that is only
/// checked periodically is not a budget, it is a target — and the whole premise
/// of this project is that resident memory stays where it was configured. The
/// periodic pass (`evict`) exists to collect entries that expired, not to hold
/// the ceiling.
class MemoryCache final : public CacheProvider {
public:
    struct Options {
        std::uint64_t maxBytes = 128ull * 1024 * 1024;

        /// Rounded down to a power of two, capped, and reduced further when the
        /// budget is too small to give each shard useful room.
        unsigned shards = 8;
    };

    explicit MemoryCache(Options options);
    ~MemoryCache() override;

    std::string_view name() const noexcept override { return "memory"; }

    CacheValuePtr get(std::string_view key) override;
    void          set(std::string_view key, CacheValuePtr value,
                      std::chrono::seconds ttl) override;
    bool          del(std::string_view key) override;
    std::size_t   evict(std::uint64_t budgetBytes) override;
    void          clear() override;
    CacheStats    stats() const override;

    /// How many shards the options actually resolved to. Exposed for tests and
    /// for the startup log, where a silently reduced shard count would
    /// otherwise be invisible.
    unsigned shardCount() const noexcept { return static_cast<unsigned>(shards_.size()); }

private:
    struct Node;
    struct Shard;

    Shard&       shardFor(std::string_view key) noexcept;
    const Shard& shardFor(std::string_view key) const noexcept;

    /// All of these require the shard's exclusive lock.
    static void        unlink(Shard& shard, Node* node) noexcept;
    static void        pushFront(Shard& shard, Node* node) noexcept;
    static void        removeLocked(Shard& shard, Node* node);
    static bool        evictOneLocked(Shard& shard);
    static std::size_t purgeExpiredLocked(Shard& shard, std::int64_t nowTicks);

    Options                             options_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::size_t                         shardMask_ = 0;
};

}  // namespace monobucket
