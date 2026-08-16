#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <shared_mutex>

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/write_buffer_manager.h>

#include "core/logging.hpp"
#include "monobucket/constants.hpp"
#include "storage/codec.hpp"
#include "storage/keyspace.hpp"
#include "storage/metadata_store.hpp"

namespace monobucket {
namespace {

/// Bumped only when the on-disk encoding changes incompatibly. Independent of
/// the CalVer release version, as CHANGELOG.md promises.
constexpr std::uint64_t kStorageFormatVersion = 1;

/// Record encoding version, written as the first byte of every value. Readers
/// tolerate a *newer* minor layout by ignoring trailing fields, but refuse an
/// unknown major one rather than misinterpreting bytes.
constexpr std::uint8_t kRecordVersion = 1;

/// Enough stripes that unrelated keys rarely collide, few enough that the
/// array stays in one cache line group. Only ever held across a Get + Write.
constexpr std::size_t kLockStripes = 64;

rocksdb::Slice toSlice(std::string_view view) { return {view.data(), view.size()}; }

std::string_view toView(const rocksdb::Slice& slice) { return {slice.data(), slice.size()}; }

[[noreturn]] void fail(StorageErrorCode code, const std::string& what) {
    throw StorageError(code, what);
}

void check(const rocksdb::Status& status, const std::string& context) {
    if (status.ok()) return;
    if (status.IsCorruption()) fail(StorageErrorCode::Corruption, context + ": " + status.ToString());
    if (status.IsIOError()) fail(StorageErrorCode::Io, context + ": " + status.ToString());
    fail(StorageErrorCode::Internal, context + ": " + status.ToString());
}

// --- Record serialisation --------------------------------------------------

void encodeMetadata(codec::Writer& writer, const UserMetadata& metadata) {
    writer.varint(metadata.size());
    for (const auto& [name, value] : metadata) {
        writer.string(name);
        writer.string(value);
    }
}

UserMetadata decodeMetadata(codec::Reader& reader) {
    UserMetadata      metadata;
    const std::uint64_t count = reader.varint();
    for (std::uint64_t i = 0; i < count; ++i) {
        // Evaluation order of function arguments is unspecified, so the two
        // reads must be sequenced explicitly or the pairs can come out swapped.
        auto name  = reader.string();
        auto value = reader.string();
        metadata.emplace(std::move(name), std::move(value));
    }
    return metadata;
}

void expectVersion(codec::Reader& reader, const char* what) {
    const std::uint8_t version = reader.u8();
    if (version != kRecordVersion) {
        fail(StorageErrorCode::Corruption,
             std::string(what) + " record has unsupported encoding version " +
                 std::to_string(version) + " (this build writes " +
                 std::to_string(kRecordVersion) + ")");
    }
}

std::string encodeBucket(const BucketRecord& bucket) {
    std::string   out;
    codec::Writer writer(out);
    writer.u8(kRecordVersion);
    writer.varint(static_cast<std::uint64_t>(bucket.createdAt));
    writer.boolean(bucket.publicRead);
    writer.string(bucket.policy);
    return out;
}

BucketRecord decodeBucket(std::string_view name, std::string_view stored) {
    codec::Reader reader(stored);
    expectVersion(reader, "bucket");

    BucketRecord bucket;
    bucket.name       = std::string(name);
    bucket.createdAt  = static_cast<TimestampMs>(reader.varint());
    bucket.publicRead = reader.boolean();
    bucket.policy     = reader.string();
    return bucket;
}

std::string encodeObject(const ObjectRecord& object) {
    std::string   out;
    codec::Writer writer(out);
    writer.u8(kRecordVersion);
    writer.string(object.blobId);
    writer.varint(object.size);
    writer.string(object.etag);
    writer.string(object.sha256);
    writer.string(object.contentType);
    writer.varint(static_cast<std::uint64_t>(object.lastModified));
    encodeMetadata(writer, object.userMetadata);
    return out;
}

ObjectRecord decodeObject(std::string_view key, std::string_view stored) {
    codec::Reader reader(stored);
    expectVersion(reader, "object");

    ObjectRecord object;
    object.key          = std::string(key);
    object.blobId       = reader.string();
    object.size         = reader.varint();
    object.etag         = reader.string();
    object.sha256       = reader.string();
    object.contentType  = reader.string();
    object.lastModified = static_cast<TimestampMs>(reader.varint());
    object.userMetadata = decodeMetadata(reader);
    return object;
}

std::string encodeUpload(const UploadRecord& upload) {
    std::string   out;
    codec::Writer writer(out);
    writer.u8(kRecordVersion);
    writer.string(upload.bucket);
    writer.string(upload.key);
    writer.string(upload.contentType);
    writer.varint(static_cast<std::uint64_t>(upload.createdAt));
    encodeMetadata(writer, upload.userMetadata);
    return out;
}

UploadRecord decodeUpload(std::string_view uploadId, std::string_view stored) {
    codec::Reader reader(stored);
    expectVersion(reader, "upload");

    UploadRecord upload;
    upload.uploadId     = std::string(uploadId);
    upload.bucket       = reader.string();
    upload.key          = reader.string();
    upload.contentType  = reader.string();
    upload.createdAt    = static_cast<TimestampMs>(reader.varint());
    upload.userMetadata = decodeMetadata(reader);
    return upload;
}

std::string encodePart(const PartRecord& part) {
    std::string   out;
    codec::Writer writer(out);
    writer.u8(kRecordVersion);
    writer.string(part.blobId);
    writer.varint(part.size);
    writer.string(part.etag);
    writer.varint(static_cast<std::uint64_t>(part.uploadedAt));
    return out;
}

PartRecord decodePart(std::uint32_t partNumber, std::string_view stored) {
    codec::Reader reader(stored);
    expectVersion(reader, "part");

    PartRecord part;
    part.partNumber = partNumber;
    part.blobId     = reader.string();
    part.size       = reader.varint();
    part.etag       = reader.string();
    part.uploadedAt = static_cast<TimestampMs>(reader.varint());
    return part;
}

// ---------------------------------------------------------------------------

class RocksMetadataStore final : public MetadataStore {
public:
    explicit RocksMetadataStore(const MetadataStoreOptions& options) : options_(options) {
        open();
        verifyFormatVersion();
        seedCounters();
    }

