#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "storage/storage_engine.hpp"
#include "temporary_directory.hpp"

using monobucket::StorageEngine;
using monobucket::StorageError;
using monobucket::StorageErrorCode;
using monobucket::testing::TemporaryDirectory;

namespace {

constexpr std::uint64_t kMiB = 1024 * 1024;

/// The smallest part S3 accepts anywhere but at the end of an upload. Every
/// multipart case here has to clear it, so an object built out of parts cannot
/// be assembled from figures small enough to make the arithmetic convenient.
constexpr std::uint64_t kMinPart = 5 * kMiB;

StorageEngine::Options optionsFor(const TemporaryDirectory& root, std::uint64_t limit,
                                  std::uint64_t ceiling = 1ull << 40) {
    StorageEngine::Options options;
    options.dataDir               = root.path();
    options.durability            = monobucket::Durability::None;
    options.chunkBytes            = 64 * 1024;
    options.metadataMemoryBytes   = 8ull * 1024 * 1024;
    options.maxUploadBytes        = limit;
    options.maxUploadCeilingBytes = ceiling;
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

/// Stores one part and returns its manifest entry.
StorageEngine::RequestedPart storePart(StorageEngine& engine, const std::string& uploadId,
                                       std::uint32_t number, std::uint64_t bytes) {
    auto writer = engine.beginWrite();
    writer.write(std::string(bytes, 'p'));
    return {number, engine.finishPart(uploadId, number, std::move(writer)).etag};
}

}  // namespace

// --- The limit itself ------------------------------------------------------

TEST_CASE("the environment figure seeds a store that has never carried a limit",
          "[upload-limit]") {
    TemporaryDirectory root("limit-seed");
    StorageEngine      engine(optionsFor(root, 4 * kMiB));

    CHECK(engine.maxUploadBytes() == 4 * kMiB);
    CHECK(engine.maxUploadCeilingBytes() == (1ull << 40));
}

TEST_CASE("a limit set from the console outlives a restart", "[upload-limit]") {
    TemporaryDirectory root("limit-persist");

    {
        StorageEngine engine(optionsFor(root, 4 * kMiB));
        engine.setMaxUploadBytes(7 * kMiB);
        engine.flush();
    }

    // Reopened with the *original* environment figure, which must not win: a
    // redeploy carrying a stale MONOBUCKET_MAX_UPLOAD_BYTES silently undoing a
    // limit somebody set is the whole failure this is persisted to avoid.
    StorageEngine engine(optionsFor(root, 4 * kMiB));
    CHECK(engine.maxUploadBytes() == 7 * kMiB);
}

TEST_CASE("the limit cannot be raised past the ceiling", "[upload-limit]") {
    TemporaryDirectory root("limit-ceiling");
    StorageEngine      engine(optionsFor(root, 4 * kMiB, 8 * kMiB));

    CHECK(codeOf([&] { engine.setMaxUploadBytes(8 * kMiB + 1); }) ==
          StorageErrorCode::ObjectTooLarge);
    // The refusal changed nothing.
    CHECK(engine.maxUploadBytes() == 4 * kMiB);

    // Exactly at the ceiling is allowed: a ceiling nobody can reach is a
    // ceiling that is really one byte lower.
    engine.setMaxUploadBytes(8 * kMiB);
    CHECK(engine.maxUploadBytes() == 8 * kMiB);
}

TEST_CASE("zero is refused rather than read as unlimited", "[upload-limit]") {
    TemporaryDirectory root("limit-zero");
    StorageEngine      engine(optionsFor(root, 4 * kMiB));

    CHECK(codeOf([&] { engine.setMaxUploadBytes(0); }) == StorageErrorCode::Internal);
    CHECK(engine.maxUploadBytes() == 4 * kMiB);
}

TEST_CASE("a stored limit above a lowered ceiling is clamped, not refused", "[upload-limit]") {
    TemporaryDirectory root("limit-clamp");

    {
        StorageEngine engine(optionsFor(root, 4 * kMiB, 16 * kMiB));
        engine.setMaxUploadBytes(12 * kMiB);
        engine.flush();
    }

    // Restarted with a tighter ceiling. Refusing to start would leave an
    // operator with a server whose console they cannot reach to fix it.
    StorageEngine engine(optionsFor(root, 4 * kMiB, 8 * kMiB));
    CHECK(engine.maxUploadBytes() == 8 * kMiB);
}

// --- Single writes ---------------------------------------------------------

TEST_CASE("an object exactly at the limit is stored", "[upload-limit][engine]") {
    TemporaryDirectory root("limit-boundary");
    StorageEngine      engine(optionsFor(root, 4 * kMiB));
    engine.createBucket("b");

    const auto record = engine.putObject(request("b", "k"), std::string(4 * kMiB, 'x'));
    CHECK(record.size == 4 * kMiB);
    CHECK(engine.bucketCapacity("b").usedBytes == 4 * kMiB);
}

TEST_CASE("one byte over the limit is refused and stores nothing", "[upload-limit][engine]") {
    TemporaryDirectory root("limit-over");
    StorageEngine      engine(optionsFor(root, 4 * kMiB));
    engine.createBucket("b");

    CHECK(codeOf([&] {
              engine.putObject(request("b", "k"), std::string(4 * kMiB + 1, 'x'));
          }) == StorageErrorCode::ObjectTooLarge);

    // Refused before the payload was linked in, so there is no object and the
    // bucket is charged nothing — a refusal that left usage behind would be a
    // bucket that fills up out of failed requests.
    CHECK_FALSE(engine.statObject("b", "k").has_value());
    CHECK(engine.bucketCapacity("b").usedBytes == 0);
}

TEST_CASE("what arrived decides, not what was declared", "[upload-limit][engine]") {
    // The property the chunked path depends on. A streaming body declares its
    // decoded length in a header the decoder only verifies once every byte has
    // been fed through, so the size that is enforced has to be the one the
    // writer actually took — which is what this call reads.
    TemporaryDirectory root("limit-undeclared");
    StorageEngine      engine(optionsFor(root, 4 * kMiB));
    engine.createBucket("b");

    // Nothing was reserved and nothing was declared: the writer is handed
    // bytes the way a chunk sink hands them over, and the limit still bites.
    auto writer = engine.beginWrite();
    writer.write(std::string(4 * kMiB + 1, 'x'));

    CHECK(codeOf([&] { engine.finishWrite(request("b", "k"), std::move(writer)); }) ==
          StorageErrorCode::ObjectTooLarge);
    CHECK_FALSE(engine.statObject("b", "k").has_value());
}

TEST_CASE("an overwrite is held to the limit like a new object", "[upload-limit][engine]") {
    TemporaryDirectory root("limit-overwrite");
    StorageEngine      engine(optionsFor(root, 4 * kMiB));
    engine.createBucket("b");

    engine.putObject(request("b", "k"), std::string(kMiB, 'a'));
    CHECK(codeOf([&] {
              engine.putObject(request("b", "k"), std::string(4 * kMiB + 1, 'b'));
          }) == StorageErrorCode::ObjectTooLarge);

    // The refused overwrite left the original alone.
    const auto record = engine.statObject("b", "k");
    REQUIRE(record.has_value());
    CHECK(record->size == kMiB);
}

TEST_CASE("lowering the limit does not disturb what is already stored",
          "[upload-limit][engine]") {
    TemporaryDirectory root("limit-lowered");
    StorageEngine      engine(optionsFor(root, 8 * kMiB));
    engine.createBucket("b");

    engine.putObject(request("b", "k"), std::string(6 * kMiB, 'x'));
    engine.setMaxUploadBytes(2 * kMiB);

    // Readable, and still counted. A limit decides what may be written, never
    // what may be kept.
    const auto record = engine.statObject("b", "k");
    REQUIRE(record.has_value());
    CHECK(record->size == 6 * kMiB);
    CHECK(engine.bucketCapacity("b").usedBytes == 6 * kMiB);

    // The next write is held to the new figure.
    CHECK(codeOf([&] {
              engine.putObject(request("b", "k2"), std::string(2 * kMiB + 1, 'x'));
          }) == StorageErrorCode::ObjectTooLarge);
}

// --- Multipart -------------------------------------------------------------

TEST_CASE("multipart cannot get past the limit by splitting the object",
          "[upload-limit][engine][multipart]") {
    TemporaryDirectory root("limit-multipart-parts");
    StorageEngine      engine(optionsFor(root, 12 * kMiB));
    engine.createBucket("b");

    const std::string uploadId = engine.createUpload(request("b", "k"));

    storePart(engine, uploadId, 1, kMinPart);
    storePart(engine, uploadId, 2, kMinPart);

    // 10 MiB stored; a third 5 MiB part would make 15 MiB. Refused at the part
    // rather than at the completion, so the bytes never reach the disk.
    CHECK(codeOf([&] { storePart(engine, uploadId, 3, kMinPart); }) ==
          StorageErrorCode::ObjectTooLarge);

    CHECK(engine.listParts(uploadId).size() == 2);
    CHECK(engine.uploadedPartBytes(uploadId) == 2 * kMinPart);
}

TEST_CASE("an upload assembling to exactly the limit completes",
          "[upload-limit][engine][multipart]") {
    TemporaryDirectory root("limit-multipart-exact");
    StorageEngine      engine(optionsFor(root, 2 * kMinPart));
    engine.createBucket("b");

    const std::string uploadId = engine.createUpload(request("b", "k"));

    std::vector<StorageEngine::RequestedPart> manifest{storePart(engine, uploadId, 1, kMinPart),
                                                       storePart(engine, uploadId, 2, kMinPart)};

    const auto record = engine.completeUpload(uploadId, manifest);
    CHECK(record.size == 2 * kMinPart);
    CHECK(engine.bucketCapacity("b").usedBytes == 2 * kMinPart);
}

TEST_CASE("a completion over the limit is refused and publishes nothing",
          "[upload-limit][engine][multipart]") {
    TemporaryDirectory root("limit-multipart-complete");
    StorageEngine      engine(optionsFor(root, 16 * kMiB));
    engine.createBucket("b");

    const std::string uploadId = engine.createUpload(request("b", "k"));

    std::vector<StorageEngine::RequestedPart> manifest{storePart(engine, uploadId, 1, kMinPart),
                                                       storePart(engine, uploadId, 2, kMinPart)};

    // Every part was admitted under the old figure. The completion is judged
    // against the limit in force when it is asked for, which is the documented
    // rule: each admission decision uses the limit at the moment it is taken.
    engine.setMaxUploadBytes(6 * kMiB);

    CHECK(codeOf([&] { engine.completeUpload(uploadId, manifest); }) ==
          StorageErrorCode::ObjectTooLarge);

    // Nothing published, and the upload is still there to abort — a refused
    // completion that had destroyed the parts would leave a client with
    // neither an object nor a way to retry.
    CHECK_FALSE(engine.statObject("b", "k").has_value());
    REQUIRE(engine.getUpload(uploadId).has_value());
    CHECK(engine.listParts(uploadId).size() == 2);
    CHECK(engine.bucketCapacity("b").usedBytes == 0);
}

TEST_CASE("a refused upload leaves no committed usage once aborted",
          "[upload-limit][engine][multipart]") {
    TemporaryDirectory root("limit-multipart-abort");
    StorageEngine      engine(optionsFor(root, 12 * kMiB));
    engine.createBucket("b");

    const std::string uploadId = engine.createUpload(request("b", "k"));
    storePart(engine, uploadId, 1, kMinPart);
    storePart(engine, uploadId, 2, kMinPart);

    CHECK(codeOf([&] { storePart(engine, uploadId, 3, kMinPart); }) ==
          StorageErrorCode::ObjectTooLarge);
    CHECK(engine.bucketCapacity("b").pendingBytes == 2 * kMinPart);

    engine.abortUpload(uploadId);

    const auto capacity = engine.bucketCapacity("b");
    CHECK(capacity.pendingBytes == 0);
    CHECK(capacity.usedBytes == 0);
}

TEST_CASE("re-uploading a part is measured against the part it replaces",
          "[upload-limit][engine][multipart]") {
    TemporaryDirectory root("limit-multipart-replace");
    StorageEngine      engine(optionsFor(root, 12 * kMiB));
    engine.createBucket("b");

    const std::string uploadId = engine.createUpload(request("b", "k"));
    storePart(engine, uploadId, 1, kMinPart);
    storePart(engine, uploadId, 2, kMinPart);

    // Replacing part 2 with a larger one is admitted on the difference: the
    // part it is replacing is not counted twice. 5 MiB + 6 MiB is under 12.
    storePart(engine, uploadId, 2, 6 * kMiB);
    CHECK(engine.uploadedPartBytes(uploadId) == kMinPart + 6 * kMiB);

    // And a replacement that would take the upload over is still refused.
    CHECK(codeOf([&] { storePart(engine, uploadId, 2, 8 * kMiB); }) ==
          StorageErrorCode::ObjectTooLarge);
}
