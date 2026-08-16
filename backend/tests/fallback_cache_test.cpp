#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <string>
#include <thread>

#include "cache/fallback_cache.hpp"
#include "cache/memory_cache.hpp"

using monobucket::CacheError;
using monobucket::CacheProvider;
using monobucket::CacheStats;
using monobucket::CacheValuePtr;
using monobucket::FallbackCache;
using monobucket::kNoExpiry;
using monobucket::makeCacheValue;
using monobucket::MemoryCache;

namespace {

/// A stand-in for Redis whose availability the test controls.
///
/// This is the whole reason the breaker lives in its own class rather than
/// inside RedisCache: an outage is a thing to unit test, not a thing to arrange
/// by stopping a server in CI.
class FlakyCache final : public CacheProvider {
public:
    std::string_view name() const noexcept override { return "flaky"; }

    CacheValuePtr get(std::string_view key) override {
        ++calls;
        failIfDown("get");
        const auto it = entries.find(std::string(key));
        return it == entries.end() ? nullptr : it->second;
    }

    void set(std::string_view key, CacheValuePtr value, std::chrono::seconds) override {
        ++calls;
        failIfDown("set");
        entries[std::string(key)] = std::move(value);
    }

    bool del(std::string_view key) override {
        ++calls;
        failIfDown("del");
        return entries.erase(std::string(key)) > 0;
    }

    std::size_t evict(std::uint64_t) override { return 0; }

    void clear() override {
        ++calls;
        failIfDown("clear");
        entries.clear();
    }

    CacheStats stats() const override { return {}; }

    bool                                        up    = true;
    int                                         calls = 0;
    std::map<std::string, CacheValuePtr>        entries;

private:
    void failIfDown(const char* op) const {
        if (!up) throw CacheError(std::string("flaky: ") + op + " is down");
    }
};

MemoryCache::Options localOptions() {
    MemoryCache::Options opts;
    opts.maxBytes = 4 * 1024 * 1024;
    opts.shards   = 1;
    return opts;
}

FallbackCache::Options fastBreaker() {
    FallbackCache::Options opts;
    opts.failureThreshold = 2;
    // Short enough to keep the suite quick, long enough that a sanitizer build
    // scheduling us late cannot lapse a window the test expects to still be
    // open.
    opts.initialCooldown = std::chrono::milliseconds(100);
    opts.maxCooldown     = std::chrono::milliseconds(400);
    opts.localTtlCap      = std::chrono::seconds(60);
    return opts;
}

/// Keeps a borrowed pointer to the shared tier so the test can take it down.
struct Fixture {
    FlakyCache*                    shared = nullptr;
    std::unique_ptr<FallbackCache> cache;

    explicit Fixture(FallbackCache::Options options = fastBreaker()) {
        auto primary = std::make_unique<FlakyCache>();
        shared       = primary.get();
        cache        = std::make_unique<FallbackCache>(
            std::move(primary), std::make_unique<MemoryCache>(localOptions()), options);
    }
};

}  // namespace

TEST_CASE("a value written through lands in both tiers", "[cache][fallback]") {
    Fixture fixture;

    fixture.cache->put("k", "value", kNoExpiry);

    CHECK(fixture.shared->entries.count("k") == 1);
    CHECK(*fixture.cache->get("k") == "value");
}

TEST_CASE("a local hit does not touch the shared tier", "[cache][fallback]") {
    Fixture fixture;

    fixture.cache->put("k", "value", kNoExpiry);
    const int afterWrite = fixture.shared->calls;

    for (int i = 0; i < 5; ++i) CHECK(fixture.cache->get("k") != nullptr);

    // The point of the local tier: reads that hit it never leave the process.
    CHECK(fixture.shared->calls == afterWrite);
}

TEST_CASE("a shared hit populates the local tier", "[cache][fallback]") {
    Fixture fixture;

    // Placed directly in the shared tier, as another instance would have.
    fixture.shared->entries["k"] = makeCacheValue("from-elsewhere");

    CHECK(*fixture.cache->get("k") == "from-elsewhere");
    const int afterFirst = fixture.shared->calls;

    CHECK(*fixture.cache->get("k") == "from-elsewhere");
    CHECK(fixture.shared->calls == afterFirst);
}

TEST_CASE("a shared-tier outage is served from the local tier", "[cache][fallback]") {
    Fixture fixture;

    fixture.cache->put("k", "value", kNoExpiry);
    fixture.shared->up = false;

    // The read never fails, and never turns into an error the caller has to
    // handle. A cache outage is not a storage outage.
    CHECK_NOTHROW(fixture.cache->get("k"));
    CHECK(*fixture.cache->get("k") == "value");
}

TEST_CASE("a write survives the shared tier being down", "[cache][fallback]") {
    Fixture fixture;
    fixture.shared->up = false;

    CHECK_NOTHROW(fixture.cache->put("k", "value", kNoExpiry));
    CHECK(*fixture.cache->get("k") == "value");
}

