#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "storage/blob_store.hpp"
#include "storage/metadata_store.hpp"
#include "storage/quota.hpp"
#include "storage/records.hpp"

namespace monobucket {

struct Config;

/// Payloads and metadata, joined by the one rule that makes the pair durable:
/// a payload is written and flushed before the metadata that names it is
/// committed, and a payload is unlinked only after the metadata that named it
/// is gone. An interruption at any point therefore leaves either the previous
/// state or the new one, plus at worst a file that reclamation will collect.
class StorageEngine {
public:
    struct Options {
        std::filesystem::path dataDir;
        Durability            durability        = Durability::Relaxed;
        std::size_t           chunkBytes        = 1024 * 1024;
        std::uint64_t         metadataMemoryBytes = 32ull * 1024 * 1024;
        int                   maxOpenFiles      = 256;

        /// How long a tracked-but-unreferenced payload is left alone before the
        /// sweeper may reclaim it. Must exceed the longest upload a client
        /// could still be in the middle of.
        std::int64_t reclaimGraceMs = 60 * 60 * 1000;

        /// How long a multipart upload may go without a part arriving before
        /// the sweeper aborts it. Zero disables the sweep.
        std::int64_t multipartExpiryMs = 168ll * 60 * 60 * 1000;

        /// What may be allocated across every bucket. Zero means "derive it
        /// from the filesystem holding the data directory", which is what the
        /// server does unless an operator names a figure — a number taken from
        /// the disk is the only one that is still true after the disk changes.
        std::uint64_t allocatableBytes = 0;

        /// The share of the derived capacity held back for the storage engine
        /// itself: RocksDB's log and SST files, the temporary a streaming write
        /// occupies before it is linked in, and the second copy a multipart
        /// completion makes while it concatenates. Ignored when
        /// `allocatableBytes` is set, because an explicit figure is already the
        /// operator's answer to this question.
        std::uint32_t capacityReservePercent = 10;

        /// The allocation a bucket gets when it is created by something that
        /// cannot ask for one — plain S3 CreateBucket. Zero leaves such a
        /// bucket unlimited, which is what every bucket was before allocations
        /// existed and therefore what an upgrade must not change.
        std::uint64_t defaultBucketQuotaBytes = 0;

        /// The maximum object-upload size to fall back on when the store has
        /// never carried one. Seeded from MONOBUCKET_MAX_UPLOAD_BYTES.
        std::uint64_t maxUploadBytes = 5ull * 1024 * 1024 * 1024;

        /// The most the persisted limit may ever be raised to. Environment
        /// only, so the console cannot lift its own ceiling.
        std::uint64_t maxUploadCeilingBytes = 5ull * 1024 * 1024 * 1024 * 1024;
    };

    static Options optionsFrom(const Config& config);

    explicit StorageEngine(Options options);
    ~StorageEngine();

    StorageEngine(const StorageEngine&)            = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;

    // --- Startup -----------------------------------------------------------

    struct RecoveryReport {
        std::size_t temporariesRemoved = 0;
        std::size_t payloadsReclaimed  = 0;
        std::size_t uploadsPending     = 0;
    };

    /// Reconciles the tree with the metadata after an unclean stop. Safe — and
    /// cheap — after a clean one.
    RecoveryReport recover();

    // --- Buckets -----------------------------------------------------------

    /// Creates a bucket with a storage allocation. Zero leaves it unlimited.
    ///
    /// Throws `InsufficientCapacity` when the allocation does not fit in what
    /// the instance has left to hand out — checked before the record is
    /// written, so a refused allocation leaves no bucket behind.
    void createBucket(std::string_view name, std::uint64_t quotaBytes = 0);

    /// Creates a bucket with whatever allocation the server hands to buckets
    /// that could not ask for one. What S3 CreateBucket calls.
    void createBucketWithDefaultQuota(std::string_view name);

