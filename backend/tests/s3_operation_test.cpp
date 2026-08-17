#include <catch2/catch_test_macros.hpp>

#include "s3/operation.hpp"

using namespace monobucket::s3;

namespace {

Operation classifyOf(const char* method, const char* path, const char* query = "") {
    const S3Request request = parseRequest(method, path, query, "");
    std::string     unsupported;
    return classify(request, unsupported);
}

}  // namespace

TEST_CASE("service-level requests", "[s3][operation]") {
    REQUIRE(classifyOf("GET", "/") == Operation::ListBuckets);
    REQUIRE(classifyOf("PUT", "/") == Operation::MethodNotAllowed);
    REQUIRE(classifyOf("DELETE", "/") == Operation::MethodNotAllowed);
}

TEST_CASE("bucket-level requests are told apart by their subresource", "[s3][operation]") {
    REQUIRE(classifyOf("GET", "/photos") == Operation::ListObjectsV1);
    REQUIRE(classifyOf("GET", "/photos", "list-type=2") == Operation::ListObjectsV2);

    // Only list-type=2 selects V2; anything else is V1, as S3 does.
    REQUIRE(classifyOf("GET", "/photos", "list-type=1") == Operation::ListObjectsV1);

    REQUIRE(classifyOf("GET", "/photos", "uploads") == Operation::ListMultipartUploads);
    REQUIRE(classifyOf("GET", "/photos", "location") == Operation::GetBucketLocation);
    REQUIRE(classifyOf("GET", "/photos", "versioning") == Operation::GetBucketVersioning);
    REQUIRE(classifyOf("GET", "/photos", "policy") == Operation::GetBucketPolicy);
    REQUIRE(classifyOf("GET", "/photos", "acl") == Operation::GetBucketAcl);

    REQUIRE(classifyOf("PUT", "/photos") == Operation::CreateBucket);
    REQUIRE(classifyOf("PUT", "/photos", "policy") == Operation::PutBucketPolicy);
    REQUIRE(classifyOf("PUT", "/photos", "acl") == Operation::PutBucketAcl);

    REQUIRE(classifyOf("DELETE", "/photos") == Operation::DeleteBucket);
    REQUIRE(classifyOf("DELETE", "/photos", "policy") == Operation::DeleteBucketPolicy);

    REQUIRE(classifyOf("HEAD", "/photos") == Operation::HeadBucket);
    REQUIRE(classifyOf("POST", "/photos", "delete") == Operation::DeleteObjects);
    REQUIRE(classifyOf("POST", "/photos") == Operation::MethodNotAllowed);
}

TEST_CASE("object-level requests", "[s3][operation]") {
    REQUIRE(classifyOf("GET", "/photos/beach.jpg") == Operation::GetObject);
    REQUIRE(classifyOf("HEAD", "/photos/beach.jpg") == Operation::HeadObject);
    REQUIRE(classifyOf("PUT", "/photos/beach.jpg") == Operation::PutObject);
    REQUIRE(classifyOf("DELETE", "/photos/beach.jpg") == Operation::DeleteObject);
    REQUIRE(classifyOf("POST", "/photos/beach.jpg") == Operation::MethodNotAllowed);
}

TEST_CASE("multipart requests are told apart from plain object ones", "[s3][operation]") {
    REQUIRE(classifyOf("POST", "/photos/beach.jpg", "uploads") ==
            Operation::CreateMultipartUpload);
    REQUIRE(classifyOf("PUT", "/photos/beach.jpg", "partNumber=1&uploadId=abc") ==
            Operation::UploadPart);
    REQUIRE(classifyOf("GET", "/photos/beach.jpg", "uploadId=abc") == Operation::ListParts);
    REQUIRE(classifyOf("POST", "/photos/beach.jpg", "uploadId=abc") ==
            Operation::CompleteMultipartUpload);
    REQUIRE(classifyOf("DELETE", "/photos/beach.jpg", "uploadId=abc") ==
            Operation::AbortMultipartUpload);
}

TEST_CASE("half a multipart request is refused rather than stored", "[s3][operation]") {
    // Either of these would otherwise fall through to PutObject and write an
    // object under a key nobody meant to write.
    REQUIRE(classifyOf("PUT", "/photos/beach.jpg", "partNumber=1") == Operation::Unsupported);
    REQUIRE(classifyOf("PUT", "/photos/beach.jpg", "uploadId=abc") == Operation::Unsupported);
}