TEST_CASE("the breaker stops calling a shared tier that keeps failing",
          "[cache][fallback]") {
    Fixture fixture;
    fixture.shared->up = false;

    // Two failures trip it (failureThreshold = 2). Each attempt is a local
    // miss first, so each one reaches the shared tier.
    fixture.cache->get("a");
    fixture.cache->get("b");
    CHECK_FALSE(fixture.cache->sharedHealthy());

    const int afterTrip = fixture.shared->calls;
    for (int i = 0; i < 20; ++i) fixture.cache->get("miss-" + std::to_string(i));

    // Retrying a dead socket on every request would satisfy "never fails" while
    // still stalling every request. It must genuinely stop calling.
    CHECK(fixture.shared->calls == afterTrip);
    CHECK(fixture.cache->bypassCount() >= 20);
}

TEST_CASE("the breaker probes and recovers once the shared tier returns",
          "[cache][fallback]") {
    Fixture fixture;
    fixture.shared->up = false;

    fixture.cache->get("a");
    fixture.cache->get("b");
    REQUIRE_FALSE(fixture.cache->sharedHealthy());

    fixture.shared->up               = true;
    fixture.shared->entries["later"] = makeCacheValue("value");

    // Nothing gets through until the cooldown lapses.
    CHECK(fixture.cache->get("later") == nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // The first request after the cooldown carries the probe, which succeeds
    // and closes the breaker.
    CHECK(fixture.cache->get("later") != nullptr);
    CHECK(fixture.cache->sharedHealthy());
}

TEST_CASE("a failed probe backs off further", "[cache][fallback]") {
    Fixture fixture;
    fixture.shared->up = false;

    fixture.cache->get("a");
    fixture.cache->get("b");
    REQUIRE_FALSE(fixture.cache->sharedHealthy());

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // One probe goes out and fails, doubling the window to 200ms.
    const int beforeProbe = fixture.shared->calls;
    fixture.cache->get("still-down");
    CHECK(fixture.shared->calls == beforeProbe + 1);

    // The original 100ms window would already have elapsed here; the doubled
    // one has not.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const int afterProbe = fixture.shared->calls;
    fixture.cache->get("still-down");
    CHECK(fixture.shared->calls == afterProbe);
}

TEST_CASE("the local copy of a shared value is capped, ttl or not",
          "[cache][fallback]") {
    auto options        = fastBreaker();
    options.localTtlCap = std::chrono::seconds(30);
    Fixture fixture(options);

    fixture.cache->put("forever", "value", kNoExpiry);
    fixture.shared->up = false;

    // Still readable now — the cap is 30s, not 0.
    CHECK(fixture.cache->get("forever") != nullptr);

    // With the cap at zero the local copy would be immortal, which is the bug
    // the cap exists to prevent; with a lapsed one it is already gone.
    auto lapsed        = fastBreaker();
    lapsed.localTtlCap = std::chrono::seconds(-1);
    Fixture expired(lapsed);
    expired.cache->put("forever", "value", kNoExpiry);
    expired.shared->up = false;
    CHECK(expired.cache->get("forever") == nullptr);
}

TEST_CASE("a delete reaches both tiers", "[cache][fallback]") {
    Fixture fixture;

    fixture.cache->put("k", "value", kNoExpiry);
    CHECK(fixture.cache->del("k"));

    CHECK(fixture.shared->entries.count("k") == 0);
    CHECK(fixture.cache->get("k") == nullptr);
}

TEST_CASE("a delete during an outage still clears the local tier",
          "[cache][fallback]") {
    Fixture fixture;

    fixture.cache->put("k", "value", kNoExpiry);
    fixture.shared->up = false;

    CHECK(fixture.cache->del("k"));
    CHECK(fixture.cache->get("k") == nullptr);
}

TEST_CASE("stats report the ratio a request actually experienced",
          "[cache][fallback]") {
    Fixture fixture;

    fixture.shared->entries["remote"] = makeCacheValue("value");

    fixture.cache->get("remote");   // local miss, shared hit -> one hit
    fixture.cache->get("nowhere");  // miss in both -> one miss

    const auto stats = fixture.cache->stats();
    CHECK(stats.hits == 1);
    CHECK(stats.misses == 1);

    // Summing the tiers would have counted the local miss that preceded the
    // shared hit, reporting a ratio no caller saw.
    CHECK(stats.hitRatio() == 0.5);
    CHECK(stats.healthy);
}

TEST_CASE("stats report the shared tier as unhealthy while it is bypassed",
          "[cache][fallback]") {
    Fixture fixture;
    fixture.shared->up = false;

    fixture.cache->get("a");
    fixture.cache->get("b");

    const auto stats = fixture.cache->stats();
    CHECK_FALSE(stats.healthy);
    CHECK(stats.errors >= 2);
}

TEST_CASE("the reported name names both tiers", "[cache][fallback]") {
    Fixture fixture;
    CHECK(fixture.cache->name() == "flaky+memory");
}
