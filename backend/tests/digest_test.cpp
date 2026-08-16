#include <catch2/catch_test_macros.hpp>

#include <string>

#include "storage/digest.hpp"

using monobucket::Digest;
using monobucket::md5Hex;
using monobucket::multipartETag;
using monobucket::sha256Hex;

// The expected values below are the published vectors for MD5 and SHA-256, and
// for the multipart case a digest computed independently. Checking our output
// against our own implementation would prove nothing — an ETag is a wire
// format, and it is only correct if it matches what every other S3 client
// computes.

TEST_CASE("digests match the published test vectors", "[digest]") {
    CHECK(md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");

    CHECK(sha256Hex("") == monobucket::kEmptySha256);
    CHECK(sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("an incremental digest matches a one-shot digest", "[digest]") {
    const std::string whole = "the quick brown fox jumps over the lazy dog";

    Digest digest;
    // Chunk boundaries must not affect the result; this is the property the
    // streaming write path depends on.
    digest.update(std::string_view(whole).substr(0, 7));
    digest.update(std::string_view(whole).substr(7, 1));
    digest.update(std::string_view(whole).substr(8));

    const auto result = digest.finish();
    CHECK(result.md5 == md5Hex(whole));
    CHECK(result.sha256 == sha256Hex(whole));
    CHECK(result.bytes == whole.size());
}

TEST_CASE("an empty digest reports the empty-input vectors", "[digest]") {
    Digest     digest;
    const auto result = digest.finish();

    CHECK(result.bytes == 0);
    CHECK(result.md5 == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(result.sha256 == monobucket::kEmptySha256);
}

TEST_CASE("the multipart ETag is the digest of the raw part digests", "[digest]") {
    const std::string part1 = std::string(1024, 'A');
    const std::string part2 = std::string(512, 'B');

    const std::string etag = multipartETag({md5Hex(part1), md5Hex(part2)});

    // Computed independently:
    //   md5(md5(b'A'*1024).digest() + md5(b'B'*512).digest()).hexdigest() + '-2'
    CHECK(etag == "9d41272d35684b4be019260f886b359e-2");

    // A multipart ETag is deliberately *not* the digest of the assembled bytes.
    // Conflating the two is the classic way to break client-side verification.
    CHECK(etag != md5Hex(part1 + part2));
}

TEST_CASE("the multipart ETag rejects malformed part digests", "[digest]") {
    CHECK_THROWS(multipartETag({"tooshort"}));
    CHECK_THROWS(multipartETag({std::string(32, 'z')}));
}

TEST_CASE("the multipart ETag records the part count", "[digest]") {
    const std::string one = multipartETag({md5Hex("a")});
    CHECK(one.ends_with("-1"));

    const std::string three = multipartETag({md5Hex("a"), md5Hex("b"), md5Hex("c")});
    CHECK(three.ends_with("-3"));
}
