#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "cache/memory_cache.hpp"

using monobucket::CacheValuePtr;
using monobucket::kNoExpiry;
using monobucket::makeCacheValue;
using monobucket::MemoryCache;

namespace {

/// Big enough that the shard count is not silently reduced, small enough that
/// eviction is reachable in a test.
MemoryCache::Options options(std::uint64_t bytes, unsigned shards = 1) {
    MemoryCache::Options opts;
    opts.maxBytes = bytes;
    opts.shards   = shards;
    return opts;
}

std::string valueOf(const CacheValuePtr& ptr) { return ptr == nullptr ? std::string() : *ptr; }

}  // namespace

TEST_CASE("a stored value comes back", "[cache]") {
    MemoryCache cache(options(4 * 1024 * 1024));

    CHECK(cache.get("absent") == nullptr);

    cache.put("greeting", "hello", kNoExpiry);
    CHECK(valueOf(cache.get("greeting")) == "hello");

    const auto stats = cache.stats();
    CHECK(stats.hits == 1);
    CHECK(stats.misses == 1);
    CHECK(stats.entries == 1);
    CHECK(stats.bytes > 5);  // the payload plus per-entry overhead
}

TEST_CASE("storing the same key twice replaces rather than accumulates", "[cache]") {
    MemoryCache cache(options(4 * 1024 * 1024));

    cache.put("k", "first", kNoExpiry);
    const auto afterFirst = cache.stats().bytes;

    cache.put("k", "second", kNoExpiry);

    CHECK(valueOf(cache.get("k")) == "second");
    CHECK(cache.stats().entries == 1);
    // "second" is one byte longer than "first"; anything larger means the old
    // entry's bytes were never given back.
    CHECK(cache.stats().bytes == afterFirst + 1);
}

TEST_CASE("deleting removes the entry and its bytes", "[cache]") {
    MemoryCache cache(options(4 * 1024 * 1024));

    cache.put("k", "value", kNoExpiry);
    CHECK(cache.del("k"));
    CHECK_FALSE(cache.del("k"));

    CHECK(cache.get("k") == nullptr);
    CHECK(cache.stats().entries == 0);
    CHECK(cache.stats().bytes == 0);
}

TEST_CASE("a null value is a delete", "[cache]") {
    MemoryCache cache(options(4 * 1024 * 1024));

    cache.put("k", "value", kNoExpiry);
    cache.set("k", nullptr, kNoExpiry);

    CHECK(cache.get("k") == nullptr);
    CHECK(cache.stats().entries == 0);
}

// The central claim of the whole layer: memory stays where it was configured.
TEST_CASE("the byte budget is held on insert, not by a timer", "[cache]") {
    // One shard so the budget is exact rather than divided.
    MemoryCache cache(options(64 * 1024, 1));

    const std::string payload(1024, 'x');
    for (int i = 0; i < 500; ++i) {
        cache.put("key-" + std::to_string(i), payload, kNoExpiry);

        // Checked on every iteration, not once at the end: a cache that
        // overshoots and then trims has still allocated the overshoot.
        REQUIRE(cache.stats().bytes <= 64 * 1024);
    }

    const auto stats = cache.stats();
    CHECK(stats.evictions > 0);
    CHECK(stats.entries > 0);
    CHECK(stats.entries < 500);
}

TEST_CASE("a value larger than the budget is refused, not accepted then evicted", "[cache]") {
    MemoryCache cache(options(1024 * 1024, 1));

    cache.put("small", "keep me", kNoExpiry);
    cache.put("huge", std::string(2 * 1024 * 1024, 'x'), kNoExpiry);

    CHECK(cache.get("huge") == nullptr);
    CHECK(cache.stats().rejections == 1);

    // Accepting it first would have emptied the cache to make room for
    // something that could never fit.
    CHECK(valueOf(cache.get("small")) == "keep me");
}

TEST_CASE("an oversized value does not leave a stale entry behind", "[cache]") {
    MemoryCache cache(options(1024 * 1024, 1));

    cache.put("k", "old", kNoExpiry);
    cache.put("k", std::string(2 * 1024 * 1024, 'x'), kNoExpiry);

    // Returning "old" here would be worse than a miss: the caller asked for the
    // new value to be cached and would be served the previous one.
    CHECK(cache.get("k") == nullptr);
}

TEST_CASE("recently read entries survive eviction", "[cache]") {
    // Roughly a dozen entries fit, so twenty-two inserts guarantee eviction.
    MemoryCache cache(options(8 * 1024, 1));

    const std::string payload(512, 'x');
    cache.put("hot", payload, kNoExpiry);
    cache.put("cold", payload, kNoExpiry);

    // Reading "hot" sets its flag; the second-chance pass must then take
    // "cold" first even though "cold" was inserted more recently.
    for (int round = 0; round < 20; ++round) {
        CHECK(cache.get("hot") != nullptr);
        cache.put("filler-" + std::to_string(round), payload, kNoExpiry);
    }

    CHECK(cache.get("hot") != nullptr);
    CHECK(cache.get("cold") == nullptr);
}

