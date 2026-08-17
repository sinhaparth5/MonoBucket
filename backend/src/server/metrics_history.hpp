#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace monobucket {

/// A fixed-size ring of counter samples, so the console can draw a rate without
/// a time-series database behind it.
///
/// The ring is allocated once and never grows: a dashboard left open overnight
/// costs exactly what one opened a second ago costs. Retention is capacity ×
/// interval and both are constants rather than knobs — anyone who wants a
/// longer window already has /metrics and a scraper that keeps history properly.
///
/// Counters are stored as differences between consecutive readings, computed
/// here rather than in the browser: the caller reads raw cumulative counters
/// and never has to reason about where the previous value came from.
class MetricsHistory {
public:
    /// Cumulative counters and instantaneous gauges, exactly as read from the
    /// live sources. Cumulative fields become deltas in the stored sample;
    /// gauge fields are copied through.
    struct Reading {
        std::int64_t atMs = 0;

        std::uint64_t requests     = 0;
        std::uint64_t succeeded    = 0;
        std::uint64_t clientErrors = 0;
        std::uint64_t serverErrors = 0;
        std::uint64_t shed         = 0;
        std::uint64_t bytesIn      = 0;
        std::uint64_t bytesOut     = 0;
        std::uint64_t cacheHits    = 0;
        std::uint64_t cacheMisses  = 0;

        std::uint64_t objects       = 0;
        std::uint64_t storedBytes   = 0;
        std::uint64_t residentBytes = 0;
        std::uint64_t cacheBytes    = 0;
        std::uint64_t ioQueued      = 0;
        std::uint64_t ioActive      = 0;
    };

    /// One point on the console's graphs. Counter fields hold what happened
    /// during `spanMs`, not since startup.
    struct Sample {
        std::int64_t  atMs   = 0;
        std::uint64_t spanMs = 0;

        std::uint64_t requests     = 0;
        std::uint64_t succeeded    = 0;
        std::uint64_t clientErrors = 0;
        std::uint64_t serverErrors = 0;
        std::uint64_t shed         = 0;
        std::uint64_t bytesIn      = 0;
        std::uint64_t bytesOut     = 0;
        std::uint64_t cacheHits    = 0;
        std::uint64_t cacheMisses  = 0;

        std::uint64_t objects       = 0;
        std::uint64_t storedBytes   = 0;
        std::uint64_t residentBytes = 0;
        std::uint64_t cacheBytes    = 0;
        std::uint64_t ioQueued      = 0;
        std::uint64_t ioActive      = 0;
    };

    MetricsHistory(std::size_t capacity, std::uint32_t intervalSeconds);

    /// Stores the difference against the previous reading. The first reading
    /// establishes a baseline and produces no sample — a delta against zero
    /// would render startup as one enormous spike.
    void record(const Reading& reading);

    /// Oldest first. Copies, because the caller renders JSON on the event loop
    /// and must not hold the lock while it does.
    std::vector<Sample> samples() const;

    std::uint32_t intervalSeconds() const noexcept { return intervalSeconds_; }
    std::size_t   capacity() const noexcept { return ring_.size(); }

private:
    mutable std::mutex  mutex_;
    std::vector<Sample> ring_;
    std::size_t         next_    = 0;
    bool                wrapped_ = false;

    Reading previous_{};
    bool    hasPrevious_ = false;

    std::uint32_t intervalSeconds_;
};

}  // namespace monobucket
