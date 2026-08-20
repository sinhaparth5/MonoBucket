#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "storage/checkpoint.hpp"
#include "storage/storage_engine.hpp"
#include "temporary_directory.hpp"

using monobucket::CheckpointReport;
using monobucket::StorageEngine;
using monobucket::StorageError;
using monobucket::testing::TemporaryDirectory;

namespace fs = std::filesystem;

namespace {

StorageEngine::Options optionsFor(const TemporaryDirectory& root) {
    StorageEngine::Options options;
    options.dataDir             = root.path();
    options.durability          = monobucket::Durability::None;
    options.chunkBytes          = 64 * 1024;
    options.metadataMemoryBytes = 8ull * 1024 * 1024;
    return options;
}

void put(StorageEngine& engine, const std::string& bucket, const std::string& key,
         const std::string& body) {
    StorageEngine::PutRequest request;
    request.bucket      = bucket;
    request.key         = key;
    request.contentType = "text/plain";

    auto reservation = engine.reserveSpace(bucket, body.size());
    auto writer      = engine.beginWrite();
    writer.write(body);
    engine.finishWrite(request, std::move(writer), std::move(reservation));
}

/// Every payload the metadata names is present in the tree. This is the whole
/// claim a backup makes, so it is asserted directly rather than inferred from
/// the absence of an error.
monobucket::StorageEngine::FsckReport check(const fs::path& dataDir) {
    StorageEngine::Options options;
    options.dataDir             = dataDir;
    options.durability          = monobucket::Durability::None;
    options.chunkBytes          = 64 * 1024;
    options.metadataMemoryBytes = 8ull * 1024 * 1024;

    StorageEngine::FsckOptions fsck;
    fsck.verifyDigests       = true;
    fsck.unreferencedGraceMs = 0;

    StorageEngine copy(options);
    return copy.fsck(fsck);
}

std::size_t findingsOfKind(const StorageEngine::FsckReport&  report,
                           StorageEngine::FsckReport::Kind kind) {
    std::size_t count = 0;
    for (const auto& finding : report.findings) {
        if (finding.kind == kind) ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("a checkpoint starts as a working store", "[checkpoint][engine]") {
    TemporaryDirectory root("checkpoint-basic");
    TemporaryDirectory into("checkpoint-basic-dest");

    {
        StorageEngine engine(optionsFor(root));
        engine.createBucket("photos", 0);
        engine.createBucket("notes", 0);
        put(engine, "photos", "a.txt", "first");
        put(engine, "notes", "deep/b.txt", "second");
    }

    const fs::path destination = into.path() / "backup";

    StorageEngine engine(optionsFor(root));
    const CheckpointReport report = engine.checkpoint(destination);

    CHECK(report.payloadsLinked == 2);
    // Same filesystem, so nothing was duplicated. This is the property that
    // makes a checkpoint of a large store affordable at all.
    CHECK(report.payloadsCopied == 0);
    CHECK(report.instant());

    // The copy opens on its own and holds what the original held.
    StorageEngine::Options copyOptions;
    copyOptions.dataDir             = destination;
    copyOptions.durability          = monobucket::Durability::None;
    copyOptions.chunkBytes          = 64 * 1024;
    copyOptions.metadataMemoryBytes = 8ull * 1024 * 1024;

    StorageEngine fromBackup(copyOptions);
    CHECK(fromBackup.listBuckets().size() == 2);

    const auto object = fromBackup.statObject("photos", "a.txt");
    REQUIRE(object.has_value());
    CHECK(object->size == 5);
}

TEST_CASE("a checkpoint does not duplicate payload bytes", "[checkpoint][engine]") {
    TemporaryDirectory root("checkpoint-links");
    TemporaryDirectory into("checkpoint-links-dest");

    StorageEngine engine(optionsFor(root));
    engine.createBucket("photos", 0);
    put(engine, "photos", "a.txt", std::string(4096, 'x'));

    const fs::path destination = into.path() / "backup";
    const auto     report      = engine.checkpoint(destination);

    REQUIRE(report.payloadsLinked == 1);

    // Hard links, proved by the link count rather than by the byte total: two
    // names for one inode is the thing being claimed.
    std::size_t checked = 0;
    for (const auto& entry : fs::recursive_directory_iterator(destination / "objects")) {
        if (!entry.is_regular_file()) continue;
        CHECK(fs::hard_link_count(entry.path()) == 2);
        ++checked;
    }
    CHECK(checked == 1);
}

TEST_CASE("a checkpoint taken beside a delete is still consistent",
          "[checkpoint][engine]") {
    TemporaryDirectory root("checkpoint-race");
    TemporaryDirectory into("checkpoint-race-dest");

    StorageEngine engine(optionsFor(root));
    engine.createBucket("photos", 0);

    // Enough objects that the payload pass takes long enough for the deletes
    // below to land inside it.
    for (int i = 0; i < 400; ++i) {
        put(engine, "photos", "key-" + std::to_string(i), std::string(512, 'a'));
    }

    std::atomic<bool> stop{false};
    std::thread       deleter([&] {
        for (int i = 0; i < 400 && !stop.load(); ++i) {
            engine.deleteObject("photos", "key-" + std::to_string(i));
        }
    });

    const fs::path destination = into.path() / "backup";
    const auto     report      = engine.checkpoint(destination);

    stop.store(true);
    deleter.join();

    // The snapshot names some set of objects — which set depends on the race,
    // and that is fine. What is not negotiable is that every payload it names
    // is in the copy. Without the reclamation barrier this is the assertion
    // that fails.
    const auto verdict = check(destination);
    CHECK(findingsOfKind(verdict, StorageEngine::FsckReport::Kind::MissingPayload) == 0);
    CHECK(findingsOfKind(verdict, StorageEngine::FsckReport::Kind::SizeMismatch) == 0);
    CHECK(findingsOfKind(verdict, StorageEngine::FsckReport::Kind::DigestMismatch) == 0);
    CHECK(report.payloadsLinked > 0);
}

TEST_CASE("writes continue during a checkpoint and are simply not in it",
          "[checkpoint][engine]") {
    TemporaryDirectory root("checkpoint-writes");
    TemporaryDirectory into("checkpoint-writes-dest");

    StorageEngine engine(optionsFor(root));
    engine.createBucket("photos", 0);
    for (int i = 0; i < 200; ++i) {
        put(engine, "photos", "before-" + std::to_string(i), "x");
    }

    std::atomic<bool> stop{false};
    std::atomic<int>  written{0};
    std::thread       writer([&] {
        for (int i = 0; i < 200 && !stop.load(); ++i) {
            put(engine, "photos", "during-" + std::to_string(i), "y");
            written.fetch_add(1);
        }
    });

    const fs::path destination = into.path() / "backup";
    engine.checkpoint(destination);

    stop.store(true);
    writer.join();

    // The server was never blocked.
    CHECK(written.load() > 0);

    // And whatever the copy caught, it is coherent: no row without its payload.
    const auto verdict = check(destination);
    CHECK(findingsOfKind(verdict, StorageEngine::FsckReport::Kind::MissingPayload) == 0);
}

TEST_CASE("reclamation resumes after a checkpoint", "[checkpoint][engine]") {
    TemporaryDirectory root("checkpoint-reclaim");
    TemporaryDirectory into("checkpoint-reclaim-dest");

    StorageEngine engine(optionsFor(root));
    engine.createBucket("photos", 0);
    put(engine, "photos", "a.txt", "payload");

    engine.checkpoint(into.path() / "backup");

    // A delete after the checkpoint reclaims immediately again — the barrier is
    // released even though the checkpoint returned normally rather than by
    // throwing.
    const auto before = engine.blobs().forEachBlob([](const auto&) {});
    engine.deleteObject("photos", "a.txt");
    const auto after = engine.blobs().forEachBlob([](const auto&) {});
    CHECK(after == before - 1);
}

// --- Refusing a destination -------------------------------------------------

TEST_CASE("a destination that already holds something is refused",
          "[checkpoint][storage]") {
    TemporaryDirectory into("checkpoint-occupied");

    const fs::path destination = into.path() / "backup";
    fs::create_directories(destination);
    { std::ofstream(destination / "stray.txt") << "x"; }

    // Refused before anything is written, so a mistyped destination cannot
    // half-fill a directory that already meant something to somebody.
    CHECK_THROWS_AS(monobucket::requireUsableDestination(destination), StorageError);
}

TEST_CASE("an empty destination directory is accepted", "[checkpoint][storage]") {
    TemporaryDirectory into("checkpoint-empty");

    const fs::path destination = into.path() / "backup";
    fs::create_directories(destination);
    CHECK_NOTHROW(monobucket::requireUsableDestination(destination));
}

TEST_CASE("a destination whose parent is missing is refused", "[checkpoint][storage]") {
    TemporaryDirectory into("checkpoint-noparent");
    CHECK_THROWS_AS(monobucket::requireUsableDestination(into.path() / "no" / "such" / "backup"),
                    StorageError);
}

TEST_CASE("a destination that is a file is refused", "[checkpoint][storage]") {
    TemporaryDirectory into("checkpoint-isfile");

    const fs::path destination = into.path() / "backup";
    { std::ofstream(destination) << "x"; }
    CHECK_THROWS_AS(monobucket::requireUsableDestination(destination), StorageError);
}

TEST_CASE("a bad destination leaves the store reclaiming normally",
          "[checkpoint][engine]") {
    TemporaryDirectory root("checkpoint-refused");
    TemporaryDirectory into("checkpoint-refused-dest");

    StorageEngine engine(optionsFor(root));
    engine.createBucket("photos", 0);
    put(engine, "photos", "a.txt", "payload");

    const fs::path destination = into.path() / "backup";
    fs::create_directories(destination);
    { std::ofstream(destination / "stray.txt") << "x"; }

    CHECK_THROWS_AS(engine.checkpoint(destination), StorageError);

    // The barrier was never raised, so a refused checkpoint does not quietly
    // switch reclamation off for the life of the process.
    const auto before = engine.blobs().forEachBlob([](const auto&) {});
    engine.deleteObject("photos", "a.txt");
    const auto after = engine.blobs().forEachBlob([](const auto&) {});
    CHECK(after == before - 1);
}
