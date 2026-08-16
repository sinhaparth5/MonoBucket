#pragma once

#include <atomic>
#include <cstdint>

// Counters for the S3 listener.
//
// Plain relaxed atomics rather than anything cleverer: these are incremented on
// every request and read only by /metrics, so the only ordering that matters is
// that they do not tear. A per-thread sharded counter would be faster to write
// and slower to reason about, for a number nobody reads more than once a
// scrape.

namespace monobucket::s3 {

struct S3Metrics {
    std::atomic<std::uint64_t> requests{0};

    /// Answered 2xx. Requests minus this and the error counters is the number
    /// still in flight, which is worth being able to derive.
    std::atomic<std::uint64_t> succeeded{0};

    /// Answered 4xx. A client's problem, not ours — but a spike means someone's
    /// credentials or clock have broken.
    std::atomic<std::uint64_t> clientErrors{0};

    /// Answered 5xx. Ours.
    std::atomic<std::uint64_t> serverErrors{0};

    /// Rejected before any work was done because the storage queue was full.
    /// Distinct from serverErrors: shedding load is working as designed, and
    /// alerting on it should be a different rule.
    std::atomic<std::uint64_t> shed{0};

    /// Signature verification failures, anonymous requests to private
    /// resources included. The one counter worth alerting on for its own sake.
    std::atomic<std::uint64_t> authFailures{0};

    /// Requests served without credentials against a public bucket.
    std::atomic<std::uint64_t> anonymous{0};

    /// Object payload bytes, excluding protocol overhead — these are meant to
    /// be comparable with the object sizes in storage, not with NIC counters.
    std::atomic<std::uint64_t> bytesIn{0};
    std::atomic<std::uint64_t> bytesOut{0};

    void observe(int status) noexcept {
        if (status >= 500) {
            serverErrors.fetch_add(1, std::memory_order_relaxed);
        } else if (status >= 400) {
            clientErrors.fetch_add(1, std::memory_order_relaxed);
        } else {
            succeeded.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

}  // namespace monobucket::s3
