#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "storage/quota.hpp"
#include "storage/storage_engine.hpp"
#include "temporary_directory.hpp"

using monobucket::BucketCapacity;
using monobucket::QuotaLedger;
using monobucket::StorageEngine;
using monobucket::StorageError;
using monobucket::StorageErrorCode;
using monobucket::testing::TemporaryDirectory;

namespace {

constexpr std::uint64_t kKiB = 1024;

StorageEngine::Options optionsFor(const TemporaryDirectory& root, std::uint64_t allocatable = 0) {
    StorageEngine::Options options;
    options.dataDir             = root.path();
    options.durability          = monobucket::Durability::None;
    options.chunkBytes          = 64 * 1024;
    options.metadataMemoryBytes = 8ull * 1024 * 1024;
    options.allocatableBytes    = allocatable;
    return options;
}

StorageEngine::PutRequest request(std::string bucket, std::string key) {
    StorageEngine::PutRequest put;
    put.bucket = std::move(bucket);
    put.key    = std::move(key);
    return put;
}

StorageErrorCode codeOf(const std::function<void()>& action) {
    try {
        action();
    } catch (const StorageError& error) {
        return error.code();
    }
    return StorageErrorCode::Internal;
}

}  // namespace

// --- The ledger on its own -------------------------------------------------

TEST_CASE("an unlimited bucket admits everything", "[quota]") {
    QuotaLedger ledger;
    ledger.track("b", 0);

    auto claim = ledger.reserve("b", 1ull << 40);
    ledger.recordObject("b", std::move(claim), 1ull << 40, 0);

    const BucketCapacity capacity = ledger.capacity("b");
    CHECK(capacity.unlimited());
    CHECK(capacity.usedBytes == (1ull << 40));
    CHECK_FALSE(capacity.remainingBytes().has_value());
}

TEST_CASE("a claim that does not fit is refused", "[quota]") {
    QuotaLedger ledger;
    ledger.track("b", 10 * kKiB);

    auto claim = ledger.reserve("b", 6 * kKiB);
    CHECK(ledger.capacity("b").reservedBytes == 6 * kKiB);

    CHECK(codeOf([&] { (void)ledger.reserve("b", 5 * kKiB); }) == StorageErrorCode::QuotaExceeded);

    // The refused claim left nothing behind: the bucket still owes exactly what
    // the first one asked for.
    CHECK(ledger.capacity("b").reservedBytes == 6 * kKiB);
}

TEST_CASE("a released claim frees the allocation again", "[quota]") {
    QuotaLedger ledger;
    ledger.track("b", 10 * kKiB);

    {
        auto claim = ledger.reserve("b", 10 * kKiB);
        CHECK(codeOf([&] { (void)ledger.reserve("b", 1); }) == StorageErrorCode::QuotaExceeded);
    }

    // The claim went out of scope without being settled — which is what a write
    // that threw looks like — and the bucket is back to empty.
    CHECK(ledger.capacity("b").reservedBytes == 0);
    CHECK_NOTHROW([&] { auto claim = ledger.reserve("b", 10 * kKiB); }());
}

TEST_CASE("settlement applies the overwrite delta, not the whole write", "[quota]") {
    QuotaLedger ledger;
    ledger.track("b", 10 * kKiB);

    ledger.recordObject("b", ledger.reserve("b", 4 * kKiB), 4 * kKiB, 0);
    CHECK(ledger.capacity("b").usedBytes == 4 * kKiB);

    // Replacing a 4 KiB object with a 6 KiB one nets 2 KiB, so a bucket that
    // could not hold 10 KiB of new objects can still hold this.
    ledger.recordObject("b", ledger.reserve("b", 6 * kKiB), 6 * kKiB, 4 * kKiB);
    CHECK(ledger.capacity("b").usedBytes == 6 * kKiB);
    CHECK(ledger.capacity("b").reservedBytes == 0);
    CHECK(*ledger.capacity("b").remainingBytes() == 4 * kKiB);
}

TEST_CASE("parts are pending until the upload completes", "[quota]") {
    QuotaLedger ledger;
    ledger.track("b", 10 * kKiB);

    ledger.recordPart("b", ledger.reserve("b", 3 * kKiB), 3 * kKiB, 0);
    ledger.recordPart("b", ledger.reserve("b", 3 * kKiB), 3 * kKiB, 0);

    // Charged, so the allocation is honoured while the upload is open — but not
    // used, because no object names those bytes yet.
    CHECK(ledger.capacity("b").pendingBytes == 6 * kKiB);
    CHECK(ledger.capacity("b").usedBytes == 0);
    CHECK(*ledger.capacity("b").remainingBytes() == 4 * kKiB);

    ledger.recordUploadCompleted("b", 6 * kKiB, 0, 6 * kKiB);
    CHECK(ledger.capacity("b").pendingBytes == 0);
    CHECK(ledger.capacity("b").usedBytes == 6 * kKiB);
    CHECK(*ledger.capacity("b").remainingBytes() == 4 * kKiB);
}

