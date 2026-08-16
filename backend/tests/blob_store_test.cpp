#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>
#include <vector>

#include "storage/blob_store.hpp"
#include "storage/digest.hpp"
#include "storage/metadata_store.hpp"
#include "temporary_directory.hpp"

using monobucket::BlobStore;
using monobucket::BlobWriter;
using monobucket::Durability;
using monobucket::StorageError;
using monobucket::StorageErrorCode;
using monobucket::testing::TemporaryDirectory;

namespace {

/// Reads a payload back in full, through the chunked interface a request
/// handler would use.
std::string readAll(BlobStore& store, const std::string& blobId) {
    auto        reader = store.open(blobId);
    std::string out;
    std::vector<std::byte> buffer(64 * 1024);

    while (true) {
        const std::size_t read = reader.read(buffer.data(), buffer.size());
        if (read == 0) break;
        out.append(reinterpret_cast<const char*>(buffer.data()), read);
    }
    return out;
}

BlobStore makeStore(const TemporaryDirectory& root) {
    return BlobStore(root.path(), Durability::None, 64 * 1024);
}

}  // namespace

TEST_CASE("a payload round-trips byte-identically", "[blob]") {
    TemporaryDirectory root("blob");
    BlobStore          store = makeStore(root);

    // Deliberately not chunk-aligned, so the write loop and the read loop both
    // have to handle a partial final chunk.
    const std::string payload = std::string(150'000, 'x') + "tail";

    auto writer = store.create();
    writer.write(payload);
    const auto committed = writer.commit();

    CHECK(committed.size == payload.size());
    CHECK(committed.md5 == monobucket::md5Hex(payload));
    CHECK(committed.sha256 == monobucket::sha256Hex(payload));
    CHECK(readAll(store, committed.blobId) == payload);
}

TEST_CASE("a payload written in many small pieces is still one object", "[blob]") {
    TemporaryDirectory root("blob-chunks");
    BlobStore          store = makeStore(root);

    std::string expected;
    auto        writer = store.create();
    for (int i = 0; i < 500; ++i) {
        const std::string piece = "chunk-" + std::to_string(i) + ';';
        writer.write(piece);
        expected += piece;
    }
    const auto committed = writer.commit();

    CHECK(committed.size == expected.size());
    CHECK(committed.md5 == monobucket::md5Hex(expected));
    CHECK(readAll(store, committed.blobId) == expected);
}

TEST_CASE("an empty payload is a valid object", "[blob]") {
    // S3 allows zero-byte objects, and they are a common way to fake
    // directories, so they must not be treated as a failed write.
    TemporaryDirectory root("blob-empty");
    BlobStore          store = makeStore(root);

    auto       writer    = store.create();
    const auto committed = writer.commit();

    CHECK(committed.size == 0);
    CHECK(committed.md5 == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(store.exists(committed.blobId));
    CHECK(readAll(store, committed.blobId).empty());
}

TEST_CASE("payloads are spread across the sharded tree", "[blob]") {
    TemporaryDirectory root("blob-shard");
    BlobStore          store = makeStore(root);

    auto       writer    = store.create();
    const auto committed = writer.commit();

    // objects/<aa>/<bb>/<id> — two levels of 256, so a million objects sit at
    // roughly fifteen entries per directory.
    const auto path = store.pathFor(committed.blobId);
    CHECK(path.filename() == committed.blobId);
    CHECK(path.parent_path().filename() == committed.blobId.substr(2, 2));
    CHECK(path.parent_path().parent_path().filename() == committed.blobId.substr(0, 2));
    CHECK(std::filesystem::exists(path));
}

TEST_CASE("an uncommitted payload leaves nothing behind", "[blob]") {
    TemporaryDirectory root("blob-abort");
    BlobStore          store = makeStore(root);

    std::string blobId;
    {
        auto writer = store.create();
        blobId      = writer.blobId();
        writer.write("half an upload that never finished");
        // Destroyed without commit() — the client vanished mid-PUT.
    }

    CHECK_FALSE(store.exists(blobId));
    CHECK(store.sweepTemporaries() == 0);  // the destructor already cleaned up
}

TEST_CASE("a payload is invisible until it is committed", "[blob]") {
    TemporaryDirectory root("blob-visibility");
    BlobStore          store = makeStore(root);

    auto writer = store.create();
    writer.write("data");

    // This is what makes a partial upload unobservable rather than corrupt:
    // the payload has no name in the tree until commit() links it.
    CHECK_FALSE(store.exists(writer.blobId()));

    writer.commit();
    CHECK(store.exists(writer.blobId()));
}

TEST_CASE("a range read returns exactly the requested window", "[blob]") {
    TemporaryDirectory root("blob-range");
    BlobStore          store = makeStore(root);

    const std::string payload = "0123456789abcdefghij";

    auto writer = store.create();
    writer.write(payload);
    const auto committed = writer.commit();

    auto reader = store.open(committed.blobId);
    reader.limitTo(5, 10);
    CHECK(reader.remaining() == 10);

    std::vector<std::byte> buffer(64);
    const std::size_t      read = reader.read(buffer.data(), buffer.size());
    CHECK(std::string(reinterpret_cast<const char*>(buffer.data()), read) == "56789abcde");
    CHECK(reader.read(buffer.data(), buffer.size()) == 0);
}

TEST_CASE("a range running past the end is clamped, not rejected", "[blob]") {
    // S3 truncates an over-long range rather than failing it, so a client
    // asking for "bytes=10-" on a short object still gets what exists.
    TemporaryDirectory root("blob-range-clamp");
    BlobStore          store = makeStore(root);

    auto writer = store.create();
    writer.write("short");
    const auto committed = writer.commit();

    auto reader = store.open(committed.blobId);
    reader.limitTo(2, 1000);
    CHECK(reader.remaining() == 3);

    reader.limitTo(500, 10);
    CHECK(reader.remaining() == 0);
}

TEST_CASE("appending one payload to another concatenates them", "[blob]") {
    TemporaryDirectory root("blob-append");
    BlobStore          store = makeStore(root);

    const std::string first  = std::string(100'000, 'A');
    const std::string second = std::string(50'000, 'B');

    auto firstWriter = store.create();
    firstWriter.write(first);
    const auto firstBlob = firstWriter.commit();

    auto secondWriter = store.create();
    secondWriter.write(second);
    const auto secondBlob = secondWriter.commit();

    // This is the CompleteMultipartUpload path: the assembled object must hash
    // as the concatenation, not as anything derived from the parts.
    auto assembler = store.create();
    assembler.appendBlob(store, firstBlob.blobId);
    assembler.appendBlob(store, secondBlob.blobId);
    const auto assembled = assembler.commit();

    CHECK(assembled.size == first.size() + second.size());
    CHECK(assembled.md5 == monobucket::md5Hex(first + second));
    CHECK(assembled.sha256 == monobucket::sha256Hex(first + second));
}

TEST_CASE("removing a payload is idempotent", "[blob]") {
    TemporaryDirectory root("blob-remove");
    BlobStore          store = makeStore(root);

    auto       writer    = store.create();
    const auto committed = writer.commit();

    CHECK(store.remove(committed.blobId));
    // Reclamation may run twice for the same id; the second must be a no-op
    // rather than an error.
    CHECK_FALSE(store.remove(committed.blobId));
    CHECK_FALSE(store.exists(committed.blobId));
}

TEST_CASE("a missing payload reports NoSuchKey", "[blob]") {
    TemporaryDirectory root("blob-missing");
    BlobStore          store = makeStore(root);

    try {
        store.open(std::string(32, 'a'));
        FAIL("opening a missing payload should throw");
    } catch (const StorageError& error) {
        CHECK(error.code() == StorageErrorCode::NoSuchKey);
    }
}

TEST_CASE("a malformed blob id cannot escape the tree", "[blob]") {
    TemporaryDirectory root("blob-traversal");
    BlobStore          store = makeStore(root);

    // A blob id read back from metadata becomes a path component, so anything
    // that did not come from newBlobId() is refused before it reaches open().
    CHECK_THROWS_AS(store.pathFor("../../etc/passwd"), StorageError);
    CHECK_THROWS_AS(store.pathFor("short"), StorageError);
    CHECK_THROWS_AS(store.pathFor(std::string(32, 'Z')), StorageError);
    CHECK_FALSE(store.exists("../../etc/passwd"));
}

TEST_CASE("stale temporaries are swept", "[blob]") {
    TemporaryDirectory root("blob-sweep");
    BlobStore          store = makeStore(root);

    // Simulates what a kill -9 mid-upload leaves behind: a file under tmp/ that
    // no metadata will ever reference.
    {
        std::ofstream stale(root.path() / "tmp" / "abandoned.part");
        stale << "interrupted";
    }

    CHECK(store.sweepTemporaries() == 1);
    CHECK(store.sweepTemporaries() == 0);
}

TEST_CASE("free space is reported for the backing filesystem", "[blob]") {
    TemporaryDirectory root("blob-space");
    BlobStore          store = makeStore(root);

    const auto space = store.space();
    CHECK(space.totalBytes > 0);
    CHECK(space.availableBytes <= space.freeBytes);
    CHECK(space.freeBytes <= space.totalBytes);
}
