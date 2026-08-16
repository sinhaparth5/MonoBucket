#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace monobucket {

/// A bounded thread pool for blocking filesystem work.
///
/// Drogon's handlers run on the event loop, and a pread() that stalls there
/// stops every other connection that loop owns. Storage operations are
/// therefore synchronous and testable in isolation, and the request layer
/// hands them here.
///
/// The queue is bounded, which is the point. An unbounded queue converts a disk
/// that cannot keep up into unbounded memory growth — the exact failure this
/// project exists to avoid. When the queue is full, submission fails and the
/// caller sheds load (S3 has a wire representation for this: 503 SlowDown).
///
/// io_uring would remove the threads entirely on Linux, at the cost of a
/// liburing dependency and a second I/O path to keep correct. The roadmap
/// sanctions this thread-pooled form as the fallback; the interface here is
/// what a future io_uring backend would implement.
class IoExecutor {
public:
    IoExecutor(unsigned threads, std::size_t queueLimit);
    ~IoExecutor();

    IoExecutor(const IoExecutor&)            = delete;
    IoExecutor& operator=(const IoExecutor&) = delete;

    /// Queues work. Returns false when the queue is at its limit or the pool is
    /// stopping — callers must handle rejection rather than assume acceptance.
    [[nodiscard]] bool post(std::function<void()> task);

    /// Stops accepting work, drains what is queued, and joins the threads.
    /// Idempotent; the destructor calls it.
    void stop();

    struct Stats {
        std::uint64_t queued    = 0;  ///< waiting right now
        std::uint64_t active    = 0;  ///< executing right now
        std::uint64_t completed = 0;
        std::uint64_t rejected  = 0;  ///< refused because the queue was full
        unsigned      threads   = 0;
        std::size_t   limit     = 0;
    };

    Stats stats() const;

private:
    void worker();

    mutable std::mutex                mutex_;
    std::condition_variable           available_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread>          threads_;
    std::size_t                       queueLimit_;
    bool                              stopping_ = false;

    std::atomic<std::uint64_t> active_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> rejected_{0};
};

}  // namespace monobucket
