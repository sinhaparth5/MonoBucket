#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include "s3/base64.hpp"
#include "s3/checksum.hpp"
#include "s3/s3_error.hpp"
#include "storage/checksum.hpp"
#include "storage/storage_engine.hpp"
#include "temporary_directory.hpp"

using monobucket::Checksum;
using monobucket::ChecksumAlgorithm;
using monobucket::ChecksumComputer;
using monobucket::checksumOf;
using monobucket::compositeChecksum;
using monobucket::StorageEngine;
using monobucket::testing::TemporaryDirectory;
using namespace monobucket::s3;

namespace {

constexpr std::uint64_t kMiB = 1024 * 1024;

/// Base64 of the raw digest, which is the form every value travels in.
std::string encode(ChecksumAlgorithm algorithm, std::string_view data) {
    return base64Encode(checksumOf(algorithm, data));
}

/// A header table standing in for a request, so the resolution rules can be
/// exercised as data rather than through a socket.
ChecksumHeaders headers(ChecksumHeaders table) { return table; }

StorageEngine::Options optionsFor(const TemporaryDirectory& root) {
    StorageEngine::Options options;
    options.dataDir             = root.path();
    options.durability          = monobucket::Durability::None;
    options.chunkBytes          = 64 * 1024;
    options.metadataMemoryBytes = 8ull * 1024 * 1024;
    return options;
}

}  // namespace

// --- The algorithms --------------------------------------------------------
//
// Pinned against values computed outside this codebase. A checksum that only
// agrees with our own implementation of it verifies nothing: the entire point
// is that an AWS SDK computed the same number over the same bytes.

