#include "storage/storage_engine.hpp"

#include <algorithm>
#include <limits>
#include <random>
#include <unordered_map>
#include <unordered_set>

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

void StorageEngine::setBucketPolicy(std::string_view name, std::string policy, bool publicRead) {
    auto bucket = metadata_->getBucket(name);
    if (!bucket) {
        throw StorageError(StorageErrorCode::NoSuchBucket, "no such bucket: " + std::string(name));
    }
    // The document and the flag derived from it are committed together. Stored
    // separately they could disagree, and the flag is what the read path
    // actually consults.
    bucket->policy     = std::move(policy);
    bucket->publicRead = publicRead;
    metadata_->updateBucket(*bucket);
}

void StorageEngine::setBucketCors(std::string_view name, std::vector<CorsRule> rules) {
    auto bucket = metadata_->getBucket(name);
    if (!bucket) {
        throw StorageError(StorageErrorCode::NoSuchBucket, "no such bucket: " + std::string(name));
    }
    bucket->cors = std::move(rules);
    metadata_->updateBucket(*bucket);
}

Durability StorageEngine::durabilityFor(std::string_view bucket) {
    // A missing bucket is not this function's error to raise: the mutation that
    // follows throws NoSuchBucket with the context the caller needs. Answering
    // with the server default keeps that the single place it is reported.
    const auto record = metadata_->getBucket(bucket);
    if (record && record->durability) return *record->durability;
    return options_.durability;
}

void StorageEngine::setBucketDurability(std::string_view              name,
                                        std::optional<Durability>     durability) {
    auto bucket = metadata_->getBucket(name);
    if (!bucket) {
        throw StorageError(StorageErrorCode::NoSuchBucket, "no such bucket: " + std::string(name));
    }
    bucket->durability = durability;
    metadata_->updateBucket(*bucket);
}

// --- Identity ---------------------------------------------------------------

// Straight passthroughs. They live on the engine rather than being reached for
// through metadata_ directly so that the console and the S3 router depend on
// one storage surface, not two.

std::optional<AdminRecord> StorageEngine::getAdmin() { return metadata_->getAdmin(); }

void StorageEngine::putAdmin(const AdminRecord& admin) { metadata_->putAdmin(admin); }

std::optional<AccessKeyRecord> StorageEngine::getAccessKey(std::string_view accessKeyId) {
    return metadata_->getAccessKey(accessKeyId);
}

std::vector<AccessKeyRecord> StorageEngine::listAccessKeys() {
    return metadata_->listAccessKeys();
}

void StorageEngine::putAccessKey(const AccessKeyRecord& key) { metadata_->putAccessKey(key); }

bool StorageEngine::deleteAccessKey(std::string_view accessKeyId) {
    return metadata_->deleteAccessKey(accessKeyId);
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

    // Resolved before the payload is flushed, because the level decides how the
    // flush is done — not just how the metadata commit is done.
    const Durability durability = durabilityFor(request.bucket);

    // Durable first, visible second. The whole ordering rests on this line
    // completing before putObject is called.
    const BlobWriter::Committed committed = writer.commit(durability);

    ObjectRecord record;
    record.key          = request.key;
    record.blobId       = committed.blobId;
    record.size         = committed.size;
    record.etag         = committed.md5;
    record.sha256       = committed.sha256;
    record.contentType  = request.contentType;
    record.lastModified = nowMs();
    record.userMetadata = request.userMetadata;

    const auto outcome = metadata_->putObject(request.bucket, record, durability);
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
    const auto outcome = metadata_->deleteObject(bucket, key, durabilityFor(bucket));
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
    // A part belongs to its upload's bucket, so it inherits that bucket's
    // level. Parts are payloads like any other and a strict bucket wants them
    // on disk before the part row that names them exists.
    const auto upload = metadata_->getUpload(uploadId);
    if (!upload) {
        throw StorageError(StorageErrorCode::NoSuchUpload,
                           "no such upload: " + std::string(uploadId));
    }
    const Durability durability = durabilityFor(upload->bucket);

    const BlobWriter::Committed committed = writer.commit(durability);

    PartRecord part;
    part.partNumber = partNumber;
    part.blobId     = committed.blobId;
    part.size       = committed.size;
    part.etag       = committed.md5;
    part.uploadedAt = nowMs();

    const auto outcome = metadata_->putPart(uploadId, part, durability);
    if (outcome.releasedBlobId) reclaimNow(*outcome.releasedBlobId);

    return part;
}

std::vector<PartRecord> StorageEngine::listParts(std::string_view uploadId) {
    return metadata_->listParts(uploadId);
}

void StorageEngine::abortUpload(std::string_view uploadId) {
    const auto       upload     = metadata_->getUpload(uploadId);
    const Durability durability = upload ? durabilityFor(upload->bucket) : options_.durability;
    reclaimNow(metadata_->abortUpload(uploadId, durability));
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

    const Durability            durability = durabilityFor(upload->bucket);
    const BlobWriter::Committed committed  = writer.commit(durability);

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

    const auto outcome = metadata_->completeUpload(upload->bucket, uploadId, record, durability);
    if (outcome.releasedBlobId) reclaimNow(*outcome.releasedBlobId);
    reclaimNow(outcome.releasedPartBlobIds);

    log::debug("completed upload ", uploadId, " into '", record.key, "' (", ordered.size(),
               " parts, ", record.size, " bytes)");
    return record;
}

// --- Consistency check -----------------------------------------------------

