#include <catch2/catch_test_macros.hpp>

#include <string>

#include "s3/bucket_policy.hpp"
#include "server/policy_reconcile.hpp"
#include "temporary_directory.hpp"
#include "s3/s3_error.hpp"

using monobucket::s3::analyseBucketPolicy;
using monobucket::s3::PolicyAnalysis;
using monobucket::s3::S3Exception;
using monobucket::s3::validateBucketPolicy;

namespace {

PolicyAnalysis analyse(const std::string& document) {
    return analyseBucketPolicy(document, "b");
}

/// One Allow statement publishing every object in bucket `b`, which is the
/// shape almost every real policy here is a variation on.
constexpr const char* kPublicRead = R"({
  "Version": "2012-10-17",
  "Statement": [
    {"Effect": "Allow", "Principal": "*", "Action": "s3:GetObject",
     "Resource": "arn:aws:s3:::b/*"}
  ]
})";

}  // namespace

// --- What is granted -------------------------------------------------------

TEST_CASE("a public-read policy grants objects and not listing", "[policy]") {
    const auto analysis = analyse(kPublicRead);
    REQUIRE(analysis.understood());
    CHECK(analysis.grants.readObjects);
    // The distinction this used to lose: publishing the objects is not
    // publishing their names.
    CHECK_FALSE(analysis.grants.listBucket);
}

TEST_CASE("listing is granted only where it is asked for", "[policy]") {
    const auto analysis = analyse(R"({"Statement": [
        {"Effect": "Allow", "Principal": "*", "Action": "s3:ListBucket",
         "Resource": "arn:aws:s3:::b"}]})");
    REQUIRE(analysis.understood());
    CHECK(analysis.grants.listBucket);
    CHECK_FALSE(analysis.grants.readObjects);
}

TEST_CASE("an action only reaches where its resource does", "[policy]") {
    // GetObject named against the bucket rather than its objects addresses
    // nothing at all, and S3 treats it the same way.
    const auto misaimed = analyse(R"({"Statement": [
        {"Effect": "Allow", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b"}]})");
    REQUIRE(misaimed.understood());
    CHECK_FALSE(misaimed.grants.readObjects);
    CHECK_FALSE(misaimed.grants.listBucket);

    const auto reversed = analyse(R"({"Statement": [
        {"Effect": "Allow", "Principal": "*", "Action": "s3:ListBucket",
         "Resource": "arn:aws:s3:::b/*"}]})");
    REQUIRE(reversed.understood());
    CHECK_FALSE(reversed.grants.listBucket);
}

TEST_CASE("a bare wildcard grants both", "[policy]") {
    const auto analysis = analyse(R"({"Statement": [
        {"Effect": "Allow", "Principal": "*", "Action": "*", "Resource": "*"}]})");
    REQUIRE(analysis.understood());
    CHECK(analysis.grants.readObjects);
    CHECK(analysis.grants.listBucket);
}

TEST_CASE("the principal may be written either way", "[policy]") {
    const auto object = analyse(R"({"Statement": [
        {"Effect": "Allow", "Principal": {"AWS": "*"}, "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/*"}]})");
    REQUIRE(object.understood());
    CHECK(object.grants.readObjects);

    const auto list = analyse(R"({"Statement": [
        {"Effect": "Allow", "Principal": {"AWS": ["*"]}, "Action": ["s3:GetObject"],
         "Resource": ["arn:aws:s3:::b/*"]}]})");
    REQUIRE(list.understood());
    CHECK(list.grants.readObjects);
}