TEST_CASE("an aborted upload gives its allocation back", "[quota]") {
    QuotaLedger ledger;
    ledger.track("b", 10 * kKiB);

    ledger.recordPart("b", ledger.reserve("b", 8 * kKiB), 8 * kKiB, 0);
    ledger.recordUploadAborted("b", 8 * kKiB);

    CHECK(ledger.capacity("b").pendingBytes == 0);
    CHECK(ledger.capacity("b").usedBytes == 0);
    CHECK(*ledger.capacity("b").remainingBytes() == 10 * kKiB);
}

TEST_CASE("re-uploading a part replaces rather than adds", "[quota]") {
    QuotaLedger ledger;
    ledger.track("b", 10 * kKiB);

    ledger.recordPart("b", ledger.reserve("b", 5 * kKiB), 5 * kKiB, 0);
    ledger.recordPart("b", ledger.reserve("b", 5 * kKiB), 5 * kKiB, 5 * kKiB);

    CHECK(ledger.capacity("b").pendingBytes == 5 * kKiB);
}

TEST_CASE("deleting frees the bytes it released", "[quota]") {
    QuotaLedger ledger;
    ledger.track("b", 10 * kKiB);

    ledger.recordObject("b", ledger.reserve("b", 9 * kKiB), 9 * kKiB, 0);
    CHECK(codeOf([&] { (void)ledger.reserve("b", 2 * kKiB); }) == StorageErrorCode::QuotaExceeded);

    ledger.recordDeletion("b", 9 * kKiB);
    CHECK(ledger.capacity("b").usedBytes == 0);
    CHECK_NOTHROW([&] { auto claim = ledger.reserve("b", 10 * kKiB); }());
}

TEST_CASE("an allocation cannot be cut below what is stored", "[quota]") {
    QuotaLedger ledger;
    ledger.track("b", 10 * kKiB);
    ledger.recordObject("b", ledger.reserve("b", 6 * kKiB), 6 * kKiB, 0);

    CHECK(codeOf([&] { ledger.setQuota("b", 5 * kKiB); }) == StorageErrorCode::QuotaBelowUsage);
    CHECK(ledger.capacity("b").quotaBytes == 10 * kKiB);

    CHECK_NOTHROW(ledger.setQuota("b", 6 * kKiB));
    CHECK(*ledger.capacity("b").remainingBytes() == 0);
}

TEST_CASE("a reduction cannot strand a write already admitted", "[quota]") {
    QuotaLedger ledger;
    ledger.track("b", 10 * kKiB);

    auto claim = ledger.reserve("b", 8 * kKiB);
    // The bytes are not stored yet, but they have been promised. A reduction
    // that ignored the claim would let the write commit over the new figure.
    CHECK(codeOf([&] { ledger.setQuota("b", 4 * kKiB); }) == StorageErrorCode::QuotaBelowUsage);
}

TEST_CASE("allocations cannot oversubscribe the instance", "[quota]") {
    QuotaLedger ledger(10 * kKiB);
    ledger.track("a", 6 * kKiB);

    CHECK_NOTHROW(ledger.admitAllocation(4 * kKiB));
    CHECK(codeOf([&] { ledger.admitAllocation(5 * kKiB); }) ==
          StorageErrorCode::InsufficientCapacity);

    ledger.track("b", 4 * kKiB);
    CHECK(ledger.instance().allocatedBytes == 10 * kKiB);
    CHECK(ledger.instance().remainingBytes() == 0);

    // Raising an existing allocation is measured against the others, not
    // against itself — otherwise no bucket could ever be given more.
    CHECK(codeOf([&] { ledger.setQuota("b", 5 * kKiB); }) ==
          StorageErrorCode::InsufficientCapacity);
    ledger.setQuota("a", 5 * kKiB);
    CHECK_NOTHROW(ledger.setQuota("b", 5 * kKiB));
}

TEST_CASE("unlimited buckets are counted but not allocated", "[quota]") {
    QuotaLedger ledger(10 * kKiB);
    ledger.seed("legacy", 0, 3 * kKiB, 0);
    ledger.track("sized", 4 * kKiB);

    const auto instance = ledger.instance();
    CHECK(instance.allocatedBytes == 4 * kKiB);
    CHECK(instance.usedBytes == 3 * kKiB);
    CHECK(instance.unlimitedBuckets == 1);
    CHECK(instance.remainingBytes() == 6 * kKiB);
}

