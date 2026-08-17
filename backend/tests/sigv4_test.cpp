#include <catch2/catch_test_macros.hpp>

#include "s3/s3_error.hpp"
#include "s3/sigv4.hpp"
#include "storage/digest.hpp"

// The vectors below are AWS's own, from the "Authenticating Requests (AWS
// Signature Version 4)" examples in the S3 documentation. They are the only
// thing that can tell a signature implementation that is self-consistent from
// one that is correct: every intermediate string here was produced by AWS, not
// by this code.

using namespace monobucket;
using namespace monobucket::s3;

namespace {

constexpr const char* kAccessKey = "AKIAIOSFODNN7EXAMPLE";
constexpr const char* kSecretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
constexpr const char* kHost      = "examplebucket.s3.amazonaws.com";
constexpr const char* kAmzDate   = "20130524T000000Z";
constexpr const char* kScope     = "20130524/us-east-1/s3/aws4_request";

/// 24 May 2013, 00:00:00 UTC — the instant every example was signed at.
constexpr std::int64_t kExampleNow = 1369353600;

std::string credentialFor(const char* date = "20130524") {
    return std::string(kAccessKey) + "/" + date + "/us-east-1/s3/aws4_request";
}

std::string authorizationHeader(const std::string& signedHeaders, const std::string& signature,
                                const char* date = "20130524") {
    return "AWS4-HMAC-SHA256 Credential=" + credentialFor(date) + ", SignedHeaders=" +
           signedHeaders + ", Signature=" + signature;
}

AuthOptions defaultOptions() {
    AuthOptions options;
    options.region     = "us-east-1";
    options.nowSeconds = kExampleNow;
    return options;
}

Credentials exampleCredentials() {
    return Credentials{kAccessKey, kSecretKey};
}

}  // namespace

