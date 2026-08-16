#include "s3/operation.hpp"

#include <array>

namespace monobucket::s3 {
namespace {

/// Subresources S3 defines that MonoBucket does not implement. Listed
/// explicitly so a request for one is answered 501 with its name, rather than
/// falling through to "list this bucket" and returning a plausible-looking
/// document that answers a different question.
///
/// Answering GetBucketVersioning and GetBucketAcl is not on this list: clients
/// probe both routinely and handle a "disabled"/"private" answer, whereas a 501
/// makes `aws s3 sync` give up.
constexpr std::array<std::string_view, 18> kUnimplementedSubresources{
    "accelerate", "analytics",    "cors",           "encryption",   "inventory",
    "lifecycle",  "logging",      "metrics",        "notification", "object-lock",
    "ownership",  "publicAccessBlock", "replication", "requestPayment", "tagging",
    "versions",   "website",      "restore",
};

/// The first subresource present, if any. S3 requests carry at most one.
std::string firstSubresource(const S3Request& request) {
    for (const auto& name : kUnimplementedSubresources) {
        if (request.hasQuery(name)) return std::string(name);
    }
    return {};
}

Operation classifyService(const S3Request& request) {
    if (request.method == "GET") return Operation::ListBuckets;
    return Operation::MethodNotAllowed;
}

Operation classifyBucket(const S3Request& request) {
    const bool hasUploadId = request.hasQuery("uploadId");

    if (request.method == "GET") {
        if (request.hasQuery("uploads"))  return Operation::ListMultipartUploads;
        if (request.hasQuery("location")) return Operation::GetBucketLocation;
        if (request.hasQuery("versioning")) return Operation::GetBucketVersioning;
        if (request.hasQuery("policy"))   return Operation::GetBucketPolicy;
        if (request.hasQuery("acl"))      return Operation::GetBucketAcl;

        const auto listType = request.queryValue("list-type");
        if (listType && *listType == "2") return Operation::ListObjectsV2;
        return Operation::ListObjectsV1;
    }

    if (request.method == "HEAD") return Operation::HeadBucket;

    if (request.method == "PUT") {
        if (request.hasQuery("policy")) return Operation::PutBucketPolicy;
        if (request.hasQuery("acl"))    return Operation::PutBucketAcl;
        return Operation::CreateBucket;
    }

    if (request.method == "DELETE") {
        if (request.hasQuery("policy")) return Operation::DeleteBucketPolicy;
        return Operation::DeleteBucket;
    }

    if (request.method == "POST") {
        if (request.hasQuery("delete")) return Operation::DeleteObjects;
        // A POST to a bucket with an uploadId is a multipart completion whose
        // key went missing — worth saying so rather than "method not allowed".
        if (hasUploadId) return Operation::Unsupported;
        return Operation::MethodNotAllowed;
    }

    return Operation::MethodNotAllowed;
}

Operation classifyObject(const S3Request& request) {
    const bool hasUploadId   = request.hasQuery("uploadId");
    const bool hasPartNumber = request.hasQuery("partNumber");

    if (request.method == "GET") {
        if (hasUploadId) return Operation::ListParts;
        if (request.hasQuery("acl")) return Operation::GetBucketAcl;  // object ACL, same answer
        return Operation::GetObject;
    }

    if (request.method == "HEAD") return Operation::HeadObject;

    if (request.method == "PUT") {
        if (hasUploadId && hasPartNumber) return Operation::UploadPart;
        // A part number without an upload id, or an upload id without a part
        // number, is a client bug that would otherwise be stored as an object
        // under a key nobody meant to write.
        if (hasUploadId || hasPartNumber) return Operation::Unsupported;
        return Operation::PutObject;
    }

    if (request.method == "POST") {
        if (request.hasQuery("uploads")) return Operation::CreateMultipartUpload;
        if (hasUploadId) return Operation::CompleteMultipartUpload;
        return Operation::MethodNotAllowed;
    }

    if (request.method == "DELETE") {
        if (hasUploadId) return Operation::AbortMultipartUpload;
        return Operation::DeleteObject;
    }

    return Operation::MethodNotAllowed;
}

}  // namespace

std::string_view toString(Operation operation) noexcept {
    switch (operation) {
        case Operation::ListBuckets:             return "ListBuckets";
        case Operation::CreateBucket:            return "CreateBucket";
        case Operation::DeleteBucket:            return "DeleteBucket";
        case Operation::HeadBucket:              return "HeadBucket";
        case Operation::ListObjectsV1:           return "ListObjects";
        case Operation::ListObjectsV2:           return "ListObjectsV2";
        case Operation::ListMultipartUploads:    return "ListMultipartUploads";
        case Operation::DeleteObjects:           return "DeleteObjects";
        case Operation::GetBucketLocation:       return "GetBucketLocation";
        case Operation::GetBucketVersioning:     return "GetBucketVersioning";
        case Operation::GetBucketPolicy:         return "GetBucketPolicy";
        case Operation::PutBucketPolicy:         return "PutBucketPolicy";
        case Operation::DeleteBucketPolicy:      return "DeleteBucketPolicy";
        case Operation::GetBucketAcl:            return "GetBucketAcl";
        case Operation::PutBucketAcl:            return "PutBucketAcl";
        case Operation::GetObject:               return "GetObject";
        case Operation::HeadObject:              return "HeadObject";
        case Operation::PutObject:               return "PutObject";
        case Operation::DeleteObject:            return "DeleteObject";
        case Operation::CreateMultipartUpload:   return "CreateMultipartUpload";
        case Operation::UploadPart:              return "UploadPart";
        case Operation::ListParts:               return "ListParts";
        case Operation::CompleteMultipartUpload: return "CompleteMultipartUpload";
        case Operation::AbortMultipartUpload:    return "AbortMultipartUpload";
        case Operation::Unsupported:             return "Unsupported";
        case Operation::MethodNotAllowed:        return "MethodNotAllowed";
    }
    return "Unknown";
}

bool isReadOnly(Operation operation) noexcept {
    switch (operation) {
        case Operation::ListBuckets:
        case Operation::HeadBucket:
        case Operation::ListObjectsV1:
        case Operation::ListObjectsV2:
        case Operation::ListMultipartUploads:
        case Operation::GetBucketLocation:
        case Operation::GetBucketVersioning:
        case Operation::GetBucketPolicy:
        case Operation::GetBucketAcl:
        case Operation::GetObject:
        case Operation::HeadObject:
        case Operation::ListParts:
            return true;
        default:
            return false;
    }
}

Operation classify(const S3Request& request, std::string& unsupported) {
    unsupported = firstSubresource(request);
    if (!unsupported.empty()) return Operation::Unsupported;

    if (request.bucket.empty()) return classifyService(request);
    if (request.key.empty())    return classifyBucket(request);
    return classifyObject(request);
}

}  // namespace monobucket::s3
