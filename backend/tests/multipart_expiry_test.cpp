#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <string>
#include <utility>
#include <vector>

#include "storage/storage_engine.hpp"
#include "temporary_directory.hpp"

using monobucket::ListUploadsRequest;
using monobucket::ListUploadsResult;
using monobucket::StorageEngine;
using monobucket::TimestampMs;
using monobucket::testing::TemporaryDirectory;

namespace {

constexpr std::uint64_t kMiB = 1024 * 1024;

/// Far enough in the future that every record written by a test counts as
/// older than it. Expiry is expressed as a cutoff rather than a duration so a
/// test can say "older than everything" without waiting for a clock.
constexpr TimestampMs kFuture = 4102444800000;  // 2100-01-01

/// The mirror image: nothing a test writes is older than the epoch.
constexpr TimestampMs kPast = 1;

StorageEngine::Options optionsFor(const TemporaryDirectory& root) {
    StorageEngine::Options options;
    options.dataDir             = root.path();
    options.durability          = monobucket::Durability::None;
    options.chunkBytes          = 64 * 1024;
    options.metadataMemoryBytes = 8ull * 1024 * 1024;
    return options;
}

StorageEngine::PutRequest request(std::string bucket, std::string key) {
    StorageEngine::PutRequest put;
    put.bucket = std::move(bucket);
    put.key    = std::move(key);
    return put;
}

void storePart(StorageEngine& engine, const std::string& uploadId, std::uint32_t number,
               std::uint64_t bytes) {
    auto writer = engine.beginWrite();
    writer.write(std::string(bytes, 'p'));
    engine.finishPart(uploadId, number, std::move(writer));
}

ListUploadsResult page(StorageEngine& engine, const std::string& bucket, std::uint32_t maxUploads,
                       std::string keyMarker = {}, std::string uploadIdMarker = {}) {
    ListUploadsRequest query;
    query.maxUploads     = maxUploads;
    query.keyMarker      = std::move(keyMarker);
    query.uploadIdMarker = std::move(uploadIdMarker);
    return engine.listUploads(bucket, query);
}

}  // namespace

// --- The sweep -------------------------------------------------------------

TEST_CASE("an abandoned upload is aborted and gives back its parts", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-abandoned");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("b", 0);

    const auto before = engine.bucketCapacity("b");

    const std::string uploadId = engine.createUpload(request("b", "big"));
    storePart(engine, uploadId, 1, 5 * kMiB);
    storePart(engine, uploadId, 2, 5 * kMiB);

    // The parts are on disk and charged the moment they are stored, which is
    // the whole reason an upload nobody finishes cannot simply be ignored.
    CHECK(engine.bucketCapacity("b").pendingBytes == 10 * kMiB);

    CHECK(engine.sweepUploadsIdleBefore(64, kFuture) == 1);

    CHECK_FALSE(engine.getUpload(uploadId).has_value());
    // Back to exactly where the bucket started: an upload that never completed
    // never produced an object, so nothing should have moved to used bytes.
    CHECK(engine.bucketCapacity("b").pendingBytes == before.pendingBytes);
    CHECK(engine.bucketCapacity("b").usedBytes == before.usedBytes);
}

TEST_CASE("an upload younger than the cutoff is left alone", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-young");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("b", 0);

    const std::string uploadId = engine.createUpload(request("b", "fresh"));
    storePart(engine, uploadId, 1, 5 * kMiB);

    CHECK(engine.sweepUploadsIdleBefore(64, kPast) == 0);
    CHECK(engine.getUpload(uploadId).has_value());
    CHECK(engine.bucketCapacity("b").pendingBytes == 5 * kMiB);
}

TEST_CASE("an upload still receiving parts is not abandoned", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-progressing");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("b", 0);

    const std::string uploadId = engine.createUpload(request("b", "slow"));
    storePart(engine, uploadId, 1, 5 * kMiB);

    // The upload record itself is old enough to be swept — a transfer that has
    // been running for longer than the expiry always will be. What decides is
    // the most recent part, and there is one from just now, so this is a
    // client making progress rather than one that walked away.
    const auto candidates = engine.listUploads("b", ListUploadsRequest{}).uploads;
    REQUIRE(candidates.size() == 1);

    const TimestampMs justAfterCreation = candidates.front().createdAt + 1;
    CHECK(engine.sweepUploadsIdleBefore(64, justAfterCreation) == 0);
    CHECK(engine.getUpload(uploadId).has_value());
}

TEST_CASE("an upload with no parts at all is still reclaimed", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-empty");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("b", 0);

    // Nothing was ever charged for it, so this leaks a metadata row rather
    // than bytes — but a store that accumulates one per abandoned client is
    // still a store that grows without bound.
    const std::string uploadId = engine.createUpload(request("b", "never-started"));

    CHECK(engine.sweepUploadsIdleBefore(64, kFuture) == 1);
    CHECK_FALSE(engine.getUpload(uploadId).has_value());
}

TEST_CASE("a completed upload is not swept afterwards", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-completed");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("b", 0);

    const std::string uploadId = engine.createUpload(request("b", "done"));
    auto              writer   = engine.beginWrite();
    writer.write(std::string(5 * kMiB, 'p'));
    const auto part = engine.finishPart(uploadId, 1, std::move(writer));

    engine.completeUpload(uploadId, {{1, part.etag}});

    CHECK(engine.sweepUploadsIdleBefore(64, kFuture) == 0);
    CHECK(engine.getObject("b", "done").has_value());
    CHECK(engine.bucketCapacity("b").usedBytes == 5 * kMiB);
    CHECK(engine.bucketCapacity("b").pendingBytes == 0);
}

