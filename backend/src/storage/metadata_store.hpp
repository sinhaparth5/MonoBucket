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

/// How many audit entries the store keeps. A few megabytes at the sizes these
/// records actually reach, which is the budget a log that lives beside the
/// object metadata deserves — anything more belongs in whatever collects this
/// server's stdout.
///
/// Raised from 5000 when console object uploads and deletes started being
/// recorded. Those arrive per click rather than per account change, so the same
/// number of entries would have covered a much shorter stretch of time — and
/// the window in *time* is what makes the log answer "what happened last
/// Tuesday". Deliberately not raised further: S3 object writes are not recorded
/// at all, so what lands here is still bounded by what a person can do through
/// a browser.
inline constexpr std::size_t kAuditCapacity = 20000;

/// Conditions the S3 layer has to distinguish, so they are modelled as codes
/// rather than as message text. Phase 4 maps each onto an S3 error code.
enum class StorageErrorCode {
    NoSuchBucket,
    NoSuchKey,
    NoSuchUpload,
    BucketAlreadyExists,
    BucketNotEmpty,
    InvalidPart,

    /// A write does not fit in what is left of its bucket's allocation.
    QuotaExceeded,

    /// An allocation would be set below what the bucket already holds.
    QuotaBelowUsage,

    /// An allocation would put the instance over its allocatable capacity.
    InsufficientCapacity,

    /// A write is larger than the instance's maximum object-upload size.
    ///
    /// Distinct from QuotaExceeded on purpose: a bucket that is full is a
    /// capacity problem somebody fixes by deleting or reallocating, and an
    /// object over the upload limit is a policy refusal that no amount of free
    /// space changes. Clients branch on the S3 code these map to, and those
    /// two codes are different.
    ObjectTooLarge,

    /// A checksum the client supplied does not match what was stored or
    /// assembled. Distinct from Corruption, which is the store disagreeing with
    /// itself: this one is the client and the store disagreeing, and only the
    /// client can act on it.
    ChecksumMismatch,

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

        /// How large that object was, or zero when the key was new.
        ///
        /// Reported by the commit that released it rather than read
        /// separately, because a bucket's usage is the running sum of these
        /// deltas: a size read outside the lock that committed the change is a
        /// size another writer may already have replaced, and the drift would
        /// be permanent.
        std::uint64_t replacedBytes = 0;
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
        std::uint64_t              releasedBytes = 0;
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

    /// In progress uploads for one bucket, ordered by key then upload id —
    /// which is the order the row keys already sort in, so a page resumes by
    /// seeking rather than by counting.
    virtual ListUploadsResult listUploads(std::string_view          bucket,
                                          const ListUploadsRequest& request) = 0;

    /// Uploads begun before `cutoff`, across every bucket, capped at `limit`.
    ///
    /// Keyed off the `U<uploadId>` index rather than the per-bucket one so a
    /// sweep is a single scan instead of one per bucket. `createdAt` is a
    /// filter, not the verdict: an upload that has been receiving parts for a
    /// week is old by this measure and not abandoned, which is why the caller
    /// consults the parts before aborting anything.
    virtual std::vector<UploadRecord> listUploadsCreatedBefore(std::size_t limit,
                                                               TimestampMs cutoff) = 0;

    struct PutPartOutcome {
        /// Re-uploading a part number replaces it; the previous payload is
        /// released here.
        std::optional<std::string> releasedBlobId;
        std::uint64_t              replacedBytes = 0;
    };

    /// Throws NoSuchUpload.
    virtual PutPartOutcome putPart(std::string_view uploadId, const PartRecord& part,
                                   Durability durability) = 0;

    /// Ascending by part number. Throws NoSuchUpload.
    virtual std::vector<PartRecord> listParts(std::string_view uploadId) = 0;

    struct AbortOutcome {
        std::vector<std::string> releasedBlobIds;

        /// The parts' total size, so the bucket's pending charge can be
        /// dropped by exactly what the same commit released.
        std::uint64_t releasedBytes = 0;

        /// Which bucket the upload belonged to. The caller has to charge
        /// somewhere, and after this returns there is no record left to ask.
        std::string bucket;
    };

    /// Discards the upload and every part. Returns all released payloads.
    /// Throws NoSuchUpload.
    virtual AbortOutcome abortUpload(std::string_view uploadId, Durability durability) = 0;

