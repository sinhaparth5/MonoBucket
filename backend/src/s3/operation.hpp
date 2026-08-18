#pragma once

#include <string_view>

#include "core/identity.hpp"
#include "s3/request.hpp"

// Which S3 operation a request names.
//
// S3 does not route on the path alone: `GET /bucket`, `GET /bucket?uploads` and
// `GET /bucket?list-type=2` are three different operations, and `POST
// /bucket/key?uploadId=x` differs from `POST /bucket?delete` only by
// subresource. Doing this as one pure function keeps the whole routing table in
// a place that can be tested exhaustively, instead of spread across handlers.

namespace monobucket::s3 {

enum class Operation {
    // Service
    ListBuckets,

    // Bucket
    CreateBucket,
    DeleteBucket,
    HeadBucket,
    ListObjectsV1,
    ListObjectsV2,
    ListMultipartUploads,
    DeleteObjects,
    GetBucketLocation,
    GetBucketVersioning,
    GetBucketPolicy,
    PutBucketPolicy,
    DeleteBucketPolicy,
    GetBucketAcl,
    PutBucketAcl,
    GetBucketCors,
    PutBucketCors,
    DeleteBucketCors,

    /// A CORS preflight. Not a bucket operation in S3's sense — it is answered
    /// from the bucket's rules without authenticating the caller, because a
    /// browser never signs one.
    Preflight,

    // Object
    GetObject,
    HeadObject,
    PutObject,
    DeleteObject,

    // Multipart
    CreateMultipartUpload,
    UploadPart,
    ListParts,
    CompleteMultipartUpload,
    AbortMultipartUpload,

    /// A well-formed S3 request for something this server does not implement.
    /// Answered with 501 and the subresource named, which is far easier to act
    /// on than a 400.
    Unsupported,

    /// The method is not one this resource accepts at all.
    MethodNotAllowed,
};

std::string_view toString(Operation operation) noexcept;

/// True for the operations that only read. Used by the public-bucket path,
/// which must never let an anonymous client mutate anything.
bool isReadOnly(Operation operation) noexcept;

/// The permission a signed request must hold to perform this operation.
///
/// Deliberately coarse: an S3 access key acts as a person, and a person who may
/// delete an object may delete any object. Per-bucket scoping would be a second
/// policy system beside the bucket policies already here, and the two would
/// have to agree about which one denies — see "Known limitations" in README.md.
///
/// Anonymous requests never reach this. Whether an unsigned request may be
/// served is the bucket's own question, answered by authorize() before any
/// identity exists to ask about.
Permission permissionFor(Operation operation) noexcept;

/// Classifies a parsed request. `unsupported` is filled with the subresource
/// that could not be served, for the error message.
Operation classify(const S3Request& request, std::string& unsupported);

}  // namespace monobucket::s3
