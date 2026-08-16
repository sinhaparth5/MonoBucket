#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "s3/uri.hpp"

// Turning an HTTP request into the (bucket, key, subresource) triple the S3
// operations are defined over, plus the header parsing several of them share.

namespace monobucket::s3 {

/// The addressed resource. An empty bucket means the service root; an empty key
/// with a non-empty bucket means the bucket itself.
struct S3Request {
    std::string             method;
    std::string             bucket;
    std::string             key;
    std::vector<QueryParam> query;

    /// The path as received, used as `<Resource>` in an error document.
    std::string resource;

    std::string requestId;

    bool hasQuery(std::string_view name) const { return s3::hasQuery(query, name); }

    std::optional<std::string> queryValue(std::string_view name) const {
        return findQuery(query, name);
    }

    /// Query value or a fallback, for the many optional list parameters.
    std::string queryOr(std::string_view name, std::string fallback = {}) const {
        auto value = findQuery(query, name);
        return value ? *value : std::move(fallback);
    }
};

/// Splits the decoded path into bucket and key.
///
/// `virtualHostBucket` is non-empty when the bucket came from the Host header
/// instead, in which case the whole path is the key.
S3Request parseRequest(std::string_view method, std::string_view decodedPath,
                       std::string_view rawQuery, std::string_view virtualHostBucket);

/// The bucket named by a virtual-host style request, or empty for path style.
///
/// `domain` is the configured S3 endpoint domain. Without one this always
/// returns empty: guessing that the first label of an arbitrary Host header is
/// a bucket name would make `Host: localhost` address a bucket called
/// "localhost", and there is no way to tell that apart from the real thing.
std::string virtualHostBucket(std::string_view host, std::string_view domain);

// --- Validation ------------------------------------------------------------

/// The DNS-compatible subset S3 enforces for new buckets: 3–63 characters,
/// lowercase letters, digits, hyphens and dots; must start and end
/// alphanumeric; no consecutive dots; not formatted as an IPv4 address.
bool isValidBucketName(std::string_view name);

/// Paths the server answers itself, which therefore cannot address a bucket.
/// Creating one would produce a bucket that no S3 client could ever reach, so
/// the attempt is refused with an explanation instead.
bool isReservedBucketName(std::string_view name);

/// Non-empty, at most 1024 bytes, and free of the control characters that
/// cannot be represented in an XML listing.
bool isValidObjectKey(std::string_view key);

// --- Header parsing --------------------------------------------------------

struct ByteRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

enum class RangeResult {
    /// No Range header, or one in a unit we do not implement. S3 ignores those
    /// and returns the whole object rather than failing.
    Absent,
    Satisfiable,
    /// Syntactically valid but outside the object: 416 with a Content-Range.
    Unsatisfiable,
};

/// Parses a single-range `bytes=` header against a known object size.
///
/// Multiple ranges are deliberately treated as Absent: S3 itself returns the
/// whole object for them, and answering with a multipart/byteranges document
/// would be a different response shape than any S3 client expects.
RangeResult parseRange(std::string_view header, std::uint64_t objectSize, ByteRange& out);

/// RFC 7231 IMF-fixdate, the format Last-Modified uses.
std::string toHttpDate(std::int64_t epochMs);

/// Parses the three date formats HTTP permits, for If-Modified-Since. Nullopt
/// when unparseable — a malformed conditional header is ignored, not fatal.
std::optional<std::int64_t> parseHttpDate(std::string_view text);

/// Strips the quotes an ETag carries on the wire, so a client that sends
/// `"abc"` and one that sends `abc` compare equal.
std::string_view unquoteETag(std::string_view etag);

/// Wraps an ETag in the quotes S3 sends. Multipart ETags are quoted too.
std::string quoteETag(std::string_view etag);

/// Decodes a `x-amz-meta-*` header name to the metadata key, or empty when the
/// name does not carry the prefix.
std::string userMetadataKey(std::string_view headerName);

/// Applies `encoding-type=url` to a key in a listing. S3 only ever url-encodes
/// when asked, because doing it unconditionally would break every client that
/// does not decode.
std::string encodeListingValue(std::string_view value, bool urlEncode);

}  // namespace monobucket::s3