    struct CompleteOutcome {
        std::optional<std::string> releasedBlobId;  ///< object this replaced
        std::uint64_t              replacedBytes = 0;
        std::vector<std::string>   releasedPartBlobIds;
        std::uint64_t              releasedPartBytes = 0;
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

    virtual std::optional<UserRecord> getUser(std::string_view username) = 0;

    /// Ordered by username, which is the order the console renders. The user
    /// list is small by construction — it is people, not objects — so this
    /// returns all of them rather than a page.
    virtual std::vector<UserRecord> listUsers() = 0;

    /// Creates an account or replaces it wholesale. Callers read, modify and
    /// write back; there is no partial update, because a partial update of a
    /// record that carries a role is a way to change a role by accident.
    virtual void putUser(const UserRecord& user) = 0;

    /// Returns false when there was no such account.
    virtual bool deleteUser(std::string_view username) = 0;

    /// How many administrators are enabled right now.
    ///
    /// Exists so that "this would remove the last administrator" can be
    /// answered before the write rather than discovered after it. A scan, but
    /// of the user keyspace only, and only on a mutation path — the alternative
    /// is a maintained counter that can disagree with the records it counts.
    virtual std::size_t countEnabledAdministrators() = 0;

    /// The single administrator account that predates per-user identities, if
    /// this data directory still has one. Read by startup migration and by
    /// nothing else.
    virtual std::optional<AdminRecord> getAdmin() = 0;

    /// Writes the legacy record. Only startup does, and only to establish the
    /// account on a store that has never been opened by a build with users —
    /// after which the same startup migrates it. Kept so a downgrade to the
    /// previous release still finds an account it understands.
    virtual void putAdmin(const AdminRecord& admin) = 0;

    /// Drops the legacy record once it has been migrated. Idempotent.
    virtual void deleteAdmin() = 0;

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

    // --- Instance settings -------------------------------------------------

    /// The instance-wide policy figures an operator changes while the server
    /// is running, rather than by restarting it with a different environment.
    ///
    /// One record, not a bag of key-value pairs. A generic settings table is a
    /// place for a value to be written that nothing reads and nothing
    /// validates; a struct is a value the compiler knows the shape of, and a
    /// field added to it has to be given a meaning here before it can be
    /// stored.
    struct InstanceSettings {
        /// The largest object this instance accepts. Zero means the operator
        /// has never set one, which is what a store written before this
        /// existed reads back as — the server then falls back to the figure
        /// the environment seeded.
        std::uint64_t maxUploadBytes = 0;
    };

    /// Nothing when this store has never carried settings.
    virtual std::optional<InstanceSettings> getInstanceSettings() = 0;

    /// Replaces them. Written durably regardless of MONOBUCKET_DURABILITY: it
    /// is one small record written when a person clicks a button, and a limit
    /// that quietly reverted across a restart would be a limit nobody could
    /// trust they had set.
    virtual void putInstanceSettings(const InstanceSettings& settings) = 0;

    // --- Audit -------------------------------------------------------------

    /// Appends one security event and assigns its sequence number.
    ///
    /// The log is a bounded ring: once it holds `kAuditCapacity` entries, each
    /// append drops the oldest. Bounded rather than growing without limit for
    /// the same reason every other queue here is — a refused request can be
    /// generated as fast as a client can send one, and an unbounded log turns
    /// that into unbounded disk.
    virtual std::uint64_t appendAudit(const AuditRecord& entry) = 0;

    /// The most recent entries, newest first, at most `limit` of them.
    virtual std::vector<AuditRecord> listAudit(std::size_t limit) = 0;

    // --- Introspection -----------------------------------------------------

    /// Counters maintained incrementally, not scanned per call — `/metrics` is
    /// scraped far too often to walk the keyspace.
    virtual UsageStats usage() const = 0;

    /// Stored object bytes and in-progress part bytes per bucket, as of the
    /// scan this store did when it opened.
    ///
    /// Read once, by the quota ledger, which maintains it incrementally from
    /// then on. Recomputing it per request would mean walking every object to
    /// answer a question the console asks on every page load.
    struct BucketCharge {
        std::uint64_t objectBytes = 0;
        std::uint64_t partBytes   = 0;
    };
    virtual std::vector<std::pair<std::string, BucketCharge>> bucketCharges() const = 0;

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