    ~RocksMetadataStore() override {
        if (db_) {
            db_->FlushWAL(true);
            db_->Close();
        }
    }

    // --- Buckets -----------------------------------------------------------

    void createBucket(const BucketRecord& bucket) override {
        std::unique_lock guard(bucketLock_);

        std::string existing;
        const auto  status = db_->Get(readOptions_, toSlice(keys::bucket(bucket.name)), &existing);
        if (status.ok()) {
            fail(StorageErrorCode::BucketAlreadyExists, "bucket '" + bucket.name + "' already exists");
        }
        if (!status.IsNotFound()) check(status, "reading bucket '" + bucket.name + "'");

        check(db_->Put(writeOptions_, toSlice(keys::bucket(bucket.name)), toSlice(encodeBucket(bucket))),
              "creating bucket '" + bucket.name + "'");

        buckets_.fetch_add(1, std::memory_order_relaxed);
    }

    std::optional<BucketRecord> getBucket(std::string_view name) override {
        std::shared_lock guard(bucketLock_);
        return loadBucket(name);
    }

    std::vector<BucketRecord> listBuckets() override {
        std::shared_lock guard(bucketLock_);

        std::vector<BucketRecord> buckets;
        const std::string         prefix = keys::bucketPrefix();
        const auto                bound  = keys::upperBound(prefix);

        rocksdb::ReadOptions read = readOptions_;
        rocksdb::Slice       boundSlice;
        if (bound) {
            boundSlice            = toSlice(*bound);
            read.iterate_upper_bound = &boundSlice;
        }

        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read));
        for (it->Seek(toSlice(prefix)); it->Valid(); it->Next()) {
            const auto stored = toView(it->key());
            if (!stored.starts_with(prefix)) break;
            buckets.push_back(decodeBucket(stored.substr(prefix.size()), toView(it->value())));
        }
        check(it->status(), "listing buckets");
        return buckets;
    }

    void deleteBucket(std::string_view name) override {
        // Exclusive: object writers hold this shared, so taking it here is what
        // stops a PUT from landing between the emptiness check and the delete.
        std::unique_lock guard(bucketLock_);

        if (!loadBucket(name)) {
            fail(StorageErrorCode::NoSuchBucket, "no such bucket: " + std::string(name));
        }
        if (hasAny(keys::objectPrefix(name))) {
            fail(StorageErrorCode::BucketNotEmpty, "bucket '" + std::string(name) + "' still contains objects");
        }
        if (hasAny(keys::uploadByKeyPrefix(name))) {
            fail(StorageErrorCode::BucketNotEmpty,
                 "bucket '" + std::string(name) + "' has multipart uploads in progress");
        }

        check(db_->Delete(writeOptions_, toSlice(keys::bucket(name))),
              "deleting bucket '" + std::string(name) + "'");

        buckets_.fetch_sub(1, std::memory_order_relaxed);
    }

    void updateBucket(const BucketRecord& bucket) override {
        std::unique_lock guard(bucketLock_);

        if (!loadBucket(bucket.name)) {
            fail(StorageErrorCode::NoSuchBucket, "no such bucket: " + bucket.name);
        }
        check(db_->Put(writeOptions_, toSlice(keys::bucket(bucket.name)), toSlice(encodeBucket(bucket))),
              "updating bucket '" + bucket.name + "'");
    }