    std::optional<BucketRecord> getBucket(std::string_view name);
    std::vector<BucketRecord>   listBuckets();
    void                        deleteBucket(std::string_view name);
    void setBucketPublicRead(std::string_view name, bool publicRead);

    /// Stores a bucket policy document together with the anonymous-read flag
    /// derived from it. An empty document removes the policy.
    void setBucketPolicy(std::string_view name, std::string policy, bool publicRead,
                         bool publicList);

    /// Replaces the bucket's CORS rules. An empty vector disables CORS, which
    /// is what DeleteBucketCors means.
    void setBucketCors(std::string_view name, std::vector<CorsRule> rules);

    /// Overrides the server's durability for this bucket, or clears the
    /// override so the bucket follows the server again.
    void setBucketDurability(std::string_view name, std::optional<Durability> durability);

    /// Replaces a bucket's storage allocation. Zero makes it unlimited.
    ///
    /// Throws `QuotaBelowUsage` when the bucket already holds more than the
    /// new allocation, and `InsufficientCapacity` when the instance cannot
    /// cover it. The ledger is updated first and rolled back if the record
    /// cannot be written, so the two never disagree about what was allowed.
    void setBucketQuota(std::string_view name, std::uint64_t quotaBytes);

    /// What one bucket has allocated and what it holds.
    BucketCapacity bucketCapacity(std::string_view name) const;

    /// Every tracked bucket's capacity, ordered by name.
    std::vector<std::pair<std::string, BucketCapacity>> bucketCapacities() const;

    /// The instance's allocatable capacity and how much of it is spoken for.
    InstanceCapacity capacity() const;

    /// The allocation a bucket created through plain S3 CreateBucket receives.
    std::uint64_t defaultBucketQuotaBytes() const noexcept {
        return options_.defaultBucketQuotaBytes;
    }

    /// The level writes to `bucket` are held to: its override if it set one,
    /// otherwise `MONOBUCKET_DURABILITY`.
    Durability durabilityFor(std::string_view bucket);

    // --- Upload limit ------------------------------------------------------
    //
    // One number, instance-wide, applying to every object however it arrives.
    //
    // Not per-bucket and not per-user, and that is the decision rather than
    // the first increment of one: the moment a limit has more than one scope
    // it needs a precedence rule, and a precedence rule is the beginning of a
    // policy language this server has deliberately refused to grow. A bucket's
    // allocation already answers "how much may this bucket hold"; this answers
    // "how large may one object be", and the two compose without either
    // needing to know about the other.

    /// The largest object this instance accepts. Never zero.
    std::uint64_t maxUploadBytes() const noexcept {
        return maxUploadBytes_.load(std::memory_order_relaxed);
    }

    /// The most `setMaxUploadBytes` will accept.
    std::uint64_t maxUploadCeilingBytes() const noexcept {
        return options_.maxUploadCeilingBytes;
    }

    /// Replaces the limit and persists it.
    ///
    /// Throws `StorageErrorCode::Internal` for zero and
    /// `StorageErrorCode::ObjectTooLarge` for a figure above the ceiling. The
    /// new limit is published only after the record is written, so a limit the
    /// store refused is not one the next request is held to.
    void setMaxUploadBytes(std::uint64_t bytes);

    /// Throws `ObjectTooLarge` when `bytes` is over the limit, with a message
    /// naming both figures.
    ///
    /// Public because every write path checks it twice — once against what the
    /// client declared, before a byte is read, and again against what actually
    /// arrived. The declared check is the one that saves the transfer; the
    /// arrived check is the one that is true.
    void requireWithinUploadLimit(std::uint64_t bytes, std::string_view what) const;

    // --- Objects -----------------------------------------------------------

    /// Opens a payload for streaming and registers it for reclamation first, so
    /// that a crash at any later point still leaves a recoverable trace.
    BlobWriter beginWrite();

