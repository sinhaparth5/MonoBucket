#include "cache/memory_cache.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace monobucket {
namespace {

/// Charged to every entry on top of key and value length.
///
/// Roughly the node itself (two list pointers, the key's own std::string
/// header, a shared_ptr, the expiry and the flag) plus the map's bucket slot
/// and hash node. It is an estimate, and it is here because the alternative is
/// worse: a cache of a million eight-byte values would otherwise report itself
/// as using 8 MB while actually holding well over a hundred.
constexpr std::uint64_t kEntryOverheadBytes = 128;

/// Below this a shard holds too few entries for the second-chance pass to mean
/// anything, so the shard count is reduced instead.
constexpr std::uint64_t kMinShardBudgetBytes = 1024 * 1024;

constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max();

/// Monotonic milliseconds. Steady rather than system time, so that an operator
/// correcting the clock cannot expire the whole cache at once.
std::int64_t nowTicks() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::int64_t expiryFrom(std::chrono::seconds ttl) noexcept {
    if (ttl.count() == 0) return kNever;
    // A negative ttl lands the deadline in the past, which is precisely what it
    // should mean. See kNoExpiry.
    return nowTicks() + std::chrono::milliseconds(ttl).count();
}

unsigned floorPowerOfTwo(unsigned value) noexcept {
    unsigned result = 1;
    while (result * 2 <= value) result *= 2;
    return result;
}

/// Transparent hashing, so get(string_view) does not allocate a std::string to
/// look up a key that is already there.
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view key) const noexcept {
        return std::hash<std::string_view>{}(key);
    }
};

}  // namespace

// ---------------------------------------------------------------------------

struct MemoryCache::Node {
    std::string   key;
    CacheValuePtr value;
    std::uint64_t bytes     = 0;
    std::int64_t  expiresAt = kNever;

    /// Set by readers under a shared lock, cleared by eviction under the
    /// exclusive one. Relaxed throughout: losing an occasional flag costs one
    /// entry a reprieve it had earned, nothing more.
    std::atomic<bool> accessed{false};

    Node* prev = nullptr;
    Node* next = nullptr;
};

struct MemoryCache::Shard {
    mutable std::shared_mutex mutex;

    std::unordered_map<std::string, std::unique_ptr<Node>, StringHash, std::equal_to<>> map;

    Node* head = nullptr;  ///< most recently inserted or reprieved
    Node* tail = nullptr;  ///< the next eviction candidate

    std::uint64_t bytes  = 0;
    std::uint64_t budget = 0;

    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
    std::atomic<std::uint64_t> evictions{0};
    std::atomic<std::uint64_t> expirations{0};
    std::atomic<std::uint64_t> rejections{0};
};

// ---------------------------------------------------------------------------

MemoryCache::MemoryCache(Options options) : options_(options) {
    unsigned count = options_.shards == 0 ? 8u : options_.shards;
    count = std::min(count, 64u);

    // Prefer fewer, larger shards over many that cannot hold anything.
    const auto affordable =
        std::max<std::uint64_t>(1, options_.maxBytes / kMinShardBudgetBytes);
    count = static_cast<unsigned>(std::min<std::uint64_t>(count, affordable));
    count = floorPowerOfTwo(count);

    shards_.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        auto shard    = std::make_unique<Shard>();
        shard->budget = options_.maxBytes / count;
        shards_.push_back(std::move(shard));
    }
    shardMask_ = shards_.size() - 1;
}

MemoryCache::~MemoryCache() = default;

MemoryCache::Shard& MemoryCache::shardFor(std::string_view key) noexcept {
    return *shards_[StringHash{}(key) & shardMask_];
}

const MemoryCache::Shard& MemoryCache::shardFor(std::string_view key) const noexcept {
    return *shards_[StringHash{}(key) & shardMask_];
}

void MemoryCache::unlink(Shard& shard, Node* node) noexcept {
    if (node->prev != nullptr) node->prev->next = node->next;
    if (node->next != nullptr) node->next->prev = node->prev;
    if (shard.head == node) shard.head = node->next;
    if (shard.tail == node) shard.tail = node->prev;
    node->prev = nullptr;
    node->next = nullptr;
}

void MemoryCache::pushFront(Shard& shard, Node* node) noexcept {
    node->prev = nullptr;
    node->next = shard.head;
    if (shard.head != nullptr) shard.head->prev = node;
    shard.head = node;
    if (shard.tail == nullptr) shard.tail = node;
}

void MemoryCache::removeLocked(Shard& shard, Node* node) {
    // The key is copied first: erase() destroys the node, and passing it a
    // reference into the very object it is about to free is asking for trouble.
    const std::string key = node->key;
    shard.bytes -= node->bytes;
    unlink(shard, node);
    shard.map.erase(key);
}

