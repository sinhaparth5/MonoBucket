#include "storage/storage_engine.hpp"

#include <algorithm>
#include <limits>
#include <random>

#include "core/config.hpp"
#include "core/logging.hpp"
#include "monobucket/constants.hpp"

namespace monobucket {
namespace {

constexpr char kIdAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

/// Upload ids are opaque to clients and must be unguessable: an id is the only
/// thing standing between a caller and someone else's in-progress upload.
std::string newUploadId() {
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> pick(0, sizeof(kIdAlphabet) - 2);

    std::string id;
    id.reserve(32);
    for (int i = 0; i < 32; ++i) id.push_back(kIdAlphabet[pick(engine)]);
    return id;
}

/// S3 sends ETags quoted; clients are inconsistent about whether they echo the
/// quotes back in a CompleteMultipartUpload manifest, so compare unquoted.
std::string_view unquote(std::string_view etag) {
    if (etag.size() >= 2 && etag.front() == '"' && etag.back() == '"') {
        return etag.substr(1, etag.size() - 2);
    }
    return etag;
}

}  // namespace

StorageEngine::Options StorageEngine::optionsFrom(const Config& config) {
    Options options;
    options.dataDir             = config.dataDir;
    options.durability          = config.durability;
    options.chunkBytes          = static_cast<std::size_t>(config.streamChunkBytes);
    options.metadataMemoryBytes = config.metadataMemoryBytes;
    options.maxOpenFiles        = static_cast<int>(config.metadataMaxOpenFiles);
    options.reclaimGraceMs      = static_cast<std::int64_t>(config.reclaimGraceSeconds) * 1000;
    return options;
}

StorageEngine::StorageEngine(Options options)
    : options_(std::move(options)),
      blobs_(options_.dataDir, options_.durability, options_.chunkBytes) {
    MetadataStoreOptions metadataOptions;
    metadataOptions.path              = (options_.dataDir / "meta").string();
    metadataOptions.memoryBudgetBytes = options_.metadataMemoryBytes;
    metadataOptions.maxOpenFiles      = options_.maxOpenFiles;
    // Only strict durability pays for an fsync of the metadata log per commit.
    metadataOptions.syncWrites = options_.durability == Durability::Strict;

    metadata_ = openRocksMetadataStore(metadataOptions);

    log::info("storage engine ready: durability=", toString(options_.durability),
              ", chunk=", options_.chunkBytes / 1024, " KiB");
}

StorageEngine::~StorageEngine() = default;

// --- Startup ---------------------------------------------------------------

StorageEngine::RecoveryReport StorageEngine::recover() {
    RecoveryReport report;

    // Nothing under tmp/ is ever referenced: a payload only leaves it at commit
    // time, and commit is what makes it reachable. Anything still there is the
    // residue of a write that did not finish.
    report.temporariesRemoved = blobs_.sweepTemporaries();

    // Nothing is in flight yet, so every tracked-but-unreferenced payload is
    // genuinely abandoned regardless of age — hence the cutoff of "now" rather
    // than the grace period the periodic sweeper has to respect.
    report.payloadsReclaimed = reclaimOlderThan(std::numeric_limits<std::size_t>::max(), nowMs());

    report.uploadsPending = static_cast<std::size_t>(metadata_->usage().uploads);

    if (report.temporariesRemoved > 0 || report.payloadsReclaimed > 0) {
        log::info("recovery: removed ", report.temporariesRemoved, " interrupted writes, reclaimed ",
                  report.payloadsReclaimed, " unreferenced payloads");
    }
    if (report.uploadsPending > 0) {
        // These are legitimate: S3 multipart uploads outlive a restart and are
        // only discarded by an explicit abort or a lifecycle rule.
        log::info("recovery: ", report.uploadsPending, " multipart uploads still in progress");
    }
    return report;
}

// --- Buckets ---------------------------------------------------------------

void StorageEngine::createBucket(std::string_view name) {
    BucketRecord bucket;
    bucket.name      = std::string(name);
    bucket.createdAt = nowMs();
    metadata_->createBucket(bucket);
    log::info("created bucket '", name, '\'');
}

std::optional<BucketRecord> StorageEngine::getBucket(std::string_view name) {
    return metadata_->getBucket(name);
}

std::vector<BucketRecord> StorageEngine::listBuckets() { return metadata_->listBuckets(); }

void StorageEngine::deleteBucket(std::string_view name) {
    metadata_->deleteBucket(name);
    log::info("deleted bucket '", name, '\'');
}

void StorageEngine::setBucketPublicRead(std::string_view name, bool publicRead) {
    auto bucket = metadata_->getBucket(name);
    if (!bucket) {
        throw StorageError(StorageErrorCode::NoSuchBucket, "no such bucket: " + std::string(name));
    }
    bucket->publicRead = publicRead;
    metadata_->updateBucket(*bucket);
}

// --- Objects ---------------------------------------------------------------

BlobWriter StorageEngine::beginWrite() {
    BlobWriter writer = blobs_.create();

    // Registered before a single byte is written. The payload is still under
    // tmp/ at this point, so the window between opening the file and recording
    // it is covered by the temporary sweep rather than by this record.
    metadata_->trackBlob(writer.blobId());
    return writer;
}

ObjectRecord StorageEngine::finishWrite(const PutRequest& request, BlobWriter writer) {
    if (request.key.empty() || request.key.size() > limits::kMaxKeyLength) {
        throw StorageError(StorageErrorCode::Internal,
                           "object key must be between 1 and " +
                               std::to_string(limits::kMaxKeyLength) + " bytes");
    }

    // Durable first, visible second. The whole ordering rests on this line
    // completing before putObject is called.
    const BlobWriter::Committed committed = writer.commit();

    ObjectRecord record;
    record.key          = request.key;
    record.blobId       = committed.blobId;
    record.size         = committed.size;
    record.etag         = committed.md5;
    record.sha256       = committed.sha256;
    record.contentType  = request.contentType;
    record.lastModified = nowMs();
    record.userMetadata = request.userMetadata;

    const auto outcome = metadata_->putObject(request.bucket, record);
    if (outcome.releasedBlobId) reclaimNow(*outcome.releasedBlobId);

    return record;
}

ObjectRecord StorageEngine::putObject(const PutRequest& request, std::string_view body) {
    BlobWriter writer = beginWrite();
    writer.write(body);
    return finishWrite(request, std::move(writer));
}

std::optional<StorageEngine::ObjectHandle> StorageEngine::getObject(std::string_view bucket,
                                                                    std::string_view key) {
    auto record = metadata_->getObject(bucket, key);
    if (!record) return std::nullopt;

    // Opening can fail if the tree was tampered with; that is a corruption
    // report, not a 404, and StorageError carries the distinction.
    BlobReader reader = blobs_.open(record->blobId);
    return ObjectHandle{std::move(*record), std::move(reader)};
}

std::optional<ObjectRecord> StorageEngine::statObject(std::string_view bucket,
                                                      std::string_view key) {
    return metadata_->getObject(bucket, key);
}

bool StorageEngine::deleteObject(std::string_view bucket, std::string_view key) {
    const auto outcome = metadata_->deleteObject(bucket, key);
    if (outcome.releasedBlobId) reclaimNow(*outcome.releasedBlobId);
    return outcome.existed;
}

ListObjectsResult StorageEngine::listObjects(std::string_view          bucket,
                                             const ListObjectsRequest& request) {
    ListObjectsRequest bounded = request;
    bounded.maxKeys            = std::min(bounded.maxKeys, limits::kMaxMaxKeys);
    return metadata_->listObjects(bucket, bounded);
}

// --- Multipart -------------------------------------------------------------

std::string StorageEngine::createUpload(const PutRequest& request) {
    UploadRecord upload;
    upload.uploadId     = newUploadId();
    upload.bucket       = request.bucket;
    upload.key          = request.key;
    upload.contentType  = request.contentType;
    upload.createdAt    = nowMs();
    upload.userMetadata = request.userMetadata;

    metadata_->createUpload(upload);
    return upload.uploadId;
}

std::optional<UploadRecord> StorageEngine::getUpload(std::string_view uploadId) {
    return metadata_->getUpload(uploadId);
}

std::vector<UploadRecord> StorageEngine::listUploads(std::string_view bucket,
                                                     std::uint32_t    maxUploads) {
    return metadata_->listUploads(bucket, maxUploads);
}

PartRecord StorageEngine::finishPart(std::string_view uploadId, std::uint32_t partNumber,
                                     BlobWriter writer) {
    const BlobWriter::Committed committed = writer.commit();

    PartRecord part;
    part.partNumber = partNumber;
    part.blobId     = committed.blobId;
    part.size       = committed.size;
    part.etag       = committed.md5;
    part.uploadedAt = nowMs();

    const auto outcome = metadata_->putPart(uploadId, part);
    if (outcome.releasedBlobId) reclaimNow(*outcome.releasedBlobId);

    return part;
}

std::vector<PartRecord> StorageEngine::listParts(std::string_view uploadId) {
    return metadata_->listParts(uploadId);
}

void StorageEngine::abortUpload(std::string_view uploadId) {
    reclaimNow(metadata_->abortUpload(uploadId));
}

ObjectRecord StorageEngine::completeUpload(std::string_view                  uploadId,
                                           const std::vector<RequestedPart>& requested) {
    const auto upload = metadata_->getUpload(uploadId);
    if (!upload) {
        throw StorageError(StorageErrorCode::NoSuchUpload, "no such upload: " + std::string(uploadId));
    }
    if (requested.empty()) {
        throw StorageError(StorageErrorCode::InvalidPart,
                           "a completed multipart upload must name at least one part");
    }

    const auto stored = metadata_->listParts(uploadId);

    std::vector<PartRecord> ordered;
    ordered.reserve(requested.size());

    std::uint32_t previous = 0;
    for (const RequestedPart& want : requested) {
        // S3 requires the manifest in ascending part-number order and rejects
        // duplicates; both would otherwise silently reorder the object.
        if (want.partNumber <= previous) {
            throw StorageError(StorageErrorCode::InvalidPart,
                               "parts must be listed in ascending order without duplicates");
        }
        previous = want.partNumber;

        const auto found = std::find_if(stored.begin(), stored.end(), [&](const PartRecord& part) {
            return part.partNumber == want.partNumber;
        });
        if (found == stored.end()) {
            throw StorageError(StorageErrorCode::InvalidPart,
                               "part " + std::to_string(want.partNumber) + " was never uploaded");
        }
        if (unquote(want.etag) != found->etag) {
            throw StorageError(StorageErrorCode::InvalidPart,
                               "the ETag supplied for part " + std::to_string(want.partNumber) +
                                   " does not match what was stored");
        }
        ordered.push_back(*found);
    }

    // Every part but the last must reach the minimum, otherwise a client could
    // build an object out of arbitrarily small pieces and multiply our
    // per-part overhead without limit.
    for (std::size_t i = 0; i + 1 < ordered.size(); ++i) {
        if (ordered[i].size < limits::kMinPartSize) {
            throw StorageError(StorageErrorCode::InvalidPart,
                               "part " + std::to_string(ordered[i].partNumber) + " is " +
                                   std::to_string(ordered[i].size) + " bytes, below the " +
                                   std::to_string(limits::kMinPartSize) + " byte minimum");
        }
    }

    // Assemble into one payload. Reading an object then becomes a single
    // sequential file read, and Range requests need no part arithmetic — paid
    // for with one copy here, at the only moment the client is expecting to
    // wait.
    BlobWriter               writer = beginWrite();
    std::vector<std::string> partDigests;
    partDigests.reserve(ordered.size());

    for (const PartRecord& part : ordered) {
        writer.appendBlob(blobs_, part.blobId);
        partDigests.push_back(part.etag);
    }

    const BlobWriter::Committed committed = writer.commit();

    ObjectRecord record;
    record.key    = upload->key;
    record.blobId = committed.blobId;
    record.size   = committed.size;
    // Not the payload's own MD5: a multipart ETag is the digest of the parts'
    // digests, and clients compare it literally.
    record.etag         = multipartETag(partDigests);
    record.sha256       = committed.sha256;
    record.contentType  = upload->contentType;
    record.lastModified = nowMs();
    record.userMetadata = upload->userMetadata;

    const auto outcome = metadata_->completeUpload(upload->bucket, uploadId, record);
    if (outcome.releasedBlobId) reclaimNow(*outcome.releasedBlobId);
    reclaimNow(outcome.releasedPartBlobIds);

    log::debug("completed upload ", uploadId, " into '", record.key, "' (", ordered.size(),
               " parts, ", record.size, " bytes)");
    return record;
}

// --- Maintenance -----------------------------------------------------------

std::size_t StorageEngine::reclaim(std::size_t limit) {
    return reclaimOlderThan(limit, nowMs() - options_.reclaimGraceMs);
}

std::size_t StorageEngine::reclaimOlderThan(std::size_t limit, TimestampMs cutoff) {
    const auto blobIds = metadata_->listOrphans(limit, cutoff);
    if (blobIds.empty()) return 0;

    for (const std::string& blobId : blobIds) blobs_.remove(blobId);
    metadata_->forgetOrphans(blobIds);

    log::debug("reclaimed ", blobIds.size(), " unreferenced payloads");
    return blobIds.size();
}

void StorageEngine::reclaimNow(const std::string& blobId) {
    blobs_.remove(blobId);
    metadata_->forgetOrphans({blobId});
}

void StorageEngine::reclaimNow(const std::vector<std::string>& blobIds) {
    if (blobIds.empty()) return;
    for (const std::string& blobId : blobIds) blobs_.remove(blobId);
    metadata_->forgetOrphans(blobIds);
}

void StorageEngine::flush() { metadata_->flush(); }

StorageEngine::Stats StorageEngine::stats() const {
    Stats stats;
    stats.usage        = metadata_->usage();
    stats.space        = blobs_.space();
    stats.engineGauges = metadata_->engineGauges();
    stats.engine       = std::string(metadata_->engineName());
    return stats;
}

}  // namespace monobucket
