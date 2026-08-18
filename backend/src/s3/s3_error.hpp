#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "storage/metadata_store.hpp"

// S3 error responses.
//
// The error code string is part of the protocol: clients branch on it — the
// AWS CLI retries `SlowDown`, boto3 raises a distinct exception per code, and
// `rclone` treats `NoSuchKey` differently from `AccessDenied`. So the code and
// its HTTP status travel together in one table and are never assembled at the
// call site.

namespace monobucket::s3 {

enum class S3ErrorCode {
    AccessDenied,
    AccessForbidden,
    BadDigest,
    BucketAlreadyExists,
    BucketAlreadyOwnedByYou,
    BucketNotEmpty,
    EntityTooLarge,
    EntityTooSmall,
    IncompleteBody,
    InternalError,
    InvalidAccessKeyId,
    InvalidArgument,
    InvalidBucketName,
    InvalidDigest,
    InvalidPart,
    InvalidPartOrder,
    InvalidRange,
    InvalidRequest,
    InvalidURI,
    InsufficientCapacity,
    KeyTooLong,
    MalformedXML,
    MethodNotAllowed,
    MissingContentLength,
    MissingSecurityHeader,
    NoSuchBucket,
    NoSuchBucketPolicy,
    NoSuchCORSConfiguration,
    NoSuchKey,
    NoSuchUpload,
    NotImplemented,
    PreconditionFailed,
    QuotaExceeded,
    RequestTimeTooSkewed,
    SignatureDoesNotMatch,
    SlowDown,
    XAmzContentSHA256Mismatch,
};

struct S3ErrorInfo {
    /// The `<Code>` element, which is what clients match on.
    const char* code;
    int         status;
    /// Default `<Message>`. Callers may substitute a more specific one; no
    /// client parses the message, but a human reading a log will.
    const char* message;
};

const S3ErrorInfo& describe(S3ErrorCode code) noexcept;

/// Thrown by handler code and turned into a response at the router boundary,
/// so that no handler has to remember to build an error document.
class S3Exception : public std::runtime_error {
public:
    explicit S3Exception(S3ErrorCode code)
        : std::runtime_error(describe(code).message), code_(code) {}

    S3Exception(S3ErrorCode code, std::string message)
        : std::runtime_error(message), code_(code) {}

    S3ErrorCode code() const noexcept { return code_; }

private:
    S3ErrorCode code_;
};

/// The S3 error document. `resource` is the request path, which clients echo
/// back in their own error messages, and `requestId` is what a bug report
/// should quote.
std::string renderError(S3ErrorCode code, std::string_view message, std::string_view resource,
                        std::string_view requestId);

/// The mapping from a storage-layer condition to the code a client expects.
/// Kept in one place so a new storage code cannot silently become a 500.
S3ErrorCode fromStorage(StorageErrorCode code) noexcept;

/// 16 hex characters, in the shape of an AWS request id. Random rather than
/// sequential: a counter would leak the request rate to anyone who makes two
/// requests.
std::string newRequestId();

}  // namespace monobucket::s3