    /// Claims `bytes` of a bucket's remaining allocation before a body is read.
    ///
    /// Throws `QuotaExceeded`. Handed to `finishWrite` or `finishPart`, which
    /// settle it against what actually arrived — and which take the claim
    /// themselves when none was made, so the enforcement does not depend on
    /// every caller remembering to ask first.
    [[nodiscard]] QuotaLedger::Reservation reserveSpace(std::string_view bucket,
                                                        std::uint64_t    bytes);

    struct PutRequest {
        std::string  bucket;
        std::string  key;
        std::string  contentType = "application/octet-stream";
        UserMetadata userMetadata;

        /// What the client asked to have verified, already computed over the
        /// bytes that arrived. Verification is the S3 layer's, not this one's:
        /// the expected value can travel in a trailer that only exists once the
        /// body has been decoded, and by then the framing is long gone.
        /// createUpload reads only the algorithm — a multipart upload declares
        /// one before it has any bytes to checksum.
        Checksum checksum;
    };

    /// Publishes a streamed payload. Consumes the writer and the claim.
    ObjectRecord finishWrite(const PutRequest& request, BlobWriter writer,
                             QuotaLedger::Reservation reservation = {});

    /// Convenience for small payloads that are already in memory — the settings
    /// panel, tests, and anything under the in-memory body threshold.
    ObjectRecord putObject(const PutRequest& request, std::string_view body);

    struct ObjectHandle {
        ObjectRecord record;
        BlobReader   reader;
    };

    /// Metadata plus an open payload. Nothing is read until the caller asks.
    std::optional<ObjectHandle> getObject(std::string_view bucket, std::string_view key);

    /// Metadata alone, for HEAD.
    std::optional<ObjectRecord> statObject(std::string_view bucket, std::string_view key);

    /// True when the key existed. Reclaims the payload before returning.
    bool deleteObject(std::string_view bucket, std::string_view key);

    ListObjectsResult listObjects(std::string_view bucket, const ListObjectsRequest& request);

    // --- Multipart ---------------------------------------------------------

    /// Returns the new upload id.
    std::string createUpload(const PutRequest& request);

    std::optional<UploadRecord> getUpload(std::string_view uploadId);
    ListUploadsResult           listUploads(std::string_view          bucket,
                                            const ListUploadsRequest& request);

    /// Aborts up to `limit` uploads that have made no progress for longer than
    /// the configured expiry, and returns how many. Zero when expiry is off.
    ///
    /// Bounded per call for the same reason `reclaim` is: a sweep runs on an
    /// I/O thread the request path is also waiting on, and a store with ten
    /// thousand abandoned uploads must not hold one for the duration. The
    /// next tick continues where this one stopped.
    std::size_t sweepExpiredUploads(std::size_t limit);

    /// Aborts uploads idle since before `cutoff`, ignoring the configured
    /// expiry. The cutoff is a parameter rather than derived so that the rule
    /// — which timestamp decides, and what happens either side of it — can be
    /// tested without a test that waits for a week to pass.
    std::size_t sweepUploadsIdleBefore(std::size_t limit, TimestampMs cutoff);

    /// Publishes one part. Consumes the writer and the claim.
    ///
    /// Parts are charged to their bucket as *pending* the moment they are
    /// stored, because the bytes are on disk from then on and an upload nobody
    /// completes holds them indefinitely. They become used bytes only when the
    /// upload completes, and are given back when it is aborted.
    PartRecord finishPart(std::string_view uploadId, std::uint32_t partNumber, BlobWriter writer,
                          QuotaLedger::Reservation reservation = {}, Checksum checksum = {});

    std::vector<PartRecord> listParts(std::string_view uploadId);

    /// What this upload's stored parts already amount to, ignoring
    /// `exceptPartNumber` — which is the part about to replace it.
    ///
    /// A prefix scan per part, which is what makes an upload refusable before
    /// its next part is sent rather than only at completion. The scan is over
    /// one upload's part rows, so it is bounded by the 10,000-part maximum and
    /// not by the object count; against a part that is at least five megabytes
    /// it does not register.
    std::uint64_t uploadedPartBytes(std::string_view uploadId,
                                    std::uint32_t    exceptPartNumber = 0);

