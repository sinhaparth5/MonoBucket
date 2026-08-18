#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "storage/records.hpp"

namespace monobucket {

/// Conditions the S3 layer has to distinguish, so they are modelled as codes
/// rather than as message text. Phase 4 maps each onto an S3 error code.
enum class StorageErrorCode {
    NoSuchBucket,
    NoSuchKey,
    NoSuchUpload,
    BucketAlreadyExists,
    BucketNotEmpty,
    InvalidPart,
    Corruption,
    Io,
    Internal,
};

std::string_view toString(StorageErrorCode code);

class StorageError : public std::runtime_error {
public:
    StorageError(StorageErrorCode code, const std::string& what)
        : std::runtime_error(what), code_(code) {}

    StorageErrorCode code() const noexcept { return code_; }

private:
    StorageErrorCode code_;
};

/// Transactional metadata for buckets, objects and multipart uploads.
///
/// Mutations that publish or retire object data take the `Durability` the
/// owning bucket resolved to, because a bucket may override the server setting
/// and only the caller knows which bucket a write belongs to. `Strict` syncs
/// the write-ahead log before the call returns. The log is shared across
/// buckets, so a strict write also happens to flush whatever its neighbours had
/// pending — every bucket gets at least what it asked for, sometimes more.
///
/// The other contract that matters is blob ownership. Every mutation that drops a
/// reference to a payload *returns* the blob id it released, in the same call
/// that atomically committed the metadata change. The caller is then obliged to
/// hand it back for reclamation. Deleting the payload first, or forgetting to,
/// is how object stores end up either corrupt or full — so the interface makes
/// the released blob impossible to overlook.
class MetadataStore {
public:
    virtual ~MetadataStore() = default;

    // --- Buckets -----------------------------------------------------------

    /// Throws BucketAlreadyExists. S3 requires this to be idempotent for the
    /// owner in some regions; that policy decision belongs in Phase 4, not here.
    virtual void createBucket(const BucketRecord& bucket) = 0;

    virtual std::optional<BucketRecord> getBucket(std::string_view name) = 0;

    /// Ordered by name, which is the order ListBuckets reports.
    virtual std::vector<BucketRecord> listBuckets() = 0;

    /// Throws NoSuchBucket, or BucketNotEmpty when objects or in-progress
    /// uploads remain. S3 refuses to delete a non-empty bucket.
    virtual void deleteBucket(std::string_view name) = 0;

    /// Replaces the mutable fields of an existing bucket (policy, public read).
    virtual void updateBucket(const BucketRecord& bucket) = 0;

    // --- Objects -----------------------------------------------------------

    struct PutOutcome {
        /// The payload of the object this PUT overwrote, if any. Now
        /// unreferenced and owed to the reclaimer.
        std::optional<std::string> releasedBlobId;
    };

    /// Makes the object visible. Throws NoSuchBucket.
    ///
    /// The payload must already be durable on disk before this is called —
    /// that ordering is the whole reason a partially written object is never
    /// observable.
    virtual PutOutcome putObject(std::string_view bucket, const ObjectRecord& object,
                                 Durability durability) = 0;

    virtual std::optional<ObjectRecord> getObject(std::string_view bucket,
                                                  std::string_view key) = 0;

    struct DeleteOutcome {
        bool                       existed = false;
        std::optional<std::string> releasedBlobId;
    };

    /// Deleting a key that does not exist is not an error: S3 returns 204
    /// either way.
    virtual DeleteOutcome deleteObject(std::string_view bucket, std::string_view key,
                                       Durability durability) = 0;

    /// Prefix, delimiter and pagination, in the exact lexicographic order S3
    /// specifies. Throws NoSuchBucket.
    virtual ListObjectsResult listObjects(std::string_view          bucket,
                                          const ListObjectsRequest& request) = 0;

    // --- Multipart uploads -------------------------------------------------

    /// Throws NoSuchBucket. The upload id must be unique and unguessable.
    virtual void createUpload(const UploadRecord& upload) = 0;

    virtual std::optional<UploadRecord> getUpload(std::string_view uploadId) = 0;

    /// In progress uploads for one bucket, ordered by key then upload id.
    virtual std::vector<UploadRecord> listUploads(std::string_view bucket,
                                                  std::uint32_t    maxUploads) = 0;

    struct PutPartOutcome {
        /// Re-uploading a part number replaces it; the previous payload is
        /// released here.
        std::optional<std::string> releasedBlobId;
    };

    /// Throws NoSuchUpload.
    virtual PutPartOutcome putPart(std::string_view uploadId, const PartRecord& part,
                                   Durability durability) = 0;

    /// Ascending by part number. Throws NoSuchUpload.
    virtual std::vector<PartRecord> listParts(std::string_view uploadId) = 0;

