#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "core/io_executor.hpp"

using monobucket::IoExecutor;

TEST_CASE("posted work runs", "[io]") {
    IoExecutor executor(2, 16);

    std::atomic<int> completed{0};
    for (int i = 0; i < 8; ++i) {
        CHECK(executor.post([&completed] { completed.fetch_add(1); }));
    }

    executor.stop();  // drains before returning
    CHECK(completed.load() == 8);
    CHECK(executor.stats().completed == 8);
}

TEST_CASE("a full queue rejects rather than growing", "[io]") {
    // One thread, held hostage, so the queue is the only place work can go.
    IoExecutor executor(1, 2);

    std::mutex              mutex;
    std::condition_variable release;
    bool                    released = false;

    CHECK(executor.post([&] {
        std::unique_lock guard(mutex);
        release.wait(guard, [&] { return released; });
    }));

    // Wait for the blocker to be picked up, otherwise it may still be queued.
    for (int i = 0; i < 200 && executor.stats().active == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(executor.stats().active == 1);

    CHECK(executor.post([] {}));
    CHECK(executor.post([] {}));

    // This is the load-shedding point. An unbounded queue would accept this and
    // convert a slow disk into unbounded memory growth — the exact failure the
    // project exists to avoid. Phase 4 turns the false into 503 SlowDown.
    CHECK_FALSE(executor.post([] {}));
    CHECK(executor.stats().rejected == 1);

    {
        std::lock_guard guard(mutex);
        released = true;
    }
    release.notify_all();
    executor.stop();
}

TEST_CASE("a throwing task does not take the pool down", "[io]") {
    IoExecutor executor(1, 8);

    CHECK(executor.post([] { throw std::runtime_error("disk on fire"); }));

    std::atomic<bool> ranAfter{false};
    CHECK(executor.post([&ranAfter] { ranAfter.store(true); }));

    executor.stop();
    CHECK(ranAfter.load());
}

TEST_CASE("queued work is drained rather than discarded", "[io]") {
    // A task in flight may hold a half-written payload whose cleanup still has
    // to run, so shutdown finishes the queue instead of dropping it.
    IoExecutor executor(2, 64);

    std::atomic<int> completed{0};
    for (int i = 0; i < 32; ++i) {
        CHECK(executor.post([&completed] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            completed.fetch_add(1);
        }));
    }

    executor.stop();
    CHECK(completed.load() == 32);
}

TEST_CASE("stopping twice is harmless", "[io]") {
    IoExecutor executor(1, 4);
    executor.stop();
    CHECK_NOTHROW(executor.stop());
    CHECK_FALSE(executor.post([] {}));  // no longer accepting
}