TEST_CASE("subresources we do not implement are named rather than misread",
          "[s3][operation]") {
    std::string unsupported;

    for (const char* subresource : {"tagging", "lifecycle", "replication", "versions",
                                    "website", "encryption", "object-lock"}) {
        const S3Request request = parseRequest("GET", "/photos", subresource, "");
        REQUIRE(classify(request, unsupported) == Operation::Unsupported);
        REQUIRE(unsupported == subresource);
    }

    // Answering these as "list this bucket" would return a plausible-looking
    // document that answers a completely different question.
    const S3Request tagging = parseRequest("GET", "/photos", "tagging", "");
    REQUIRE(classify(tagging, unsupported) != Operation::ListObjectsV1);
}

TEST_CASE("CORS is routed on the subresource and OPTIONS on the method", "[s3][operation]") {
    REQUIRE(classifyOf("GET", "/photos", "cors") == Operation::GetBucketCors);
    REQUIRE(classifyOf("PUT", "/photos", "cors") == Operation::PutBucketCors);
    REQUIRE(classifyOf("DELETE", "/photos", "cors") == Operation::DeleteBucketCors);

    // Without ?cors these are still the plain bucket operations — a stray
    // subresource must not be able to turn a CreateBucket into a config write.
    REQUIRE(classifyOf("PUT", "/photos") == Operation::CreateBucket);
    REQUIRE(classifyOf("DELETE", "/photos") == Operation::DeleteBucket);

    // A preflight is classified by its method alone: the browser sends OPTIONS
    // against the resource the real request will name, which may be an object,
    // a bucket, or something carrying a subresource we do not implement.
    REQUIRE(classifyOf("OPTIONS", "/photos") == Operation::Preflight);
    REQUIRE(classifyOf("OPTIONS", "/photos/beach.jpg") == Operation::Preflight);
    REQUIRE(classifyOf("OPTIONS", "/photos", "tagging") == Operation::Preflight);
    REQUIRE(classifyOf("OPTIONS", "/") == Operation::Preflight);

    // Reading the configuration is a read; the two writes are not, so an
    // anonymous caller against a public bucket cannot reach them.
    REQUIRE(isReadOnly(Operation::GetBucketCors));
    REQUIRE_FALSE(isReadOnly(Operation::PutBucketCors));
    REQUIRE_FALSE(isReadOnly(Operation::DeleteBucketCors));
}

TEST_CASE("versioning and ACL probes are answered rather than refused", "[s3][operation]") {
    // Clients probe both routinely; a 501 makes `aws s3 sync` give up, whereas
    // a "not versioned"/"private" answer is both true and useful.
    std::string unsupported;
    const S3Request versioning = parseRequest("GET", "/photos", "versioning", "");
    REQUIRE(classify(versioning, unsupported) == Operation::GetBucketVersioning);
    REQUIRE(unsupported.empty());
}

TEST_CASE("read-only operations are exactly the ones a public bucket may serve",
          "[s3][operation]") {
    REQUIRE(isReadOnly(Operation::GetObject));
    REQUIRE(isReadOnly(Operation::HeadObject));
    REQUIRE(isReadOnly(Operation::ListObjectsV2));
    REQUIRE(isReadOnly(Operation::HeadBucket));

    REQUIRE_FALSE(isReadOnly(Operation::PutObject));
    REQUIRE_FALSE(isReadOnly(Operation::DeleteObject));
    REQUIRE_FALSE(isReadOnly(Operation::DeleteObjects));
    REQUIRE_FALSE(isReadOnly(Operation::CreateBucket));
    REQUIRE_FALSE(isReadOnly(Operation::DeleteBucket));
    REQUIRE_FALSE(isReadOnly(Operation::PutBucketPolicy));
    REQUIRE_FALSE(isReadOnly(Operation::PutBucketAcl));
    REQUIRE_FALSE(isReadOnly(Operation::CompleteMultipartUpload));
    REQUIRE_FALSE(isReadOnly(Operation::UploadPart));
    REQUIRE_FALSE(isReadOnly(Operation::AbortMultipartUpload));

    // A request we could not classify must never be treated as harmless.
    REQUIRE_FALSE(isReadOnly(Operation::Unsupported));
    REQUIRE_FALSE(isReadOnly(Operation::MethodNotAllowed));
}

TEST_CASE("every operation has a name", "[s3][operation]") {
    // The names appear in logs and in the metrics labels, so an unnamed one
    // would show up as "Unknown" in production before anyone noticed here.
    for (int i = 0; i <= static_cast<int>(Operation::MethodNotAllowed); ++i) {
        REQUIRE(toString(static_cast<Operation>(i)) != "Unknown");
    }
}