    void abortUpload(std::string_view uploadId);

    /// What the client claims it uploaded, as sent in CompleteMultipartUpload.
    struct RequestedPart {
        std::uint32_t partNumber = 0;
        std::string   etag;
    };

    /// Validates the manifest, concatenates the parts into one payload and
    /// publishes it. Throws StorageError(InvalidPart) when the manifest does
    /// not match what was stored.
    ///
    /// `expected` is the full-object checksum the client claims the completed
    /// object will have. It is compared against the composite computed from the
    /// stored parts *before* anything is published — a checksum checked after
    /// the object is visible is a checksum that let a wrong object be read.
    /// Throws StorageError(ChecksumMismatch) when they disagree.
    ObjectRecord completeUpload(std::string_view uploadId,
                                const std::vector<RequestedPart>& parts,
                                const Checksum&                   expected = {});

    // --- Identity ----------------------------------------------------------

    /// One console account, or nothing when there is no such user.
    std::optional<UserRecord> getUser(std::string_view username);

    /// Every account, ordered by username.
    std::vector<UserRecord> listUsers();

    void putUser(const UserRecord& user);
    bool deleteUser(std::string_view username);

    /// How many administrators are currently enabled. Consulted before any
    /// change that could remove one, so a console cannot be locked out of
    /// itself by a demotion nobody realised was the last.
    std::size_t countEnabledAdministrators();

    /// The single administrator record that predates per-user identities.
    /// Startup migrates it into a UserRecord and drops it; nothing else reads
    /// these three.
    std::optional<AdminRecord> getAdmin();
    void                       putAdmin(const AdminRecord& admin);
    void                       deleteAdmin();

    /// Resolves an S3 access key id to its record. Called once per signed
    /// request, so it is a point lookup and nothing more.
    std::optional<AccessKeyRecord> getAccessKey(std::string_view accessKeyId);

    std::vector<AccessKeyRecord> listAccessKeys();
    void                         putAccessKey(const AccessKeyRecord& key);

    /// False when there was no such key. Revocation takes effect on the next
    /// request: nothing caches the secret between them.
    bool deleteAccessKey(std::string_view accessKeyId);

    // --- Audit -------------------------------------------------------------

    /// Records one security event and returns its sequence number. The log is a
    /// bounded ring — see MetadataStore::appendAudit.
    std::uint64_t appendAudit(const AuditRecord& entry);

    /// The newest entries first, at most `limit` of them.
    std::vector<AuditRecord> listAudit(std::size_t limit);

    // --- Consistency check -------------------------------------------------

    /// What a full check found. Kept as a list of findings rather than a bool
    /// so an operator can tell a leaked file (wasted space) apart from a
    /// missing payload (lost data) — the responses are nothing alike.
    struct FsckReport {
        enum class Kind {
            /// A live row names a payload that is not in the tree. Data loss:
            /// a GET of this key already fails.
            MissingPayload,
            /// The payload exists but is not the length the row records.
            SizeMismatch,
            /// The payload's SHA-256 differs from the one recorded at write
            /// time. Only reported by a deep check.
            DigestMismatch,
            /// A file in the tree that no row names and no reclamation record
            /// covers. This is the case the reclamation log cannot see, and
            /// the reason a tree walk exists at all.
            UnreferencedPayload,
            /// A file in the tree whose name could not have come from
            /// newBlobId(), so nothing in the store could ever reference it.
            MalformedName,
        };

        struct Finding {
            Kind        kind = Kind::MissingPayload;
            std::string blobId;
            /// Where the reference came from — `bucket/key`, or `upload
            /// <id> part <n>`. Empty for findings about a file with no
            /// referrer.
            std::string reference;
            std::string detail;
        };

        std::uint64_t bucketsScanned = 0;
        std::uint64_t objectsScanned = 0;
        std::uint64_t partsScanned   = 0;
        std::uint64_t filesScanned   = 0;
        std::uint64_t bytesRead      = 0;  ///< non-zero only for a deep check