TEST_CASE("concurrent claims cannot exceed the allocation", "[quota]") {
    // Sixteen threads racing for ten slots against a ten-slot bucket. The
    // check and the charge happen under one lock, so exactly ten win — an
    // implementation that released a claim before recording its charge would
    // let a straggler in and finish over the allocation.
    QuotaLedger ledger;
    ledger.track("b", 10 * kKiB);

    std::atomic<int>         admitted{0};
    std::vector<std::thread> racers;
    racers.reserve(16);

    for (int i = 0; i < 16; ++i) {
        racers.emplace_back([&] {
            try {
                auto claim = ledger.reserve("b", kKiB);
                ledger.recordObject("b", std::move(claim), kKiB, 0);
                admitted.fetch_add(1);
            } catch (const StorageError&) {
                // Refused, which is the correct outcome for six of them.
            }
        });
    }
    for (auto& racer : racers) racer.join();

    CHECK(admitted.load() == 10);
    CHECK(ledger.capacity("b").usedBytes == 10 * kKiB);
    CHECK(ledger.capacity("b").reservedBytes == 0);
    CHECK(*ledger.capacity("b").remainingBytes() == 0);
}

// --- Through the storage engine --------------------------------------------

TEST_CASE("a write past the allocation is refused before it commits", "[quota][engine]") {
    TemporaryDirectory root("quota-refuse");
    StorageEngine      storage(optionsFor(root));

    storage.createBucket("b", 4 * kKiB);
    storage.putObject(request("b", "small"), std::string(3 * kKiB, 'x'));

    const auto oversized = std::string(2 * kKiB, 'y');
    CHECK(codeOf([&] { storage.putObject(request("b", "big"), oversized); }) ==
          StorageErrorCode::QuotaExceeded);

    // Refused before the metadata commit, so the object never became visible
    // and the bucket's usage is exactly what the first write left.
    CHECK_FALSE(storage.statObject("b", "big").has_value());
    CHECK(storage.bucketCapacity("b").usedBytes == 3 * kKiB);
}

TEST_CASE("an unlimited bucket is not enforced", "[quota][engine]") {
    TemporaryDirectory root("quota-unlimited");
    StorageEngine      storage(optionsFor(root));

    storage.createBucket("b");
    CHECK(storage.getBucket("b")->quotaBytes == 0);
    CHECK_NOTHROW(storage.putObject(request("b", "k"), std::string(64 * kKiB, 'x')));
    CHECK(storage.bucketCapacity("b").usedBytes == 64 * kKiB);
}

TEST_CASE("usage follows overwrites and deletes", "[quota][engine]") {
    TemporaryDirectory root("quota-deltas");
    StorageEngine      storage(optionsFor(root));

    storage.createBucket("b", 16 * kKiB);
    storage.putObject(request("b", "k"), std::string(8 * kKiB, 'x'));
    CHECK(storage.bucketCapacity("b").usedBytes == 8 * kKiB);

    storage.putObject(request("b", "k"), std::string(2 * kKiB, 'y'));
    CHECK(storage.bucketCapacity("b").usedBytes == 2 * kKiB);

    storage.deleteObject("b", "k");
    CHECK(storage.bucketCapacity("b").usedBytes == 0);
    CHECK(*storage.bucketCapacity("b").remainingBytes() == 16 * kKiB);
}

TEST_CASE("an allocation survives a restart and usage is recounted", "[quota][engine]") {
    TemporaryDirectory root("quota-restart");

    {
        StorageEngine storage(optionsFor(root));
        storage.createBucket("b", 32 * kKiB);
        storage.putObject(request("b", "one"), std::string(5 * kKiB, 'x'));
        storage.putObject(request("b", "two"), std::string(7 * kKiB, 'y'));
    }

    StorageEngine reopened(optionsFor(root));
    const auto    capacity = reopened.bucketCapacity("b");
    CHECK(capacity.quotaBytes == 32 * kKiB);
    CHECK(capacity.usedBytes == 12 * kKiB);
    CHECK(*capacity.remainingBytes() == 20 * kKiB);
}

TEST_CASE("an open upload's parts are charged across a restart", "[quota][engine]") {
    TemporaryDirectory root("quota-parts-restart");
    std::string        uploadId;

    {
        StorageEngine storage(optionsFor(root));
        storage.createBucket("b", 32 * kKiB);
        uploadId = storage.createUpload(request("b", "k"));

        auto writer = storage.beginWrite();
        writer.write(std::string(6 * kKiB, 'p'));
        storage.finishPart(uploadId, 1, std::move(writer));
        CHECK(storage.bucketCapacity("b").pendingBytes == 6 * kKiB);
    }

    // A multipart upload outlives a restart, and so must the charge: parts that
    // were forgotten here would be free storage until somebody aborted them.
    StorageEngine reopened(optionsFor(root));
    CHECK(reopened.bucketCapacity("b").pendingBytes == 6 * kKiB);
    CHECK(reopened.bucketCapacity("b").usedBytes == 0);

    reopened.abortUpload(uploadId);
    CHECK(reopened.bucketCapacity("b").pendingBytes == 0);
}

