#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "s3/checksum.hpp"
#include "s3/metrics.hpp"
#include "s3/operation.hpp"
#include "s3/request.hpp"
#include "s3/sigv4.hpp"
#include "storage/records.hpp"

namespace monobucket {
struct Config;
class CacheProvider;
class StorageEngine;
}  // namespace monobucket

namespace monobucket::s3 {

/// Everything a handler is allowed to touch. Passed by reference and never
/// owned: the server outlives every request, and a handler that needed to
/// extend the lifetime of the storage engine would be a design error.
struct S3Context {
    const Config&  config;
    StorageEngine& storage;
    CacheProvider& cache;
    S3Metrics&     metrics;
};

/// The request body, with the framing and verification the signature demands.
///
/// Bodies arrive whole: Drogon spills anything above
/// MONOBUCKET_MAX_MEMORY_BODY_BYTES to a file and hands back a mapping of it,
/// so `raw` is bounded in heap terms regardless of the object size. Nothing
/// here copies it.
class S3Body {
public:
    S3Body(std::string_view raw, const AuthOutcome& auth, std::string_view contentEncoding,
           std::uint64_t decodedLength);

    /// Verifies the payload against the signature and hands the decoded bytes
    /// to `sink`, which may be called more than once. Throws S3Exception on a
    /// digest or chunk-signature mismatch, before `sink` sees anything that
    /// failed to verify.
    void streamTo(const std::function<void(std::string_view)>& sink) const;

    /// The decoded body as one string. Only for the small XML documents —
    /// CompleteMultipartUpload and DeleteObjects — never for object payloads.
    std::string materialise() const;

    std::string_view raw() const noexcept { return raw_; }

    /// How many bytes the payload decodes to: what the client declared for a
    /// chunked body, and what arrived for a plain one. What a storage
    /// allocation is checked against before the body is written anywhere.
    std::uint64_t decodedLength() const noexcept {
        return chunked_ ? decodedLength_ : raw_.size();
    }

    /// The trailing headers an aws-chunked body ended with, lowercased. Empty
    /// until streamTo() has run, and empty for a body that carried none.
    ///
    /// This is where a modern SDK puts its checksum, which is the whole reason
    /// verification cannot happen before the payload is read: the expected
    /// value arrives after the bytes it describes.
    const std::map<std::string, std::string>& trailers() const noexcept { return trailers_; }

private:
    std::string_view raw_;
    const AuthOutcome& auth_;
    bool          chunked_ = false;
    std::uint64_t decodedLength_ = 0;

