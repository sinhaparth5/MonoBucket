#include <catch2/catch_test_macros.hpp>

#include "s3/request.hpp"

using namespace monobucket::s3;

TEST_CASE("path style splits into bucket and key", "[s3][request]") {
    SECTION("service root") {
        const S3Request request = parseRequest("GET", "/", "", "");
        REQUIRE(request.bucket.empty());
        REQUIRE(request.key.empty());
    }

    SECTION("bucket") {
        const S3Request request = parseRequest("GET", "/photos", "", "");
        REQUIRE(request.bucket == "photos");
        REQUIRE(request.key.empty());
    }

    SECTION("bucket with a trailing slash is still the bucket") {
        const S3Request request = parseRequest("GET", "/photos/", "", "");
        REQUIRE(request.bucket == "photos");
        REQUIRE(request.key.empty());
    }

    SECTION("a key keeps its own separators") {
        const S3Request request = parseRequest("GET", "/photos/2026/summer/beach.jpg", "", "");
        REQUIRE(request.bucket == "photos");
        REQUIRE(request.key == "2026/summer/beach.jpg");
    }
}

TEST_CASE("virtual-host style takes the bucket from the Host header", "[s3][request]") {
    const S3Request request = parseRequest("GET", "/beach.jpg", "", "photos");
    REQUIRE(request.bucket == "photos");
    // The whole path is the key: none of it names the bucket.
    REQUIRE(request.key == "beach.jpg");
}

TEST_CASE("virtual-host detection needs a configured domain", "[s3][request]") {
    // Without one, `Host: localhost` would address a bucket called "localhost"
    // and there would be no way to tell that apart from the real thing.
    REQUIRE(virtualHostBucket("photos.s3.example.com", "").empty());
    REQUIRE(virtualHostBucket("localhost:9000", "").empty());

    REQUIRE(virtualHostBucket("photos.s3.example.com", "s3.example.com") == "photos");
    REQUIRE(virtualHostBucket("photos.s3.example.com:9000", "s3.example.com") == "photos");

    // The endpoint itself is path style.
    REQUIRE(virtualHostBucket("s3.example.com", "s3.example.com").empty());
    REQUIRE(virtualHostBucket("s3.example.com:9000", "s3.example.com").empty());

    // A different domain is not ours to interpret.
    REQUIRE(virtualHostBucket("photos.s3.other.com", "s3.example.com").empty());

    // A prefix that could not be a bucket name is more likely another service
    // on the same domain than a bucket we should invent.
    REQUIRE(virtualHostBucket("UPPER.s3.example.com", "s3.example.com") == "upper");
    REQUIRE(virtualHostBucket("x.s3.example.com", "s3.example.com").empty());

    // A dotted bucket name is legal and addressable.
    REQUIRE(virtualHostBucket("my.photos.s3.example.com", "s3.example.com") == "my.photos");
}

TEST_CASE("bucket names follow the DNS-compatible rules", "[s3][request]") {
    REQUIRE(isValidBucketName("photos"));
    REQUIRE(isValidBucketName("my-bucket-2026"));
    REQUIRE(isValidBucketName("my.bucket"));
    REQUIRE(isValidBucketName("abc"));
    REQUIRE(isValidBucketName(std::string(63, 'a')));

    REQUIRE_FALSE(isValidBucketName("ab"));                     // too short
    REQUIRE_FALSE(isValidBucketName(std::string(64, 'a')));     // too long
    REQUIRE_FALSE(isValidBucketName("Photos"));                 // uppercase
    REQUIRE_FALSE(isValidBucketName("-photos"));                // must start alphanumeric
    REQUIRE_FALSE(isValidBucketName("photos-"));                // must end alphanumeric
    REQUIRE_FALSE(isValidBucketName("my..bucket"));             // consecutive dots
    REQUIRE_FALSE(isValidBucketName("my_bucket"));              // underscore
    REQUIRE_FALSE(isValidBucketName("photos/2026"));            // separator
    REQUIRE_FALSE(isValidBucketName(""));

    // A name shaped like an address would be ambiguous with the endpoint in a
    // virtual-host URL.
    REQUIRE_FALSE(isValidBucketName("192.168.0.1"));
    REQUIRE(isValidBucketName("192.168.0.1.x"));
}

TEST_CASE("names the server already answers on are reserved", "[s3][request]") {
    // Drogon matches an exact path before the catch-all, so a bucket with one
    // of these names would exist and be unreachable over HTTP.
    REQUIRE(isReservedBucketName("healthz"));
    REQUIRE(isReservedBucketName("readyz"));
    REQUIRE(isReservedBucketName("metrics"));
    REQUIRE(isReservedBucketName("_mb"));
    REQUIRE_FALSE(isReservedBucketName("photos"));
}