TEST_CASE("a bucket cannot be allocated more than the instance has", "[quota][engine]") {
    TemporaryDirectory root("quota-instance");
    StorageEngine      storage(optionsFor(root, 10 * kKiB));

    storage.createBucket("a", 7 * kKiB);
    CHECK(codeOf([&] { storage.createBucket("b", 4 * kKiB); }) ==
          StorageErrorCode::InsufficientCapacity);

    // Refused before the record was written, so no half-created bucket is left
    // holding an allocation the instance cannot back.
    CHECK_FALSE(storage.getBucket("b").has_value());

    CHECK_NOTHROW(storage.createBucket("b", 3 * kKiB));
    CHECK(storage.capacity().remainingBytes() == 0);
}

TEST_CASE("a bucket created over S3 takes the configured default", "[quota][engine]") {
    TemporaryDirectory     root("quota-default");
    StorageEngine::Options options   = optionsFor(root, 64 * kKiB);
    options.defaultBucketQuotaBytes  = 8 * kKiB;
    StorageEngine storage(options);

    storage.createBucketWithDefaultQuota("b");
    CHECK(storage.getBucket("b")->quotaBytes == 8 * kKiB);
    CHECK(codeOf([&] { storage.putObject(request("b", "k"), std::string(9 * kKiB, 'x')); }) ==
          StorageErrorCode::QuotaExceeded);
}

TEST_CASE("changing an allocation is persisted and enforced", "[quota][engine]") {
    TemporaryDirectory root("quota-change");
    StorageEngine      storage(optionsFor(root, 64 * kKiB));

    storage.createBucket("b", 8 * kKiB);
    storage.putObject(request("b", "k"), std::string(6 * kKiB, 'x'));

    CHECK(codeOf([&] { storage.setBucketQuota("b", 4 * kKiB); }) ==
          StorageErrorCode::QuotaBelowUsage);
    CHECK(storage.getBucket("b")->quotaBytes == 8 * kKiB);

    storage.setBucketQuota("b", 16 * kKiB);
    CHECK(storage.getBucket("b")->quotaBytes == 16 * kKiB);
    CHECK_NOTHROW(storage.putObject(request("b", "k2"), std::string(9 * kKiB, 'y')));
}

TEST_CASE("deleting a bucket returns its allocation", "[quota][engine]") {
    TemporaryDirectory root("quota-delete-bucket");
    StorageEngine      storage(optionsFor(root, 10 * kKiB));

    storage.createBucket("a", 10 * kKiB);
    CHECK(codeOf([&] { storage.createBucket("b", kKiB); }) ==
          StorageErrorCode::InsufficientCapacity);

    storage.deleteBucket("a");
    CHECK(storage.capacity().allocatedBytes == 0);
    CHECK_NOTHROW(storage.createBucket("b", 10 * kKiB));
}

TEST_CASE("a completed upload does not double-count its parts", "[quota][engine]") {
    TemporaryDirectory root("quota-complete");
    StorageEngine      storage(optionsFor(root));

    // Real part sizes: every part but the last must clear the 5 MiB minimum, so
    // this is the smallest upload the completion path will actually accept.
    constexpr std::uint64_t kMiB = 1024 * 1024;

    storage.createBucket("b", 32 * kMiB);
    const std::string uploadId = storage.createUpload(request("b", "k"));

    std::vector<StorageEngine::RequestedPart> manifest;
    for (const auto& [number, size] :
         std::vector<std::pair<std::uint32_t, std::uint64_t>>{{1u, 6 * kMiB}, {2u, 2 * kMiB}}) {
        auto writer = storage.beginWrite();
        writer.write(std::string(size, 'p'));
        const auto part = storage.finishPart(uploadId, number, std::move(writer));
        manifest.push_back({number, part.etag});
    }

    CHECK(storage.bucketCapacity("b").pendingBytes == 8 * kMiB);

    storage.completeUpload(uploadId, manifest);
    const auto capacity = storage.bucketCapacity("b");
    CHECK(capacity.pendingBytes == 0);
    CHECK(capacity.usedBytes == 8 * kMiB);
    CHECK(*capacity.remainingBytes() == 24 * kMiB);
}
