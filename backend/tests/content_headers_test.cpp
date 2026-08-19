#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include "monobucket/constants.hpp"
#include "s3/content_headers.hpp"
#include "s3/request.hpp"
#include "storage/codec.hpp"
#include "storage/keyspace.hpp"
#include "storage/metadata_store.hpp"
#include "storage/records.hpp"
#include "storage/storage_engine.hpp"
#include "temporary_directory.hpp"

using monobucket::ContentHeaders;
using monobucket::ObjectRecord;
using monobucket::StorageEngine;
using monobucket::StorageError;
using monobucket::UploadRecord;
using monobucket::testing::TemporaryDirectory;
using monobucket::s3::S3Request;

namespace {

/// One of each, so a test that mixed two fields up fails on the value rather
/// than passing because both were the same string.
ContentHeaders sample() {
    ContentHeaders headers;
    headers.cacheControl       = "public, max-age=31536000, immutable";
    headers.contentDisposition = "attachment; filename=\"report.pdf\"";
    headers.contentEncoding    = "gzip";
    headers.contentLanguage    = "en-GB";
    headers.expires            = "Wed, 21 Oct 2026 07:28:00 GMT";
    return headers;
}

StorageEngine::Options optionsFor(const TemporaryDirectory& root) {
    StorageEngine::Options options;
    options.dataDir             = root.path();
    options.durability          = monobucket::Durability::None;
    options.chunkBytes          = 64 * 1024;
    options.metadataMemoryBytes = 8ull * 1024 * 1024;
    return options;
}

void checkSame(const ContentHeaders& actual, const ContentHeaders& expected) {
    CHECK(actual.cacheControl == expected.cacheControl);
    CHECK(actual.contentDisposition == expected.contentDisposition);
    CHECK(actual.contentEncoding == expected.contentEncoding);
    CHECK(actual.contentLanguage == expected.contentLanguage);
    CHECK(actual.expires == expected.expires);
}

std::string encoded(const ContentHeaders& headers) {
    std::string           out;
    monobucket::codec::Writer writer(out);
    monobucket::encodeContentHeaders(writer, headers);
    return out;
}

ContentHeaders decoded(std::string_view bytes) {
    monobucket::codec::Reader reader(bytes);
    return monobucket::decodeContentHeaders(reader);
}

/// A GET carrying the given query string, which is where the `response-*`
/// overrides arrive.
S3Request get(std::string_view query) {
    return monobucket::s3::parseRequest("GET", "/photos/beach.jpg", query, "");
}

std::string headerValue(const std::vector<monobucket::s3::ResolvedHeader>& resolved,
                        std::string_view                                  name) {
    for (const auto& header : resolved) {
        if (header.name == name) return header.value;
    }
    return {};
}

}  // namespace

// --- Encoding ---------------------------------------------------------------

TEST_CASE("every content header survives the record encoding", "[content-headers][codec]") {
    checkSame(decoded(encoded(sample())), sample());
}

TEST_CASE("headers nobody set cost one byte and read back empty",
          "[content-headers][codec]") {
    const std::string bytes = encoded(ContentHeaders{});
    // The count, and nothing else. An object with no cache directives is the
    // ordinary case and must not pay five empty strings for it.
    CHECK(bytes.size() == 1);
    CHECK(decoded(bytes).empty());
}

TEST_CASE("only the headers that were set are written", "[content-headers][codec]") {
    ContentHeaders one;
    one.cacheControl = "no-store";

    const ContentHeaders back = decoded(encoded(one));
    CHECK(back.cacheControl == "no-store");
    CHECK(back.contentDisposition.empty());
    CHECK(back.contentEncoding.empty());
    CHECK(back.contentLanguage.empty());
    CHECK(back.expires.empty());
}

TEST_CASE("a header a newer build added is skipped, not fatal",
          "[content-headers][codec]") {
    // Two fields: one this build knows, one it does not. The unknown id must
    // consume its value so the known one that follows still decodes — the whole
    // reason the fields are tagged rather than positional.
    std::string               bytes;
    monobucket::codec::Writer writer(bytes);
    writer.varint(2);
    writer.u8(200);  // not an id any build has ever assigned
    writer.string("whatever a later version stores here");
    writer.u8(1);  // Cache-Control
    writer.string("max-age=60");

    const ContentHeaders back = decoded(bytes);
    CHECK(back.cacheControl == "max-age=60");
}

