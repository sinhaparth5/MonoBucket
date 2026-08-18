#include "storage/quota.hpp"

#include <algorithm>
#include <utility>

#include "storage/metadata_store.hpp"

namespace monobucket {
namespace {

/// Subtraction that cannot wrap.
///
/// Every charge here is decremented by a number that came back from the commit
/// that released it, so the arithmetic should never go negative. Should is not
/// the same as does, and a tally that wrapped to 2^64 would refuse every
/// subsequent write to the bucket with no way to explain why.
std::uint64_t take(std::uint64_t from, std::uint64_t amount) noexcept {
    return from > amount ? from - amount : 0;
}

std::string bytesPhrase(std::uint64_t bytes) { return std::to_string(bytes) + " bytes"; }

}  // namespace

std::optional<std::uint64_t> BucketCapacity::remainingBytes() const noexcept {
    if (unlimited()) return std::nullopt;
    return take(quotaBytes, chargedBytes());
}

std::uint64_t InstanceCapacity::remainingBytes() const noexcept {
    return take(allocatableBytes, allocatedBytes);
}

// --- Reservation -----------------------------------------------------------

QuotaLedger::Reservation& QuotaLedger::Reservation::operator=(Reservation&& other) noexcept {
    if (this == &other) return *this;
    release();
    ledger_ = std::exchange(other.ledger_, nullptr);
    bucket_ = std::move(other.bucket_);
    bytes_  = std::exchange(other.bytes_, 0);
    return *this;
}

void QuotaLedger::Reservation::release() noexcept {
    if (ledger_ == nullptr) return;
    ledger_->giveBack(bucket_, bytes_);
    ledger_ = nullptr;
    bytes_  = 0;
}

// --- Membership ------------------------------------------------------------

void QuotaLedger::seed(std::string bucket, std::uint64_t quotaBytes, std::uint64_t usedBytes,
                       std::uint64_t pendingBytes) {
    const std::lock_guard guard(mutex_);
    Entry&                entry = buckets_[std::move(bucket)];
    entry.quotaBytes            = quotaBytes;
    entry.usedBytes             = usedBytes;
    entry.pendingBytes          = pendingBytes;
    entry.reservedBytes         = 0;
}

void QuotaLedger::track(std::string bucket, std::uint64_t quotaBytes) {
    const std::lock_guard guard(mutex_);
    Entry&                entry = buckets_[std::move(bucket)];
    entry.quotaBytes            = quotaBytes;
}

void QuotaLedger::forget(std::string_view bucket) {
    const std::lock_guard guard(mutex_);
    if (const auto it = buckets_.find(bucket); it != buckets_.end()) buckets_.erase(it);
}

// --- Allocations -----------------------------------------------------------

std::uint64_t QuotaLedger::allocatedLocked(std::string_view except) const {
    std::uint64_t total = 0;
    for (const auto& [name, entry] : buckets_) {
        if (name == except) continue;
        total += entry.quotaBytes;
    }
    return total;
}

void QuotaLedger::setQuota(std::string_view bucket, std::uint64_t quotaBytes) {
    const std::lock_guard guard(mutex_);

    Entry& entry = entryFor(bucket);

    // A reduction is checked against everything the bucket is answerable for,
    // reservations included. Checking only committed bytes would let an
    // allocation be cut out from under a write that had already been admitted.
    if (quotaBytes != 0 && quotaBytes < entry.chargedBytes()) {
        throw StorageError(StorageErrorCode::QuotaBelowUsage,
                           "bucket '" + std::string(bucket) + "' already holds " +
                               bytesPhrase(entry.chargedBytes()) +
                               ", which is more than the requested allocation of " +
                               bytesPhrase(quotaBytes));
    }

    if (allocatableBytes_ != 0 && quotaBytes != 0) {
        const std::uint64_t others = allocatedLocked(bucket);
        if (others + quotaBytes > allocatableBytes_) {
            throw StorageError(
                StorageErrorCode::InsufficientCapacity,
                "an allocation of " + bytesPhrase(quotaBytes) + " leaves the instance over its " +
                    bytesPhrase(allocatableBytes_) + " allocatable capacity; " +
                    bytesPhrase(take(allocatableBytes_, others)) + " remain unallocated");
        }
    }

    entry.quotaBytes = quotaBytes;
}

void QuotaLedger::admitAllocation(std::uint64_t quotaBytes) const {
    if (allocatableBytes_ == 0 || quotaBytes == 0) return;

    const std::lock_guard guard(mutex_);
    const std::uint64_t   allocated = allocatedLocked({});
    if (allocated + quotaBytes > allocatableBytes_) {
        throw StorageError(
            StorageErrorCode::InsufficientCapacity,
            "an allocation of " + bytesPhrase(quotaBytes) + " exceeds the " +
                bytesPhrase(take(allocatableBytes_, allocated)) +
                " still unallocated of this instance's " + bytesPhrase(allocatableBytes_) +
                " allocatable capacity");
    }
}

// --- Admission -------------------------------------------------------------

QuotaLedger::Entry& QuotaLedger::entryFor(std::string_view bucket) {
    if (const auto it = buckets_.find(bucket); it != buckets_.end()) return it->second;
    return buckets_.emplace(std::string(bucket), Entry{}).first->second;
}

QuotaLedger::Reservation QuotaLedger::reserve(std::string_view bucket, std::uint64_t bytes) {
    Reservation reservation;
    extend(reservation, bucket, bytes);
    return reservation;
}

void QuotaLedger::extend(Reservation& reservation, std::string_view bucket, std::uint64_t bytes) {
    if (reservation.held() && reservation.bucket() != bucket) {
        // Only ever a programming error: a reservation belongs to the write
        // that made it, and that write knows one bucket.
        throw StorageError(StorageErrorCode::Internal,
                           "a storage reservation for bucket '" + reservation.bucket() +
                               "' was extended against bucket '" + std::string(bucket) + "'");
    }
    if (bytes <= reservation.bytes()) return;

    const std::uint64_t wanted = bytes - reservation.bytes();

    {
        const std::lock_guard guard(mutex_);

        Entry& entry = entryFor(bucket);
        if (entry.quotaBytes != 0) {
            const std::uint64_t charged = entry.chargedBytes();
            if (charged + wanted > entry.quotaBytes) {
                throw StorageError(StorageErrorCode::QuotaExceeded,
                                   "bucket '" + std::string(bucket) + "' has " +
                                       bytesPhrase(take(entry.quotaBytes, charged)) +
                                       " left of its " + bytesPhrase(entry.quotaBytes) +
                                       " allocation, and this write needs " + bytesPhrase(wanted));
            }
        }
        entry.reservedBytes += wanted;
    }

    reservation.ledger_ = this;
    reservation.bucket_ = std::string(bucket);
    reservation.bytes_  = bytes;
}

void QuotaLedger::giveBack(const std::string& bucket, std::uint64_t bytes) noexcept {
    if (bytes == 0) return;
    const std::lock_guard guard(mutex_);
    if (const auto it = buckets_.find(bucket); it != buckets_.end()) {
        it->second.reservedBytes = take(it->second.reservedBytes, bytes);
    }
}

// --- Settlement ------------------------------------------------------------

void QuotaLedger::consumeLocked(Entry& entry, Reservation& reservation) noexcept {
    if (!reservation.held()) return;
    entry.reservedBytes = take(entry.reservedBytes, reservation.bytes_);
    reservation.ledger_ = nullptr;
    reservation.bytes_  = 0;
}

void QuotaLedger::recordObject(std::string_view bucket, Reservation reservation,
                               std::uint64_t storedBytes, std::uint64_t replacedBytes) {
    const std::lock_guard guard(mutex_);
    Entry&                entry = entryFor(bucket);
    consumeLocked(entry, reservation);
    entry.usedBytes = take(entry.usedBytes + storedBytes, replacedBytes);
}

void QuotaLedger::recordPart(std::string_view bucket, Reservation reservation,
                             std::uint64_t storedBytes, std::uint64_t replacedBytes) {
    const std::lock_guard guard(mutex_);
    Entry&                entry = entryFor(bucket);
    consumeLocked(entry, reservation);
    entry.pendingBytes = take(entry.pendingBytes + storedBytes, replacedBytes);
}

void QuotaLedger::recordDeletion(std::string_view bucket, std::uint64_t releasedBytes) {
    if (releasedBytes == 0) return;
    const std::lock_guard guard(mutex_);
    Entry&                entry = entryFor(bucket);
    entry.usedBytes             = take(entry.usedBytes, releasedBytes);
}

void QuotaLedger::recordUploadAborted(std::string_view bucket, std::uint64_t releasedPartBytes) {
    if (releasedPartBytes == 0) return;
    const std::lock_guard guard(mutex_);
    Entry&                entry = entryFor(bucket);
    entry.pendingBytes          = take(entry.pendingBytes, releasedPartBytes);
}

void QuotaLedger::recordUploadCompleted(std::string_view bucket, std::uint64_t storedBytes,
                                        std::uint64_t replacedBytes,
                                        std::uint64_t releasedPartBytes) {
    const std::lock_guard guard(mutex_);
    Entry&                entry = entryFor(bucket);
    entry.pendingBytes          = take(entry.pendingBytes, releasedPartBytes);
    entry.usedBytes             = take(entry.usedBytes + storedBytes, replacedBytes);
}

// --- Reporting -------------------------------------------------------------

BucketCapacity QuotaLedger::capacity(std::string_view bucket) const {
    const std::lock_guard guard(mutex_);

    BucketCapacity capacity;
    if (const auto it = buckets_.find(bucket); it != buckets_.end()) {
        capacity.quotaBytes    = it->second.quotaBytes;
        capacity.usedBytes     = it->second.usedBytes;
        capacity.pendingBytes  = it->second.pendingBytes;
        capacity.reservedBytes = it->second.reservedBytes;
    }
    return capacity;
}

std::vector<std::pair<std::string, BucketCapacity>> QuotaLedger::all() const {
    const std::lock_guard guard(mutex_);

    std::vector<std::pair<std::string, BucketCapacity>> out;
    out.reserve(buckets_.size());
    for (const auto& [name, entry] : buckets_) {
        out.emplace_back(name, BucketCapacity{entry.quotaBytes, entry.usedBytes, entry.pendingBytes,
                                              entry.reservedBytes});
    }
    std::sort(out.begin(), out.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    return out;
}

InstanceCapacity QuotaLedger::instance() const {
    const std::lock_guard guard(mutex_);

    InstanceCapacity capacity;
    capacity.allocatableBytes = allocatableBytes_;
    for (const auto& [name, entry] : buckets_) {
        capacity.allocatedBytes += entry.quotaBytes;
        capacity.usedBytes += entry.usedBytes;
        if (entry.quotaBytes == 0) ++capacity.unlimitedBuckets;
    }
    return capacity;
}

}  // namespace monobucket
