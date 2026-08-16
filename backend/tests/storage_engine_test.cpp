#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>
#include <vector>

#include "storage/digest.hpp"
#include "storage/storage_engine.hpp"
#include "temporary_directory.hpp"

using monobucket::StorageEngine;
using monobucket::StorageError;
using monobucket::StorageErrorCode;
using monobucket::testing::TemporaryDirectory;

namespace {

StorageEngine::Options optionsFor(const TemporaryDirectory& root) {
    StorageEngine::Options options;
    options.dataDir             = root.path();
    options.durability          = monobucket::Durability::None;  // tests are throwaway
    options.chunkBytes          = 64 * 1024;
    options.metadataMemoryBytes = 8ull * 1024 * 1024;
    return options;
}

std::string readAll(StorageEngine::ObjectHandle& handle) {
    std::string            out;
    std::vector<std::byte> buffer(32 * 1024);
    while (true) {
        const std::size_t read = handle.reader.read(buffer.data(), buffer.size());
        if (read == 0) break;
        out.append(reinterpret_cast<const char*>(buffer.data()), read);
    }
    return out;
}

StorageEngine::PutRequest request(std::string bucket, std::string key) {
    StorageEngine::PutRequest put;
    put.bucket = std::move(bucket);
    put.key    = std::move(key);
    return put;
}

}  // namespace