TEST_CASE("object keys are bounded and free of unrepresentable characters", "[s3][request]") {
    REQUIRE(isValidObjectKey("a"));
    REQUIRE(isValidObjectKey("2026/summer/beach.jpg"));
    REQUIRE(isValidObjectKey("with space.txt"));
    REQUIRE(isValidObjectKey("unicode-\xc3\xa9.txt"));
    REQUIRE(isValidObjectKey(std::string(1024, 'k')));

    REQUIRE_FALSE(isValidObjectKey(""));
    REQUIRE_FALSE(isValidObjectKey(std::string(1025, 'k')));
    REQUIRE_FALSE(isValidObjectKey(std::string("nul\0key", 7)));
    REQUIRE_FALSE(isValidObjectKey("newline\nkey"));

    // Payloads are named by blob id rather than by key, so traversal never
    // reaches the filesystem — but it would still be a key no client could
    // address unambiguously.
    REQUIRE_FALSE(isValidObjectKey(".."));
    REQUIRE_FALSE(isValidObjectKey("../etc/passwd"));
    REQUIRE_FALSE(isValidObjectKey("a/../../b"));
}

TEST_CASE("Range headers resolve against the object size", "[s3][request]") {
    ByteRange range;

    SECTION("a closed range") {
        REQUIRE(parseRange("bytes=0-9", 100, range) == RangeResult::Satisfiable);
        REQUIRE(range.offset == 0);
        REQUIRE(range.length == 10);
    }

    SECTION("an open-ended range runs to the end") {
        REQUIRE(parseRange("bytes=90-", 100, range) == RangeResult::Satisfiable);
        REQUIRE(range.offset == 90);
        REQUIRE(range.length == 10);
    }

    SECTION("a suffix range counts back from the end") {
        REQUIRE(parseRange("bytes=-10", 100, range) == RangeResult::Satisfiable);
        REQUIRE(range.offset == 90);
        REQUIRE(range.length == 10);
    }

    SECTION("a suffix larger than the object is the whole object") {
        REQUIRE(parseRange("bytes=-500", 100, range) == RangeResult::Satisfiable);
        REQUIRE(range.offset == 0);
        REQUIRE(range.length == 100);
    }

    SECTION("an end past the object is clamped, not refused") {
        REQUIRE(parseRange("bytes=50-999", 100, range) == RangeResult::Satisfiable);
        REQUIRE(range.offset == 50);
        REQUIRE(range.length == 50);
    }

    SECTION("a start past the object is unsatisfiable") {
        REQUIRE(parseRange("bytes=100-", 100, range) == RangeResult::Unsatisfiable);
        REQUIRE(parseRange("bytes=0-0", 0, range) == RangeResult::Unsatisfiable);
        REQUIRE(parseRange("bytes=-0", 100, range) == RangeResult::Unsatisfiable);
        REQUIRE(parseRange("bytes=20-10", 100, range) == RangeResult::Unsatisfiable);
    }

    SECTION("anything we do not implement is ignored, as S3 ignores it") {
        REQUIRE(parseRange("", 100, range) == RangeResult::Absent);
        REQUIRE(parseRange("items=0-9", 100, range) == RangeResult::Absent);
        REQUIRE(parseRange("bytes=0-9,20-29", 100, range) == RangeResult::Absent);
        REQUIRE(parseRange("bytes=abc", 100, range) == RangeResult::Absent);
        REQUIRE(parseRange("bytes=-", 100, range) == RangeResult::Absent);
    }
}

TEST_CASE("HTTP dates render and parse in the fixed C locale", "[s3][request]") {
    // 24 May 2013 00:00:00 UTC.
    REQUIRE(toHttpDate(1369353600000) == "Fri, 24 May 2013 00:00:00 GMT");
    REQUIRE(parseHttpDate("Fri, 24 May 2013 00:00:00 GMT") == 1369353600000);

    // The other two formats HTTP permits, which older clients still send.
    REQUIRE(parseHttpDate("Friday, 24-May-13 00:00:00 GMT") == 1369353600000);
    REQUIRE(parseHttpDate("Fri May 24 00:00:00 2013") == 1369353600000);

    // A malformed conditional header is ignored rather than fatal.
    REQUIRE_FALSE(parseHttpDate("not a date").has_value());
    REQUIRE_FALSE(parseHttpDate("").has_value());
}

TEST_CASE("ETags compare with or without their quotes", "[s3][request]") {
    REQUIRE(unquoteETag("\"abc\"") == "abc");
    REQUIRE(unquoteETag("abc") == "abc");
    REQUIRE(unquoteETag("W/\"abc\"") == "abc");
    REQUIRE(quoteETag("abc") == "\"abc\"");
    REQUIRE(quoteETag("abc-3") == "\"abc-3\"");
}

TEST_CASE("user metadata header names lose their prefix", "[s3][request]") {
    REQUIRE(userMetadataKey("x-amz-meta-author") == "author");
    REQUIRE(userMetadataKey("X-Amz-Meta-Author") == "author");
    REQUIRE(userMetadataKey("x-amz-meta-") == "");
    REQUIRE(userMetadataKey("content-type").empty());
    REQUIRE(userMetadataKey("x-amz-date").empty());
}

TEST_CASE("listing values are only url-encoded when the client asks", "[s3][request]") {
    // Encoding unconditionally would break every client that does not decode.
    REQUIRE(encodeListingValue("a b/c", false) == "a b/c");
    REQUIRE(encodeListingValue("a b/c", true) == "a%20b%2Fc");
}

TEST_CASE("every request gets a distinct id", "[s3][request]") {
    const S3Request first  = parseRequest("GET", "/a", "", "");
    const S3Request second = parseRequest("GET", "/a", "", "");
    REQUIRE(first.requestId.size() == 16);
    REQUIRE(first.requestId != second.requestId);
}