    // --- Objects -----------------------------------------------------------

    PutOutcome putObject(std::string_view bucket, const ObjectRecord& object) override {
        std::shared_lock bucketGuard(bucketLock_);
        requireBucket(bucket);

        const std::string storedKey = keys::object(bucket, object.key);
        std::lock_guard   keyGuard(stripeFor(storedKey));

        PutOutcome outcome;
        if (auto previous = loadObject(storedKey, object.key)) {
            outcome.releasedBlobId = previous->blobId;
            bytes_.fetch_sub(previous->size, std::memory_order_relaxed);
        } else {
            objects_.fetch_add(1, std::memory_order_relaxed);
        }

        rocksdb::WriteBatch batch;
        batch.Put(toSlice(storedKey), toSlice(encodeObject(object)));

        // The payload is durable and now referenced, so it is no longer owed to
        // the reclaimer. Dropping the marker and publishing the object in one
        // batch is what makes the transition atomic.
        batch.Delete(toSlice(keys::orphan(object.blobId)));
        orphans_.fetch_sub(1, std::memory_order_relaxed);

        if (outcome.releasedBlobId) {
            batch.Put(toSlice(keys::orphan(*outcome.releasedBlobId)), toSlice(orphanValue()));
            orphans_.fetch_add(1, std::memory_order_relaxed);
        }

        check(db_->Write(writeOptions_, &batch), "putting object '" + object.key + "'");
        bytes_.fetch_add(object.size, std::memory_order_relaxed);
        return outcome;
    }

    std::optional<ObjectRecord> getObject(std::string_view bucket, std::string_view key) override {
        std::shared_lock guard(bucketLock_);
        return loadObject(keys::object(bucket, key), key);
    }

    DeleteOutcome deleteObject(std::string_view bucket, std::string_view key) override {
        std::shared_lock bucketGuard(bucketLock_);
        requireBucket(bucket);

        const std::string storedKey = keys::object(bucket, key);
        std::lock_guard   keyGuard(stripeFor(storedKey));

        DeleteOutcome outcome;
        const auto    existing = loadObject(storedKey, key);
        if (!existing) return outcome;

        outcome.existed        = true;
        outcome.releasedBlobId = existing->blobId;

        rocksdb::WriteBatch batch;
        batch.Delete(toSlice(storedKey));
        batch.Put(toSlice(keys::orphan(existing->blobId)), toSlice(orphanValue()));

        check(db_->Write(writeOptions_, &batch), "deleting object '" + std::string(key) + "'");

        objects_.fetch_sub(1, std::memory_order_relaxed);
        bytes_.fetch_sub(existing->size, std::memory_order_relaxed);
        orphans_.fetch_add(1, std::memory_order_relaxed);
        return outcome;
    }