TEST_CASE("the derived signing key matches the AWS worked example", "[sigv4]") {
    const std::string key = deriveSigningKey(kSecretKey, "20130524", "us-east-1", "s3");
    REQUIRE(key.size() == 32);

    // Signing the documented string-to-sign with it must reproduce the
    // documented signature; that pins the key without depending on a published
    // hex form of the key itself.
    const std::string toSign =
        stringToSign(kAmzDate, kScope,
                     "7344ae5b7ee6c3e7e6b0fe0640412a37625d1fbfff95c48bbb2dc43964946972");
    const std::string raw = hmacSha256(key, toSign);
    REQUIRE(toHex(std::span<const unsigned char>(
                reinterpret_cast<const unsigned char*>(raw.data()), raw.size())) ==
            "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
}

TEST_CASE("GET object with a Range header reproduces the documented canonical request",
          "[sigv4]") {
    SigningRequest request;
    request.method = "GET";
    request.uri    = "/test.txt";
    request.headers = {
        {"host", kHost},
        {"range", "bytes=0-9"},
        {"x-amz-content-sha256", kEmptySha256},
        {"x-amz-date", kAmzDate},
    };

    const CanonicalRequest canonical = buildCanonicalRequest(
        request, {"host", "range", "x-amz-content-sha256", "x-amz-date"}, kEmptySha256);

    REQUIRE(canonical.signedHeaders == "host;range;x-amz-content-sha256;x-amz-date");
    REQUIRE(sha256Hex(canonical.render()) ==
            "7344ae5b7ee6c3e7e6b0fe0640412a37625d1fbfff95c48bbb2dc43964946972");

    request.headers.emplace_back(
        "authorization",
        authorizationHeader("host;range;x-amz-content-sha256;x-amz-date",
                            "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"));

    const AuthOutcome outcome =
        authenticate(request, {}, exampleCredentials(), defaultOptions());
    REQUIRE_FALSE(outcome.anonymous);
    REQUIRE(outcome.accessKey == kAccessKey);
    REQUIRE(outcome.payload == PayloadMode::Signed);
    REQUIRE(outcome.scope == kScope);
}

TEST_CASE("PUT object with a signed body reproduces the documented signature", "[sigv4]") {
    const std::string body   = "Welcome to Amazon S3.";
    const std::string digest = sha256Hex(body);
    REQUIRE(digest == "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072");

    const std::string signedHeaders =
        "date;host;x-amz-content-sha256;x-amz-date;x-amz-storage-class";

    SigningRequest request;
    request.method  = "PUT";
    // The key contains a '$', which the client percent-encoded on the wire. The
    // canonical URI must be that encoded form, which is why the raw path is
    // signed rather than the decoded one.
    request.uri     = "/test%24file.text";
    request.headers = {
        {"date", "Fri, 24 May 2013 00:00:00 GMT"},
        {"host", kHost},
        {"x-amz-content-sha256", digest},
        {"x-amz-date", kAmzDate},
        {"x-amz-storage-class", "REDUCED_REDUNDANCY"},
        {"authorization",
         authorizationHeader(signedHeaders,
                             "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd")},
    };

    const AuthOutcome outcome =
        authenticate(request, {}, exampleCredentials(), defaultOptions());
    REQUIRE(outcome.payloadSha256 == digest);
}

TEST_CASE("a bare subresource canonicalises with a trailing equals sign", "[sigv4]") {
    // `?lifecycle` must canonicalise to `lifecycle=`. Getting this wrong breaks
    // every subresource request and nothing else, which makes it exactly the
    // kind of bug that reaches production.
    SigningRequest request;
    request.method  = "GET";
    request.uri     = "/";
    request.query   = "lifecycle";
    request.headers = {
        {"host", kHost},
        {"x-amz-content-sha256", kEmptySha256},
        {"x-amz-date", kAmzDate},
        {"authorization",
         authorizationHeader("host;x-amz-content-sha256;x-amz-date",
                             "fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543")},
    };

    REQUIRE_NOTHROW(authenticate(request, parseQuery(request.query), exampleCredentials(),
                                 defaultOptions()));
}

TEST_CASE("query parameters are sorted by name before signing", "[sigv4]") {
    SigningRequest request;
    request.method = "GET";
    request.uri    = "/";
    // Sent in the opposite order to the canonical one, which is what makes the
    // sort observable.
    request.query  = "prefix=J&max-keys=2";
    request.headers = {
        {"host", kHost},
        {"x-amz-content-sha256", kEmptySha256},
        {"x-amz-date", kAmzDate},
        {"authorization",
         authorizationHeader("host;x-amz-content-sha256;x-amz-date",
                             "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7")},
    };

    REQUIRE_NOTHROW(authenticate(request, parseQuery(request.query), exampleCredentials(),
                                 defaultOptions()));
}

TEST_CASE("a presigned URL verifies against the documented signature", "[sigv4]") {
    const std::string query =
        "X-Amz-Algorithm=AWS4-HMAC-SHA256"
        "&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
        "&X-Amz-Date=20130524T000000Z"
        "&X-Amz-Expires=86400"
        "&X-Amz-SignedHeaders=host"
        "&X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404";

    SigningRequest request;
    request.method  = "GET";
    request.uri     = "/test.txt";
    request.query   = query;
    request.headers = {{"host", kHost}};

    const AuthOutcome outcome =
        authenticate(request, parseQuery(query), exampleCredentials(), defaultOptions());
    REQUIRE(outcome.presigned);
    REQUIRE(outcome.accessKey == kAccessKey);
    // A presigned request never covers the body, whatever the client sends.
    REQUIRE(outcome.payload == PayloadMode::Unsigned);
}

TEST_CASE("a presigned URL stops working once it expires", "[sigv4]") {
    const std::string query =
        "X-Amz-Algorithm=AWS4-HMAC-SHA256"
        "&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
        "&X-Amz-Date=20130524T000000Z"
        "&X-Amz-Expires=86400"
        "&X-Amz-SignedHeaders=host"
        "&X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404";

    SigningRequest request;
    request.method  = "GET";
    request.uri     = "/test.txt";
    request.query   = query;
    request.headers = {{"host", kHost}};

    AuthOptions options = defaultOptions();
    options.nowSeconds  = kExampleNow + 86400;
    REQUIRE_NOTHROW(authenticate(request, parseQuery(query), exampleCredentials(), options));

    options.nowSeconds = kExampleNow + 86401;
    REQUIRE_THROWS_AS(authenticate(request, parseQuery(query), exampleCredentials(), options),
                      S3Exception);
}

TEST_CASE("presigning reproduces the documented AWS query string", "[sigv4]") {
    // The same worked example the verification test above consumes, generated
    // rather than parsed. Matching it byte for byte is what says the console's
    // links are S3's links and not merely ones this server would accept.
    PresignRequest request;
    request.method         = "GET";
    request.host           = kHost;
    request.uri            = "/test.txt";
    request.region         = "us-east-1";
    request.nowSeconds     = kExampleNow;
    request.expiresSeconds = 86400;

    REQUIRE(presignQuery(request, exampleCredentials()) ==
            "X-Amz-Algorithm=AWS4-HMAC-SHA256"
            "&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
            "&X-Amz-Date=20130524T000000Z"
            "&X-Amz-Expires=86400"
            "&X-Amz-SignedHeaders=host"
            "&X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404");
}

TEST_CASE("a minted URL grants one method against one host", "[sigv4]") {
    PresignRequest request;
    request.host           = "storage.example.com:9000";
    request.uri            = "/photos/summer%20holiday.jpg";
    request.nowSeconds     = kExampleNow;
    request.expiresSeconds = 900;

    const std::string query = presignQuery(request, exampleCredentials());

    SigningRequest redeem;
    redeem.method  = "GET";
    redeem.uri     = request.uri;
    redeem.query   = query;
    redeem.headers = {{"host", request.host}};
    REQUIRE_NOTHROW(authenticate(redeem, parseQuery(query), exampleCredentials(), defaultOptions()));

    // The method is inside the signature, so a link handed out for reading
    // cannot be turned into one for writing.
    SigningRequest asPut = redeem;
    asPut.method         = "PUT";
    REQUIRE_THROWS_AS(authenticate(asPut, parseQuery(query), exampleCredentials(), defaultOptions()),
                      S3Exception);

    // As is the host, so the link does not carry to a second endpoint the same
    // credentials happen to reach.
    SigningRequest elsewhere = redeem;
    elsewhere.headers        = {{"host", "storage.example.com:9001"}};
    REQUIRE_THROWS_AS(
        authenticate(elsewhere, parseQuery(query), exampleCredentials(), defaultOptions()),
        S3Exception);
}

TEST_CASE("presigning refuses a lifetime S3 would not accept", "[sigv4]") {
    PresignRequest request;
    request.host       = kHost;
    request.uri        = "/test.txt";
    request.nowSeconds = kExampleNow;

    request.expiresSeconds = 0;
    REQUIRE_THROWS_AS(presignQuery(request, exampleCredentials()), S3Exception);

    request.expiresSeconds = 604801;
    REQUIRE_THROWS_AS(presignQuery(request, exampleCredentials()), S3Exception);

    // A URL nothing binds to a host would be redeemable anywhere, and
    // authenticate() would refuse it on arrival regardless.
    request.expiresSeconds = 3600;
    request.host           = "";
    REQUIRE_THROWS_AS(presignQuery(request, exampleCredentials()), S3Exception);
}

TEST_CASE("no credentials at all is reported as anonymous rather than refused", "[sigv4]") {
    SigningRequest request;
    request.method  = "GET";
    request.uri     = "/public/thing.txt";
    request.headers = {{"host", kHost}};

    const AuthOutcome outcome =
        authenticate(request, {}, exampleCredentials(), defaultOptions());
    REQUIRE(outcome.anonymous);
    REQUIRE(outcome.accessKey.empty());
}

TEST_CASE("tampering with any signed component is detected", "[sigv4]") {
    const std::string signedHeaders = "host;range;x-amz-content-sha256;x-amz-date";
    const std::string signature =
        "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41";

    const auto build = [&] {
        SigningRequest request;
        request.method  = "GET";
        request.uri     = "/test.txt";
        request.headers = {
            {"host", kHost},
            {"range", "bytes=0-9"},
            {"x-amz-content-sha256", kEmptySha256},
            {"x-amz-date", kAmzDate},
            {"authorization", authorizationHeader(signedHeaders, signature)},
        };
        return request;
    };

    REQUIRE_NOTHROW(authenticate(build(), {}, exampleCredentials(), defaultOptions()));

    SECTION("a changed path") {
        SigningRequest request = build();
        request.uri            = "/other.txt";
        REQUIRE_THROWS_AS(authenticate(request, {}, exampleCredentials(), defaultOptions()),
                          S3Exception);
    }

    SECTION("a changed header value") {
        SigningRequest request = build();
        request.headers[1].second = "bytes=0-19";
        REQUIRE_THROWS_AS(authenticate(request, {}, exampleCredentials(), defaultOptions()),
                          S3Exception);
    }

    SECTION("an added query parameter") {
        SigningRequest request = build();
        request.query          = "versionId=2";
        REQUIRE_THROWS_AS(authenticate(request, parseQuery(request.query), exampleCredentials(),
                                       defaultOptions()),
                          S3Exception);
    }

    SECTION("a different secret key") {
        REQUIRE_THROWS_AS(
            authenticate(build(), {}, Credentials{kAccessKey, "not-the-right-secret"},
                         defaultOptions()),
            S3Exception);
    }

    SECTION("an unknown access key") {
        REQUIRE_THROWS_AS(
            authenticate(build(), {}, Credentials{"AKIAOTHER", kSecretKey}, defaultOptions()),
            S3Exception);
    }
}

TEST_CASE("a signed header that was not sent is refused rather than skipped", "[sigv4]") {
    SigningRequest request;
    request.method  = "GET";
    request.uri     = "/test.txt";
    request.headers = {{"host", kHost}};

    // Silently omitting the missing header would produce a canonical request
    // the client never signed — and, worse, one an attacker could arrange.
    REQUIRE_THROWS_AS(buildCanonicalRequest(request, {"host", "x-amz-acl"}, kEmptySha256),
                      S3Exception);
}

TEST_CASE("host must be among the signed headers", "[sigv4]") {
    SigningRequest request;
    request.method  = "GET";
    request.uri     = "/test.txt";
    request.headers = {
        {"host", kHost},
        {"x-amz-content-sha256", kEmptySha256},
        {"x-amz-date", kAmzDate},
        {"authorization", authorizationHeader("x-amz-content-sha256;x-amz-date", "deadbeef")},
    };

    // Without host in the signature the same signature would authenticate the
    // request against any endpoint these credentials reach.
    REQUIRE_THROWS_AS(authenticate(request, {}, exampleCredentials(), defaultOptions()),
                      S3Exception);
}

TEST_CASE("a request signed too long ago is refused for skew", "[sigv4]") {
    SigningRequest request;
    request.method  = "GET";
    request.uri     = "/test.txt";
    request.headers = {
        {"host", kHost},
        {"range", "bytes=0-9"},
        {"x-amz-content-sha256", kEmptySha256},
        {"x-amz-date", kAmzDate},
        {"authorization",
         authorizationHeader("host;range;x-amz-content-sha256;x-amz-date",
                             "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41")},
    };

    AuthOptions options = defaultOptions();

    // Inside the window from either side.
    options.nowSeconds = kExampleNow + limits::kSignatureSkewSeconds;
    REQUIRE_NOTHROW(authenticate(request, {}, exampleCredentials(), options));
    options.nowSeconds = kExampleNow - limits::kSignatureSkewSeconds;
    REQUIRE_NOTHROW(authenticate(request, {}, exampleCredentials(), options));

    // And outside it, in both directions: a clock that is ahead is as much of a
    // replay risk as one that is behind.
    options.nowSeconds = kExampleNow + limits::kSignatureSkewSeconds + 1;
    REQUIRE_THROWS_AS(authenticate(request, {}, exampleCredentials(), options), S3Exception);
    options.nowSeconds = kExampleNow - limits::kSignatureSkewSeconds - 1;
    REQUIRE_THROWS_AS(authenticate(request, {}, exampleCredentials(), options), S3Exception);
}

TEST_CASE("the credential scope date must agree with the request date", "[sigv4]") {
    SigningRequest request;
    request.method  = "GET";
    request.uri     = "/test.txt";
    request.headers = {
        {"host", kHost},
        {"x-amz-content-sha256", kEmptySha256},
        {"x-amz-date", kAmzDate},
        // Yesterday's scope with today's timestamp: a replay of a signature
        // computed under a key derived for a different day.
        {"authorization",
         authorizationHeader("host;x-amz-content-sha256;x-amz-date", "deadbeef", "20130523")},
    };

    REQUIRE_THROWS_AS(authenticate(request, {}, exampleCredentials(), defaultOptions()),
                      S3Exception);
}

TEST_CASE("x-amz-content-sha256 is required on a signed request", "[sigv4]") {
    SigningRequest request;
    request.method  = "GET";
    request.uri     = "/test.txt";
    request.headers = {
        {"host", kHost},
        {"x-amz-date", kAmzDate},
        {"authorization", authorizationHeader("host;x-amz-date", "deadbeef")},
    };

    // Defaulting a missing header to UNSIGNED-PAYLOAD would let a client opt
    // out of body integrity by leaving it off.
    REQUIRE_THROWS_AS(authenticate(request, {}, exampleCredentials(), defaultOptions()),
                      S3Exception);
}

TEST_CASE("the streaming payload markers are recognised", "[sigv4]") {
    // Self-signed rather than taken from a published vector: the algorithm is
    // already pinned by the four AWS examples above, so what is under test here
    // is only which PayloadMode a marker selects.
    const auto outcomeFor = [](const char* marker) {
        const std::string signedHeaders = "host;x-amz-content-sha256;x-amz-date";

        SigningRequest request;
        request.method  = "PUT";
        request.uri     = "/bucket/key";
        request.headers = {
            {"host", kHost},
            {"x-amz-content-sha256", marker},
            {"x-amz-date", kAmzDate},
        };

        const CanonicalRequest canonical = buildCanonicalRequest(
            request, {"host", "x-amz-content-sha256", "x-amz-date"}, marker);
        const std::string key = deriveSigningKey(kSecretKey, "20130524", "us-east-1", "s3");
        const std::string raw =
            hmacSha256(key, stringToSign(kAmzDate, kScope, sha256Hex(canonical.render())));
        const std::string signature = toHex(std::span<const unsigned char>(
            reinterpret_cast<const unsigned char*>(raw.data()), raw.size()));

        request.headers.emplace_back("authorization",
                                     authorizationHeader(signedHeaders, signature));
        return authenticate(request, {}, exampleCredentials(), defaultOptions());
    };

    REQUIRE(outcomeFor(kStreamingSigned).payload == PayloadMode::StreamingSigned);
    REQUIRE(outcomeFor(kStreamingSignedTrailer).payload == PayloadMode::StreamingSigned);
    REQUIRE(outcomeFor(kStreamingUnsignedTrailer).payload == PayloadMode::StreamingUnsigned);
    REQUIRE(outcomeFor(kUnsignedPayload).payload == PayloadMode::Unsigned);

    // The seed signature and derived key travel with the outcome so the chunk
    // decoder can follow the chain without seeing the secret.
    const AuthOutcome streaming = outcomeFor(kStreamingSigned);
    REQUIRE(streaming.signingKey.size() == 32);
    REQUIRE(streaming.signature.size() == 64);
}

TEST_CASE("timestamps round-trip and impossible dates are refused", "[sigv4]") {
    REQUIRE(parseAmzDate("20130524T000000Z") == kExampleNow);
    REQUIRE(formatAmzDate(kExampleNow) == "20130524T000000Z");

    REQUIRE_FALSE(parseAmzDate("20130524T000000").has_value());   // no zone
    REQUIRE_FALSE(parseAmzDate("2013-05-24T00:00:00Z").has_value());
    REQUIRE_FALSE(parseAmzDate("").has_value());
    // timegm() normalises rather than rejecting, so 30 February would silently
    // become 2 March without the round-trip check.
    REQUIRE_FALSE(parseAmzDate("20130230T000000Z").has_value());
    REQUIRE_FALSE(parseAmzDate("2013052xT000000Z").has_value());
}

TEST_CASE("signature comparison does not short-circuit on length or content", "[sigv4]") {
    REQUIRE(secureEquals("abc", "abc"));
    REQUIRE_FALSE(secureEquals("abc", "abd"));
    REQUIRE_FALSE(secureEquals("abc", "ab"));
    REQUIRE(secureEquals("", ""));
}
