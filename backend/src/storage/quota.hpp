#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Per-bucket storage allocations, and the arithmetic that keeps them honest.
//
// The ledger — not RocksDB — is the authority on whether a write is admitted.
// That is forced rather than chosen: admitting a write means comparing a
// bucket's charge against its allocation and then committing, and the two have
// to happen without another writer slipping between them. RocksDB has no
// compare-and-commit that spans a payload write, and there is exactly one
// writer process by design (see the note on `bucketLock_` in the RocksDB
// store), so a mutex over an in-memory tally is both sufficient and the only
// thing that is actually atomic here.
//
// Nothing in the ledger is persisted. Allocations live in the bucket records;
// the charges are re-derived at startup from the metadata scan that already
// walks every object. A counter written beside the records it counts is a
// counter that can disagree with them, and there is no way to tell which one
// is wrong.

namespace monobucket {

/// One bucket's allocation and what is charged against it.
struct BucketCapacity {
    /// The bucket's allocation. Zero means unlimited — the state a bucket
    /// created by plain S3 CreateBucket lands in when no default allocation is
    /// configured, and the state every bucket written before allocations
    /// existed is read back in.
    std::uint64_t quotaBytes = 0;

    /// Committed object payloads.
    std::uint64_t usedBytes = 0;

    /// Multipart parts that have been stored but whose upload has not
    /// completed. Charged, because the bytes are on disk and an abandoned
    /// upload holds them until a client says otherwise — but never counted as
    /// *used*, because no object names them yet.
    std::uint64_t pendingBytes = 0;

    /// Writes in flight. Released when the write commits or fails.
    std::uint64_t reservedBytes = 0;

    bool unlimited() const noexcept { return quotaBytes == 0; }

    /// Everything the bucket is answerable for right now.
    std::uint64_t chargedBytes() const noexcept {
        return usedBytes + pendingBytes + reservedBytes;
    }

    /// What is left of the allocation, or nothing when the bucket is
    /// unlimited. Saturates at zero: an allocation reduced to below what is
    /// already stored is over-full, not negatively remaining.
    std::optional<std::uint64_t> remainingBytes() const noexcept;
};

/// The instance's allocatable capacity and how much of it has been handed out.
struct InstanceCapacity {
    /// What may be allocated to buckets in total, after the operational
    /// reserve is taken off the filesystem.
    std::uint64_t allocatableBytes = 0;

    /// The sum of every bucket's allocation. Unlimited buckets contribute
    /// nothing, because there is no number to contribute.
    std::uint64_t allocatedBytes = 0;

    /// Stored object bytes across every bucket, allocated or not.
    std::uint64_t usedBytes = 0;

    /// How many buckets carry no allocation. Reported rather than hidden: with
    /// one of these present, `remainingBytes()` is an upper bound and not a
    /// guarantee, and the console says so.
    std::uint64_t unlimitedBuckets = 0;

    /// Allocatable minus allocated, saturating at zero.
    std::uint64_t remainingBytes() const noexcept;
};

/// The tally, and the admission decisions taken against it.
class QuotaLedger {
public:
    /// A claim on a bucket's remaining allocation, held for as long as the
    /// write that made it is in flight.
    ///
    /// RAII because every write path here throws: a claim that had to be given
    /// back by hand would be leaked by the first refused digest, and the leak
    /// would only ever surface as a bucket that quietly stopped accepting
    /// writes with no stored bytes to explain it.
    class Reservation {
    public:
        Reservation() = default;
        ~Reservation() { release(); }

        Reservation(Reservation&& other) noexcept { *this = std::move(other); }
        Reservation& operator=(Reservation&& other) noexcept;

        Reservation(const Reservation&)            = delete;
        Reservation& operator=(const Reservation&) = delete;

        std::uint64_t      bytes() const noexcept { return bytes_; }
        const std::string& bucket() const noexcept { return bucket_; }
        bool               held() const noexcept { return ledger_ != nullptr; }

        /// Gives the claim back without recording a write. Idempotent, and
        /// what the destructor does.
        void release() noexcept;

    private:
        friend class QuotaLedger;

        QuotaLedger*  ledger_ = nullptr;
        std::string   bucket_;
        std::uint64_t bytes_ = 0;
    };

    /// `allocatableBytes` is what the whole instance may hand out. Zero
    /// disables the instance-wide ceiling; per-bucket allocations still apply.
    explicit QuotaLedger(std::uint64_t allocatableBytes = 0)
        : allocatableBytes_(allocatableBytes) {}

    QuotaLedger(const QuotaLedger&)            = delete;
    QuotaLedger& operator=(const QuotaLedger&) = delete;

    // --- Membership --------------------------------------------------------

    /// Establishes a bucket's tally from a startup scan.
    void seed(std::string bucket, std::uint64_t quotaBytes, std::uint64_t usedBytes,
              std::uint64_t pendingBytes);

    /// Starts tracking a bucket that has just been created.
    void track(std::string bucket, std::uint64_t quotaBytes);

    /// Stops tracking a deleted bucket. A bucket can only be deleted while
    /// empty, so nothing is charged when this runs.
    void forget(std::string_view bucket);

    // --- Allocations -------------------------------------------------------