    ListObjectsResult listObjects(std::string_view          bucket,
                                  const ListObjectsRequest& request) override {
        std::shared_lock guard(bucketLock_);
        requireBucket(bucket);

        ListObjectsResult result;
        if (request.maxKeys == 0) {
            result.truncated = hasAny(keys::objectPrefix(bucket) + request.prefix);
            return result;
        }

        const std::string tablePrefix = keys::objectPrefix(bucket);
        const std::string scanPrefix  = tablePrefix + request.prefix;

        rocksdb::ReadOptions read = readOptions_;
        const auto           bound = keys::upperBound(scanPrefix);
        rocksdb::Slice       boundSlice;
        if (bound) {
            boundSlice               = toSlice(*bound);
            read.iterate_upper_bound = &boundSlice;
        }

        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read));
        it->Seek(toSlice(resumePoint(tablePrefix, request)));

        std::string lastEmitted;
        while (it->Valid() && result.objects.size() + result.commonPrefixes.size() < request.maxKeys) {
            const auto stored = toView(it->key());
            if (!stored.starts_with(scanPrefix)) break;

            const std::string_view key = stored.substr(tablePrefix.size());

            if (!request.delimiter.empty()) {
                const auto split = key.find(request.delimiter, request.prefix.size());
                if (split != std::string_view::npos) {
                    std::string common(key.substr(0, split + request.delimiter.size()));

                    result.commonPrefixes.push_back(common);
                    lastEmitted = common;

                    // Jump the whole group rather than walking it. A prefix with
                    // a million keys under it costs one seek, not a million
                    // iterator steps.
                    const auto next = keys::upperBound(tablePrefix + common);
                    if (!next) break;
                    it->Seek(toSlice(*next));
                    continue;
                }
            }

            result.objects.push_back(decodeObject(key, toView(it->value())));
            lastEmitted = result.objects.back().key;
            it->Next();
        }
        check(it->status(), "listing objects");

        // Truncated only if something remains beyond what we emitted.
        if (it->Valid() && toView(it->key()).starts_with(scanPrefix)) {
            result.truncated      = true;
            result.nextStartAfter = lastEmitted;
        }
        return result;
    }

    // --- Multipart ---------------------------------------------------------

    void createUpload(const UploadRecord& upload) override {
        std::shared_lock guard(bucketLock_);
        requireBucket(upload.bucket);

        const std::string byId = keys::uploadById(upload.uploadId);
        std::lock_guard   keyGuard(stripeFor(byId));

        rocksdb::WriteBatch batch;
        batch.Put(toSlice(byId), toSlice(encodeUpload(upload)));
        // A second entry ordered by key, because ListMultipartUploads pages by
        // key and an upload-id-ordered scan cannot answer that.
        batch.Put(toSlice(keys::uploadByKey(upload.bucket, upload.key, upload.uploadId)),
                  toSlice(std::string_view{}));

        check(db_->Write(writeOptions_, &batch), "creating upload for '" + upload.key + "'");
        uploads_.fetch_add(1, std::memory_order_relaxed);
    }

    std::optional<UploadRecord> getUpload(std::string_view uploadId) override {
        return loadUpload(uploadId);
    }

    std::vector<UploadRecord> listUploads(std::string_view bucket,
                                          std::uint32_t    maxUploads) override {
        std::shared_lock guard(bucketLock_);
        requireBucket(bucket);

        std::vector<UploadRecord> uploads;
        const std::string         prefix = keys::uploadByKeyPrefix(bucket);
        const auto                bound  = keys::upperBound(prefix);

        rocksdb::ReadOptions read = readOptions_;
        rocksdb::Slice       boundSlice;
        if (bound) {
            boundSlice               = toSlice(*bound);
            read.iterate_upper_bound = &boundSlice;
        }

        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read));
        for (it->Seek(toSlice(prefix)); it->Valid() && uploads.size() < maxUploads; it->Next()) {
            const auto stored = toView(it->key());
            if (!stored.starts_with(prefix)) break;

            const auto parts = keys::uploadKeyParts(stored, bucket);
            if (!parts) continue;

            if (auto upload = loadUpload(parts->uploadId)) uploads.push_back(std::move(*upload));
        }
        check(it->status(), "listing multipart uploads");
        return uploads;
    }

    PutPartOutcome putPart(std::string_view uploadId, const PartRecord& part) override {
        if (part.partNumber == 0 || part.partNumber > limits::kMaxPartNumber) {
            fail(StorageErrorCode::InvalidPart,
                 "part number " + std::to_string(part.partNumber) + " is outside 1.." +
                     std::to_string(limits::kMaxPartNumber));
        }

        const std::string storedKey = keys::part(uploadId, part.partNumber);
        std::lock_guard   keyGuard(stripeFor(storedKey));

        if (!loadUpload(uploadId)) {
            fail(StorageErrorCode::NoSuchUpload, "no such upload: " + std::string(uploadId));
        }

        PutPartOutcome outcome;
        std::string    existing;
        const auto     status = db_->Get(readOptions_, toSlice(storedKey), &existing);
        if (status.ok()) {
            outcome.releasedBlobId = decodePart(part.partNumber, existing).blobId;
        } else if (!status.IsNotFound()) {
            check(status, "reading part " + std::to_string(part.partNumber));
        }

        rocksdb::WriteBatch batch;
        batch.Put(toSlice(storedKey), toSlice(encodePart(part)));
        batch.Delete(toSlice(keys::orphan(part.blobId)));
        orphans_.fetch_sub(1, std::memory_order_relaxed);

        if (outcome.releasedBlobId) {
            batch.Put(toSlice(keys::orphan(*outcome.releasedBlobId)), toSlice(orphanValue()));
            orphans_.fetch_add(1, std::memory_order_relaxed);
        }

        check(db_->Write(writeOptions_, &batch),
              "storing part " + std::to_string(part.partNumber));
        return outcome;
    }

    std::vector<PartRecord> listParts(std::string_view uploadId) override {
        if (!loadUpload(uploadId)) {
            fail(StorageErrorCode::NoSuchUpload, "no such upload: " + std::string(uploadId));
        }
        return collectParts(uploadId);
    }

    std::vector<std::string> abortUpload(std::string_view uploadId) override {
        const auto upload = loadUpload(uploadId);
        if (!upload) fail(StorageErrorCode::NoSuchUpload, "no such upload: " + std::string(uploadId));

        std::lock_guard keyGuard(stripeFor(keys::uploadById(uploadId)));

        const auto parts = collectParts(uploadId);

        rocksdb::WriteBatch      batch;
        std::vector<std::string> released;
        released.reserve(parts.size());

        for (const auto& part : parts) {
            batch.Delete(toSlice(keys::part(uploadId, part.partNumber)));
            // Released, not deleted: the payload outlives the metadata by
            // design, and the reclaimer unlinks it after this batch commits.
            batch.Put(toSlice(keys::orphan(part.blobId)), toSlice(orphanValue()));
            released.push_back(part.blobId);
        }

        batch.Delete(toSlice(keys::uploadById(uploadId)));
        batch.Delete(toSlice(keys::uploadByKey(upload->bucket, upload->key, uploadId)));

        check(db_->Write(writeOptions_, &batch), "aborting upload " + std::string(uploadId));

        uploads_.fetch_sub(1, std::memory_order_relaxed);
        orphans_.fetch_add(released.size(), std::memory_order_relaxed);
        return released;
    }

    CompleteOutcome completeUpload(std::string_view bucket, std::string_view uploadId,
                                   const ObjectRecord& object) override {
        std::shared_lock bucketGuard(bucketLock_);
        requireBucket(bucket);

        const auto upload = loadUpload(uploadId);
        if (!upload) fail(StorageErrorCode::NoSuchUpload, "no such upload: " + std::string(uploadId));

        const std::string storedKey = keys::object(bucket, object.key);
        std::lock_guard   keyGuard(stripeFor(storedKey));

        CompleteOutcome outcome;
        if (auto previous = loadObject(storedKey, object.key)) {
            outcome.releasedBlobId = previous->blobId;
            bytes_.fetch_sub(previous->size, std::memory_order_relaxed);
        } else {
            objects_.fetch_add(1, std::memory_order_relaxed);
        }

        rocksdb::WriteBatch batch;
        batch.Put(toSlice(storedKey), toSlice(encodeObject(object)));
        batch.Delete(toSlice(keys::orphan(object.blobId)));
        orphans_.fetch_sub(1, std::memory_order_relaxed);

        if (outcome.releasedBlobId) {
            batch.Put(toSlice(keys::orphan(*outcome.releasedBlobId)), toSlice(orphanValue()));
            orphans_.fetch_add(1, std::memory_order_relaxed);
        }

        for (const auto& part : collectParts(uploadId)) {
            batch.Delete(toSlice(keys::part(uploadId, part.partNumber)));
            batch.Put(toSlice(keys::orphan(part.blobId)), toSlice(orphanValue()));
            outcome.releasedPartBlobIds.push_back(part.blobId);
        }

        batch.Delete(toSlice(keys::uploadById(uploadId)));
        batch.Delete(toSlice(keys::uploadByKey(upload->bucket, upload->key, uploadId)));

        check(db_->Write(writeOptions_, &batch), "completing upload " + std::string(uploadId));

        bytes_.fetch_add(object.size, std::memory_order_relaxed);
        uploads_.fetch_sub(1, std::memory_order_relaxed);
        orphans_.fetch_add(outcome.releasedPartBlobIds.size(), std::memory_order_relaxed);
        return outcome;
    }

    // --- Reclamation -------------------------------------------------------

    void trackBlob(std::string_view blobId) override {
        check(db_->Put(writeOptions_, toSlice(keys::orphan(blobId)), toSlice(orphanValue())),
              "tracking blob " + std::string(blobId));
        orphans_.fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<std::string> listOrphans(std::size_t limit,
                                         TimestampMs queuedAtOrBefore) override {
        std::vector<std::string> blobIds;
        if (limit == 0) return blobIds;

        const std::string prefix = keys::orphanPrefix();
        const auto        bound  = keys::upperBound(prefix);

        rocksdb::ReadOptions read = readOptions_;
        rocksdb::Slice       boundSlice;
        if (bound) {
            boundSlice               = toSlice(*bound);
            read.iterate_upper_bound = &boundSlice;
        }

        // Ordered by blob id, not by queue time, so this scans past young
        // records rather than stopping at the first one. Orphans are few by
        // construction — the list is drained continuously — so a second index
        // on time would cost more to maintain than it saves here.
        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read));
        for (it->Seek(toSlice(prefix)); it->Valid() && blobIds.size() < limit; it->Next()) {
            const auto stored = toView(it->key());
            if (!stored.starts_with(prefix)) break;

            if (decodeOrphanQueuedAt(toView(it->value())) > queuedAtOrBefore) continue;
            blobIds.emplace_back(stored.substr(prefix.size()));
        }
        check(it->status(), "listing orphaned blobs");
        return blobIds;
    }

    void forgetOrphans(const std::vector<std::string>& blobIds) override {
        if (blobIds.empty()) return;

        rocksdb::WriteBatch batch;
        for (const auto& blobId : blobIds) batch.Delete(toSlice(keys::orphan(blobId)));

        check(db_->Write(writeOptions_, &batch), "forgetting reclaimed blobs");

        // Saturating: trackBlob is also called by writers that may not have
        // committed, so the counter is an estimate that must not wrap.
        std::uint64_t current = orphans_.load(std::memory_order_relaxed);
        const auto    removed = static_cast<std::uint64_t>(blobIds.size());
        orphans_.store(current > removed ? current - removed : 0, std::memory_order_relaxed);
    }

    // --- Introspection -----------------------------------------------------

    UsageStats usage() const override {
        UsageStats stats;
        stats.buckets     = buckets_.load(std::memory_order_relaxed);
        stats.objects     = objects_.load(std::memory_order_relaxed);
        stats.bytes       = bytes_.load(std::memory_order_relaxed);
        stats.uploads     = uploads_.load(std::memory_order_relaxed);
        stats.orphanBlobs = orphans_.load(std::memory_order_relaxed);
        return stats;
    }

    std::vector<std::pair<std::string, std::uint64_t>> engineGauges() const override {
        // These three are the ones that answer "why is RSS what it is?".
        static constexpr std::array<std::pair<const char*, const char*>, 4> kProperties{{
            {"memtable_bytes", "rocksdb.cur-size-all-mem-tables"},
            {"block_cache_bytes", "rocksdb.block-cache-usage"},
            {"table_readers_bytes", "rocksdb.estimate-table-readers-mem"},
            {"sst_bytes", "rocksdb.total-sst-files-size"},
        }};

        std::vector<std::pair<std::string, std::uint64_t>> gauges;
        gauges.reserve(kProperties.size());
        for (const auto& [name, property] : kProperties) {
            std::uint64_t value = 0;
            if (db_->GetAggregatedIntProperty(property, &value)) gauges.emplace_back(name, value);
        }
        return gauges;
    }

    void flush() override {
        if (!db_) return;
        rocksdb::FlushOptions flushOptions;
        flushOptions.wait = true;
        db_->Flush(flushOptions);
        db_->FlushWAL(true);
    }

    std::string_view engineName() const noexcept override { return "rocksdb"; }