TEST_CASE("an object round-trips through the engine", "[engine]") {
    TemporaryDirectory root("engine-roundtrip");
    StorageEngine      engine(optionsFor(root));

    engine.createBucket("photos");

    const std::string payload = std::string(200'000, 'p') + "end";

    auto put = request("photos", "holiday.jpg");
    put.contentType  = "image/jpeg";
    put.userMetadata = {{"author", "someone"}};

    const auto record = engine.putObject(put, payload);
    CHECK(record.size == payload.size());
    CHECK(record.etag == monobucket::md5Hex(payload));
    CHECK(record.sha256 == monobucket::sha256Hex(payload));

    auto handle = engine.getObject("photos", "holiday.jpg");
    REQUIRE(handle.has_value());
    CHECK(handle->record.contentType == "image/jpeg");
    CHECK(handle->record.userMetadata.at("author") == "someone");
    CHECK(readAll(*handle) == payload);
}

TEST_CASE("a missing object is absent rather than an error", "[engine]") {
    TemporaryDirectory root("engine-missing");
    StorageEngine      engine(optionsFor(root));

    engine.createBucket("photos");
    CHECK_FALSE(engine.getObject("photos", "nothing").has_value());
    CHECK_FALSE(engine.statObject("photos", "nothing").has_value());
    CHECK_FALSE(engine.deleteObject("photos", "nothing"));
}

TEST_CASE("overwriting an object reclaims the payload it replaced",
          "[engine][reclaim]") {
    TemporaryDirectory root("engine-overwrite");
    StorageEngine      engine(optionsFor(root));

    engine.createBucket("photos");

    const auto first = engine.putObject(request("photos", "k"), "first version");
    CHECK(engine.blobs().exists(first.blobId));

    const auto second = engine.putObject(request("photos", "k"), "second version");

    // The old payload is unlinked as part of the overwrite. Leaving it would be
    // invisible until the disk filled up.
    CHECK_FALSE(engine.blobs().exists(first.blobId));
    CHECK(engine.blobs().exists(second.blobId));

    auto handle = engine.getObject("photos", "k");
    REQUIRE(handle.has_value());
    CHECK(readAll(*handle) == "second version");
    CHECK(engine.stats().usage.objects == 1);
}

TEST_CASE("deleting an object reclaims its payload", "[engine][reclaim]") {
    TemporaryDirectory root("engine-delete");
    StorageEngine      engine(optionsFor(root));

    engine.createBucket("photos");
    const auto record = engine.putObject(request("photos", "k"), "payload");

    CHECK(engine.deleteObject("photos", "k"));
    CHECK_FALSE(engine.blobs().exists(record.blobId));
    CHECK(engine.stats().usage.objects == 0);
    CHECK(engine.stats().usage.bytes == 0);
}

TEST_CASE("an abandoned streaming write leaves no payload", "[engine][reclaim]") {
    TemporaryDirectory root("engine-abandoned");
    StorageEngine      engine(optionsFor(root));

    engine.createBucket("photos");

    std::string blobId;
    {
        auto writer = engine.beginWrite();
        blobId      = writer.blobId();
        writer.write("a client that disconnected mid-PUT");
    }

    CHECK_FALSE(engine.blobs().exists(blobId));

    // It is still owed to the reclaimer, because the crash could equally have
    // happened after commit. Recovery, where nothing is in flight, collects it.
    const auto report = engine.recover();
    CHECK(report.payloadsReclaimed == 1);
    CHECK(engine.stats().usage.orphanBlobs == 0);
}

TEST_CASE("recovery removes interrupted writes", "[engine][recovery]") {
    TemporaryDirectory root("engine-recovery");

    {
        StorageEngine engine(optionsFor(root));
        engine.createBucket("photos");
        engine.putObject(request("photos", "keep-me"), "durable");
        engine.flush();
    }

    // What a kill -9 during an upload leaves behind.
    {
        std::ofstream stale(root.path() / "tmp" / "interrupted.part");
        stale << "half an object";
    }

    StorageEngine engine(optionsFor(root));
    const auto    report = engine.recover();

    CHECK(report.temporariesRemoved == 1);

    // The committed object is untouched: recovery reconciles, it does not
    // rebuild.
    auto handle = engine.getObject("photos", "keep-me");
    REQUIRE(handle.has_value());
    CHECK(readAll(*handle) == "durable");
}

TEST_CASE("data survives a clean restart", "[engine][recovery]") {
    TemporaryDirectory root("engine-restart");

    const std::string payload = std::string(100'000, 'z');
    std::string       etag;

    {
        StorageEngine engine(optionsFor(root));
        engine.createBucket("photos");
        etag = engine.putObject(request("photos", "a/b/c.bin"), payload).etag;
        engine.flush();
    }

    StorageEngine engine(optionsFor(root));
    engine.recover();

    auto handle = engine.getObject("photos", "a/b/c.bin");
    REQUIRE(handle.has_value());
    CHECK(handle->record.etag == etag);
    CHECK(readAll(*handle) == payload);
    CHECK(engine.stats().usage.bytes == payload.size());
}

TEST_CASE("a multipart upload assembles into one object", "[engine][multipart]") {
    TemporaryDirectory root("engine-multipart");
    StorageEngine      engine(optionsFor(root));

    engine.createBucket("archive");

    const std::string uploadId = engine.createUpload(request("archive", "big.bin"));

    // All but the last part must reach the 5 MiB minimum.
    const std::string first  = std::string(5 * 1024 * 1024, 'A');
    const std::string second = std::string(1024, 'B');

    std::vector<StorageEngine::RequestedPart> manifest;
    {
        auto writer = engine.beginWrite();
        writer.write(first);
        const auto part = engine.finishPart(uploadId, 1, std::move(writer));
        manifest.push_back({1, part.etag});
    }
    {
        auto writer = engine.beginWrite();
        writer.write(second);
        const auto part = engine.finishPart(uploadId, 2, std::move(writer));
        manifest.push_back({2, part.etag});
    }

    const auto record = engine.completeUpload(uploadId, manifest);

    CHECK(record.size == first.size() + second.size());
    // The ETag is the digest of the part digests, not of the assembled bytes.
    CHECK(record.etag == monobucket::multipartETag(
                             {monobucket::md5Hex(first), monobucket::md5Hex(second)}));
    CHECK(record.etag.ends_with("-2"));
    CHECK(record.sha256 == monobucket::sha256Hex(first + second));

    auto handle = engine.getObject("archive", "big.bin");
    REQUIRE(handle.has_value());
    CHECK(readAll(*handle) == first + second);

    // The upload and its parts are gone once it completes.
    CHECK_FALSE(engine.getUpload(uploadId).has_value());
    CHECK(engine.stats().usage.uploads == 0);
}

TEST_CASE("completing an upload reclaims the part payloads",
          "[engine][multipart][reclaim]") {
    TemporaryDirectory root("engine-multipart-reclaim");
    StorageEngine      engine(optionsFor(root));

    engine.createBucket("archive");
    const std::string uploadId = engine.createUpload(request("archive", "big.bin"));

    std::vector<std::string>                  partBlobs;
    std::vector<StorageEngine::RequestedPart> manifest;
    for (std::uint32_t number : {1u, 2u}) {
        auto writer = engine.beginWrite();
        writer.write(std::string(5 * 1024 * 1024, static_cast<char>('A' + number)));
        const auto part = engine.finishPart(uploadId, number, std::move(writer));
        partBlobs.push_back(part.blobId);
        manifest.push_back({number, part.etag});
    }

    engine.completeUpload(uploadId, manifest);

    // The assembled object owns its own payload, so the parts are dead weight
    // the moment it is published.
    for (const auto& blobId : partBlobs) CHECK_FALSE(engine.blobs().exists(blobId));
}

TEST_CASE("aborting an upload reclaims its parts", "[engine][multipart][reclaim]") {
    TemporaryDirectory root("engine-multipart-abort");
    StorageEngine      engine(optionsFor(root));

    engine.createBucket("archive");
    const std::string uploadId = engine.createUpload(request("archive", "big.bin"));

    auto writer = engine.beginWrite();
    writer.write("a part that will never be completed");
    const auto part = engine.finishPart(uploadId, 1, std::move(writer));
    CHECK(engine.blobs().exists(part.blobId));

    engine.abortUpload(uploadId);

    CHECK_FALSE(engine.blobs().exists(part.blobId));
    CHECK_FALSE(engine.getUpload(uploadId).has_value());
}

TEST_CASE("a mismatched manifest is refused", "[engine][multipart]") {
    TemporaryDirectory root("engine-multipart-manifest");
    StorageEngine      engine(optionsFor(root));

    engine.createBucket("archive");
    const std::string uploadId = engine.createUpload(request("archive", "big.bin"));

    auto writer = engine.beginWrite();
    writer.write(std::string(5 * 1024 * 1024, 'A'));
    const auto part = engine.finishPart(uploadId, 1, std::move(writer));

    const auto expectInvalidPart = [&](const std::vector<StorageEngine::RequestedPart>& manifest) {
        try {
            engine.completeUpload(uploadId, manifest);
            FAIL("an invalid manifest should be refused");
        } catch (const StorageError& error) {
            CHECK(error.code() == StorageErrorCode::InvalidPart);
        }
    };

    expectInvalidPart({});                                  // no parts at all
    expectInvalidPart({{1, "the-wrong-etag"}});             // ETag disagrees
    expectInvalidPart({{2, part.etag}});                    // part never uploaded
    expectInvalidPart({{1, part.etag}, {1, part.etag}});    // duplicated
}

TEST_CASE("quoted ETags in a manifest are accepted", "[engine][multipart]") {
    TemporaryDirectory root("engine-multipart-quotes");
    StorageEngine      engine(optionsFor(root));

    engine.createBucket("archive");
    const std::string uploadId = engine.createUpload(request("archive", "big.bin"));

    auto writer = engine.beginWrite();
    writer.write("a single small part is allowed as the last one");
    const auto part = engine.finishPart(uploadId, 1, std::move(writer));

    // S3 sends ETags quoted and clients are inconsistent about echoing the
    // quotes back, so both forms have to work.
    CHECK_NOTHROW(engine.completeUpload(uploadId, {{1, '"' + part.etag + '"'}}));
}

TEST_CASE("an undersized middle part is refused", "[engine][multipart]") {
    TemporaryDirectory root("engine-multipart-small");
    StorageEngine      engine(optionsFor(root));

    engine.createBucket("archive");
    const std::string uploadId = engine.createUpload(request("archive", "big.bin"));

    std::vector<StorageEngine::RequestedPart> manifest;
    for (std::uint32_t number : {1u, 2u}) {
        auto writer = engine.beginWrite();
        writer.write("tiny");
        const auto part = engine.finishPart(uploadId, number, std::move(writer));
        manifest.push_back({number, part.etag});
    }

    // Without this, a client could build an object out of arbitrarily many
    // tiny pieces and multiply our per-part overhead without limit.
    try {
        engine.completeUpload(uploadId, manifest);
        FAIL("an undersized middle part should be refused");
    } catch (const StorageError& error) {
        CHECK(error.code() == StorageErrorCode::InvalidPart);
    }
}

TEST_CASE("listing paginates through the engine", "[engine][list]") {
    TemporaryDirectory root("engine-list");
    StorageEngine      engine(optionsFor(root));

    engine.createBucket("b");
    for (const char* key : {"docs/a.txt", "docs/b.txt", "images/1.png", "top.txt"}) {
        engine.putObject(request("b", key), "x");
    }

    monobucket::ListObjectsRequest listing;
    listing.delimiter = "/";

    const auto result = engine.listObjects("b", listing);
    REQUIRE(result.objects.size() == 1);
    CHECK(result.objects[0].key == "top.txt");
    CHECK(result.commonPrefixes == std::vector<std::string>{"docs/", "images/"});
}

TEST_CASE("statistics reflect what is stored", "[engine]") {
    TemporaryDirectory root("engine-stats");
    StorageEngine      engine(optionsFor(root));

    engine.createBucket("b");
    engine.putObject(request("b", "one"), std::string(1000, 'a'));
    engine.putObject(request("b", "two"), std::string(2000, 'b'));

    const auto stats = engine.stats();
    CHECK(stats.usage.buckets == 1);
    CHECK(stats.usage.objects == 2);
    CHECK(stats.usage.bytes == 3000);
    CHECK(stats.space.totalBytes > 0);
    CHECK(stats.engine == "rocksdb");
}