        /// Bytes held by files that nothing references — what a reclaim would
        /// return to the filesystem.
        std::uint64_t leakedBytes = 0;

        std::vector<Finding> findings;

        bool clean() const noexcept { return findings.empty(); }
    };

    struct FsckOptions {
        /// Re-read every payload and compare its SHA-256 against the digest
        /// recorded when it was written. Costs one full pass over the data, so
        /// it is opt-in: the structural check alone is bounded by metadata.
        bool verifyDigests = false;

        /// Files younger than this are not judged unreferenced.
        ///
        /// Load-bearing for the same reason `listOrphans` takes a cutoff: a
        /// payload is linked into the tree a moment before the metadata naming
        /// it is committed, so a walk that raced that window would report a
        /// perfectly good object as a leak. Zero is correct only when nothing
        /// else is running.
        std::int64_t unreferencedGraceMs = 60 * 60 * 1000;

        /// Stop after this many findings. A store that is comprehensively
        /// broken should not produce a report nobody can read.
        std::size_t maxFindings = 1000;
    };

    /// Walks the metadata and the payload tree and reports every way they
    /// disagree.
    ///
    /// This is the check the reclamation log cannot do. The log knows about
    /// payloads it was told about; it cannot know about a file that was never
    /// tracked, nor that a file it does track has the wrong contents. Only a
    /// walk of the tree finds those, and only reading the bytes finds the last.
    ///
    /// Reports; never repairs. Deleting data on the strength of a scan that
    /// raced a live writer is how a consistency checker becomes the outage.
    FsckReport fsck(const FsckOptions& options);

    // --- Maintenance -------------------------------------------------------

    /// Unlinks up to `limit` payloads that are no longer referenced and are
    /// older than the reclaim grace period. Returns how many were reclaimed.
    /// Called periodically; deletions reclaim their own payload directly.
    std::size_t reclaim(std::size_t limit);

    /// Flushes metadata. Registered as a shutdown hook.
    void flush();

    struct Stats {
        UsageStats                                        usage;
        BlobStore::SpaceInfo                              space;
        std::vector<std::pair<std::string, std::uint64_t>> engineGauges;
        std::string                                       engine;
    };

    Stats stats() const;

    const BlobStore& blobs() const noexcept { return blobs_; }

private:
    /// Unlinks a payload and drops its reclamation record. Only ever called
    /// after the metadata that referenced it has been committed away.
    void reclaimNow(const std::string& blobId);
    void reclaimNow(const std::vector<std::string>& blobIds);

    /// Reclaims payloads queued at or before `cutoff`. Startup recovery passes
    /// the current time because nothing can be in flight yet; the periodic
    /// sweeper subtracts the grace period so it cannot collect a payload an
    /// upload is still writing to.
    std::size_t reclaimOlderThan(std::size_t limit, TimestampMs cutoff);

    /// Establishes the ledger from the store's opening scan. Called once, from
    /// the constructor, before anything can write.
    void seedQuotas();

    /// Reads the persisted upload limit, or stamps the seed from the
    /// environment on a store that has never carried one. Called once, from
    /// the constructor.
    void seedUploadLimit();

    Options                        options_;
    BlobStore                      blobs_;
    std::unique_ptr<MetadataStore> metadata_;

    /// Declared after `metadata_` and seeded from it: the ledger is only
    /// meaningful once the store has counted what is already there.
    QuotaLedger quotas_;

    /// The effective upload limit. Atomic rather than guarded, because it is
    /// read on every write and changed roughly never; a relaxed load is the
    /// whole cost, and a request that crosses the change is held to one of the
    /// two figures rather than to something in between.
    std::atomic<std::uint64_t> maxUploadBytes_{0};
};

/// Renders a finding kind for logs and the `--fsck` report.
std::string_view toString(StorageEngine::FsckReport::Kind kind);

}  // namespace monobucket