private:
    // --- Setup -------------------------------------------------------------

    void open() {
        rocksdb::Options dbOptions;
        dbOptions.create_if_missing = true;

        // One shared budget for block cache *and* memtables. Left to itself
        // RocksDB sizes them independently, and the container only sees the
        // sum; charging the WriteBufferManager to the same cache is what turns
        // MONOBUCKET_METADATA_MEMORY_BYTES into an actual ceiling.
        cache_ = rocksdb::NewLRUCache(options_.memoryBudgetBytes);

        const std::size_t writeBuffer = std::max<std::size_t>(
            1u << 20, static_cast<std::size_t>(options_.memoryBudgetBytes / 8));

        dbOptions.write_buffer_manager =
            std::make_shared<rocksdb::WriteBufferManager>(writeBuffer * 2, cache_);
        dbOptions.write_buffer_size      = writeBuffer;
        dbOptions.max_write_buffer_number = 2;

        rocksdb::BlockBasedTableOptions tableOptions;
        tableOptions.block_cache = cache_;
        // Index and filter blocks are otherwise held outside the cache and grow
        // without bound as the SST count does.
        tableOptions.cache_index_and_filter_blocks             = true;
        tableOptions.pin_l0_filter_and_index_blocks_in_cache   = true;
        tableOptions.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        dbOptions.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tableOptions));

        // Every open SST pins a table reader; unbounded, that is a slow leak on
        // a long-lived process with many objects.
        dbOptions.max_open_files = options_.maxOpenFiles;

        dbOptions.max_background_jobs               = 2;
        dbOptions.level_compaction_dynamic_level_bytes = true;
        dbOptions.compression                       = rocksdb::kNoCompression;
        dbOptions.bottommost_compression            = rocksdb::kZSTD;
        dbOptions.avoid_unnecessary_blocking_io     = true;
        dbOptions.keep_log_file_num                 = 4;
        dbOptions.recycle_log_file_num              = 2;

        rocksdb::DB* raw = nullptr;
        const auto   status = rocksdb::DB::Open(dbOptions, options_.path, &raw);
        if (!status.ok()) {
            fail(StorageErrorCode::Io,
                 "cannot open the metadata store at '" + options_.path + "': " + status.ToString());
        }
        db_.reset(raw);

        writeOptions_.sync = options_.syncWrites;
        readOptions_.verify_checksums = true;

        log::info("metadata store open at ", options_.path, " (rocksdb ", ROCKSDB_MAJOR, '.',
                  ROCKSDB_MINOR, '.', ROCKSDB_PATCH, ", budget ",
                  options_.memoryBudgetBytes / (1024 * 1024), " MiB, sync=",
                  options_.syncWrites ? "on" : "off", ')');
    }

    void verifyFormatVersion() {
        const std::string key = keys::meta("format-version");

        std::string stored;
        const auto  status = db_->Get(readOptions_, toSlice(key), &stored);

        if (status.IsNotFound()) {
            check(db_->Put(writeOptions_, toSlice(key),
                           toSlice(std::to_string(kStorageFormatVersion))),
                  "stamping the storage format version");
            return;
        }
        check(status, "reading the storage format version");

        const auto found = std::strtoull(stored.c_str(), nullptr, 10);
        if (found != kStorageFormatVersion) {
            fail(StorageErrorCode::Corruption,
                 "the data directory was written with storage format version " + stored +
                     " but this build speaks version " + std::to_string(kStorageFormatVersion) +
                     "; see CHANGELOG.md for the migration");
        }
    }

    /// Counters are maintained incrementally at runtime, so they only need
    /// seeding once. This is the only full scan in the process lifetime and it
    /// doubles as a decode check over every stored record.
    void seedCounters() {
        const auto started = std::chrono::steady_clock::now();

        std::uint64_t buckets = 0;
        std::uint64_t objects = 0;
        std::uint64_t bytes   = 0;
        std::uint64_t uploads = 0;
        std::uint64_t orphans = 0;

        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(readOptions_));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            const auto stored = toView(it->key());
            if (stored.empty()) continue;

            switch (stored.front()) {
                case keys::kBucket: ++buckets; break;
                case keys::kUploadById: ++uploads; break;
                case keys::kOrphan: ++orphans; break;
                case keys::kObject: {
                    ++objects;
                    codec::Reader reader(toView(it->value()));
                    expectVersion(reader, "object");
                    reader.string();  // blobId
                    bytes += reader.varint();
                    break;
                }
                default: break;
            }
        }
        check(it->status(), "scanning the metadata store at startup");

        buckets_.store(buckets, std::memory_order_relaxed);
        objects_.store(objects, std::memory_order_relaxed);
        bytes_.store(bytes, std::memory_order_relaxed);
        uploads_.store(uploads, std::memory_order_relaxed);
        orphans_.store(orphans, std::memory_order_relaxed);

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        log::info("metadata scan: ", buckets, " buckets, ", objects, " objects, ", uploads,
                  " uploads in progress, ", orphans, " blobs pending reclaim (", elapsed.count(),
                  " ms)");
    }

    // --- Helpers -----------------------------------------------------------

    /// The value of an orphan marker is its queue time — enough to age
    /// reclamation without a second index.
    static std::string orphanValue() {
        std::string   out;
        codec::Writer writer(out);
        writer.u8(kRecordVersion);
        writer.varint(static_cast<std::uint64_t>(nowMs()));
        return out;
    }

    /// A marker that cannot be decoded is treated as ancient rather than
    /// fatal: refusing to reclaim is how a store fills up, and the worst case
    /// of being wrong here is unlinking a payload nothing references.
    static TimestampMs decodeOrphanQueuedAt(std::string_view stored) {
        try {
            codec::Reader reader(stored);
            expectVersion(reader, "orphan");
            return static_cast<TimestampMs>(reader.varint());
        } catch (const std::exception&) {
            return 0;
        }
    }

    std::mutex& stripeFor(std::string_view key) {
        return stripes_[std::hash<std::string_view>{}(key) % kLockStripes];
    }

    std::optional<BucketRecord> loadBucket(std::string_view name) {
        std::string stored;
        const auto  status = db_->Get(readOptions_, toSlice(keys::bucket(name)), &stored);
        if (status.IsNotFound()) return std::nullopt;
        check(status, "reading bucket '" + std::string(name) + "'");
        return decodeBucket(name, stored);
    }

    void requireBucket(std::string_view name) {
        std::string stored;
        const auto  status = db_->Get(readOptions_, toSlice(keys::bucket(name)), &stored);
        if (status.IsNotFound()) {
            fail(StorageErrorCode::NoSuchBucket, "no such bucket: " + std::string(name));
        }
        check(status, "reading bucket '" + std::string(name) + "'");
    }

    std::optional<ObjectRecord> loadObject(const std::string& storedKey, std::string_view key) {
        std::string stored;
        const auto  status = db_->Get(readOptions_, toSlice(storedKey), &stored);
        if (status.IsNotFound()) return std::nullopt;
        check(status, "reading object '" + std::string(key) + "'");
        return decodeObject(key, stored);
    }

    std::optional<UploadRecord> loadUpload(std::string_view uploadId) {
        std::string stored;
        const auto  status = db_->Get(readOptions_, toSlice(keys::uploadById(uploadId)), &stored);
        if (status.IsNotFound()) return std::nullopt;
        check(status, "reading upload " + std::string(uploadId));
        return decodeUpload(uploadId, stored);
    }

    std::vector<PartRecord> collectParts(std::string_view uploadId) {
        std::vector<PartRecord> parts;
        const std::string       prefix = keys::partPrefix(uploadId);
        const auto              bound  = keys::upperBound(prefix);

        rocksdb::ReadOptions read = readOptions_;
        rocksdb::Slice       boundSlice;
        if (bound) {
            boundSlice               = toSlice(*bound);
            read.iterate_upper_bound = &boundSlice;
        }

        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read));
        for (it->Seek(toSlice(prefix)); it->Valid(); it->Next()) {
            const auto stored = toView(it->key());
            if (!stored.starts_with(prefix)) break;

            const auto number = keys::partNumber(stored, uploadId);
            if (!number) continue;
            parts.push_back(decodePart(*number, toView(it->value())));
        }
        check(it->status(), "listing parts of upload " + std::string(uploadId));
        return parts;
    }

    bool hasAny(const std::string& prefix) {
        const auto           bound = keys::upperBound(prefix);
        rocksdb::ReadOptions read  = readOptions_;
        rocksdb::Slice       boundSlice;
        if (bound) {
            boundSlice               = toSlice(*bound);
            read.iterate_upper_bound = &boundSlice;
        }

        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read));
        it->Seek(toSlice(prefix));
        const bool found = it->Valid() && toView(it->key()).starts_with(prefix);
        check(it->status(), "probing for keys under a prefix");
        return found;
    }

    /// Where a listing resumes.
    ///
    /// `startAfter` is exclusive, and no object key may contain NUL, so
    /// `<startAfter>\0` is the least key strictly greater than it.
    ///
    /// The exception is resuming after a *common prefix*. S3 reports the last
    /// common prefix as the continuation marker, and seeking just past the
    /// literal string would land back inside the group and re-emit the same
    /// prefix forever. When the marker ends with the delimiter it therefore
    /// denotes the whole group, and the resume point is past all of it.
    static std::string resumePoint(const std::string& tablePrefix,
                                   const ListObjectsRequest& request) {
        if (request.startAfter.empty() || request.startAfter < request.prefix) {
            return tablePrefix + request.prefix;
        }

        if (!request.delimiter.empty() && request.startAfter.ends_with(request.delimiter)) {
            if (auto next = keys::upperBound(tablePrefix + request.startAfter)) return *next;
        }

        return tablePrefix + request.startAfter + keys::kSeparator;
    }

    MetadataStoreOptions             options_;
    std::unique_ptr<rocksdb::DB>     db_;
    std::shared_ptr<rocksdb::Cache>  cache_;
    rocksdb::ReadOptions             readOptions_;
    rocksdb::WriteOptions            writeOptions_;

    /// Held shared by anything that touches a bucket's contents and exclusively
    /// by bucket creation and deletion, so "is this bucket empty?" cannot be
    /// invalidated between the check and the delete. There is exactly one
    /// writer process, so in-process locking is sufficient; RocksDB's own
    /// transactions would add conflict detection for a second writer that by
    /// design never exists.
    mutable std::shared_mutex                 bucketLock_;
    std::array<std::mutex, kLockStripes>      stripes_;

    std::atomic<std::uint64_t> buckets_{0};
    std::atomic<std::uint64_t> objects_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> uploads_{0};
    std::atomic<std::uint64_t> orphans_{0};
};

}  // namespace

std::string_view toString(StorageErrorCode code) {
    switch (code) {
        case StorageErrorCode::NoSuchBucket:        return "NoSuchBucket";
        case StorageErrorCode::NoSuchKey:           return "NoSuchKey";
        case StorageErrorCode::NoSuchUpload:        return "NoSuchUpload";
        case StorageErrorCode::BucketAlreadyExists: return "BucketAlreadyExists";
        case StorageErrorCode::BucketNotEmpty:      return "BucketNotEmpty";
        case StorageErrorCode::InvalidPart:         return "InvalidPart";
        case StorageErrorCode::Corruption:          return "Corruption";
        case StorageErrorCode::Io:                  return "Io";
        case StorageErrorCode::Internal:            return "Internal";
    }
    return "Internal";
}

std::unique_ptr<MetadataStore> openRocksMetadataStore(const MetadataStoreOptions& options) {
    return std::make_unique<RocksMetadataStore>(options);
}

}  // namespace monobucket
