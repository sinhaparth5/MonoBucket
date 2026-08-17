#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "monobucket/constants.hpp"
#include "s3/uri.hpp"

// AWS Signature Version 4.
//
// Everything here is expressed over plain strings rather than over Drogon
// types. That is not incidental: signature verification is the one part of the
// server where a subtle mistake is a security hole rather than a bug, and it is
// only testable against the AWS reference vectors if it does not need a socket.

namespace monobucket::s3 {

inline constexpr const char* kAlgorithm       = "AWS4-HMAC-SHA256";
inline constexpr const char* kChunkAlgorithm  = "AWS4-HMAC-SHA256-PAYLOAD";
inline constexpr const char* kTerminator      = "aws4_request";
inline constexpr const char* kService         = "s3";

inline constexpr const char* kUnsignedPayload = "UNSIGNED-PAYLOAD";
inline constexpr const char* kStreamingSigned = "STREAMING-AWS4-HMAC-SHA256-PAYLOAD";
inline constexpr const char* kStreamingSignedTrailer =
    "STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER";
inline constexpr const char* kStreamingUnsignedTrailer = "STREAMING-UNSIGNED-PAYLOAD-TRAILER";

/// How the body is authenticated, decided by `x-amz-content-sha256`.
enum class PayloadMode {
    /// The header holds the SHA-256 of the whole body, which we verify.
    Signed,
    /// `UNSIGNED-PAYLOAD`, and everything presigned. The body is not covered by
    /// the signature; the URL or the headers still are.
    Unsigned,
    /// `aws-chunked` framing with a signature per chunk, chained from the
    /// request signature.
    StreamingSigned,
    /// `aws-chunked` framing without chunk signatures, with a trailer. Sent by
    /// current AWS SDKs whenever they attach a checksum.
    StreamingUnsigned,
};

/// One HTTP request, reduced to what the signature covers.
struct SigningRequest {
    std::string method;

    /// The path exactly as it arrived, still percent-encoded. S3 signs the URI
    /// it was sent without normalising it, so decoding and re-encoding here
    /// would be a guess at what the client did rather than a fact about it.
    std::string uri;

    /// The query string as it arrived, without the leading `?`.
    std::string query;

    /// Header names lowercased. Order is irrelevant — the canonical form sorts.
    std::vector<std::pair<std::string, std::string>> headers;

    /// Empty when absent; header values are never legitimately empty in the
    /// places this is used.
    std::string_view header(std::string_view name) const;
    bool             hasHeader(std::string_view name) const;
};

/// The six-part string SigV4 hashes. Kept as a struct so tests can compare the
/// parts individually — when a signature mismatches, knowing *which* line
/// differs is the entire debugging problem.
struct CanonicalRequest {
    std::string method;
    std::string uri;
    std::string query;
    std::string headers;        ///< each line `name:value\n`, sorted
    std::string signedHeaders;  ///< `name;name;name`
    std::string payloadHash;

    std::string render() const;
};

struct Credentials {
    std::string accessKey;
    std::string secretKey;
};

struct AuthOptions {
    /// Reported by GetBucketLocation. Not enforced against the credential
    /// scope — see the note in authenticate().
    std::string region = "us-east-1";

    /// Unix seconds. Injected rather than read from the clock so that skew
    /// handling is testable without waiting or faking the system time.
    std::int64_t nowSeconds = 0;

    std::int64_t skewSeconds = limits::kSignatureSkewSeconds;

    /// A second method the signature may have been computed over.
    ///
    /// Drogon rewrites HEAD to GET before routing and offers no way to recover
    /// the original, but the client signed the method it actually sent. Trying
    /// both is not a weakening: a signature is still required to verify, and
    /// which one verifies is what tells us the method. AuthOutcome::method
    /// reports the answer.
    std::string alternateMethod;
};

struct AuthOutcome {
    /// No credentials were presented at all. The caller decides whether the
    /// operation is one an anonymous client may perform.
    bool anonymous = false;

    /// True when the credentials came from the query string rather than the
    /// Authorization header.
    bool presigned = false;

    std::string accessKey;

    /// The method the signature verified under — the request's own unless the
    /// alternate was the one that matched. Empty for an anonymous request,
    /// where there is no signature to ask.
    std::string method;

    PayloadMode payload = PayloadMode::Unsigned;

    /// Set for PayloadMode::Signed: the digest the body must hash to.
    std::string payloadSha256;

    /// `20260816/us-east-1/s3/aws4_request`.
    std::string scope;

