#include <catch2/catch_test_macros.hpp>

#include "s3/s3_error.hpp"
#include "s3/xml.hpp"

using namespace monobucket;
using namespace monobucket::s3;

TEST_CASE("the error document has the shape clients parse", "[s3][error]") {
    const std::string document =
        renderError(S3ErrorCode::NoSuchKey, "", "/photos/missing.jpg", "ABCDEF0123456789");

    const XmlNode root = parseXml(document);
    REQUIRE(root.name == "Error");
    REQUIRE(root.childText("Code") == "NoSuchKey");
    REQUIRE(root.childText("Resource") == "/photos/missing.jpg");
    REQUIRE(root.childText("RequestId") == "ABCDEF0123456789");
    REQUIRE_FALSE(root.childText("Message").empty());

    // S3's own error documents carry no namespace, and at least one client
    // rejects the document if one is present.
    REQUIRE(document.find("xmlns") == std::string::npos);
}

TEST_CASE("a supplied message replaces the default", "[s3][error]") {
    const std::string document =
        renderError(S3ErrorCode::InvalidArgument, "max-keys must be a number.", "/photos", "ID");
    REQUIRE(parseXml(document).childText("Message") == "max-keys must be a number.");
}

TEST_CASE("a resource containing XML metacharacters is escaped", "[s3][error]") {
    // The resource is the request path, which is client-controlled: an
    // unescaped one would let a caller inject elements into our own document.
    const std::string document =
        renderError(S3ErrorCode::NoSuchKey, "", "/photos/<script>&", "ID");

    REQUIRE(document.find("<script>") == std::string::npos);
    REQUIRE(parseXml(document).childText("Resource") == "/photos/<script>&");
}

TEST_CASE("codes carry the status and spelling clients branch on", "[s3][error]") {
    REQUIRE(describe(S3ErrorCode::NoSuchBucket).status == 404);
    REQUIRE(describe(S3ErrorCode::NoSuchKey).status == 404);
    REQUIRE(describe(S3ErrorCode::AccessDenied).status == 403);
    REQUIRE(describe(S3ErrorCode::SignatureDoesNotMatch).status == 403);
    REQUIRE(describe(S3ErrorCode::RequestTimeTooSkewed).status == 403);
    REQUIRE(describe(S3ErrorCode::BucketNotEmpty).status == 409);
    REQUIRE(describe(S3ErrorCode::BucketAlreadyOwnedByYou).status == 409);
    REQUIRE(describe(S3ErrorCode::PreconditionFailed).status == 412);
    REQUIRE(describe(S3ErrorCode::MethodNotAllowed).status == 405);
    REQUIRE(describe(S3ErrorCode::MissingContentLength).status == 411);
    REQUIRE(describe(S3ErrorCode::NotImplemented).status == 501);

    // 416 must still carry an S3 error document, not a bare status.
    REQUIRE(describe(S3ErrorCode::InvalidRange).status == 416);

    // SlowDown is what makes the AWS CLI back off and retry rather than fail
    // the transfer, so its status is load-bearing.
    REQUIRE(describe(S3ErrorCode::SlowDown).status == 503);

    // The wire code carries the "Error" suffix. Not a typo — clients match it
    // literally.
    REQUIRE(std::string(describe(S3ErrorCode::KeyTooLong).code) == "KeyTooLongError");

    // 403 rather than 507: a bucket over its allocation is terminal, and 403 is
    // what stops the AWS CLI instead of making it back off and retry.
    REQUIRE(describe(S3ErrorCode::QuotaExceeded).status == 403);
    REQUIRE(std::string(describe(S3ErrorCode::QuotaExceeded).code) == "QuotaExceeded");

    // Distinct from QuotaExceeded on purpose: one says this bucket is full, the
    // other says the server has nothing left to give a new bucket.
    REQUIRE(describe(S3ErrorCode::InsufficientCapacity).status == 507);
}

