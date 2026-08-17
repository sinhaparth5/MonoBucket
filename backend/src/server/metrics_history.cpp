#include "server/metrics_history.hpp"

#include <algorithm>

namespace monobucket {
namespace {

/// Counters only ever climb within a process, so a smaller reading means the
/// source was reset rather than that time ran backwards. Reporting 0 keeps one
/// flat point on the graph; reporting the wrapped difference would draw a spike
/// of billions.
std::uint64_t delta(std::uint64_t current, std::uint64_t previous) noexcept {
    return current >= previous ? current - previous : 0;
}

}  // namespace

MetricsHistory::MetricsHistory(std::size_t capacity, std::uint32_t intervalSeconds)
    : ring_(capacity == 0 ? 1 : capacity), intervalSeconds_(intervalSeconds) {}

void MetricsHistory::record(const Reading& reading) {
    const std::lock_guard<std::mutex> guard(mutex_);

    if (!hasPrevious_) {
        previous_    = reading;
        hasPrevious_ = true;
        return;
    }

    Sample sample;
    sample.atMs   = reading.atMs;
    sample.spanMs = static_cast<std::uint64_t>(std::max<std::int64_t>(0, reading.atMs - previous_.atMs));

    sample.requests     = delta(reading.requests, previous_.requests);
    sample.succeeded    = delta(reading.succeeded, previous_.succeeded);
    sample.clientErrors = delta(reading.clientErrors, previous_.clientErrors);
    sample.serverErrors = delta(reading.serverErrors, previous_.serverErrors);
    sample.shed         = delta(reading.shed, previous_.shed);
    sample.bytesIn      = delta(reading.bytesIn, previous_.bytesIn);
    sample.bytesOut     = delta(reading.bytesOut, previous_.bytesOut);
    sample.cacheHits    = delta(reading.cacheHits, previous_.cacheHits);
    sample.cacheMisses  = delta(reading.cacheMisses, previous_.cacheMisses);

    sample.objects       = reading.objects;
    sample.storedBytes   = reading.storedBytes;
    sample.residentBytes = reading.residentBytes;
    sample.cacheBytes    = reading.cacheBytes;
    sample.ioQueued      = reading.ioQueued;
    sample.ioActive      = reading.ioActive;

    ring_[next_] = sample;
    next_        = (next_ + 1) % ring_.size();
    if (next_ == 0) wrapped_ = true;

    previous_ = reading;
}

std::vector<MetricsHistory::Sample> MetricsHistory::samples() const {
    const std::lock_guard<std::mutex> guard(mutex_);

    std::vector<Sample> out;
    if (!wrapped_) {
        out.assign(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(next_));
        return out;
    }

    out.reserve(ring_.size());
    out.insert(out.end(), ring_.begin() + static_cast<std::ptrdiff_t>(next_), ring_.end());
    out.insert(out.end(), ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(next_));
    return out;
}

}  // namespace monobucket
