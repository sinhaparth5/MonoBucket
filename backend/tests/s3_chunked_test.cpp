#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "s3/chunked.hpp"
#include "s3/s3_error.hpp"
#include "s3/sigv4.hpp"
#include "storage/digest.hpp"

using namespace monobucket;
using namespace monobucket::s3;

namespace {

constexpr const char* kAmzDate = "20260816T120000Z";
constexpr const char* kScope   = "20260816/us-east-1/s3/aws4_request";
constexpr const char* kSeed    = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::string signingKey() {
    return deriveSigningKey("secret-key", "20260816", "us-east-1", "s3");
}

std::string hexSize(std::size_t value) {
    std::ostringstream os;
    os << std::hex << value;
    return os.str();
}

/// Frames the given chunks the way an AWS SDK would, chaining the signatures
/// from the seed.
std::string frameSigned(const std::vector<std::string>& chunks,
                        const std::string& trailerBlock = {}) {
    const std::string key      = signingKey();
    std::string       previous = kSeed;
    std::string       out;

    const auto emit = [&](const std::string& payload) {
        const std::string signature =
            chunkSignature(key, kAmzDate, kScope, previous, sha256Hex(payload));
        previous = signature;
        out += hexSize(payload.size());
        out += ";chunk-signature=";
        out += signature;
        out += "\r\n";
        out += payload;
        out += "\r\n";
    };

    for (const auto& chunk : chunks) emit(chunk);

    // The terminating zero-length chunk, which is signed like any other.
    const std::string signature = chunkSignature(key, kAmzDate, kScope, previous, sha256Hex(""));
    out += "0;chunk-signature=";
    out += signature;
    out += "\r\n";
    out += trailerBlock;
    out += "\r\n";
    return out;
}

std::string frameUnsigned(const std::vector<std::string>& chunks,
                          const std::string& trailerBlock = {}) {
    std::string out;
    for (const auto& chunk : chunks) {
        out += hexSize(chunk.size());
        out += "\r\n";
        out += chunk;
        out += "\r\n";
    }
    out += "0\r\n";
    out += trailerBlock;
    out += "\r\n";
    return out;
}

ChunkedDecoder::Options signedOptions(std::uint64_t declaredLength = 0) {
    ChunkedDecoder::Options options;
    options.verifySignatures = true;
    options.signingKey       = signingKey();
    options.amzDate          = kAmzDate;
    options.scope            = kScope;
    options.seedSignature    = kSeed;
    options.declaredLength   = declaredLength;
    return options;
}

std::string decodeAll(ChunkedDecoder& decoder, std::string_view body) {
    std::string out;
    decoder.decode(body, [&out](std::string_view chunk) { out.append(chunk); });
    return out;
}

}  // namespace

TEST_CASE("a signed chunked body decodes to its payload", "[s3][chunked]") {
    const std::string body = frameSigned({"hello ", "world"});

    ChunkedDecoder decoder(signedOptions(11));
    REQUIRE(decodeAll(decoder, body) == "hello world");
    REQUIRE(decoder.decodedLength() == 11);
}

TEST_CASE("the chunk signature chain is followed, not just checked once",
          "[s3][chunked]") {
    // Each signature covers the previous one, so altering the second chunk must
    // be caught at the second chunk rather than at the end of the body.
    std::string body = frameSigned({"first-chunk", "second-chunk"});

    const std::size_t position = body.find("second-chunk");
    REQUIRE(position != std::string::npos);
    body[position] = 'S';

    ChunkedDecoder decoder(signedOptions());
    REQUIRE_THROWS_AS(decodeAll(decoder, body), S3Exception);
}

TEST_CASE("an unrelated signature is refused", "[s3][chunked]") {
    std::string body = frameSigned({"payload"});

    // Replace the first chunk signature with a well-formed but wrong one.
    const std::size_t position = body.find("chunk-signature=") + 16;
    body.replace(position, 64, std::string(64, 'a'));

    ChunkedDecoder decoder(signedOptions());
    REQUIRE_THROWS_AS(decodeAll(decoder, body), S3Exception);
}

