#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "storage/records.hpp"

// A consistent copy of a store that is still being written to.
//
// The alternative was "stop the server, copy the data directory, start it
// again". Copying it live races the metadata against the payload tree and
// produces a store whose rows name payloads the copy does not contain — which
// is indistinguishable from corruption, and is reported as such by --fsck.
//
// Two things make this cheap. RocksDB's checkpoint API writes a coherent
// point-in-time copy of the metadata using hard links, and the payload tree is
// content-addressed and immutable, so its files can be hard-linked too. On one
// filesystem a checkpoint of a terabyte costs a few thousand directory entries
// and no data blocks at all.

namespace monobucket {

class BlobStore;
class MetadataStore;

struct CheckpointReport {
    std::filesystem::path destination;

    std::uint64_t payloadsLinked = 0;

    /// Non-zero only when the destination is on a different filesystem, where
    /// a hard link is impossible and the bytes have to be duplicated. Kept
    /// separate from the link count because the two have wildly different
    /// costs and an operator should be told which one they just paid.
    std::uint64_t payloadsCopied = 0;
    std::uint64_t bytesCopied    = 0;

    TimestampMs takenAtMs = 0;
    std::int64_t elapsedMs = 0;

    /// True when every payload was linked rather than copied.
    bool instant() const noexcept { return payloadsCopied == 0; }
};

/// Refuses a destination before anything is written to it.
///
/// Throws StorageError when it already exists and is not an empty directory,
/// when its parent is missing, or when it cannot be written. Separate from
/// writeCheckpoint so a caller can refuse a bad request without having taken
/// the metadata checkpoint first — a half-written backup directory is worse
/// than none, because it looks like one.
void requireUsableDestination(const std::filesystem::path& destination);

/// Writes `metadata` and `blobs` into `destination` as a startable data
/// directory.
///
/// The metadata is checkpointed **first** and the payloads linked second. That
/// ordering is the same rule the write path follows and is load-bearing for the
/// same reason: a payload written after the metadata snapshot is simply absent
/// from a copy that never names it, whereas a payload linked before the
/// snapshot could be named by a row the snapshot then failed to include.
///
/// It is only half of the guarantee. Nothing here stops a *delete* landing
/// between the two passes and unlinking a payload the snapshot still names —
/// that is why StorageEngine::checkpoint() holds off reclamation for the
/// duration, and why this function is not the public entry point.
CheckpointReport writeCheckpoint(MetadataStore& metadata, const BlobStore& blobs,
                                 const std::filesystem::path& destination);

}  // namespace monobucket
