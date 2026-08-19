#include "s3/s3_error.hpp"

#include <array>
#include <random>

#include "s3/xml.hpp"

namespace monobucket::s3 {
namespace {

// Status codes follow the S3 error reference. The ones worth noting because
// they are not what a first guess would produce:
//   BucketAlreadyOwnedByYou is 409, not 200 — outside us-east-1, where S3 makes
//   CreateBucket idempotent for the owner and answers 200.
//   InvalidRange is 416, and must still carry an S3 error document.
//   SlowDown is 503, which is what makes the AWS CLI back off and retry rather
//   than fail the transfer.
constexpr S3ErrorInfo kUnknown{"InternalError", 500, "We encountered an internal error."};

const S3ErrorInfo& lookup(S3ErrorCode code) noexcept {
    switch (code) {
        case S3ErrorCode::AccessDenied: {
            static constexpr S3ErrorInfo info{"AccessDenied", 403, "Access Denied"};
            return info;
        }
        case S3ErrorCode::AccessForbidden: {
            // The code S3 uses for a preflight that no CORS rule permits. It is
            // distinct from AccessDenied on purpose: nothing about the caller's
            // credentials is wrong, and telling them to re-sign would send them
            // looking in the wrong place.
            static constexpr S3ErrorInfo info{
                "AccessForbidden", 403, "CORSResponse: This CORS request is not allowed."};
            return info;
        }
        case S3ErrorCode::BadDigest: {
            static constexpr S3ErrorInfo info{
                "BadDigest", 400,
                "The Content-MD5 you specified did not match what we received."};
            return info;
        }
        case S3ErrorCode::BucketAlreadyExists: {
            static constexpr S3ErrorInfo info{
                "BucketAlreadyExists", 409,
                "The requested bucket name is not available."};
            return info;
        }
        case S3ErrorCode::BucketAlreadyOwnedByYou: {
            static constexpr S3ErrorInfo info{
                "BucketAlreadyOwnedByYou", 409,
                "Your previous request to create the named bucket succeeded and you already "
                "own it."};
            return info;
        }
        case S3ErrorCode::BucketNotEmpty: {
            static constexpr S3ErrorInfo info{"BucketNotEmpty", 409,
                                              "The bucket you tried to delete is not empty."};
            return info;
        }
        case S3ErrorCode::EntityTooLarge: {
            static constexpr S3ErrorInfo info{
                "EntityTooLarge", 400, "Your proposed upload exceeds the maximum allowed size."};
            return info;
        }
        case S3ErrorCode::EntityTooSmall: {
            static constexpr S3ErrorInfo info{
                "EntityTooSmall", 400,
                "Your proposed upload is smaller than the minimum allowed object size."};
            return info;
        }
        case S3ErrorCode::IncompleteBody: {
            static constexpr S3ErrorInfo info{
                "IncompleteBody", 400,
                "You did not provide the number of bytes specified by the Content-Length HTTP "
                "header."};
            return info;
        }
        case S3ErrorCode::InternalError:
            return kUnknown;
        case S3ErrorCode::InvalidAccessKeyId: {
            static constexpr S3ErrorInfo info{
                "InvalidAccessKeyId", 403,
                "The AWS access key id you provided does not exist in our records."};
            return info;
        }
        case S3ErrorCode::InvalidArgument: {
            static constexpr S3ErrorInfo info{"InvalidArgument", 400, "Invalid Argument"};
            return info;
        }
        case S3ErrorCode::InvalidBucketName: {
            static constexpr S3ErrorInfo info{
                "InvalidBucketName", 400, "The specified bucket is not valid."};
            return info;
        }
        case S3ErrorCode::InvalidDigest: {
            static constexpr S3ErrorInfo info{
                "InvalidDigest", 400, "The Content-MD5 you specified is not valid."};
            return info;
        }
        case S3ErrorCode::InvalidPart: {
            static constexpr S3ErrorInfo info{
                "InvalidPart", 400,
                "One or more of the specified parts could not be found, or the entity tag did "
                "not match the part's entity tag."};
            return info;
        }
        case S3ErrorCode::InvalidPartOrder: {
            static constexpr S3ErrorInfo info{
                "InvalidPartOrder", 400,
                "The list of parts was not in ascending order. Parts must be ordered by part "
                "number."};
            return info;
        }
        case S3ErrorCode::InsufficientCapacity: {
            // 507, which is not an S3 code — S3 has no notion of an instance
            // running out of capacity to allocate, because AWS does not run
            // out. It is deliberately distinct from QuotaExceeded: one says
            // this bucket is full, the other says the server has nothing left
            // to give a new bucket, and the operator's next step differs.
            static constexpr S3ErrorInfo info{
                "InsufficientCapacity", 507,
                "The server has no remaining allocatable capacity for this bucket."};
            return info;
        }
        case S3ErrorCode::InvalidRange: {
            static constexpr S3ErrorInfo info{
                "InvalidRange", 416, "The requested range is not satisfiable."};
            return info;
        }
        case S3ErrorCode::InvalidRequest: {
            static constexpr S3ErrorInfo info{"InvalidRequest", 400, "Invalid Request"};
            return info;
        }
        case S3ErrorCode::InvalidURI: {
            static constexpr S3ErrorInfo info{"InvalidURI", 400, "Could not parse the specified URI."};
            return info;
        }
        case S3ErrorCode::KeyTooLong: {
            // The wire code carries the "Error" suffix. It is not a typo, and
            // clients match on it literally.
            static constexpr S3ErrorInfo info{"KeyTooLongError", 400,
                                              "Your key is too long."};
            return info;
        }
        case S3ErrorCode::MalformedXML: {
            static constexpr S3ErrorInfo info{
                "MalformedXML", 400,
                "The XML you provided was not well-formed or did not validate against our "
                "published schema."};
            return info;
        }
        case S3ErrorCode::MethodNotAllowed: {
            static constexpr S3ErrorInfo info{
                "MethodNotAllowed", 405,
                "The specified method is not allowed against this resource."};
            return info;
        }
        case S3ErrorCode::MissingContentLength: {
            static constexpr S3ErrorInfo info{
                "MissingContentLength", 411,
                "You must provide the Content-Length HTTP header."};
            return info;
        }
        case S3ErrorCode::MissingSecurityHeader: {
            static constexpr S3ErrorInfo info{
                "MissingSecurityHeader", 400, "Your request is missing a required header."};
            return info;
        }
        case S3ErrorCode::NoSuchBucket: {
            static constexpr S3ErrorInfo info{"NoSuchBucket", 404,
                                              "The specified bucket does not exist."};
            return info;
        }
        case S3ErrorCode::NoSuchBucketPolicy: {
            static constexpr S3ErrorInfo info{
                "NoSuchBucketPolicy", 404, "The bucket policy does not exist."};
            return info;
        }
        case S3ErrorCode::NoSuchCORSConfiguration: {
            static constexpr S3ErrorInfo info{
                "NoSuchCORSConfiguration", 404,
                "The CORS configuration does not exist."};
            return info;
        }
        case S3ErrorCode::NoSuchKey: {
            static constexpr S3ErrorInfo info{"NoSuchKey", 404,
                                              "The specified key does not exist."};
            return info;
        }
        case S3ErrorCode::NoSuchUpload: {
            static constexpr S3ErrorInfo info{
                "NoSuchUpload", 404,
                "The specified multipart upload does not exist. The upload id may be invalid, "
                "or the upload may have been aborted or completed."};
            return info;
        }
        case S3ErrorCode::NotImplemented: {
            static constexpr S3ErrorInfo info{
                "NotImplemented", 501,
                "A header or query parameter you provided implies functionality that is not "
                "implemented."};
            return info;
        }
        case S3ErrorCode::PreconditionFailed: {
            static constexpr S3ErrorInfo info{
                "PreconditionFailed", 412,
                "At least one of the preconditions you specified did not hold."};
            return info;
        }
        case S3ErrorCode::QuotaExceeded: {
            // 403 rather than 507, matching what other S3-compatible servers
            // return for a bucket over its quota. It is terminal: a client
            // that retries will be refused again, and the AWS CLI treats 403
            // as a reason to stop rather than to back off.
            static constexpr S3ErrorInfo info{
                "QuotaExceeded", 403,
                "The write exceeds the storage allocation of the specified bucket."};
            return info;
        }
        case S3ErrorCode::RequestTimeTooSkewed: {
            static constexpr S3ErrorInfo info{
                "RequestTimeTooSkewed", 403,
                "The difference between the request time and the current time is too large."};
            return info;
        }
        case S3ErrorCode::SignatureDoesNotMatch: {
            static constexpr S3ErrorInfo info{
                "SignatureDoesNotMatch", 403,
                "The request signature we calculated does not match the signature you provided."};
            return info;
        }
        case S3ErrorCode::SlowDown: {
            static constexpr S3ErrorInfo info{"SlowDown", 503, "Please reduce your request rate."};
            return info;
        }
        case S3ErrorCode::XAmzContentSHA256Mismatch: {
            static constexpr S3ErrorInfo info{
                "XAmzContentSHA256Mismatch", 400,
                "The provided x-amz-content-sha256 header does not match what was computed."};
            return info;
        }
    }
    return kUnknown;
}

}  // namespace

const S3ErrorInfo& describe(S3ErrorCode code) noexcept {
    return lookup(code);
}

std::string renderError(S3ErrorCode code, std::string_view message, std::string_view resource,
                        std::string_view requestId) {
    const S3ErrorInfo& info = describe(code);

    // No xmlns: S3's own error documents do not carry one, and at least one
    // client rejects the document if it does.
    XmlWriter writer("Error", "");
    writer.element("Code", info.code);
    writer.element("Message", message.empty() ? std::string_view(info.message) : message);
    writer.elementIfSet("Resource", resource);
    writer.element("RequestId", requestId);
    return writer.finish();
}

S3ErrorCode fromStorage(StorageErrorCode code) noexcept {
    switch (code) {
        case StorageErrorCode::NoSuchBucket:        return S3ErrorCode::NoSuchBucket;
        case StorageErrorCode::NoSuchKey:           return S3ErrorCode::NoSuchKey;
        case StorageErrorCode::NoSuchUpload:        return S3ErrorCode::NoSuchUpload;
        case StorageErrorCode::BucketAlreadyExists: return S3ErrorCode::BucketAlreadyExists;
        case StorageErrorCode::BucketNotEmpty:      return S3ErrorCode::BucketNotEmpty;
        case StorageErrorCode::InvalidPart:         return S3ErrorCode::InvalidPart;
        case StorageErrorCode::QuotaExceeded:       return S3ErrorCode::QuotaExceeded;
        // A reduction below what is stored can only come from the console,
        // which never signs an S3 request; if one ever reaches here it is a
        // bad argument, not a full bucket.
        case StorageErrorCode::QuotaBelowUsage:     return S3ErrorCode::InvalidArgument;
        case StorageErrorCode::ObjectTooLarge:      return S3ErrorCode::EntityTooLarge;
        case StorageErrorCode::InsufficientCapacity:
            return S3ErrorCode::InsufficientCapacity;
        // Corruption, Io and Internal are all "the server broke", and the
        // client's recourse is identical: retry, then report it. Distinguishing
        // them on the wire would only tell an attacker about our disk.
        case StorageErrorCode::Corruption:
        case StorageErrorCode::Io:
        case StorageErrorCode::Internal:            return S3ErrorCode::InternalError;
    }
    return S3ErrorCode::InternalError;
}

std::string newRequestId() {
    static thread_local std::mt19937_64 engine{std::random_device{}()};

    std::uint64_t value = engine();
    std::string   out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = "0123456789ABCDEF"[value & 0xF];
        value >>= 4;
    }
    return out;
}

}  // namespace monobucket::s3
