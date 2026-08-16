#include "core/io_executor.hpp"

#include <algorithm>

#include "core/logging.hpp"

namespace monobucket {

IoExecutor::IoExecutor(unsigned threads, std::size_t queueLimit)
    : queueLimit_(std::max<std::size_t>(queueLimit, 1)) {
    const unsigned count = std::max(1u, threads);
    threads_.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        threads_.emplace_back([this] { worker(); });
    }
    log::debug("io executor started with ", count, " threads, queue limit ", queueLimit_);
}

IoExecutor::~IoExecutor() { stop(); }

bool IoExecutor::post(std::function<void()> task) {
    {
        std::lock_guard guard(mutex_);
        if (stopping_ || queue_.size() >= queueLimit_) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        queue_.push_back(std::move(task));
    }
    available_.notify_one();
    return true;
}

void IoExecutor::stop() {
    {
        std::lock_guard guard(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    // Queued work is drained rather than discarded: a task in flight may hold a
    // half-written payload that still needs its cleanup to run.
    available_.notify_all();

    for (auto& thread : threads_) {
        if (thread.joinable()) thread.join();
    }
    threads_.clear();
}

void IoExecutor::worker() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock guard(mutex_);
            available_.wait(guard, [this] { return stopping_ || !queue_.empty(); });

            if (queue_.empty()) {
                if (stopping_) return;
                continue;  // spurious wakeup
            }

            task = std::move(queue_.front());
            queue_.pop_front();
        }

        active_.fetch_add(1, std::memory_order_relaxed);
        try {
            task();
        } catch (const std::exception& ex) {
            // A task that throws must not take the pool down with it; the
            // request it belongs to has already been failed by its own handler.
            log::error("io task failed: ", ex.what());
        } catch (...) {
            log::error("io task failed with an unknown exception");
        }
        active_.fetch_sub(1, std::memory_order_relaxed);
        completed_.fetch_add(1, std::memory_order_relaxed);
    }
}

IoExecutor::Stats IoExecutor::stats() const {
    Stats stats;
    {
        std::lock_guard guard(mutex_);
        stats.queued  = queue_.size();
        stats.threads = static_cast<unsigned>(threads_.size());
        stats.limit   = queueLimit_;
    }
    stats.active    = active_.load(std::memory_order_relaxed);
    stats.completed = completed_.load(std::memory_order_relaxed);
    stats.rejected  = rejected_.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace monobucket
