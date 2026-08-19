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

namespace {

/// What this instance may allocate to buckets in total.
///
/// Derived from the filesystem rather than defaulted to a constant, and
/// deliberately less than the whole of it: the payload tree is not the only
/// thing on that disk. RocksDB's log and SST files live there, a streaming
/// write occupies a temporary before it is linked in, and a multipart
/// completion holds the parts and the assembled object at once. Handing out
/// every byte would make the first of those the thing that breaks.
std::uint64_t allocatableFrom(const StorageEngine::Options& options, const BlobStore& blobs) {
    if (options.allocatableBytes > 0) return options.allocatableBytes;

    const std::uint64_t total = blobs.space().totalBytes;
    if (total == 0) return 0;

    const std::uint32_t reserve = std::min<std::uint32_t>(options.capacityReservePercent, 100);
    return total / 100 * (100 - reserve);
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
    options.multipartExpiryMs =
        static_cast<std::int64_t>(config.multipartExpiryHours) * 60 * 60 * 1000;
    options.allocatableBytes        = config.allocatableBytes;
    options.capacityReservePercent  = config.capacityReservePercent;
    options.defaultBucketQuotaBytes = config.defaultBucketQuotaBytes;
    options.maxUploadBytes          = config.maxUploadBytes;
    options.maxUploadCeilingBytes   = config.maxUploadCeilingBytes;
    return options;
}

StorageEngine::StorageEngine(Options options)
    : options_(std::move(options)),
      blobs_(options_.dataDir, options_.durability, options_.chunkBytes),
      quotas_(allocatableFrom(options_, blobs_)) {
    // Recorded so that everything downstream reports the figure that is
    // actually enforced, not the zero that asked for it to be derived.
    options_.allocatableBytes = quotas_.allocatableBytes();

    MetadataStoreOptions metadataOptions;
    metadataOptions.path              = (options_.dataDir / "meta").string();
    metadataOptions.memoryBudgetBytes = options_.metadataMemoryBytes;
    metadataOptions.maxOpenFiles      = options_.maxOpenFiles;
    // Only strict durability pays for an fsync of the metadata log per commit.
    metadataOptions.syncWrites = options_.durability == Durability::Strict;

    metadata_ = openRocksMetadataStore(metadataOptions);
    seedQuotas();
    seedUploadLimit();

    log::info("storage engine ready: durability=", toString(options_.durability),
              ", chunk=", options_.chunkBytes / 1024, " KiB");
}

void StorageEngine::seedUploadLimit() {
    const auto stored = metadata_->getInstanceSettings();

    // The environment figure is a seed, not an override. Once an operator has
    // set a limit from the console it is the store's, and a redeploy carrying
    // a stale MONOBUCKET_MAX_UPLOAD_BYTES must not silently undo it — that is
    // the difference between a persisted setting and a configured one.
    std::uint64_t limit = (stored && stored->maxUploadBytes != 0) ? stored->maxUploadBytes
                                                                  : options_.maxUploadBytes;

    // Clamped rather than refused. A ceiling lowered below a limit somebody
    // set earlier is an operator deliberately tightening the instance, and
    // refusing to start would leave them with a server they cannot reach the
    // console of to fix it.
    if (limit > options_.maxUploadCeilingBytes) {
        log::warn("stored upload limit of ", limit, " bytes exceeds the configured ceiling of ",
                  options_.maxUploadCeilingBytes, " bytes; clamping");
        limit = options_.maxUploadCeilingBytes;
    }

    maxUploadBytes_.store(limit, std::memory_order_relaxed);

    if (!stored || stored->maxUploadBytes != limit) {
        MetadataStore::InstanceSettings settings = stored.value_or(MetadataStore::InstanceSettings{});
        settings.maxUploadBytes                  = limit;
        metadata_->putInstanceSettings(settings);
    }

    log::info("maximum object upload: ", limit, " bytes (ceiling ",
              options_.maxUploadCeilingBytes, ")");
}

void StorageEngine::setMaxUploadBytes(std::uint64_t bytes) {
    if (bytes == 0) {
        throw StorageError(StorageErrorCode::Internal,
                           "the maximum upload size must be at least 1 byte");
    }
    if (bytes > options_.maxUploadCeilingBytes) {
        throw StorageError(StorageErrorCode::ObjectTooLarge,
                           "the maximum upload size cannot exceed this instance's ceiling of " +
                               std::to_string(options_.maxUploadCeilingBytes) +
                               " bytes, which is set by MONOBUCKET_MAX_UPLOAD_CEILING_BYTES");
    }

    MetadataStore::InstanceSettings settings =
        metadata_->getInstanceSettings().value_or(MetadataStore::InstanceSettings{});
    settings.maxUploadBytes = bytes;

    // Persisted first. Publishing the new figure and then failing to write it
    // would leave a limit in force that a restart silently reverts, which is
    // the one behaviour an operator lowering a limit cannot afford.
    metadata_->putInstanceSettings(settings);
    maxUploadBytes_.store(bytes, std::memory_order_relaxed);

    log::info("maximum object upload changed to ", bytes, " bytes");
}

void StorageEngine::requireWithinUploadLimit(std::uint64_t bytes, std::string_view what) const {
    const std::uint64_t limit = maxUploadBytes();
    if (bytes <= limit) return;

    throw StorageError(StorageErrorCode::ObjectTooLarge,
                       std::string(what) + " is " + std::to_string(bytes) +
                           " bytes, over this instance's maximum upload size of " +
                           std::to_string(limit) + " bytes");
}

void StorageEngine::seedQuotas() {
    // The store counted every object as it opened, so the ledger starts from
    // what is actually on disk rather than from a number somebody wrote down
    // last time. Everything after this is a delta reported by the commit that
    // caused it.
    const auto scanned = metadata_->bucketCharges();
    const std::unordered_map<std::string, MetadataStore::BucketCharge> charges(scanned.begin(),
                                                                              scanned.end());

    std::uint64_t allocated = 0;
    std::size_t   allocatedBuckets = 0;
    for (const BucketRecord& bucket : metadata_->listBuckets()) {
        const auto charge = charges.find(bucket.name);
        quotas_.seed(bucket.name, bucket.quotaBytes,
                     charge == charges.end() ? 0 : charge->second.objectBytes,
                     charge == charges.end() ? 0 : charge->second.partBytes);
        if (bucket.quotaBytes != 0) {
            allocated += bucket.quotaBytes;
            ++allocatedBuckets;
        }
    }

    if (options_.allocatableBytes > 0) {
        log::info("storage allocations: ", allocatedBuckets, " of ",
                  metadata_->usage().buckets, " buckets hold ", allocated, " of ",
                  options_.allocatableBytes, " allocatable bytes");
    }
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

void StorageEngine::createBucket(std::string_view name, std::uint64_t quotaBytes) {
    // Checked before the record is written and again as the ledger takes the
    // allocation, so a refusal never leaves a bucket that exists with an
    // allocation the instance cannot back.
    quotas_.admitAllocation(quotaBytes);

    BucketRecord bucket;
    bucket.name       = std::string(name);
    bucket.createdAt  = nowMs();
    bucket.quotaBytes = quotaBytes;
    metadata_->createBucket(bucket);
    quotas_.track(bucket.name, quotaBytes);

    log::info("created bucket '", name, '\'',
              quotaBytes > 0 ? " with an allocation of " + std::to_string(quotaBytes) + " bytes"
                             : std::string(" with no allocation"));
}

void StorageEngine::createBucketWithDefaultQuota(std::string_view name) {
    createBucket(name, options_.defaultBucketQuotaBytes);
}

std::optional<BucketRecord> StorageEngine::getBucket(std::string_view name) {
    return metadata_->getBucket(name);
}

std::vector<BucketRecord> StorageEngine::listBuckets() { return metadata_->listBuckets(); }

void StorageEngine::deleteBucket(std::string_view name) {
    // The store refuses a bucket that still holds anything, so by the time this
    // returns the bucket's charges are zero and dropping its tally cannot lose
    // an accounted byte.
    metadata_->deleteBucket(name);
    quotas_.forget(name);
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

void StorageEngine::setBucketQuota(std::string_view name, std::uint64_t quotaBytes) {
    auto bucket = metadata_->getBucket(name);
    if (!bucket) {
        throw StorageError(StorageErrorCode::NoSuchBucket, "no such bucket: " + std::string(name));
    }

    const std::uint64_t previous = bucket->quotaBytes;

    // The ledger decides first, because it is the one that has to be right: it
    // is what every concurrent write is being admitted against, and it knows
    // about reservations the record cannot see. If the record then fails to
    // write, the ledger is put back — a bucket enforcing an allocation nobody
    // asked for would be far harder to explain than a failed request.
    quotas_.setQuota(name, quotaBytes);
    try {
        bucket->quotaBytes = quotaBytes;
        metadata_->updateBucket(*bucket);
    } catch (...) {
        quotas_.setQuota(name, previous);
        throw;
    }

    log::info("bucket '", name, "' allocation set to ", quotaBytes, " bytes");
}

BucketCapacity StorageEngine::bucketCapacity(std::string_view name) const {
    return quotas_.capacity(name);
}

std::vector<std::pair<std::string, BucketCapacity>> StorageEngine::bucketCapacities() const {
    return quotas_.all();
}

InstanceCapacity StorageEngine::capacity() const { return quotas_.instance(); }

// --- Identity ---------------------------------------------------------------

// Straight passthroughs. They live on the engine rather than being reached for
// through metadata_ directly so that the console and the S3 router depend on
// one storage surface, not two.

std::optional<UserRecord> StorageEngine::getUser(std::string_view username) {
    return metadata_->getUser(username);
}

std::vector<UserRecord> StorageEngine::listUsers() { return metadata_->listUsers(); }

void StorageEngine::putUser(const UserRecord& user) { metadata_->putUser(user); }

bool StorageEngine::deleteUser(std::string_view username) {
    return metadata_->deleteUser(username);
}

std::size_t StorageEngine::countEnabledAdministrators() {
    return metadata_->countEnabledAdministrators();
}

std::optional<AdminRecord> StorageEngine::getAdmin() { return metadata_->getAdmin(); }

void StorageEngine::deleteAdmin() { metadata_->deleteAdmin(); }

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

std::uint64_t StorageEngine::appendAudit(const AuditRecord& entry) {
    return metadata_->appendAudit(entry);
}

std::vector<AuditRecord> StorageEngine::listAudit(std::size_t limit) {
    return metadata_->listAudit(limit);
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

QuotaLedger::Reservation StorageEngine::reserveSpace(std::string_view bucket,
                                                     std::uint64_t    bytes) {
    return quotas_.reserve(bucket, bytes);
}

ObjectRecord StorageEngine::finishWrite(const PutRequest& request, BlobWriter writer,
                                        QuotaLedger::Reservation reservation) {
    if (request.key.empty() || request.key.size() > limits::kMaxKeyLength) {
        throw StorageError(StorageErrorCode::Internal,
                           "object key must be between 1 and " +
                               std::to_string(limits::kMaxKeyLength) + " bytes");
    }

    // The authoritative size check, and it is here rather than in the handler
    // because this is the last place a caller cannot skip. Before the commit,
    // so a refused object is never linked into the payload tree: the writer's
    // temporary is discarded by its own destructor and the reclamation record
    // beginWrite() left behind is collected on schedule.
    requireWithinUploadLimit(writer.written(), "the object");

    // Resolved before the payload is flushed, because the level decides how the
    // flush is done — not just how the metadata commit is done.
    const Durability durability = durabilityFor(request.bucket);

    // Durable first, visible second. The whole ordering rests on this line
    // completing before putObject is called.
    const BlobWriter::Committed committed = writer.commit(durability);

    // Now that the size is known for certain, the claim is raised to cover it.
    // A body whose length was declared has already been admitted and this
    // costs nothing; one that arrived chunked is admitted here, and a refusal
    // still lands before the metadata commit — so the object never becomes
    // visible and the payload is left to reclamation.
    //
    // An overwrite is admitted as if the key were new, deliberately: both
    // payloads are on disk until the commit, so the bytes the old object will
    // release are not available to the new one yet.
    quotas_.extend(reservation, request.bucket, committed.size);

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
    quotas_.recordObject(request.bucket, std::move(reservation), record.size,
                         outcome.replacedBytes);
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
    quotas_.recordDeletion(bucket, outcome.releasedBytes);
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

ListUploadsResult StorageEngine::listUploads(std::string_view          bucket,
                                             const ListUploadsRequest& request) {
    return metadata_->listUploads(bucket, request);
}

std::size_t StorageEngine::sweepExpiredUploads(std::size_t limit) {
    if (options_.multipartExpiryMs <= 0) return 0;
    return sweepUploadsIdleBefore(limit, nowMs() - options_.multipartExpiryMs);
}

std::size_t StorageEngine::sweepUploadsIdleBefore(std::size_t limit, TimestampMs cutoff) {
    // `createdAt` narrows the scan; it does not decide anything. An upload that
    // was begun a fortnight ago and received a part an hour ago is a client
    // still working, and aborting it would destroy a transfer that is making
    // progress — the one outcome this sweep must never produce. So every
    // candidate is measured on its most recent part instead, and only an
    // upload whose newest part is also past the cutoff is abandoned.
    //
    // The parts are read only for uploads the cheap filter already flagged, so
    // the extra scan is paid on the stale ones rather than on every upload.
    const auto candidates = metadata_->listUploadsCreatedBefore(limit, cutoff);

    std::size_t swept = 0;
    for (const UploadRecord& upload : candidates) {
        TimestampMs lastActivity = upload.createdAt;
        for (const PartRecord& part : metadata_->listParts(upload.uploadId)) {
            lastActivity = std::max(lastActivity, part.uploadedAt);
        }
        if (lastActivity >= cutoff) continue;

        try {
            abortUpload(upload.uploadId);
        } catch (const StorageError& ex) {
            // A client that aborted or completed it between the scan and here
            // wins the race, and there is nothing left to do. Anything else is
            // worth a line, but never worth abandoning the rest of the sweep.
            if (ex.code() != StorageErrorCode::NoSuchUpload) {
                log::warn("could not expire upload ", upload.uploadId, ": ", ex.what());
            }
            continue;
        }

        log::info("expired multipart upload ", upload.uploadId, " for ", upload.bucket, '/',
                  upload.key, ", idle since ", lastActivity);
        swept += 1;
    }

    return swept;
}

PartRecord StorageEngine::finishPart(std::string_view uploadId, std::uint32_t partNumber,
                                     BlobWriter writer, QuotaLedger::Reservation reservation) {
    // A part belongs to its upload's bucket, so it inherits that bucket's
    // level. Parts are payloads like any other and a strict bucket wants them
    // on disk before the part row that names them exists.
    const auto upload = metadata_->getUpload(uploadId);
    if (!upload) {
        throw StorageError(StorageErrorCode::NoSuchUpload,
                           "no such upload: " + std::string(uploadId));
    }
    // A part on its own can never exceed the object limit, and the parts
    // already stored plus this one cannot either. The second check is what
    // stops an upload being built past the limit a piece at a time and only
    // discovered at completion, by which point the bytes are all on disk.
    //
    // Two clients racing on different part numbers can each pass this and
    // together exceed the limit; the check at completion is the one that
    // closes that, and it is single-point by construction. This one exists to
    // refuse early, not to be the last word.
    const std::uint64_t others = uploadedPartBytes(uploadId, partNumber);
    requireWithinUploadLimit(others + writer.written(), "the completed object");

    const Durability durability = durabilityFor(upload->bucket);

    const BlobWriter::Committed committed = writer.commit(durability);
    quotas_.extend(reservation, upload->bucket, committed.size);

    PartRecord part;
    part.partNumber = partNumber;
    part.blobId     = committed.blobId;
    part.size       = committed.size;
    part.etag       = committed.md5;
    part.uploadedAt = nowMs();

    const auto outcome = metadata_->putPart(uploadId, part, durability);
    quotas_.recordPart(upload->bucket, std::move(reservation), part.size, outcome.replacedBytes);
    if (outcome.releasedBlobId) reclaimNow(*outcome.releasedBlobId);

    return part;
}

std::vector<PartRecord> StorageEngine::listParts(std::string_view uploadId) {
    return metadata_->listParts(uploadId);
}

std::uint64_t StorageEngine::uploadedPartBytes(std::string_view uploadId,
                                               std::uint32_t    exceptPartNumber) {
    std::uint64_t total = 0;
    for (const PartRecord& part : metadata_->listParts(uploadId)) {
        if (part.partNumber == exceptPartNumber) continue;
        total += part.size;
    }
    return total;
}

void StorageEngine::abortUpload(std::string_view uploadId) {
    const auto       upload     = metadata_->getUpload(uploadId);
    const Durability durability = upload ? durabilityFor(upload->bucket) : options_.durability;

    const auto outcome = metadata_->abortUpload(uploadId, durability);
    // The parts were charged as pending from the moment each was stored, so an
    // abandoned upload gives its bucket's allocation back in full and leaves
    // nothing in used bytes — an upload that never completed never produced an
    // object to account for.
    quotas_.recordUploadAborted(outcome.bucket, outcome.releasedBytes);
    reclaimNow(outcome.releasedBlobIds);
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

    // The last word on the limit, and the only check multipart cannot get
    // around: whatever each part was admitted under, this is the object that
    // is about to exist. Before the concatenation, so a refusal costs no copy
    // and leaves the parts exactly as they were — the client can abort the
    // upload or complete a subset of it, and nothing has been published.
    std::uint64_t assembledBytes = 0;
    for (const PartRecord& part : ordered) assembledBytes += part.size;
    requireWithinUploadLimit(assembledBytes, "the completed object");

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
    // No fresh admission: the parts have been charged since they were stored,
    // and the assembled object is the same bytes under a different heading. A
    // bucket at its allocation must still be able to finish an upload it was
    // already charged for.
    quotas_.recordUploadCompleted(upload->bucket, record.size, outcome.replacedBytes,
                                  outcome.releasedPartBytes);
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
        ListUploadsRequest uploadPage;
        uploadPage.maxUploads = 1000;
        while (true) {
            const ListUploadsResult uploads = metadata_->listUploads(bucket.name, uploadPage);
            for (const UploadRecord& upload : uploads.uploads) {
                for (const PartRecord& part : metadata_->listParts(upload.uploadId)) {
                    ++report.partsScanned;
                    referenced.emplace(part.blobId,
                                       Reference{"upload " + upload.uploadId + " part " +
                                                     std::to_string(part.partNumber),
                                                 part.size, std::string{}});
                }
            }
            if (!uploads.truncated) break;
            uploadPage.keyMarker      = uploads.nextKeyMarker;
            uploadPage.uploadIdMarker = uploads.nextUploadIdMarker;
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