TEST_CASE("an expired entry reads as a miss and is dropped", "[cache]") {
    MemoryCache cache(options(4 * 1024 * 1024));

    cache.put("brief", "value", std::chrono::seconds(1));
    CHECK(valueOf(cache.get("brief")) == "value");

    // The expiry is stored as a deadline, so re-storing with a lapsed TTL is
    // equivalent to waiting — and does not put a second of latency in the suite.
    cache.put("brief", "value", std::chrono::seconds(-1));

    CHECK(cache.get("brief") == nullptr);
    CHECK(cache.stats().entries == 0);
    CHECK(cache.stats().expirations == 1);
}

TEST_CASE("a lapsed ttl is not mistaken for no expiry", "[cache]") {
    MemoryCache cache(options(4 * 1024 * 1024));

    cache.put("forever", "value", kNoExpiry);
    CHECK(cache.get("forever") != nullptr);
    CHECK(cache.get("forever") != nullptr);
    CHECK(cache.stats().expirations == 0);
}

TEST_CASE("the periodic sweep collects expired entries nobody read again", "[cache]") {
    MemoryCache cache(options(4 * 1024 * 1024));

    cache.put("live", "value", kNoExpiry);
    for (int i = 0; i < 10; ++i) {
        cache.put("dead-" + std::to_string(i), "value", std::chrono::seconds(-1));
    }
    REQUIRE(cache.stats().entries == 11);

    // Passing the configured budget: nothing is over it, so anything removed
    // was removed for having expired.
    CHECK(cache.evict(4 * 1024 * 1024) == 10);
    CHECK(cache.stats().entries == 1);
    CHECK(cache.get("live") != nullptr);
}

TEST_CASE("evict shrinks the cache below a smaller budget", "[cache]") {
    MemoryCache cache(options(1024 * 1024, 1));

    const std::string payload(1024, 'x');
    for (int i = 0; i < 100; ++i) cache.put("key-" + std::to_string(i), payload, kNoExpiry);
    REQUIRE(cache.stats().bytes > 32 * 1024);

    cache.evict(32 * 1024);
    CHECK(cache.stats().bytes <= 32 * 1024);
}

TEST_CASE("clear empties the cache without disturbing the budget", "[cache]") {
    MemoryCache cache(options(4 * 1024 * 1024));

    cache.put("a", "1", kNoExpiry);
    cache.put("b", "2", kNoExpiry);
    cache.clear();

    CHECK(cache.stats().entries == 0);
    CHECK(cache.stats().bytes == 0);

    cache.put("c", "3", kNoExpiry);
    CHECK(valueOf(cache.get("c")) == "3");
}

// A value handed to a reader must stay valid even if eviction takes the entry
// out from under them on another thread. This is why values are shared_ptr.
TEST_CASE("a value outlives the entry that held it", "[cache]") {
    MemoryCache cache(options(1024 * 1024, 1));

    cache.put("k", "durable", kNoExpiry);
    const CacheValuePtr held = cache.get("k");
    REQUIRE(held != nullptr);

    cache.clear();

    CHECK(*held == "durable");
}

TEST_CASE("the shard count is reduced rather than leaving shards with no room", "[cache]") {
    // 2 MiB across 32 shards would give each 64 KiB, which holds almost
    // nothing. Two shards is the honest answer.
    MemoryCache cache(options(2 * 1024 * 1024, 32));
    CHECK(cache.shardCount() == 2);

    // A budget that cannot fill even one shard still gets one.
    MemoryCache tiny(options(64 * 1024, 8));
    CHECK(tiny.shardCount() == 1);
}

TEST_CASE("the shard count is a power of two", "[cache]") {
    MemoryCache cache(options(256 * 1024 * 1024, 30));
    const unsigned shards = cache.shardCount();
    CHECK(shards == 16);
    CHECK((shards & (shards - 1)) == 0);
}

TEST_CASE("the hit ratio is defined before the first lookup", "[cache]") {
    MemoryCache cache(options(4 * 1024 * 1024));
    CHECK(cache.stats().hitRatio() == 0.0);

    cache.put("k", "v", kNoExpiry);
    cache.get("k");
    cache.get("miss");
    CHECK(cache.stats().hitRatio() == 0.5);
}

TEST_CASE("concurrent readers and writers do not corrupt the cache", "[cache]") {
    MemoryCache cache(options(8 * 1024 * 1024, 8));

    constexpr int kThreads = 8;
    constexpr int kRounds  = 2000;

    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, &failed, t] {
            for (int i = 0; i < kRounds; ++i) {
                const std::string key = "key-" + std::to_string((t * kRounds + i) % 512);
                cache.put(key, std::string(64, static_cast<char>('a' + t)), monobucket::kNoExpiry);

                if (const auto value = cache.get(key)) {
                    // Any value is acceptable — another thread may have
                    // overwritten it — but it must be intact.
                    if (value->size() != 64) failed = true;
                }
                if (i % 97 == 0) cache.del(key);
                if (i % 401 == 0) cache.evict(4 * 1024 * 1024);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    CHECK_FALSE(failed.load());
    CHECK(cache.stats().bytes <= 8 * 1024 * 1024);

    // Accounting must still add up after all that: emptying the cache has to
    // bring the byte count back to exactly zero, not merely close to it.
    cache.clear();
    CHECK(cache.stats().bytes == 0);
    CHECK(cache.stats().entries == 0);
}
