#include <catch2/catch_test_macros.hpp>

#include "s3/base64.hpp"
#include "s3/uri.hpp"

using namespace monobucket::s3;

TEST_CASE("uriEncode follows the AWS unreserved set, not RFC 3986's", "[s3][uri]") {
    REQUIRE(uriEncode("abcXYZ019-_.~", true) == "abcXYZ019-_.~");

    // The four characters RFC 3986 also leaves unreserved in some contexts but
    // AWS does not. Getting any of them wrong breaks signing for keys that
    // contain them, and for nothing else.
    REQUIRE(uriEncode("a b", true) == "a%20b");
    REQUIRE(uriEncode("a+b", true) == "a%2Bb");
    REQUIRE(uriEncode("a*b", true) == "a%2Ab");
    REQUIRE(uriEncode("a!b", true) == "a%21b");

    // Hex digits are uppercase, and that is part of the byte string being
    // hashed rather than a stylistic choice.
    REQUIRE(uriEncode("\x01", true) == "%01");
    REQUIRE(uriEncode("\xff", true) == "%FF");
}

TEST_CASE("a path keeps its separators and a query parameter does not", "[s3][uri]") {
    REQUIRE(uriEncode("photos/2026/summer.jpg", false) == "photos/2026/summer.jpg");
    REQUIRE(uriEncode("photos/2026/summer.jpg", true) == "photos%2F2026%2Fsummer.jpg");
}

TEST_CASE("uriDecode reverses escapes and leaves a plus alone", "[s3][uri]") {
    REQUIRE(uriDecode("a%20b") == "a b");
    REQUIRE(uriDecode("%2F") == "/");
    REQUIRE(uriDecode("%2f") == "/");

    // Treating '+' as a space here would rewrite a signed byte string into a
    // different one and reject a valid request.
    REQUIRE(uriDecode("a+b") == "a+b");

    // A key is allowed to contain a literal percent sign.
    REQUIRE(uriDecode("100%") == "100%");
    REQUIRE(uriDecode("%zz") == "%zz");
    REQUIRE(uriDecode("%2") == "%2");
}

TEST_CASE("encoding a decoded path round-trips", "[s3][uri]") {
    for (const char* key : {"plain.txt", "with space.txt", "with%2Fescape", "unicode-\xc3\xa9",
                            "a+b", "100%"}) {
        REQUIRE(uriDecode(uriEncode(key, true)) == key);
    }
}

TEST_CASE("parseQuery separates bare flags from empty values", "[s3][uri]") {
    const auto params = parseQuery("uploads&prefix=&max-keys=10");
    REQUIRE(params.size() == 3);

    REQUIRE(params[0].name == "uploads");
    REQUIRE_FALSE(params[0].hasValue);

    REQUIRE(params[1].name == "prefix");
    REQUIRE(params[1].hasValue);
    REQUIRE(params[1].value.empty());

    REQUIRE(params[2].value == "10");
}

TEST_CASE("parseQuery decodes names and values", "[s3][uri]") {
    const auto params = parseQuery("prefix=a%2Fb&key%2Dname=v%20w");
    REQUIRE(findQuery(params, "prefix") == "a/b");
    REQUIRE(findQuery(params, "key-name") == "v w");
}

TEST_CASE("parseQuery tolerates the shapes clients actually send", "[s3][uri]") {
    REQUIRE(parseQuery("").empty());
    REQUIRE(parseQuery("?").empty());
    REQUIRE(parseQuery("&&").empty());
    REQUIRE(parseQuery("?delete").size() == 1);

    // A value containing '=' keeps everything after the first one.
    const auto params = parseQuery("token=a=b=c");
    REQUIRE(findQuery(params, "token") == "a=b=c");
}

TEST_CASE("the canonical query string sorts and re-encodes", "[s3][uri]") {
    REQUIRE(canonicalQueryString(parseQuery("prefix=J&max-keys=2")) == "max-keys=2&prefix=J");

    // A bare flag renders with a trailing '='.
    REQUIRE(canonicalQueryString(parseQuery("uploads")) == "uploads=");

    // Sorting is by the encoded name, then the encoded value.
    REQUIRE(canonicalQueryString(parseQuery("a=2&a=1&b=0")) == "a=1&a=2&b=0");

    REQUIRE(canonicalQueryString(parseQuery("key=a/b")) == "key=a%2Fb");
}

TEST_CASE("the signature parameter is excluded from its own canonical form", "[s3][uri]") {
    const auto params = parseQuery("X-Amz-Date=20260816T000000Z&X-Amz-Signature=abc&a=1");
    const std::string canonical = canonicalQueryString(params, "X-Amz-Signature");

    REQUIRE(canonical.find("X-Amz-Signature") == std::string::npos);
    REQUIRE(canonical == "X-Amz-Date=20260816T000000Z&a=1");
}

TEST_CASE("base64 round-trips and rejects malformed input", "[s3][base64]") {
    REQUIRE(base64Encode("") == "");
    REQUIRE(base64Encode("f") == "Zg==");
    REQUIRE(base64Encode("fo") == "Zm8=");
    REQUIRE(base64Encode("foo") == "Zm9v");
    REQUIRE(base64Encode("foobar") == "Zm9vYmFy");

    REQUIRE(base64Decode("Zm9vYmFy") == "foobar");
    REQUIRE(base64Decode("Zg==") == "f");

    // Binary survives, which matters because Content-MD5 is a raw digest.
    const std::string binary("\x00\x01\xfe\xff", 4);
    REQUIRE(base64Decode(base64Encode(binary)) == binary);

    REQUIRE_FALSE(base64Decode("Zm9vYmF").has_value());   // not a multiple of four
    REQUIRE_FALSE(base64Decode("Zm9v!mFy").has_value());  // illegal character
    REQUIRE_FALSE(base64Decode("Z===").has_value());      // over-padded
    REQUIRE_FALSE(base64Decode("Zg==Zg==").has_value());  // data after padding
    REQUIRE_FALSE(base64Decode("").has_value());
}
