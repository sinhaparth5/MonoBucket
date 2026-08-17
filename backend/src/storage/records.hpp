#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "storage/codec.hpp"

// The metadata schema, as plain structs. These cross the boundary between the
// storage engine and the S3 protocol layer, so field names track S3 vocabulary
// rather than RocksDB vocabulary.

namespace monobucket {

/// Milliseconds since the Unix epoch. S3 renders timestamps to second
/// precision, but multipart part ordering and the dashboard both benefit from
/// keeping the finer resolution we already have.
using TimestampMs = std::int64_t;

TimestampMs nowMs() noexcept;

/// Renders a timestamp as ISO-8601 in UTC, the format S3 responses use.
std::string toIso8601(TimestampMs ms);

/// User-supplied `x-amz-meta-*` headers, minus the prefix. Ordered so that a
/// round-trip through the store is byte-stable, which keeps ETags of the
/// metadata itself comparable and makes tests deterministic.
using UserMetadata = std::map<std::string, std::string>;

/// One rule of a bucket's CORS configuration.
///
/// Stored decomposed rather than as the XML it arrived in: a rule is consulted
/// on every cross-origin request, and re-parsing a document to answer a header
/// question would put an XML parse on the read path. GetBucketCors renders it
/// back, which is also what makes the round trip normalising rather than
/// byte-preserving — as S3's own is.
struct CorsRule {
    /// Optional client-supplied label, echoed back untouched.
    std::string id;

    /// Each entry is an exact origin or a pattern with at most one `*`.
    std::vector<std::string> allowedOrigins;

    /// Uppercased. S3 accepts only GET, PUT, POST, DELETE and HEAD here.
    std::vector<std::string> allowedMethods;

    /// Lowercased, because the header names they are matched against are
    /// case-insensitive and the comparison should not have to keep saying so.
    std::vector<std::string> allowedHeaders;

    /// Response headers a browser script is allowed to read. Without these a
    /// cross-origin fetch can see the body but not the ETag.
    std::vector<std::string> exposeHeaders;

    /// Negative means the rule sets no max age and the browser picks.
    std::int32_t maxAgeSeconds = -1;
};

/// The CORS rules' wire form, shared by the RocksDB bucket record and the
/// request-path bucket cache. One encoding rather than two: the cache is a
/// second copy of the same record, and a cached bucket that had quietly lost
/// its rules would answer preflights differently depending on whether the last
/// write had aged out.
void                  encodeCorsRules(codec::Writer& writer, const std::vector<CorsRule>& rules);
std::vector<CorsRule> decodeCorsRules(codec::Reader& reader);

struct BucketRecord {
    std::string name;
    TimestampMs createdAt = 0;

    /// Anonymous GETs are allowed when set. Phase 4 reads this on the
    /// unauthenticated path; a full policy document lands with bucket policies.
    bool publicRead = false;

    /// Reserved for the Phase 4 policy editor. Empty means "no policy".
    std::string policy;

    /// Empty means CORS is not enabled, which is not the same as enabled with
    /// no rules: S3 answers a preflight against the former with 403 and the
    /// latter cannot occur, because an empty CORSConfiguration is refused.
    std::vector<CorsRule> cors;
};

struct ObjectRecord {
    std::string key;

    /// Identifies the payload in the blob store. An object row and its blob are
    /// written in that order and torn down in the reverse, which is what makes
    /// a half-finished PUT invisible rather than corrupt.
    std::string blobId;

    std::uint64_t size = 0;

    /// MD5 hex for a single-part object, `<hex>-<parts>` for a completed
    /// multipart upload. Returned verbatim in the ETag header.
    std::string etag;

    /// Full-payload SHA-256. Lets a future fsck verify integrity without
    /// trusting the filesystem, and satisfies clients that ask for it back.
    std::string sha256;

    std::string  contentType = "application/octet-stream";
    TimestampMs  lastModified = 0;
    UserMetadata userMetadata;
};

struct PartRecord {
    std::uint32_t partNumber = 0;
    std::string   blobId;
    std::uint64_t size = 0;
    std::string   etag;  ///< MD5 hex of this part alone
    TimestampMs   uploadedAt = 0;
};

struct UploadRecord {
    std::string  uploadId;
    std::string  bucket;
    std::string  key;
    std::string  contentType = "application/octet-stream";
    TimestampMs  createdAt   = 0;
    UserMetadata userMetadata;
};

/// One page of ListObjects/ListObjectsV2.
struct ListObjectsRequest {
    std::string prefix;
    std::string delimiter;

    /// Exclusive lower bound. Serves both `start-after` (V2) and `marker` (V1),
    /// and is what a continuation token resolves to.
    std::string startAfter;

    std::uint32_t maxKeys = 1000;
};

struct ListObjectsResult {
    std::vector<ObjectRecord> objects;

    /// Keys rolled up by the delimiter, each ending with the delimiter itself.
    std::vector<std::string> commonPrefixes;

    bool truncated = false;

    /// The `startAfter` that resumes this listing. Only meaningful when
    /// truncated; a common prefix can be the resumption point, not just a key.
    std::string nextStartAfter;
};

/// Aggregate counters for `/metrics` and the dashboard's capacity panel.
struct UsageStats {
    std::uint64_t buckets      = 0;
    std::uint64_t objects      = 0;
    std::uint64_t bytes        = 0;
    std::uint64_t uploads      = 0;  ///< multipart uploads currently in progress
    std::uint64_t orphanBlobs  = 0;  ///< payloads awaiting reclamation
};

}  // namespace monobucket