    /// Mutable because streamTo() is logically const — it does not change the
    /// body — while the trailers are a fact about that body which only decoding
    /// it can reveal.
    mutable std::map<std::string, std::string> trailers_;
};

/// Resolves the bucket, throwing NoSuchBucket rather than returning nullopt:
/// every handler that needs one needs it to exist.
BucketRecord requireBucket(const S3Context& context, std::string_view name);

/// Object metadata, through the cache. A hit avoids a RocksDB lookup on the
/// hottest path in the server, which is what the Phase 3 cache was built for.
std::optional<ObjectRecord> statObject(const S3Context& context, std::string_view bucket,
                                       std::string_view key);

/// Drops the cached metadata for a key. Called on every mutation, before the
/// response is sent, so a single instance is always read-after-write consistent
/// with itself.
void invalidateObject(const S3Context& context, std::string_view bucket, std::string_view key);

/// Drops everything cached under a bucket. A bucket only disappears when it is
/// already empty, so this exists for the bucket record itself.
void invalidateBucket(const S3Context& context, std::string_view name);

/// The keys a bucket record and an object record are cached under. Exposed
/// because the console mutates both without an S3Context, and a stale entry
/// here is what would keep the S3 read path answering from state the console
/// has just replaced.
std::string bucketCacheKey(std::string_view name);
std::string objectCacheKey(std::string_view bucket, std::string_view key);

// --- Handlers --------------------------------------------------------------
//
// Each returns a complete response and runs on an I/O thread, never on the
// event loop: every one of them touches RocksDB or the filesystem.

drogon::HttpResponsePtr handleListBuckets(const S3Context&, const S3Request&);
drogon::HttpResponsePtr handleCreateBucket(const S3Context&, const S3Request&);
drogon::HttpResponsePtr handleDeleteBucket(const S3Context&, const S3Request&);
drogon::HttpResponsePtr handleHeadBucket(const S3Context&, const S3Request&);
drogon::HttpResponsePtr handleListObjects(const S3Context&, const S3Request&, bool version2);
drogon::HttpResponsePtr handleGetBucketLocation(const S3Context&, const S3Request&);
drogon::HttpResponsePtr handleGetBucketVersioning(const S3Context&, const S3Request&);
drogon::HttpResponsePtr handleGetBucketAcl(const S3Context&, const S3Request&);
drogon::HttpResponsePtr handlePutBucketAcl(const S3Context&, const S3Request&,
                                           const drogon::HttpRequestPtr&, const S3Body&);
drogon::HttpResponsePtr handleGetBucketPolicy(const S3Context&, const S3Request&);
drogon::HttpResponsePtr handlePutBucketPolicy(const S3Context&, const S3Request&, const S3Body&);
drogon::HttpResponsePtr handleDeleteBucketPolicy(const S3Context&, const S3Request&);

// Policy reading and validation now live in `s3/bucket_policy.hpp`. The console
// writes policies too, and two readings of the same document would eventually
// be two different answers to "is this bucket public".
drogon::HttpResponsePtr handleDeleteObjects(const S3Context&, const S3Request&,
                                            const drogon::HttpRequestPtr&, const S3Body&);

// CORS. The matching and the document schema live in `s3/cors.hpp`, which knows
// nothing about Drogon; only these four need a request and a response.
drogon::HttpResponsePtr handleGetBucketCors(const S3Context&, const S3Request&);
drogon::HttpResponsePtr handlePutBucketCors(const S3Context&, const S3Request&, const S3Body&);
drogon::HttpResponsePtr handleDeleteBucketCors(const S3Context&, const S3Request&);

/// Answers an `OPTIONS` preflight.
///
/// Runs before authentication and must: a browser never attaches credentials to
/// a preflight, so requiring a signature here would refuse every cross-origin
/// request to a private bucket before the real, signed one was ever sent.
drogon::HttpResponsePtr handlePreflight(const S3Context&, const S3Request&,
                                        const drogon::HttpRequestPtr&);

/// Adds the `Access-Control-*` headers to a response that has already been
/// built. Does nothing when the request carried no Origin, when the bucket has
/// no CORS rules, or when none of them match.
void applyCorsHeaders(const S3Context&, const S3Request&, const drogon::HttpRequestPtr&,
                      const drogon::HttpResponsePtr&);

/// Serves HeadObject as well: Drogon rewrites HEAD to GET before routing, and
/// suppresses the body when it writes the response, so the two operations share
/// one implementation whether we want them to or not.
drogon::HttpResponsePtr handleGetObject(const S3Context&, const S3Request&,
                                        const drogon::HttpRequestPtr&);
drogon::HttpResponsePtr handlePutObject(const S3Context&, const S3Request&,
                                        const drogon::HttpRequestPtr&, const S3Body&);
drogon::HttpResponsePtr handleDeleteObject(const S3Context&, const S3Request&);

drogon::HttpResponsePtr handleCreateMultipartUpload(const S3Context&, const S3Request&,
                                                    const drogon::HttpRequestPtr&);
drogon::HttpResponsePtr handleUploadPart(const S3Context&, const S3Request&,
                                         const drogon::HttpRequestPtr&, const S3Body&);
drogon::HttpResponsePtr handleListParts(const S3Context&, const S3Request&);
drogon::HttpResponsePtr handleCompleteMultipartUpload(const S3Context&, const S3Request&,
                                                      const drogon::HttpRequestPtr&,
                                                      const S3Body&);
drogon::HttpResponsePtr handleAbortMultipartUpload(const S3Context&, const S3Request&);
drogon::HttpResponsePtr handleListMultipartUploads(const S3Context&, const S3Request&);

/// Collects `x-amz-meta-*` into the metadata map, rejecting a set that is too
/// large to store. S3 caps user metadata at 2 KB of headers.
UserMetadata collectUserMetadata(const drogon::HttpRequestPtr& request);

/// The content type to store, defaulting the way S3 does.
std::string contentTypeOf(const drogon::HttpRequestPtr& request);

/// Verifies a Content-MD5 header when the client sent one. Throws BadDigest on
/// a mismatch and InvalidDigest when the header is not base64 of 16 bytes.
///
/// Kept working alongside the x-amz-checksum family rather than superseded by
/// it. A client that sends both is asking for both to be checked, and the older
/// header is still the only integrity check some tooling knows how to send.
void verifyContentMd5(const drogon::HttpRequestPtr& request, std::string_view payload);

/// The checksum-bearing headers of one request, lowercased — what
/// resolveChecksumRequest() reads. Only those headers are copied; the rules
/// need to enumerate them, and enumerating the whole table per request would
/// cost more than the checksum does.
ChecksumHeaders checksumHeaders(const drogon::HttpRequestPtr& request);

/// Streams the body through `sink` while computing every integrity check the
/// request asked for over the same bytes, then verifies them and returns the
/// checksum to store.
///
/// Both families are checked here: the `x-amz-checksum-*` value in `wanted`,
/// and `Content-MD5` when the request carries one. A client that sends both is
/// asking for both, and the older header is still the only integrity check some
/// tooling knows how to send.
///
/// The sink is wrapped rather than each caller being asked to remember to
/// update a digest of its own: a checksum taken over a subset of the bytes that
/// were written is worse than no checksum, and this is the only place the two
/// can be kept in step by construction.
///
/// Throws BadDigest on a mismatch, before anything is committed. Returns an
/// empty Checksum when the request named none, which is an ordinary upload.
Checksum verifiedChecksum(const drogon::HttpRequestPtr& request, const ChecksumRequest& wanted,
                          const S3Body& body, const std::function<void(std::string_view)>& sink);

/// Adds `x-amz-checksum-<alg>` when there is one to report. Every write path
/// answers with what it stored, so a client can compare without reading the
/// object back.
void applyChecksumHeader(const drogon::HttpResponsePtr& response, const Checksum& checksum);

}  // namespace monobucket::s3