TEST_CASE("an implausible field count is refused rather than reserved",
          "[content-headers][codec]") {
    std::string               bytes;
    monobucket::codec::Writer writer(bytes);
    writer.varint(1'000'000);

    CHECK_THROWS_AS(decoded(bytes), monobucket::codec::DecodeError);
}

// --- Storage round trip -----------------------------------------------------

TEST_CASE("a stored object returns its content headers", "[content-headers][engine]") {
    TemporaryDirectory root("content-headers-put");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("assets");

    StorageEngine::PutRequest put;
    put.bucket  = "assets";
    put.key     = "app.js.gz";
    put.content = sample();
    engine.putObject(put, "compressed-bytes");

    const auto record = engine.statObject("assets", "app.js.gz");
    REQUIRE(record.has_value());
    checkSame(record->content, sample());
}

TEST_CASE("a multipart object carries what CreateMultipartUpload named",
          "[content-headers][engine]") {
    TemporaryDirectory root("content-headers-multipart");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("archive");

    StorageEngine::PutRequest put;
    put.bucket  = "archive";
    put.key     = "big.bin";
    put.content = sample();

    const std::string uploadId = engine.createUpload(put);

    // The upload record holds them between the create and the completion.
    const auto upload = engine.getUpload(uploadId);
    REQUIRE(upload.has_value());
    checkSame(upload->content, sample());

    std::vector<StorageEngine::RequestedPart> manifest;
    {
        auto writer = engine.beginWrite();
        writer.write(std::string(5 * 1024 * 1024, 'A'));
        manifest.push_back({1, engine.finishPart(uploadId, 1, std::move(writer)).etag});
    }
    {
        auto writer = engine.beginWrite();
        writer.write(std::string(1024, 'B'));
        manifest.push_back({2, engine.finishPart(uploadId, 2, std::move(writer)).etag});
    }

    const ObjectRecord record = engine.completeUpload(uploadId, manifest);
    checkSame(record.content, sample());

    // And on the way back out of the store, not just on the record completion
    // happened to return.
    const auto stored = engine.statObject("archive", "big.bin");
    REQUIRE(stored.has_value());
    checkSame(stored->content, sample());
}

TEST_CASE("an object written before content headers existed still reads",
          "[content-headers][engine]") {
    TemporaryDirectory root("content-headers-legacy");

    monobucket::MetadataStoreOptions options;
    options.path              = (root.path() / "meta").string();
    options.memoryBudgetBytes = 8ull * 1024 * 1024;

    {
        auto store = monobucket::openRocksMetadataStore(options);

        monobucket::BucketRecord bucket;
        bucket.name      = "old";
        bucket.createdAt = monobucket::nowMs();
        store->createBucket(bucket);
    }

    // Written byte for byte the way the encoder wrote an object before this
    // change: version, payload identity, content type, timestamp, metadata,
    // checksum — and then nothing. Spelled out literally rather than produced
    // by the current encoder, because the point is what an older build left on
    // disk, and a test that asked today's code what that was would agree with
    // any answer it gave.
    std::string               legacy;
    monobucket::codec::Writer writer(legacy);
    writer.u8(1);                      // kRecordVersion
    writer.string(std::string(32, 'a'));  // blobId
    writer.varint(11);                 // size
    writer.string("etag-value");
    writer.string("sha256-value");
    writer.string("text/plain");
    writer.varint(1'700'000'000'000);  // lastModified
    writer.varint(1);                  // one metadata pair
    writer.string("author");
    writer.string("someone");
    writer.u8(0);                      // no checksum

    {
        rocksdb::DB*     raw = nullptr;
        rocksdb::Options open;
        REQUIRE(rocksdb::DB::Open(open, options.path, &raw).ok());
        const std::unique_ptr<rocksdb::DB> db(raw);
        REQUIRE(db->Put(rocksdb::WriteOptions(), monobucket::keys::object("old", "note.txt"),
                        legacy)
                    .ok());
    }

    auto       store  = monobucket::openRocksMetadataStore(options);
    const auto record = store->getObject("old", "note.txt");

    REQUIRE(record.has_value());
    CHECK(record->contentType == "text/plain");
    CHECK(record->size == 11);
    CHECK(record->userMetadata.at("author") == "someone");
    // Absent, and no error on the way to finding that out.
    CHECK(record->content.empty());
}