TEST_CASE("a signed stream missing its signatures is refused", "[s3][chunked]") {
    // The framing is valid; what is missing is the thing that makes it signed.
    const std::string body = frameUnsigned({"payload"});

    ChunkedDecoder decoder(signedOptions());
    REQUIRE_THROWS_AS(decodeAll(decoder, body), S3Exception);
}

TEST_CASE("an unsigned streaming body decodes without signatures", "[s3][chunked]") {
    // STREAMING-UNSIGNED-PAYLOAD-TRAILER, which current AWS SDKs send whenever
    // they attach a checksum.
    const std::string body = frameUnsigned({"abc", "def"});

    ChunkedDecoder::Options options;
    options.declaredLength = 6;
    ChunkedDecoder decoder(std::move(options));

    REQUIRE(decodeAll(decoder, body) == "abcdef");
}

TEST_CASE("trailing headers are parsed rather than treated as payload",
          "[s3][chunked]") {
    const std::string body =
        frameUnsigned({"abc"}, "x-amz-checksum-crc32:AAAAAA==\r\n");

    ChunkedDecoder decoder(ChunkedDecoder::Options{});
    REQUIRE(decodeAll(decoder, body) == "abc");
    REQUIRE(decoder.trailers().size() == 1);
    REQUIRE(decoder.trailers().at("x-amz-checksum-crc32") == "AAAAAA==");
}

TEST_CASE("a truncated body is an error rather than a short object",
          "[s3][chunked]") {
    const std::string body = frameSigned({"hello world"});

    SECTION("cut mid-payload") {
        ChunkedDecoder decoder(signedOptions());
        REQUIRE_THROWS_AS(decodeAll(decoder, body.substr(0, body.size() / 2)), S3Exception);
    }

    SECTION("missing the terminating chunk") {
        const std::size_t terminator = body.rfind("0;chunk-signature=");
        REQUIRE(terminator != std::string::npos);

        ChunkedDecoder decoder(signedOptions());
        REQUIRE_THROWS_AS(decodeAll(decoder, body.substr(0, terminator)), S3Exception);
    }
}

TEST_CASE("the declared decoded length is enforced", "[s3][chunked]") {
    const std::string body = frameSigned({"hello world"});

    // Silently storing 11 bytes under a header promising 20 would produce an
    // object that is complete as far as every later reader can tell.
    ChunkedDecoder decoder(signedOptions(20));
    REQUIRE_THROWS_AS(decodeAll(decoder, body), S3Exception);
}

TEST_CASE("malformed framing is refused", "[s3][chunked]") {
    const auto rejects = [](std::string_view body) {
        ChunkedDecoder decoder(ChunkedDecoder::Options{});
        REQUIRE_THROWS_AS(decodeAll(decoder, body), S3Exception);
    };

    rejects("not-hex\r\ndata\r\n0\r\n\r\n");
    rejects("5\r\nabc\r\n0\r\n\r\n");            // shorter than declared
    rejects("3\r\nabcXX0\r\n\r\n");              // chunk not terminated by CRLF
    rejects("3;unknown-extension=1\r\nabc\r\n0\r\n\r\n");
    rejects("3\r\nabc\r\n");                     // no terminating chunk
    rejects("ffffffffffffffff\r\n");             // a length nothing could satisfy
}

TEST_CASE("an empty body still needs its terminating chunk", "[s3][chunked]") {
    ChunkedDecoder terminated(ChunkedDecoder::Options{});
    REQUIRE(decodeAll(terminated, "0\r\n\r\n").empty());
    REQUIRE(terminated.decodedLength() == 0);

    ChunkedDecoder empty(ChunkedDecoder::Options{});
    REQUIRE_THROWS_AS(decodeAll(empty, ""), S3Exception);
}

TEST_CASE("aws-chunked is recognised in a list of encodings", "[s3][chunked]") {
    REQUIRE(isAwsChunked("aws-chunked"));
    REQUIRE(isAwsChunked("aws-chunked,gzip"));
    REQUIRE(isAwsChunked("gzip, aws-chunked"));
    REQUIRE_FALSE(isAwsChunked("gzip"));
    REQUIRE_FALSE(isAwsChunked(""));
    REQUIRE_FALSE(isAwsChunked("aws-chunkedx"));
}