StorageEngine::FsckReport StorageEngine::fsck(const FsckOptions& options) {
    FsckReport report;

    // blobId -> what references it and how big the referrer says it is. Held
    // resident because the tree walk needs to ask about every file, and the
    // alternative — a metadata lookup per file — turns a linear scan into a
    // random one over the whole keyspace.
    struct Reference {
        std::string   referrer;
        std::uint64_t size = 0;
        std::string   sha256;  ///< empty for parts, which record no full digest
    };
    std::unordered_map<std::string, Reference> referenced;

    const auto note = [&](FsckReport::Kind kind, std::string blobId, std::string reference,
                          std::string detail) {
        if (report.findings.size() >= options.maxFindings) return;
        report.findings.push_back(
            {kind, std::move(blobId), std::move(reference), std::move(detail)});
    };

    // --- 1. every payload the metadata claims ------------------------------

    for (const BucketRecord& bucket : metadata_->listBuckets()) {
        ++report.bucketsScanned;

        ListObjectsRequest page;
        page.maxKeys = 1000;
        while (true) {
            const ListObjectsResult listing = metadata_->listObjects(bucket.name, page);
            for (const ObjectRecord& object : listing.objects) {
                ++report.objectsScanned;
                referenced.emplace(object.blobId,
                                   Reference{bucket.name + '/' + object.key, object.size,
                                             object.sha256});
            }
            if (!listing.truncated) break;
            page.startAfter = listing.nextStartAfter;
        }

        // Parts of uploads that have not completed. These are live data — an
        // upload survives a restart — so a missing part payload is a real
        // finding, not residue.
        for (const UploadRecord& upload :
             metadata_->listUploads(bucket.name, std::numeric_limits<std::uint32_t>::max())) {
            for (const PartRecord& part : metadata_->listParts(upload.uploadId)) {
                ++report.partsScanned;
                referenced.emplace(part.blobId,
                                   Reference{"upload " + upload.uploadId + " part " +
                                                 std::to_string(part.partNumber),
                                             part.size, std::string{}});
            }
        }
    }

    // Blobs the reclamation log already knows are unreferenced. A file matching
    // one of these is not a leak — the sweeper is coming for it — so the walk
    // below must not report it. Read before the walk so that anything the
    // sweeper retires while we scan is, at worst, reported as a leak that has
    // already been cleaned up rather than missed.
    std::unordered_set<std::string> tracked;
    for (const auto& blobId :
         metadata_->listOrphans(std::numeric_limits<std::size_t>::max(), nowMs())) {
        tracked.insert(blobId);
    }

    // --- 2. what is actually on disk ---------------------------------------

    const std::int64_t youngerThan = nowMs() - options.unreferencedGraceMs;
    std::unordered_set<std::string> present;

    report.filesScanned = blobs_.forEachBlob([&](const BlobStore::TreeEntry& entry) {
        if (!entry.wellFormed) {
            note(FsckReport::Kind::MalformedName, entry.blobId, {},
                 "not a payload this store could have written");
            return;
        }
        present.insert(entry.blobId);

        const auto reference = referenced.find(entry.blobId);
        if (reference == referenced.end()) {
            if (tracked.count(entry.blobId) != 0) return;  // queued for reclamation

            // The grace period, not an optimisation: a payload is linked into
            // the tree just before the metadata naming it is committed, so a
            // file younger than any in-flight write could still be on its way
            // to being referenced.
            if (entry.modifiedMs > youngerThan) return;

            report.leakedBytes += entry.size;
            note(FsckReport::Kind::UnreferencedPayload, entry.blobId, {},
                 "untracked and unreferenced, " + std::to_string(entry.size) + " bytes");
            return;
        }

        if (entry.size != reference->second.size) {
            note(FsckReport::Kind::SizeMismatch, entry.blobId, reference->second.referrer,
                 "metadata records " + std::to_string(reference->second.size) + " bytes, file is " +
                     std::to_string(entry.size));
        }
    });

    // --- 3. references with nothing behind them ----------------------------

    for (const auto& [blobId, reference] : referenced) {
        if (present.count(blobId) == 0) {
            note(FsckReport::Kind::MissingPayload, blobId, reference.referrer,
                 "referenced but absent from the payload tree");
        }
    }

    if (!options.verifyDigests) return report;

    // --- 4. deep: does the payload still hash to what was recorded? --------

    std::vector<std::byte> buffer(options_.chunkBytes);
    for (const auto& [blobId, reference] : referenced) {
        // Parts carry an MD5 but no full-payload SHA-256, and a payload we
        // already know is missing has nothing to read.
        if (reference.sha256.empty() || present.count(blobId) == 0) continue;

        Digest digest;
        try {
            BlobReader reader = blobs_.open(blobId);
            while (true) {
                const std::size_t read = reader.read(buffer.data(), buffer.size());
                if (read == 0) break;
                digest.update(std::span<const std::byte>(buffer.data(), read));
                report.bytesRead += read;
            }
        } catch (const StorageError& ex) {
            note(FsckReport::Kind::MissingPayload, blobId, reference.referrer, ex.what());
            continue;
        }

        const Digest::Result actual = digest.finish();
        if (actual.sha256 != reference.sha256) {
            note(FsckReport::Kind::DigestMismatch, blobId, reference.referrer,
                 "recorded " + reference.sha256.substr(0, 16) + "…, computed " +
                     actual.sha256.substr(0, 16) + "…");
        }
    }

    return report;
}

std::string_view toString(StorageEngine::FsckReport::Kind kind) {
    switch (kind) {
        case StorageEngine::FsckReport::Kind::MissingPayload:      return "missing-payload";
        case StorageEngine::FsckReport::Kind::SizeMismatch:        return "size-mismatch";
        case StorageEngine::FsckReport::Kind::DigestMismatch:      return "digest-mismatch";
        case StorageEngine::FsckReport::Kind::UnreferencedPayload: return "unreferenced-payload";
        case StorageEngine::FsckReport::Kind::MalformedName:       return "malformed-name";
    }
    return "unknown";
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
