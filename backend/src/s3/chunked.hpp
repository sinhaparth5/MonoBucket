#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

// The `aws-chunked` content encoding.
//
// This is not HTTP chunked transfer encoding, and the two can be layered — the
// HTTP layer strips its own framing before we ever see the body, so what
// arrives here is the AWS framing on its own:
//
//     <hex-size>[;chunk-signature=<64 hex>]\r\n<data>\r\n
//     ...
//     0[;chunk-signature=<64 hex>]\r\n
//     [<trailer-name>:<value>\r\n]*
//     \r\n
//
// Every AWS SDK uses it for streaming uploads, so `aws s3 cp` does not work
// without it. The signed variant chains a signature per chunk from the request
// signature, which means a truncated or altered body is caught at the chunk it
// was altered in rather than at the end.

namespace monobucket::s3 {

class ChunkedDecoder {
public:
    struct Options {
        /// When set, every chunk signature is verified and the chain is
        /// followed. False for STREAMING-UNSIGNED-PAYLOAD-TRAILER, where the
        /// body carries the same framing but no signatures.
        bool verifySignatures = false;

        std::string signingKey;     ///< raw 32 bytes from AuthOutcome
        std::string amzDate;
        std::string scope;
        std::string seedSignature;  ///< the request signature

        /// The client declared a trailing header block, so the body must end
        /// with an `x-amz-trailer-signature` covering it. Only meaningful
        /// alongside `verifySignatures`: the unsigned streaming framing carries
        /// trailers with nothing to verify them by, and that is the client's
        /// choice to make, not a failure.
        bool expectTrailerSignature = false;

        /// `x-amz-decoded-content-length`. Zero means the client did not send
        /// it, in which case the decoded length is whatever the framing says.
        std::uint64_t declaredLength = 0;
    };

    explicit ChunkedDecoder(Options options);

    /// Decodes `body`, handing each chunk's payload to `sink` in order. The
    /// views passed to `sink` point into `body` — nothing is copied, which is
    /// what lets a multi-gigabyte upload stream straight into the blob writer.
    ///
    /// Throws S3Exception: IncompleteBody for malformed or truncated framing,
    /// SignatureDoesNotMatch for a chunk whose signature does not verify.
    void decode(std::string_view body, const std::function<void(std::string_view)>& sink);

    /// Total payload bytes handed to the sink.
    std::uint64_t decodedLength() const noexcept { return decoded_; }

    /// Trailing headers, lowercased, `x-amz-trailer-signature` excluded — that
    /// one is framing rather than content, and it has already been checked by
    /// the time a caller can read this.
    ///
    /// Only populated once decode() has run: a trailer is, by construction, a
    /// fact about the body that is not knowable until the body has been read.
    const std::map<std::string, std::string>& trailers() const noexcept { return trailers_; }

private:
    Options                            options_;
    std::uint64_t                      decoded_ = 0;
    std::map<std::string, std::string> trailers_;
};

/// True when the request declares `aws-chunked` framing on its body.
bool isAwsChunked(std::string_view contentEncoding);

}  // namespace monobucket::s3
