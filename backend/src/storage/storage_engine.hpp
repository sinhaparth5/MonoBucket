#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "storage/blob_store.hpp"
#include "storage/metadata_store.hpp"
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

    void                        createBucket(std::string_view name);
    std::optional<BucketRecord> getBucket(std::string_view name);
    std::vector<BucketRecord>   listBuckets();
    void                        deleteBucket(std::string_view name);
    void setBucketPublicRead(std::string_view name, bool publicRead);

    // --- Objects -----------------------------------------------------------

    /// Opens a payload for streaming and registers it for reclamation first, so
    /// that a crash at any later point still leaves a recoverable trace.
    BlobWriter beginWrite();

    struct PutRequest {
        std::string  bucket;
        std::string  key;
        std::string  contentType = "application/octet-stream";
        UserMetadata userMetadata;
    };

    /// Publishes a streamed payload. Consumes the writer.
    ObjectRecord finishWrite(const PutRequest& request, BlobWriter writer);

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
    std::vector<UploadRecord>   listUploads(std::string_view bucket, std::uint32_t maxUploads);

    /// Publishes one part. Consumes the writer.
    PartRecord finishPart(std::string_view uploadId, std::uint32_t partNumber, BlobWriter writer);

    std::vector<PartRecord> listParts(std::string_view uploadId);

    void abortUpload(std::string_view uploadId);

    /// What the client claims it uploaded, as sent in CompleteMultipartUpload.
    struct RequestedPart {
        std::uint32_t partNumber = 0;
        std::string   etag;
    };

    /// Validates the manifest, concatenates the parts into one payload and
    /// publishes it. Throws StorageError(InvalidPart) when the manifest does
    /// not match what was stored.
    ObjectRecord completeUpload(std::string_view uploadId,
                                const std::vector<RequestedPart>& parts);

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

    Options                        options_;
    BlobStore                      blobs_;
    std::unique_ptr<MetadataStore> metadata_;
};

}  // namespace monobucket
