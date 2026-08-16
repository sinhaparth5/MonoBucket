#pragma once

#include <cstddef>
#include <cstdint>

// Protocol- and format-level constants shared across the storage engine, the
// cache layer and the S3 request handlers. Anything that a client can observe
// on the wire belongs here so there is a single place to audit it.

namespace monobucket::limits {

// --- S3 protocol -----------------------------------------------------------

/// Object keys are UTF-8 and capped at 1024 bytes by the S3 specification.
inline constexpr std::size_t kMaxKeyLength = 1024;

/// DNS-compatible bucket naming rules.
inline constexpr std::size_t kMinBucketNameLength = 3;
inline constexpr std::size_t kMaxBucketNameLength = 63;

/// ListObjects/ListObjectsV2 page size when the client does not ask for one.
inline constexpr std::uint32_t kDefaultMaxKeys = 1000;
inline constexpr std::uint32_t kMaxMaxKeys     = 1000;

// --- Multipart upload ------------------------------------------------------

/// Every part except the last must be at least 5 MiB.
inline constexpr std::size_t kMinPartSize = 5ull * 1024 * 1024;

/// Hard ceiling on a single part (5 GiB, as per S3).
inline constexpr std::size_t kMaxPartSize = 5ull * 1024 * 1024 * 1024;

inline constexpr std::uint32_t kMaxPartNumber = 10000;

// --- Streaming -------------------------------------------------------------

/// Read/write granularity for the streaming I/O engine. Fixed-size chunks are
/// what keeps resident memory flat regardless of object size.
inline constexpr std::size_t kDefaultChunkSize = 1ull * 1024 * 1024;

/// SigV4 rejects requests whose timestamp drifts beyond this window.
inline constexpr std::int64_t kSignatureSkewSeconds = 15 * 60;

}  // namespace monobucket::limits

namespace monobucket::headers {

inline constexpr const char* kAmzDate          = "x-amz-date";
inline constexpr const char* kAmzContentSha256 = "x-amz-content-sha256";
inline constexpr const char* kAmzRequestId     = "x-amz-request-id";
inline constexpr const char* kAuthorization    = "authorization";
inline constexpr const char* kETag             = "ETag";

}  // namespace monobucket::headers