TEST_CASE("a single statement need not be wrapped in an array", "[policy]") {
    const auto analysis = analyse(R"({"Statement":
        {"Effect": "Allow", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/*"}})");
    REQUIRE(analysis.understood());
    CHECK(analysis.grants.readObjects);
}

// --- What is refused -------------------------------------------------------

TEST_CASE("a Deny is refused rather than ignored", "[policy]") {
    // The bug this exists for. Read as an Allow-only document, this publishes
    // everything including private/, which is the opposite of what it says.
    const auto analysis = analyse(R"({"Statement": [
        {"Effect": "Allow", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/*"},
        {"Effect": "Deny", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/private/*"}]})");

    CHECK_FALSE(analysis.understood());
    CHECK(analysis.unsupported.find("Deny") != std::string::npos);
    // And nothing is granted on the strength of the Allow that preceded it.
    CHECK_FALSE(analysis.grants.readObjects);
    CHECK_FALSE(analysis.grants.listBucket);
}

TEST_CASE("a Deny before an Allow is refused too", "[policy]") {
    // Statement order carries no meaning in IAM, so it must carry none here.
    const auto analysis = analyse(R"({"Statement": [
        {"Effect": "Deny", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/private/*"},
        {"Effect": "Allow", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/*"}]})");

    CHECK_FALSE(analysis.understood());
    CHECK_FALSE(analysis.grants.readObjects);
}

TEST_CASE("a Deny over the whole bucket is refused, not read as private",
          "[policy]") {
    // It would be tempting to accept this one, since a bucket-wide Deny and no
    // grant at all reach the same place. Accepting it would mean the grammar
    // depends on what the Deny happens to cover, which is exactly the kind of
    // rule nobody can predict from the outside.
    const auto analysis = analyse(R"({"Statement": [
        {"Effect": "Deny", "Principal": "*", "Action": "*", "Resource": "*"}]})");
    CHECK_FALSE(analysis.understood());
    CHECK(analysis.unsupported.find("Deny") != std::string::npos);
}

TEST_CASE("a Condition beside a Deny is still refused", "[policy]") {
    const auto analysis = analyse(R"({"Statement": [
        {"Effect": "Deny", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/*",
         "Condition": {"IpAddress": {"aws:SourceIp": "10.0.0.0/8"}}}]})");
    CHECK_FALSE(analysis.understood());
}

TEST_CASE("a Condition is refused rather than treated as unconditional",
          "[policy]") {
    const auto analysis = analyse(R"({"Statement": [
        {"Effect": "Allow", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/*",
         "Condition": {"IpAddress": {"aws:SourceIp": "10.0.0.0/8"}}}]})");
    CHECK_FALSE(analysis.understood());
    CHECK(analysis.unsupported.find("Condition") != std::string::npos);
    // Previously this was skipped and granted nothing, which was safe but
    // silent. The grant is still not made; now the operator is told.
    CHECK_FALSE(analysis.grants.readObjects);
}

TEST_CASE("a prefix-scoped resource is refused rather than silently inert",
          "[policy]") {
    const auto analysis = analyse(R"({"Statement": [
        {"Effect": "Allow", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/public/*"}]})");
    CHECK_FALSE(analysis.understood());
    CHECK(analysis.unsupported.find("part of a bucket") != std::string::npos);
    CHECK_FALSE(analysis.grants.readObjects);
}

TEST_CASE("a resource naming another bucket is refused", "[policy]") {
    const auto analysis = analyse(R"({"Statement": [
        {"Effect": "Allow", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::other/*"}]})");
    CHECK_FALSE(analysis.understood());
}

TEST_CASE("a named principal is refused", "[policy]") {
    // Nothing here authorises a signed request from a policy, so a statement
    // about a particular identity has no surface to act on.
    const auto analysis = analyse(R"({"Statement": [
        {"Effect": "Allow", "Principal": {"AWS": "arn:aws:iam::1234:root"},
         "Action": "s3:GetObject", "Resource": "arn:aws:s3:::b/*"}]})");
    CHECK_FALSE(analysis.understood());
    CHECK(analysis.unsupported.find("Principal") != std::string::npos);
}

TEST_CASE("an anonymous write is refused rather than quietly dropped",
          "[policy]") {
    // The reading that matters: somebody granting this believes they enabled
    // anonymous uploads. They did not, and they need to be told so.
    const auto analysis = analyse(R"({"Statement": [
        {"Effect": "Allow", "Principal": "*", "Action": "s3:PutObject",
         "Resource": "arn:aws:s3:::b/*"}]})");
    CHECK_FALSE(analysis.understood());
    CHECK(analysis.unsupported.find("s3:PutObject") != std::string::npos);
}

TEST_CASE("the inverted elements are refused", "[policy]") {
    for (const char* element : {"NotAction", "NotResource", "NotPrincipal"}) {
        const std::string document = std::string(R"({"Statement": [
            {"Effect": "Allow", "Principal": "*", "Action": "s3:GetObject",
             "Resource": "arn:aws:s3:::b/*", ")") + element + R"(": "x"}]})";
        INFO(element);
        CHECK_FALSE(analyse(document).understood());
    }
}

TEST_CASE("an unknown Version is refused", "[policy]") {
    const auto analysis = analyse(R"({"Version": "2099-01-01", "Statement": [
        {"Effect": "Allow", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/*"}]})");
    CHECK_FALSE(analysis.understood());
}

// --- The existing conservatism is preserved --------------------------------

TEST_CASE("a document that is not JSON grants nothing", "[policy]") {
    const auto analysis = analyse("not a policy at all");
    CHECK_FALSE(analysis.understood());
    CHECK_FALSE(analysis.grants.any());
}

TEST_CASE("a document with no Statement grants nothing", "[policy]") {
    CHECK_FALSE(analyse(R"({"Version": "2012-10-17"})").understood());
    CHECK_FALSE(analyse("[]").understood());
}

TEST_CASE("an empty statement list grants nothing and is not an error",
          "[policy]") {
    // Well formed and says nothing, which is different from malformed.
    const auto analysis = analyse(R"({"Statement": []})");
    CHECK(analysis.understood());
    CHECK_FALSE(analysis.grants.any());
}

// --- Validation ------------------------------------------------------------

TEST_CASE("validation accepts what the grammar covers", "[policy]") {
    CHECK_NOTHROW(validateBucketPolicy(kPublicRead, "b"));
}

TEST_CASE("validation refuses an unevaluated element by name", "[policy]") {
    const std::string denying = R"({"Statement": [
        {"Effect": "Allow", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/*"},
        {"Effect": "Deny", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/private/*"}]})";

    try {
        validateBucketPolicy(denying, "b");
        FAIL("a policy containing a Deny was accepted");
    } catch (const S3Exception& error) {
        // Naming the element is the point: "invalid policy" would leave an
        // operator to guess which of five statements this build objects to.
        const std::string message = error.what();
        CHECK(message.find("Deny") != std::string::npos);
    }
}

TEST_CASE("validation still refuses the shapes S3 itself refuses", "[policy]") {
    CHECK_THROWS_AS(validateBucketPolicy("", "b"), S3Exception);
    CHECK_THROWS_AS(validateBucketPolicy("{", "b"), S3Exception);
    CHECK_THROWS_AS(validateBucketPolicy(std::string(20 * 1024 + 1, 'x'), "b"), S3Exception);
}

TEST_CASE("a policy is read against the bucket it is attached to", "[policy]") {
    // The same document is a grant on one bucket and a refusal on another.
    CHECK(analyseBucketPolicy(kPublicRead, "b").grants.readObjects);
    CHECK_FALSE(analyseBucketPolicy(kPublicRead, "other").understood());
}

TEST_CASE("the console's public-read template is accepted", "[policy]") {
    // Kept in step with PUBLIC_READ_TEMPLATE in the bucket page. A grammar that
    // refused the console's own template would refuse the one document most
    // users of this server ever write.
    const std::string templated = R"({
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "PublicRead",
      "Effect": "Allow",
      "Principal": "*",
      "Action": "s3:GetObject",
      "Resource": "arn:aws:s3:::b/*"
    }
  ]
})";

    CHECK_NOTHROW(validateBucketPolicy(templated, "b"));
    const auto analysis = analyse(templated);
    REQUIRE(analysis.understood());
    CHECK(analysis.grants.readObjects);
    CHECK_FALSE(analysis.grants.listBucket);
}

// --- Reconciling what is already stored ------------------------------------

TEST_CASE("a stored policy is re-read at startup", "[policy][engine]") {
    using monobucket::reconcileBucketPolicies;
    using monobucket::StorageEngine;
    using monobucket::testing::TemporaryDirectory;

    TemporaryDirectory     root("policy-reconcile");
    StorageEngine::Options options;
    options.dataDir             = root.path();
    options.durability          = monobucket::Durability::None;
    options.metadataMemoryBytes = 8ull * 1024 * 1024;

    StorageEngine engine(std::move(options));
    engine.createBucket("b", 0);

    const std::string denying = R"({"Statement": [
        {"Effect": "Allow", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/*"},
        {"Effect": "Deny", "Principal": "*", "Action": "s3:GetObject",
         "Resource": "arn:aws:s3:::b/private/*"}]})";

    // Exactly the state an older build left behind: the Deny was invisible to
    // it, so the bucket was marked public on the strength of the Allow alone.
    engine.setBucketPolicy("b", denying, true, true);
    REQUIRE(engine.getBucket("b")->publicRead);

    const auto report = reconcileBucketPolicies(engine);
    CHECK(report.examined == 1);
    REQUIRE(report.unenforceable.size() == 1);
    CHECK(report.unenforceable.front() == "b");

    const auto record = engine.getBucket("b");
    REQUIRE(record.has_value());
    CHECK_FALSE(record->publicRead);
    CHECK_FALSE(record->publicList);
    // The document survives. It is the operator's text and the only record of
    // what they meant; GetBucketPolicy has always returned it verbatim.
    CHECK(record->policy == denying);
}

TEST_CASE("reconciliation narrows a listing grant nobody asked for",
          "[policy][engine]") {
    using monobucket::reconcileBucketPolicies;
    using monobucket::StorageEngine;
    using monobucket::testing::TemporaryDirectory;

    TemporaryDirectory     root("policy-narrow");
    StorageEngine::Options options;
    options.dataDir             = root.path();
    options.durability          = monobucket::Durability::None;
    options.metadataMemoryBytes = 8ull * 1024 * 1024;

    StorageEngine engine(std::move(options));
    engine.createBucket("b", 0);

    // What one flag used to mean: a GetObject grant that also opened the
    // bucket to anonymous enumeration.
    engine.setBucketPolicy("b", kPublicRead, true, true);

    const auto report = reconcileBucketPolicies(engine);
    REQUIRE(report.narrowed.size() == 1);

    const auto record = engine.getBucket("b");
    CHECK(record->publicRead);
    CHECK_FALSE(record->publicList);
}

TEST_CASE("a bucket with no policy is left alone", "[policy][engine]") {
    using monobucket::reconcileBucketPolicies;
    using monobucket::StorageEngine;
    using monobucket::testing::TemporaryDirectory;

    TemporaryDirectory     root("policy-untouched");
    StorageEngine::Options options;
    options.dataDir             = root.path();
    options.durability          = monobucket::Durability::None;
    options.metadataMemoryBytes = 8ull * 1024 * 1024;

    StorageEngine engine(std::move(options));
    engine.createBucket("b", 0);
    engine.setBucketPublicRead("b", true);

    // Its access came from the console toggle, not from a document, so there
    // is nothing to re-derive and nothing to take away.
    const auto report = reconcileBucketPolicies(engine);
    CHECK(report.examined == 0);
    CHECK(engine.getBucket("b")->publicRead);
}