    /// `20260816T120000Z`, as signed.
    std::string amzDate;

    /// The request signature, which seeds the chunk signature chain.
    std::string signature;

    /// Raw 32-byte derived key, kept so chunk signatures can be verified
    /// without the secret being passed any further down.
    std::string signingKey;
};

// --- Primitives ------------------------------------------------------------

/// Raw 32-byte HMAC-SHA256. Returned as a std::string because it is a key for
/// the next round, not text.
std::string hmacSha256(std::string_view key, std::string_view data);

/// `AWS4<secret>` folded through date, region, service and terminator.
std::string deriveSigningKey(std::string_view secret, std::string_view dateStamp,
                             std::string_view region, std::string_view service);

/// `AWS4-HMAC-SHA256\n<amzDate>\n<scope>\n<hex sha256 of the canonical request>`.
std::string stringToSign(std::string_view amzDate, std::string_view scope,
                         std::string_view canonicalRequestHash);

/// Builds the canonical request for the given signed-header list. Throws
/// S3Exception(SignatureDoesNotMatch) when a header the client claims to have
/// signed is not actually present, which is otherwise an opaque mismatch.
CanonicalRequest buildCanonicalRequest(const SigningRequest& request,
                                       const std::vector<std::string>& signedHeaders,
                                       std::string_view payloadHash);

/// Parses `20260816T120000Z` to Unix seconds. Nullopt on anything else,
/// including a syntactically valid but impossible date.
std::optional<std::int64_t> parseAmzDate(std::string_view text);

/// Renders Unix seconds as `20260816T120000Z`.
std::string formatAmzDate(std::int64_t seconds);

/// Constant-time comparison. A signature check that returns early on the first
/// differing byte leaks the correct prefix to anyone willing to time it.
bool secureEquals(std::string_view a, std::string_view b) noexcept;

// --- Verification ----------------------------------------------------------

/// Verifies a request against a single credential pair.
///
/// The scope's region is used as the client sent it rather than being forced to
/// match our own. The signature is computed over that region, so accepting it
/// gives an attacker nothing — while rejecting it turns the very common case of
/// a client configured for a different region into a signature failure nobody
/// can diagnose from the error text.
///
/// Throws S3Exception on every failure. Returns `anonymous` rather than
/// throwing when no credentials were presented.
AuthOutcome authenticate(const SigningRequest& request, const std::vector<QueryParam>& query,
                         const Credentials& credentials, const AuthOptions& options);

// --- Presigning ------------------------------------------------------------

/// What a presigned URL is minted from.
struct PresignRequest {
    /// A presigned GET does not authorise a PUT: the method is inside the
    /// signature, so one URL grants exactly one verb.
    std::string method = "GET";

    /// The authority the client will send as `Host`, port included unless it is
    /// the scheme default. It is signed, so a URL minted for one endpoint does
    /// not carry to another.
    std::string host;

    /// The path, already percent-encoded by the caller — the same bytes that
    /// will travel on the wire, because those are the bytes that get signed.
    std::string uri;

    std::string region = "us-east-1";

    /// Unix seconds. Injected rather than read from the clock so the output is
    /// reproducible in a test.
    std::int64_t nowSeconds = 0;

    /// S3's own bounds, 1 to 604800. Checked rather than clamped: a caller
    /// asking for eight days wants something we cannot give, and quietly
    /// handing back seven would be discovered when the link died early.
    std::int64_t expiresSeconds = 3600;
};

/// Builds the query string of a presigned URL, `X-Amz-Signature` included and
/// ready to follow a `?`.
///
/// It shares buildCanonicalRequest() and deriveSigningKey() with authenticate()
/// deliberately. A generator that reimplements the canonical form drifts from
/// the verifier, and the symptom is a 403 that says nothing about which of the
/// two sides is wrong.
///
/// Throws S3Exception on an out-of-range lifetime or a missing host.
std::string presignQuery(const PresignRequest& request, const Credentials& credentials);

/// The signature for one `aws-chunked` chunk, chained from the previous one.
/// The first call passes the request signature as `previousSignature`.
std::string chunkSignature(std::string_view signingKey, std::string_view amzDate,
                           std::string_view scope, std::string_view previousSignature,
                           std::string_view chunkSha256Hex);

/// The signature covering the trailing header block of a chunked upload.
std::string trailerSignature(std::string_view signingKey, std::string_view amzDate,
                             std::string_view scope, std::string_view previousSignature,
                             std::string_view trailerSha256Hex);

}  // namespace monobucket::s3
