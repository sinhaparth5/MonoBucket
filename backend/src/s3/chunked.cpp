#include "s3/chunked.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

#include "s3/s3_error.hpp"
#include "s3/sigv4.hpp"
#include "storage/digest.hpp"

namespace monobucket::s3 {
namespace {

/// A single chunk cannot be larger than the largest object we accept, and the
/// header is hex, so a hostile length field must be rejected before it is used
/// to index anything.
constexpr std::uint64_t kMaxChunkBytes = 5ull * 1024 * 1024 * 1024;

/// Bounds the framing overhead a client can impose. A body made entirely of
/// zero-length chunks would otherwise be an unbounded loop over a bounded body.
constexpr std::size_t kMaxChunks = 4'000'000;

constexpr std::size_t kMaxChunkHeaderBytes = 1024;
constexpr std::size_t kMaxTrailers         = 32;

[[noreturn]] void malformed(const char* what) {
    throw S3Exception(S3ErrorCode::IncompleteBody,
                      std::string("Malformed aws-chunked body: ") + what);
}

/// Consumes up to the next CRLF, returning the line without it.
std::string_view takeLine(std::string_view& input, std::size_t limit) {
    const std::size_t end = input.find("\r\n");
    if (end == std::string_view::npos) malformed("a line is not terminated by CRLF");
    if (end > limit) malformed("a header line is unreasonably long");

    const std::string_view line = input.substr(0, end);
    input.remove_prefix(end + 2);
    return line;
}

std::uint64_t parseHexSize(std::string_view text) {
    if (text.empty() || text.size() > 16) malformed("chunk size is not a valid hex number");

    std::uint64_t value = 0;
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        int                 digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else malformed("chunk size is not a valid hex number");

        value = value * 16 + static_cast<std::uint64_t>(digit);
    }

    if (value > kMaxChunkBytes) malformed("chunk size exceeds the maximum object size");
    return value;
}

std::string toLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

}  // namespace

ChunkedDecoder::ChunkedDecoder(Options options) : options_(std::move(options)) {}

void ChunkedDecoder::decode(std::string_view body,
                            const std::function<void(std::string_view)>& sink) {
    std::string previousSignature = options_.seedSignature;
    std::size_t chunkCount        = 0;
    bool        sawFinalChunk     = false;

    while (!body.empty()) {
        if (++chunkCount > kMaxChunks) malformed("too many chunks");

        const std::string_view header = takeLine(body, kMaxChunkHeaderBytes);

        // `<size>` or `<size>;chunk-signature=<hex>`. Extensions other than
        // chunk-signature are not defined for aws-chunked, so anything else is
        // treated as framing we do not understand rather than ignored.
        std::string_view sizeField      = header;
        std::string_view signatureField;
        if (const std::size_t semicolon = header.find(';'); semicolon != std::string_view::npos) {
            sizeField = header.substr(0, semicolon);

            const std::string_view extension = header.substr(semicolon + 1);
            constexpr std::string_view kPrefix = "chunk-signature=";
            if (extension.compare(0, kPrefix.size(), kPrefix) != 0) {
                malformed("unrecognised chunk extension");
            }
            signatureField = extension.substr(kPrefix.size());
        }

        const std::uint64_t size = parseHexSize(sizeField);

        if (options_.verifySignatures && signatureField.empty()) {
            throw S3Exception(S3ErrorCode::SignatureDoesNotMatch,
                              "A chunk in a signed streaming upload carried no signature.");
        }

        if (size > body.size()) malformed("a chunk is shorter than its declared size");
        const std::string_view payload = body.substr(0, static_cast<std::size_t>(size));
        body.remove_prefix(static_cast<std::size_t>(size));

        if (options_.verifySignatures) {
            const std::string expected =
                chunkSignature(options_.signingKey, options_.amzDate, options_.scope,
                               previousSignature, sha256Hex(payload));
            if (!secureEquals(expected, toLower(signatureField))) {
                throw S3Exception(S3ErrorCode::SignatureDoesNotMatch,
                                  "The signature of chunk " + std::to_string(chunkCount) +
                                      " does not match.");
            }
            previousSignature = expected;
        }

        if (size == 0) {
            sawFinalChunk = true;
            // The zero-length chunk's CRLF is followed by the trailer block, if
            // any, then a blank line. Some clients end the body there instead.
            if (!body.empty() && body.compare(0, 2, "\r\n") == 0) body.remove_prefix(2);
            break;
        }

        if (body.compare(0, 2, "\r\n") != 0) malformed("a chunk is not terminated by CRLF");
        body.remove_prefix(2);

        decoded_ += size;
        sink(payload);
    }

    if (!sawFinalChunk) malformed("the body ended without a zero-length chunk");

    // Trailers. The only ones S3 defines are checksums, and a checksum is worth
    // exactly as much as the thing that authenticates it: on a signed streaming
    // body the trailer block carries its own signature, chained from the final
    // chunk's, and anything on the path could otherwise rewrite the checksum to
    // match bytes it had also rewritten.
    //
    // The canonical form the signature covers is the trailing headers as
    // `name:value\n`, lowercased and trimmed, in the order they were sent, with
    // the signature line itself excluded — it cannot cover itself.
    std::string canonicalTrailers;
    std::string declaredSignature;

    while (!body.empty()) {
        const std::string_view line = takeLine(body, kMaxChunkHeaderBytes);
        if (line.empty()) break;
        if (trailers_.size() >= kMaxTrailers) malformed("too many trailing headers");

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) malformed("a trailing header has no colon");

        std::string       name  = toLower(trim(line.substr(0, colon)));
        const std::string value = std::string(trim(line.substr(colon + 1)));

        if (name == "x-amz-trailer-signature") {
            declaredSignature = toLower(value);
            continue;
        }

        canonicalTrailers += name;
        canonicalTrailers += ':';
        canonicalTrailers += value;
        canonicalTrailers += '\n';
        trailers_.emplace(std::move(name), value);
    }

    if (options_.verifySignatures && options_.expectTrailerSignature) {
        if (declaredSignature.empty()) {
            throw S3Exception(S3ErrorCode::SignatureDoesNotMatch,
                              "A signed streaming upload declared trailing headers but sent no "
                              "x-amz-trailer-signature.");
        }

        const std::string expected =
            trailerSignature(options_.signingKey, options_.amzDate, options_.scope,
                             previousSignature, sha256Hex(canonicalTrailers));
        if (!secureEquals(expected, declaredSignature)) {
            throw S3Exception(S3ErrorCode::SignatureDoesNotMatch,
                              "The signature of the trailing header block does not match.");
        }
    }

    if (options_.declaredLength != 0 && decoded_ != options_.declaredLength) {
        throw S3Exception(S3ErrorCode::IncompleteBody,
                          "The body decoded to " + std::to_string(decoded_) +
                              " bytes, but x-amz-decoded-content-length declared " +
                              std::to_string(options_.declaredLength) + ".");
    }
}

bool isAwsChunked(std::string_view contentEncoding) {
    // The header may list several encodings, e.g. `aws-chunked,gzip`.
    std::size_t pos = 0;
    while (pos <= contentEncoding.size()) {
        const std::size_t comma = contentEncoding.find(',', pos);
        const std::size_t end = (comma == std::string_view::npos) ? contentEncoding.size() : comma;
        if (trim(contentEncoding.substr(pos, end - pos)) == "aws-chunked") return true;
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return false;
}

}  // namespace monobucket::s3