bool MemoryCache::evictOneLocked(Shard& shard) {
    // Every reprieve clears the flag it consumed, so at worst one full pass
    // over the shard happens before something is genuinely evicted.
    std::size_t reprieves = shard.map.size();

    while (shard.tail != nullptr) {
        Node* victim = shard.tail;

        if (reprieves > 0 && victim->accessed.load(std::memory_order_relaxed)) {
            --reprieves;
            victim->accessed.store(false, std::memory_order_relaxed);
            unlink(shard, victim);
            pushFront(shard, victim);
            continue;
        }

        removeLocked(shard, victim);
        shard.evictions.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

std::size_t MemoryCache::purgeExpiredLocked(Shard& shard, std::int64_t ticks) {
    std::size_t removed = 0;
    for (auto it = shard.map.begin(); it != shard.map.end();) {
        Node* node = it->second.get();
        if (node->expiresAt > ticks) {
            ++it;
            continue;
        }
        shard.bytes -= node->bytes;
        unlink(shard, node);
        it = shard.map.erase(it);
        ++removed;
    }
    shard.expirations.fetch_add(removed, std::memory_order_relaxed);
    return removed;
}

// ---------------------------------------------------------------------------

CacheValuePtr MemoryCache::get(std::string_view key) {
    Shard& shard = shardFor(key);
    const std::int64_t ticks = nowTicks();

    {
        std::shared_lock lock(shard.mutex);
        const auto it = shard.map.find(key);
        if (it == shard.map.end()) {
            shard.misses.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }

        Node* node = it->second.get();
        if (node->expiresAt > ticks) {
            node->accessed.store(true, std::memory_order_relaxed);
            shard.hits.fetch_add(1, std::memory_order_relaxed);
            return node->value;
        }
    }

    // Expired. Drop it on the read that found it rather than leaving it for the
    // sweeper: an expired entry that is still being asked for is precisely the
    // one worth reclaiming, and this is the cold path anyway.
    {
        std::unique_lock lock(shard.mutex);
        const auto it = shard.map.find(key);
        if (it != shard.map.end() && it->second->expiresAt <= ticks) {
            removeLocked(shard, it->second.get());
            shard.expirations.fetch_add(1, std::memory_order_relaxed);
        }
    }

    shard.misses.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void MemoryCache::set(std::string_view key, CacheValuePtr value, std::chrono::seconds ttl) {
    if (value == nullptr) {
        del(key);
        return;
    }

    const std::uint64_t bytes =
        static_cast<std::uint64_t>(key.size()) + value->size() + kEntryOverheadBytes;
    const std::int64_t expiresAt = expiryFrom(ttl);

    Shard& shard = shardFor(key);
    std::unique_lock lock(shard.mutex);

    // A value larger than its whole shard would evict everything and still not
    // fit. Refuse it, and drop any older entry under the same key rather than
    // leaving a stale one behind.
    if (bytes > shard.budget) {
        const auto stale = shard.map.find(key);
        if (stale != shard.map.end()) removeLocked(shard, stale->second.get());
        shard.rejections.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (const auto it = shard.map.find(key); it != shard.map.end()) {
        Node* node = it->second.get();
        shard.bytes -= node->bytes;
        node->value     = std::move(value);
        node->bytes     = bytes;
        node->expiresAt = expiresAt;
        node->accessed.store(false, std::memory_order_relaxed);
        shard.bytes += bytes;
        unlink(shard, node);
        pushFront(shard, node);
    } else {
        // Both arguments to emplace() are independent locals: reading
        // owned->key while owned is being moved from would be a lifetime bug
        // with unspecified evaluation order.
        std::string keyCopy(key);
        auto        owned = std::make_unique<Node>();
        owned->key        = keyCopy;
        owned->value      = std::move(value);
        owned->bytes      = bytes;
        owned->expiresAt  = expiresAt;

        Node* raw = owned.get();
        shard.map.emplace(std::move(keyCopy), std::move(owned));
        pushFront(shard, raw);
        shard.bytes += bytes;
    }

    while (shard.bytes > shard.budget && evictOneLocked(shard)) {
    }
}

bool MemoryCache::del(std::string_view key) {
    Shard& shard = shardFor(key);
    std::unique_lock lock(shard.mutex);

    const auto it = shard.map.find(key);
    if (it == shard.map.end()) return false;

    removeLocked(shard, it->second.get());
    return true;
}

std::size_t MemoryCache::evict(std::uint64_t budgetBytes) {
    const std::uint64_t perShard = budgetBytes / shards_.size();
    const std::int64_t  ticks    = nowTicks();

    std::size_t removed = 0;
    for (auto& shard : shards_) {
        std::unique_lock lock(shard->mutex);

        // Expired entries first: they cost nothing to give up, so taking them
        // before the cold ones keeps something warm that is still valid.
        removed += purgeExpiredLocked(*shard, ticks);
        while (shard->bytes > perShard && evictOneLocked(*shard)) ++removed;
    }
    return removed;
}

void MemoryCache::clear() {
    for (auto& shard : shards_) {
        std::unique_lock lock(shard->mutex);
        shard->map.clear();
        shard->head  = nullptr;
        shard->tail  = nullptr;
        shard->bytes = 0;
    }
}

CacheStats MemoryCache::stats() const {
    CacheStats out;
    out.limitBytes = options_.maxBytes;

    for (const auto& shard : shards_) {
        out.hits += shard->hits.load(std::memory_order_relaxed);
        out.misses += shard->misses.load(std::memory_order_relaxed);
        out.evictions += shard->evictions.load(std::memory_order_relaxed);
        out.expirations += shard->expirations.load(std::memory_order_relaxed);
        out.rejections += shard->rejections.load(std::memory_order_relaxed);

        std::shared_lock lock(shard->mutex);
        out.entries += shard->map.size();
        out.bytes += shard->bytes;
    }
    return out;
}

}  // namespace monobucket