// --- Emitting ---------------------------------------------------------------

TEST_CASE("what was stored is what is emitted", "[content-headers][s3]") {
    const auto resolved = resolveContentHeaders(sample(), get(""));

    REQUIRE(resolved.size() == 5);
    CHECK(headerValue(resolved, "Cache-Control") == sample().cacheControl);
    CHECK(headerValue(resolved, "Content-Disposition") == sample().contentDisposition);
    CHECK(headerValue(resolved, "Content-Encoding") == sample().contentEncoding);
    CHECK(headerValue(resolved, "Content-Language") == sample().contentLanguage);
    CHECK(headerValue(resolved, "Expires") == sample().expires);
}

TEST_CASE("a header that was never set is omitted, not sent empty",
          "[content-headers][s3]") {
    CHECK(resolveContentHeaders(ContentHeaders{}, get("")).empty());
}

TEST_CASE("a response- override wins over the stored value", "[content-headers][s3]") {
    const auto resolved = resolveContentHeaders(sample(), get("response-cache-control=no-store"));

    CHECK(headerValue(resolved, "Cache-Control") == "no-store");
    // And only that one: an override is a statement about one header.
    CHECK(headerValue(resolved, "Content-Disposition") == sample().contentDisposition);
    CHECK(headerValue(resolved, "Content-Language") == sample().contentLanguage);
}

TEST_CASE("an override applies to an object that stored nothing",
          "[content-headers][s3]") {
    // What a presigned download link relies on: forcing an attachment for an
    // object that was uploaded without a disposition of its own.
    const auto resolved = resolveContentHeaders(
        ContentHeaders{}, get("response-content-disposition=attachment%3B%20filename%3Dx.bin"));

    REQUIRE(resolved.size() == 1);
    CHECK(resolved.front().name == "Content-Disposition");
    CHECK(resolved.front().value == "attachment; filename=x.bin");
}

TEST_CASE("an empty override is not an override", "[content-headers][s3]") {
    // A parameter present but blank means the link author set nothing, and the
    // stored value stands. Clearing a header is not something S3 offers here.
    const auto resolved = resolveContentHeaders(sample(), get("response-cache-control="));
    CHECK(headerValue(resolved, "Cache-Control") == sample().cacheControl);
}

// --- Validation -------------------------------------------------------------

TEST_CASE("a header value that could split the response is refused",
          "[content-headers][storage]") {
    using monobucket::isStorableHeaderValue;

    CHECK(isStorableHeaderValue("public, max-age=60"));
    CHECK(isStorableHeaderValue("attachment; filename=\"rapport-été.pdf\""));  // UTF-8 is fine

    CHECK_FALSE(isStorableHeaderValue("no-store\r\nX-Injected: yes"));
    CHECK_FALSE(isStorableHeaderValue("no-store\rX: y"));
    CHECK_FALSE(isStorableHeaderValue("no-store\nX: y"));
    CHECK_FALSE(isStorableHeaderValue(std::string("no-store\0hidden", 15)));
    CHECK_FALSE(isStorableHeaderValue("no-store\x7F"));

    CHECK(isStorableHeaderValue(std::string(monobucket::limits::kMaxContentHeaderLength, 'a')));
    CHECK_FALSE(
        isStorableHeaderValue(std::string(monobucket::limits::kMaxContentHeaderLength + 1, 'a')));
}

TEST_CASE("the engine refuses to store a header it could not emit",
          "[content-headers][engine]") {
    // The S3 layer refuses these before a byte is read. This is the check
    // behind that one — the last place a caller cannot skip, and what makes the
    // console upload path safe without repeating the rule.
    TemporaryDirectory root("content-headers-injection");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("assets");

    StorageEngine::PutRequest put;
    put.bucket                      = "assets";
    put.key                         = "note.txt";
    put.content.cacheControl        = "no-store\r\nX-Injected: yes";

    CHECK_THROWS_AS(engine.putObject(put, "body"), monobucket::StorageError);
    // And nothing was stored under that key.
    CHECK_FALSE(engine.statObject("assets", "note.txt").has_value());

    // The same guard on the multipart path, where the value arrives one request
    // before the bytes it will be attached to.
    CHECK_THROWS_AS(engine.createUpload(put), monobucket::StorageError);
}