TEST_CASE("every storage condition maps to a code a client can act on",
          "[s3][error]") {
    REQUIRE(fromStorage(StorageErrorCode::NoSuchBucket) == S3ErrorCode::NoSuchBucket);
    REQUIRE(fromStorage(StorageErrorCode::NoSuchKey) == S3ErrorCode::NoSuchKey);
    REQUIRE(fromStorage(StorageErrorCode::NoSuchUpload) == S3ErrorCode::NoSuchUpload);
    REQUIRE(fromStorage(StorageErrorCode::BucketAlreadyExists) ==
            S3ErrorCode::BucketAlreadyExists);
    REQUIRE(fromStorage(StorageErrorCode::BucketNotEmpty) == S3ErrorCode::BucketNotEmpty);
    REQUIRE(fromStorage(StorageErrorCode::InvalidPart) == S3ErrorCode::InvalidPart);

    REQUIRE(fromStorage(StorageErrorCode::QuotaExceeded) == S3ErrorCode::QuotaExceeded);
    REQUIRE(fromStorage(StorageErrorCode::InsufficientCapacity) ==
            S3ErrorCode::InsufficientCapacity);

    // A reduction below what is stored can only come from the console, which
    // never signs an S3 request. If one ever reaches here it is a bad argument,
    // not a full bucket, and saying "QuotaExceeded" would send the reader off
    // to delete objects that were never the problem.
    REQUIRE(fromStorage(StorageErrorCode::QuotaBelowUsage) == S3ErrorCode::InvalidArgument);
    // Not QuotaExceeded: a bucket that is full and an object that is too large
    // are different problems with different fixes, and clients branch on the
    // code string rather than on the message.
    REQUIRE(fromStorage(StorageErrorCode::ObjectTooLarge) == S3ErrorCode::EntityTooLarge);
    REQUIRE(describe(S3ErrorCode::EntityTooLarge).status == 400);

    // Corruption, Io and Internal are all "the server broke" and the client's
    // recourse is identical; distinguishing them on the wire would only tell an
    // attacker about our disk.
    REQUIRE(fromStorage(StorageErrorCode::Corruption) == S3ErrorCode::InternalError);
    REQUIRE(fromStorage(StorageErrorCode::Io) == S3ErrorCode::InternalError);
    REQUIRE(fromStorage(StorageErrorCode::Internal) == S3ErrorCode::InternalError);
}

TEST_CASE("no code falls through to the default", "[s3][error]") {
    // A new enumerator without a table entry would otherwise become a silent
    // 500 with the wrong name on it.
    for (int i = 0; i <= static_cast<int>(S3ErrorCode::XAmzContentSHA256Mismatch); ++i) {
        const auto code = static_cast<S3ErrorCode>(i);
        const S3ErrorInfo& info = describe(code);

        REQUIRE(info.status >= 400);
        REQUIRE(info.status < 600);
        if (code != S3ErrorCode::InternalError) {
            REQUIRE(std::string(info.code) != "InternalError");
        }
    }
}

TEST_CASE("the exception carries its code and a usable message", "[s3][error]") {
    const S3Exception plain(S3ErrorCode::NoSuchKey);
    REQUIRE(plain.code() == S3ErrorCode::NoSuchKey);
    REQUIRE(std::string(plain.what()) == describe(S3ErrorCode::NoSuchKey).message);

    const S3Exception detailed(S3ErrorCode::InvalidArgument, "max-keys must be a number.");
    REQUIRE(detailed.code() == S3ErrorCode::InvalidArgument);
    REQUIRE(std::string(detailed.what()) == "max-keys must be a number.");
}

TEST_CASE("request ids are distinct and in AWS's shape", "[s3][error]") {
    const std::string first  = newRequestId();
    const std::string second = newRequestId();

    REQUIRE(first.size() == 16);
    REQUIRE(first != second);
    for (const char ch : first) {
        REQUIRE(std::string("0123456789ABCDEF").find(ch) != std::string::npos);
    }
}