TEST_CASE("the sweep is bounded and resumes on the next pass", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-bounded");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("b", 0);

    for (int i = 0; i < 5; ++i) {
        engine.createUpload(request("b", "key-" + std::to_string(i)));
    }

    CHECK(engine.sweepUploadsIdleBefore(2, kFuture) == 2);
    CHECK(engine.sweepUploadsIdleBefore(2, kFuture) == 2);
    CHECK(engine.sweepUploadsIdleBefore(2, kFuture) == 1);
    CHECK(engine.sweepUploadsIdleBefore(2, kFuture) == 0);

    CHECK(page(engine, "b", 100).uploads.empty());
}

TEST_CASE("the sweep spans every bucket in one pass", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-buckets");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("a", 0);
    engine.createBucket("b", 0);

    engine.createUpload(request("a", "one"));
    engine.createUpload(request("b", "two"));

    // One scan of the by-id index rather than one listing per bucket, which is
    // what keeps a sweep from costing more as buckets are added.
    CHECK(engine.sweepUploadsIdleBefore(64, kFuture) == 2);
}

TEST_CASE("expiry disabled sweeps nothing", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-off");
    auto               options   = optionsFor(root);
    options.multipartExpiryMs    = 0;
    StorageEngine engine(std::move(options));
    engine.createBucket("b", 0);

    const std::string uploadId = engine.createUpload(request("b", "kept"));
    storePart(engine, uploadId, 1, 5 * kMiB);

    CHECK(engine.sweepExpiredUploads(64) == 0);
    CHECK(engine.getUpload(uploadId).has_value());
}

TEST_CASE("a configured expiry reaches the sweep", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-configured");
    auto               options  = optionsFor(root);
    options.multipartExpiryMs   = 1;
    StorageEngine engine(std::move(options));
    engine.createBucket("b", 0);

    const std::string uploadId = engine.createUpload(request("b", "gone"));

    // The only case that has to go through the clock rather than a supplied
    // cutoff, because what it checks is that sweepExpiredUploads derives one
    // from the option at all. A millisecond expiry and a wait far longer than
    // the timestamp resolution keeps that honest without making it a race.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CHECK(engine.sweepExpiredUploads(64) == 1);
    CHECK_FALSE(engine.getUpload(uploadId).has_value());
}

// --- Listing ---------------------------------------------------------------

TEST_CASE("a listing pages through every upload exactly once", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-paging");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("b", 0);

    for (int i = 0; i < 7; ++i) {
        engine.createUpload(request("b", "key-" + std::to_string(i)));
    }

    std::vector<std::string> seen;
    ListUploadsResult        listing;
    do {
        listing = page(engine, "b", 3, listing.nextKeyMarker, listing.nextUploadIdMarker);
        for (const auto& upload : listing.uploads) seen.push_back(upload.key);
    } while (listing.truncated);

    REQUIRE(seen.size() == 7);
    // In key order, and with no key returned twice — the two ways a resumable
    // listing goes wrong.
    CHECK(std::is_sorted(seen.begin(), seen.end()));
    CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}

TEST_CASE("a listing that fits reports itself complete", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-untruncated");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("b", 0);

    engine.createUpload(request("b", "only"));

    const auto listing = page(engine, "b", 10);
    CHECK(listing.uploads.size() == 1);
    CHECK_FALSE(listing.truncated);
    // Markers are reported empty rather than invented when there is nothing to
    // resume from; a client that echoes them back must not skip a page.
    CHECK(listing.nextKeyMarker.empty());
    CHECK(listing.nextUploadIdMarker.empty());
}

TEST_CASE("a page filled exactly is not reported truncated", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-exact");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("b", 0);

    engine.createUpload(request("b", "one"));
    engine.createUpload(request("b", "two"));

    // Two uploads and room for two: there is no next page, and claiming one
    // would send the client back for an empty answer.
    const auto listing = page(engine, "b", 2);
    CHECK(listing.uploads.size() == 2);
    CHECK_FALSE(listing.truncated);
}

TEST_CASE("several uploads of one key page apart", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-samekey");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("b", 0);

    // S3 allows any number of concurrent uploads of the same key, which is why
    // the key alone cannot be the resumption point.
    for (int i = 0; i < 3; ++i) engine.createUpload(request("b", "same"));

    std::vector<std::string> seen;
    ListUploadsResult        listing;
    do {
        listing = page(engine, "b", 1, listing.nextKeyMarker, listing.nextUploadIdMarker);
        for (const auto& upload : listing.uploads) seen.push_back(upload.uploadId);
    } while (listing.truncated);

    REQUIRE(seen.size() == 3);
    std::sort(seen.begin(), seen.end());
    CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}

TEST_CASE("a prefix selects only the keys under it", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-prefix");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("b", 0);

    engine.createUpload(request("b", "logs/a"));
    engine.createUpload(request("b", "logs/b"));
    engine.createUpload(request("b", "photos/a"));

    ListUploadsRequest query;
    query.prefix     = "logs/";
    query.maxUploads = 100;

    const auto listing = engine.listUploads("b", query);
    REQUIRE(listing.uploads.size() == 2);
    for (const auto& upload : listing.uploads) {
        CHECK(upload.key.rfind("logs/", 0) == 0);
    }
}

TEST_CASE("an expired upload disappears from the listing", "[multipart-expiry]") {
    TemporaryDirectory root("expiry-listing");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("b", 0);

    engine.createUpload(request("b", "vanishing"));
    REQUIRE(page(engine, "b", 100).uploads.size() == 1);

    engine.sweepUploadsIdleBefore(64, kFuture);
    CHECK(page(engine, "b", 100).uploads.empty());
}