TEST_CASE("each algorithm matches its published value", "[checksum]") {
    SECTION("the empty payload") {
        CHECK(encode(ChecksumAlgorithm::Crc32, "") == "AAAAAA==");
        CHECK(encode(ChecksumAlgorithm::Crc32c, "") == "AAAAAA==");
        CHECK(encode(ChecksumAlgorithm::Crc64Nvme, "") == "AAAAAAAAAAA=");
        CHECK(encode(ChecksumAlgorithm::Sha1, "") == "2jmj7l5rSw0yVb/vlWAYkK/YBwk=");
        CHECK(encode(ChecksumAlgorithm::Sha256, "") ==
              "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=");
    }

    SECTION("abc") {
        CHECK(encode(ChecksumAlgorithm::Crc32, "abc") == "NSRBwg==");
        CHECK(encode(ChecksumAlgorithm::Crc32c, "abc") == "Nks/tw==");
        CHECK(encode(ChecksumAlgorithm::Crc64Nvme, "abc") == "BeXKuz/B+us=");
        CHECK(encode(ChecksumAlgorithm::Sha1, "abc") == "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
        CHECK(encode(ChecksumAlgorithm::Sha256, "abc") ==
              "ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=");
    }

    SECTION("the CRC check vector") {
        // 0xCBF43926, 0xE3069283 and 0xAE8B14860A799888 are the published check
        // values for CRC-32, CRC-32C and CRC-64/NVME over "123456789". Getting
        // the reflection wrong produces a plausible-looking number that fails
        // exactly here.
        CHECK(encode(ChecksumAlgorithm::Crc32, "123456789") == "y/Q5Jg==");
        CHECK(encode(ChecksumAlgorithm::Crc32c, "123456789") == "4waSgw==");
        CHECK(encode(ChecksumAlgorithm::Crc64Nvme, "123456789") == "rosUhgp5mIg=");
    }
}

TEST_CASE("a checksum computed in pieces matches one computed whole", "[checksum]") {
    // The write path never sees the payload in one go, so this is the property
    // that makes streaming verification equivalent to buffering it.
    for (const auto algorithm :
         {ChecksumAlgorithm::Crc32, ChecksumAlgorithm::Crc32c, ChecksumAlgorithm::Crc64Nvme,
          ChecksumAlgorithm::Sha1, ChecksumAlgorithm::Sha256}) {
        ChecksumComputer computer(algorithm);
        computer.update("123");
        computer.update("");
        computer.update("456");
        computer.update("789");
        CHECK(computer.finish() == checksumOf(algorithm, "123456789"));
    }
}

TEST_CASE("algorithm names round-trip in both spellings", "[checksum]") {
    for (const auto algorithm :
         {ChecksumAlgorithm::Crc32, ChecksumAlgorithm::Crc32c, ChecksumAlgorithm::Crc64Nvme,
          ChecksumAlgorithm::Sha1, ChecksumAlgorithm::Sha256}) {
        const std::string name(toString(algorithm));
        CHECK(monobucket::checksumAlgorithmFromString(name) == algorithm);

        // The header suffix arrives lowercased, the SDK header uppercased.
        std::string lower = name;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        CHECK(monobucket::checksumAlgorithmFromString(lower) == algorithm);
    }

    CHECK_FALSE(monobucket::checksumAlgorithmFromString("md5").has_value());
    CHECK_FALSE(monobucket::checksumAlgorithmFromString("").has_value());
}

// --- The composite ---------------------------------------------------------

TEST_CASE("the composite is a checksum of the parts' raw checksums", "[checksum]") {
    const Checksum composite = compositeChecksum(
        ChecksumAlgorithm::Crc32,
        {checksumOf(ChecksumAlgorithm::Crc32, "abc"),
         checksumOf(ChecksumAlgorithm::Crc32, "123456789")});

    CHECK(composite.parts == 2);
    CHECK(encodeChecksum(composite) == "CyRHwQ==-2");

    // Not the checksum of the concatenated payloads, which is the mistake that
    // makes a composite look right until a client checks it.
    CHECK(composite.value != checksumOf(ChecksumAlgorithm::Crc32, "abc123456789"));
}

TEST_CASE("a composite is reported as such", "[checksum]") {
    const Checksum composite =
        compositeChecksum(ChecksumAlgorithm::Sha256, {checksumOf(ChecksumAlgorithm::Sha256, "a")});
    CHECK(checksumTypeOf(composite) == "COMPOSITE");

    Checksum whole;
    whole.algorithm = ChecksumAlgorithm::Sha256;
    whole.value     = checksumOf(ChecksumAlgorithm::Sha256, "a");
    CHECK(checksumTypeOf(whole) == "FULL_OBJECT");
}

// --- The wire form ---------------------------------------------------------

TEST_CASE("the wire form round-trips", "[checksum]") {
    Checksum original;
    original.algorithm = ChecksumAlgorithm::Sha1;
    original.value     = checksumOf(ChecksumAlgorithm::Sha1, "abc");
    original.parts     = 7;

    const auto decoded = decodeChecksum(ChecksumAlgorithm::Sha1, encodeChecksum(original));
    REQUIRE(decoded.has_value());
    CHECK(decoded->value == original.value);
    CHECK(decoded->parts == 7);
}

TEST_CASE("a malformed wire value is refused rather than guessed at", "[checksum]") {
    // Right algorithm, wrong length: a CRC32 value offered as a SHA-256.
    CHECK_FALSE(decodeChecksum(ChecksumAlgorithm::Sha256, "NSRBwg==").has_value());
    CHECK_FALSE(decodeChecksum(ChecksumAlgorithm::Crc32, "not base64!").has_value());
    CHECK_FALSE(decodeChecksum(ChecksumAlgorithm::Crc32, "").has_value());

    // `-0` parts is not a composite over nothing, it is a broken value.
    CHECK_FALSE(decodeChecksum(ChecksumAlgorithm::Crc32, "NSRBwg==-0").has_value());
}

// --- What the request asks for ---------------------------------------------

TEST_CASE("a checksum header names both the algorithm and the value", "[checksum]") {
    const auto wanted =
        resolveChecksumRequest(headers({{"x-amz-checksum-crc32", "NSRBwg=="}}));

    REQUIRE(wanted.algorithm == ChecksumAlgorithm::Crc32);
    CHECK(wanted.expected == "NSRBwg==");
    CHECK_FALSE(wanted.inTrailer);
}

TEST_CASE("a request naming no checksum is an ordinary upload", "[checksum]") {
    // The one property this must never lose: a client that says nothing about
    // integrity still gets its object stored.
    const auto wanted = resolveChecksumRequest(headers({{"content-type", "text/plain"}}));
    CHECK_FALSE(wanted.wanted());
}

TEST_CASE("a trailer is resolved before the body is read", "[checksum]") {
    const auto wanted = resolveChecksumRequest(
        headers({{"x-amz-trailer", "x-amz-checksum-crc32c"},
                 {"x-amz-sdk-checksum-algorithm", "CRC32C"}}));

    REQUIRE(wanted.algorithm == ChecksumAlgorithm::Crc32c);
    CHECK(wanted.inTrailer);
    CHECK(wanted.trailerName == "x-amz-checksum-crc32c");
    CHECK(wanted.expected.empty());
}

TEST_CASE("an algorithm may be declared with no value", "[checksum]") {
    // CreateMultipartUpload has no payload to checksum; it is choosing what the
    // parts will carry.
    const auto wanted =
        resolveChecksumRequest(headers({{"x-amz-checksum-algorithm", "SHA256"}}));

    REQUIRE(wanted.algorithm == ChecksumAlgorithm::Sha256);
    CHECK(wanted.expected.empty());
    CHECK_FALSE(wanted.inTrailer);
}

TEST_CASE("x-amz-checksum-algorithm is not read as a checksum value", "[checksum]") {
    // Regression: `algorithm`, `mode` and `type` share the `x-amz-checksum-`
    // prefix with the value headers. Reading them as values would refuse every
    // CreateMultipartUpload an SDK sends.
    CHECK_NOTHROW(resolveChecksumRequest(headers({{"x-amz-checksum-mode", "ENABLED"}})));
    CHECK_NOTHROW(resolveChecksumRequest(headers({{"x-amz-checksum-type", "FULL_OBJECT"}})));
}

TEST_CASE("two checksums under different algorithms are refused", "[checksum]") {
    CHECK_THROWS_AS(resolveChecksumRequest(headers({{"x-amz-checksum-crc32", "NSRBwg=="},
                                                    {"x-amz-checksum-sha1", "qZk="}})),
                    S3Exception);
}

TEST_CASE("an algorithm this build cannot compute is refused, not ignored", "[checksum]") {
    // The whole defect: a checksum accepted and discarded looks to the client
    // exactly like one that was verified.
    CHECK_THROWS_AS(resolveChecksumRequest(headers({{"x-amz-checksum-md5", "abc"}})),
                    S3Exception);
    CHECK_THROWS_AS(
        resolveChecksumRequest(headers({{"x-amz-sdk-checksum-algorithm", "WHIRLPOOL"}})),
        S3Exception);
    CHECK_THROWS_AS(resolveChecksumRequest(headers({{"x-amz-trailer", "x-amz-meta-something"}})),
                    S3Exception);
}

// --- Verification ----------------------------------------------------------

TEST_CASE("a matching checksum verifies", "[checksum]") {
    CHECK_NOTHROW(verifyChecksum(ChecksumAlgorithm::Crc32, "NSRBwg==",
                                 checksumOf(ChecksumAlgorithm::Crc32, "abc")));
}

TEST_CASE("a mismatched checksum is BadDigest", "[checksum]") {
    try {
        verifyChecksum(ChecksumAlgorithm::Crc32, "NSRBwg==",
                       checksumOf(ChecksumAlgorithm::Crc32, "abd"));
        FAIL("a mismatched checksum was accepted");
    } catch (const S3Exception& error) {
        CHECK(error.code() == S3ErrorCode::BadDigest);
    }
}

TEST_CASE("a malformed checksum header is InvalidRequest, not BadDigest", "[checksum]") {
    // A header that is not a digest at all is the client's request being wrong,
    // not the body being corrupt, and a client retries those differently.
    try {
        verifyChecksum(ChecksumAlgorithm::Crc32, "!!!!",
                       checksumOf(ChecksumAlgorithm::Crc32, "abc"));
        FAIL("a malformed checksum was accepted");
    } catch (const S3Exception& error) {
        CHECK(error.code() == S3ErrorCode::InvalidRequest);
    }
}

// --- Through the storage engine --------------------------------------------

TEST_CASE("an object carries its checksum through the store", "[checksum][engine]") {
    TemporaryDirectory root("checksum");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("photos");

    StorageEngine::PutRequest put;
    put.bucket             = "photos";
    put.key                = "a.txt";
    put.checksum.algorithm = ChecksumAlgorithm::Crc32;
    put.checksum.value     = checksumOf(ChecksumAlgorithm::Crc32, "abc");

    engine.putObject(put, "abc");

    const auto stored = engine.statObject("photos", "a.txt");
    REQUIRE(stored.has_value());
    REQUIRE(stored->checksum.present());
    CHECK(stored->checksum.algorithm == ChecksumAlgorithm::Crc32);
    CHECK(encodeChecksum(stored->checksum) == "NSRBwg==");
}

TEST_CASE("an object stored without a checksum reads back without one",
          "[checksum][engine]") {
    // A store that predates checksums decodes the same way, which is what makes
    // the appended field safe to add without a version bump.
    TemporaryDirectory root("checksum");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("photos");

    StorageEngine::PutRequest put;
    put.bucket = "photos";
    put.key    = "a.txt";
    engine.putObject(put, "abc");

    const auto stored = engine.statObject("photos", "a.txt");
    REQUIRE(stored.has_value());
    CHECK_FALSE(stored->checksum.present());
}

TEST_CASE("a completed multipart upload reports the composite", "[checksum][engine]") {
    TemporaryDirectory root("checksum");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("photos");

    StorageEngine::PutRequest put;
    put.bucket             = "photos";
    put.key                = "big.bin";
    put.checksum.algorithm = ChecksumAlgorithm::Crc32;

    const std::string uploadId = engine.createUpload(put);

    const std::string first(5 * kMiB, 'a');
    const std::string second = "bbb";

    std::vector<StorageEngine::RequestedPart> manifest;
    std::uint32_t                             number = 1;
    for (const std::string& payload : {first, second}) {
        auto writer = engine.beginWrite();
        writer.write(payload);

        Checksum checksum;
        checksum.algorithm = ChecksumAlgorithm::Crc32;
        checksum.value     = checksumOf(ChecksumAlgorithm::Crc32, payload);

        const auto part = engine.finishPart(uploadId, number, std::move(writer), {}, checksum);
        manifest.push_back({number, part.etag});
        number += 1;
    }

    const auto record = engine.completeUpload(uploadId, manifest);
    REQUIRE(record.checksum.present());
    CHECK(record.checksum.parts == 2);
    CHECK(encodeChecksum(record.checksum) == "k5yxow==-2");
}

TEST_CASE("a completion whose checksum disagrees publishes nothing",
          "[checksum][engine]") {
    TemporaryDirectory root("checksum");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("photos");

    StorageEngine::PutRequest put;
    put.bucket             = "photos";
    put.key                = "big.bin";
    put.checksum.algorithm = ChecksumAlgorithm::Crc32;

    const std::string uploadId = engine.createUpload(put);

    auto writer = engine.beginWrite();
    writer.write("only-part");

    Checksum checksum;
    checksum.algorithm = ChecksumAlgorithm::Crc32;
    checksum.value     = checksumOf(ChecksumAlgorithm::Crc32, "only-part");
    const auto part    = engine.finishPart(uploadId, 1, std::move(writer), {}, checksum);

    Checksum wrong;
    wrong.algorithm = ChecksumAlgorithm::Crc32;
    wrong.value     = checksumOf(ChecksumAlgorithm::Crc32, "something else");
    wrong.parts     = 1;

    CHECK_THROWS_AS(engine.completeUpload(uploadId, {{1, part.etag}}, wrong),
                    monobucket::StorageError);

    // Refused before anything was published: the key does not exist and the
    // upload is still there to be retried or aborted.
    CHECK_FALSE(engine.statObject("photos", "big.bin").has_value());
    CHECK(engine.getUpload(uploadId).has_value());
}

TEST_CASE("an upload whose parts carry no checksum completes without one",
          "[checksum][engine]") {
    TemporaryDirectory root("checksum");
    StorageEngine      engine(optionsFor(root));
    engine.createBucket("photos");

    StorageEngine::PutRequest put;
    put.bucket = "photos";
    put.key    = "big.bin";

    const std::string uploadId = engine.createUpload(put);

    auto writer = engine.beginWrite();
    writer.write("only-part");
    const auto part = engine.finishPart(uploadId, 1, std::move(writer));

    const auto record = engine.completeUpload(uploadId, {{1, part.etag}});
    CHECK_FALSE(record.checksum.present());
}