    /// Discards the upload and every part. Returns all released payloads.
    /// Throws NoSuchUpload.
    virtual std::vector<std::string> abortUpload(std::string_view uploadId,
                                                 Durability       durability) = 0;

    struct CompleteOutcome {
        std::optional<std::string> releasedBlobId;  ///< object this replaced
        std::vector<std::string>   releasedPartBlobIds;
    };

    /// Atomically retires the upload and publishes `object` in its place.
    ///
    /// `object.blobId` must already hold the assembled payload. The part
    /// payloads are released, not deleted, for the same reason as everywhere
    /// else: the metadata commit is the point of no return, and unlinking
    /// happens strictly after it.
    virtual CompleteOutcome completeUpload(std::string_view bucket, std::string_view uploadId,
                                           const ObjectRecord& object,
                                           Durability          durability) = 0;

    // --- Blob reclamation --------------------------------------------------

    /// Records a blob id as unreferenced *before* its payload is written.
    ///
    /// This is what makes a crash mid-PUT recoverable in bounded time. Without
    /// it, a process that dies between writing the payload and committing the
    /// metadata leaves a file that nothing points at, and finding it again
    /// would mean walking the entire object tree. Tracked at birth, the blob is
    /// already on the reclamation list; the metadata commit is what removes it.
    virtual void trackBlob(std::string_view blobId) = 0;

    /// Blobs awaiting unlink, capped at `limit`, restricted to those queued at
    /// or before `queuedAtOrBefore`.
    ///
    /// The age filter is not an optimisation. A blob is tracked *before* its
    /// payload is written, so an in-flight upload is on this list for as long as
    /// it takes to arrive. Reclaiming one would find no file to unlink, drop the
    /// tracking record, and leave the upload's payload untracked — a permanent
    /// leak if the process then died. The periodic sweeper therefore only
    /// considers records older than any upload could plausibly be; startup
    /// recovery, where nothing is in flight, passes the current time.
    virtual std::vector<std::string> listOrphans(std::size_t limit,
                                                 TimestampMs queuedAtOrBefore) = 0;

    /// Drops reclamation records once the payloads are gone.
    virtual void forgetOrphans(const std::vector<std::string>& blobIds) = 0;

    // --- Identity ----------------------------------------------------------

    // Credential records are written with Durability::Strict regardless of the
    // server setting. Everything else in the store can be re-derived or re-sent
    // by a client; a secret the console displayed exactly once cannot be, and a
    // credential that survived being shown but not a power cut is worse than
    // one that was never issued.

    virtual std::optional<AdminRecord> getAdmin() = 0;

    /// Creates the administrator or replaces its verifier. There is exactly one
    /// account, so this is a write, not an insert.
    virtual void putAdmin(const AdminRecord& admin) = 0;

    virtual std::optional<AccessKeyRecord> getAccessKey(std::string_view accessKeyId) = 0;

    /// Ordered by access key id, which is the order the console renders.
    virtual std::vector<AccessKeyRecord> listAccessKeys() = 0;

    /// Issues a new credential or replaces the secret of an existing one.
    virtual void putAccessKey(const AccessKeyRecord& key) = 0;

    /// Revocation is a delete rather than a flag. A tombstone would have to be
    /// consulted on the S3 hot path for the rest of the store's life to answer
    /// a question the absence of the record already answers.
    ///
    /// Returns false when there was nothing to revoke.
    virtual bool deleteAccessKey(std::string_view accessKeyId) = 0;

    // --- Introspection -----------------------------------------------------

    /// Counters maintained incrementally, not scanned per call — `/metrics` is
    /// scraped far too often to walk the keyspace.
    virtual UsageStats usage() const = 0;

    /// Engine-specific gauges for `/metrics`, already namespaced by the caller.
    virtual std::vector<std::pair<std::string, std::uint64_t>> engineGauges() const = 0;

    /// Flushes in-memory state so a subsequent open sees it. Called on shutdown.
    virtual void flush() = 0;

    virtual std::string_view engineName() const noexcept = 0;
};

struct MetadataStoreOptions {
    std::string path;

    /// Total budget for block cache *and* memtables together. RocksDB will
    /// otherwise size both independently and the sum is what the container sees.
    std::uint64_t memoryBudgetBytes = 32ull * 1024 * 1024;

    /// fsync the write-ahead log on every commit that does not carry its own
    /// durability — bucket rows, upload creation and reclamation bookkeeping.
    /// Object writes pass the level their bucket resolved to instead.
    bool syncWrites = false;

    /// Bounds the table-reader memory that open SST files pin.
    int maxOpenFiles = 256;
};

/// Opens (creating if needed) the RocksDB-backed store.
std::unique_ptr<MetadataStore> openRocksMetadataStore(const MetadataStoreOptions& options);

}  // namespace monobucket