    /// Replaces a bucket's allocation.
    ///
    /// Throws `StorageErrorCode::QuotaBelowUsage` when the new allocation is
    /// under what the bucket already holds — a reduction that stranded stored
    /// data would leave a bucket that could neither accept a write nor explain
    /// itself — and `StorageErrorCode::InsufficientCapacity` when it would put
    /// the instance over its allocatable capacity.
    void setQuota(std::string_view bucket, std::uint64_t quotaBytes);

    /// Checks that `quotaBytes` could be allocated to a bucket that does not
    /// exist yet, without allocating it. Throws `InsufficientCapacity`.
    void admitAllocation(std::uint64_t quotaBytes) const;

    // --- Admission ---------------------------------------------------------

    /// Claims `bytes` of the bucket's remaining allocation.
    ///
    /// Throws `StorageErrorCode::QuotaExceeded` when the claim does not fit.
    /// An unknown bucket is admitted unconditionally: the mutation that
    /// follows reports NoSuchBucket with the context the caller needs, and
    /// answering here would move that report somewhere it reads as a quota
    /// problem.
    [[nodiscard]] Reservation reserve(std::string_view bucket, std::uint64_t bytes);

    /// Raises an existing claim to cover `bytes` in total, admitting only the
    /// difference. For a body whose length was not declared up front, where
    /// what arrived is only known once it has.
    void extend(Reservation& reservation, std::string_view bucket, std::uint64_t bytes);

    // --- Settlement --------------------------------------------------------
    //
    // Each of these consumes the claim and applies the true delta, which is
    // never assumed: `replacedBytes` comes back from the same commit that
    // released the payload it refers to, so an overwrite cannot drift.

    /// An object was published. `replacedBytes` is the size of the object it
    /// overwrote, or zero.
    ///
    /// The bucket is named separately rather than taken from the claim: a
    /// zero-length object reserves nothing, and an empty object is a perfectly
    /// ordinary thing for a client to store.
    void recordObject(std::string_view bucket, Reservation reservation,
                      std::uint64_t storedBytes, std::uint64_t replacedBytes);

    /// A multipart part was stored. `replacedBytes` is the size of the part it
    /// replaced, or zero.
    void recordPart(std::string_view bucket, Reservation reservation, std::uint64_t storedBytes,
                    std::uint64_t replacedBytes);

    /// An object was deleted.
    void recordDeletion(std::string_view bucket, std::uint64_t releasedBytes);

    /// An upload was abandoned and its parts released.
    void recordUploadAborted(std::string_view bucket, std::uint64_t releasedPartBytes);

    /// An upload became an object. The parts stop being pending and the
    /// assembled payload becomes used, in one step, because they are the same
    /// bytes and a bucket at its allocation would otherwise be unable to
    /// finish an upload it had already been charged for.
    void recordUploadCompleted(std::string_view bucket, std::uint64_t storedBytes,
                               std::uint64_t replacedBytes, std::uint64_t releasedPartBytes);

    // --- Reporting ---------------------------------------------------------

    BucketCapacity capacity(std::string_view bucket) const;

    /// Every tracked bucket, ordered by name so the console renders stably.
    std::vector<std::pair<std::string, BucketCapacity>> all() const;

    InstanceCapacity instance() const;

    std::uint64_t allocatableBytes() const noexcept { return allocatableBytes_; }

private:
    struct Entry {
        std::uint64_t quotaBytes    = 0;
        std::uint64_t usedBytes     = 0;
        std::uint64_t pendingBytes  = 0;
        std::uint64_t reservedBytes = 0;

        std::uint64_t chargedBytes() const noexcept {
            return usedBytes + pendingBytes + reservedBytes;
        }
    };

    /// The tally for `bucket`, created unlimited if it is not there.
    ///
    /// Created rather than refused so that a bucket the startup scan somehow
    /// missed is still accounted, just not enforced. Dropping the arithmetic
    /// instead would make the miss invisible until the numbers were wrong.
    Entry& entryFor(std::string_view bucket);

    /// Gives `bytes` of `bucket`'s reservation back. The counterpart to
    /// `reserve`, called by Reservation.
    void giveBack(const std::string& bucket, std::uint64_t bytes) noexcept;

    /// Drops a claim from `entry` and disarms it, with the lock already held.
    ///
    /// Settlement has to release the claim and apply the charge without
    /// unlocking in between. Releasing first and charging second is what would
    /// let a second writer be admitted into space the first is about to
    /// occupy, which is exactly the over-allocation this ledger exists to
    /// prevent.
    static void consumeLocked(Entry& entry, Reservation& reservation) noexcept;

    /// Sum of allocations, ignoring `except`. Caller holds the lock.
    std::uint64_t allocatedLocked(std::string_view except) const;

    /// One mutex, not a stripe set. It is held for a handful of additions over
    /// a map sized by the bucket count — which is people-scale, not
    /// object-scale — and every decision it guards has to see every other
    /// bucket's allocation anyway.
    mutable std::mutex mutex_;

    /// Transparent so a `string_view` bucket name looks up without allocating
    /// a `std::string` on every admission decision.
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view name) const noexcept {
            return std::hash<std::string_view>{}(name);
        }
    };

    std::unordered_map<std::string, Entry, NameHash, std::equal_to<>> buckets_;

    std::uint64_t allocatableBytes_ = 0;
};

}  // namespace monobucket
