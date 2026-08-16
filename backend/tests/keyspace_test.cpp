#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "storage/keyspace.hpp"

namespace keys = monobucket::keys;

TEST_CASE("object keys sort in the order S3 lists them", "[keyspace]") {
    // ListObjectsV2 is specified to return keys in UTF-8 binary order, and the
    // implementation gets that from RocksDB's byte ordering rather than from a
    // sort — so the encoding has to preserve it.
    std::vector<std::string> encoded;
    for (const char* key : {"a", "a/b", "a/b/c", "ab", "b", "z"}) {
        encoded.push_back(keys::object("bucket", key));
    }

    std::vector<std::string> sorted = encoded;
    std::sort(sorted.begin(), sorted.end());
    CHECK(sorted == encoded);
}

TEST_CASE("one bucket's objects cannot bleed into another's", "[keyspace]") {
    // The separator is why: `photos` and `photos2` would otherwise share a
    // prefix, and listing the first would return the second's keys.
    const std::string shortPrefix = keys::objectPrefix("photos");
    const std::string longKey     = keys::object("photos2", "cat.jpg");

    CHECK_FALSE(longKey.starts_with(shortPrefix));
    CHECK(keys::object("photos", "cat.jpg").starts_with(shortPrefix));
}

TEST_CASE("the object key survives encoding", "[keyspace]") {
    const std::string key = "deep/nested/path with spaces/Ünicode.txt";

    // The result borrows from `encoded`, so it has to outlive the check —
    // passing keys::object(...) inline would leave a view into a destroyed
    // temporary.
    const std::string encoded = keys::object("bucket", key);

    const auto recovered = keys::objectKey(encoded, "bucket");
    REQUIRE(recovered.has_value());
    CHECK(*recovered == key);

    // Asking for the wrong bucket must not produce a plausible-looking answer.
    CHECK_FALSE(keys::objectKey(encoded, "other").has_value());
}

TEST_CASE("part numbers iterate in numeric order", "[keyspace]") {
    // Big-endian encoding is load-bearing: decimal or little-endian ordering
    // would concatenate a completed upload's parts in the wrong sequence.
    const std::string second = keys::part("upload", 2);
    const std::string tenth  = keys::part("upload", 10);
    const std::string last   = keys::part("upload", 10000);

    CHECK(second < tenth);
    CHECK(tenth < last);

    CHECK(keys::partNumber(second, "upload") == 2u);
    CHECK(keys::partNumber(tenth, "upload") == 10u);
    CHECK(keys::partNumber(last, "upload") == 10000u);
}

TEST_CASE("upload keys split back into their parts", "[keyspace]") {
    const std::string encoded = keys::uploadByKey("bucket", "a/b/c.txt", "UPLOADID");

    const auto parts = keys::uploadKeyParts(encoded, "bucket");
    REQUIRE(parts.has_value());
    CHECK(parts->key == "a/b/c.txt");
    CHECK(parts->uploadId == "UPLOADID");
}

TEST_CASE("the upper bound excludes exactly the prefix", "[keyspace]") {
    const auto bound = keys::upperBound("abc");
    REQUIRE(bound.has_value());
    CHECK(*bound == "abd");

    // Everything under the prefix must fall below the bound...
    CHECK(std::string("abc") < *bound);
    CHECK(std::string("abcZZZ") < *bound);
    // ...and the next sibling must not.
    CHECK_FALSE(std::string("abd") < *bound);
}

TEST_CASE("the upper bound carries across trailing 0xFF", "[keyspace]") {
    const std::string prefix = std::string("a") + char(0xFF) + char(0xFF);

    const auto bound = keys::upperBound(prefix);
    REQUIRE(bound.has_value());
    CHECK(*bound == "b");
    CHECK(prefix < *bound);
}

TEST_CASE("an all-0xFF prefix has no upper bound", "[keyspace]") {
    // There is no representable successor, so callers must fall back to
    // checking the prefix per key rather than trusting an iterator bound.
    CHECK_FALSE(keys::upperBound(std::string(3, char(0xFF))).has_value());
    CHECK_FALSE(keys::upperBound("").has_value());
}

TEST_CASE("record types occupy disjoint regions", "[keyspace]") {
    // A scan for one type must never see another, since the startup counter
    // scan dispatches purely on the leading tag byte.
    CHECK(keys::bucket("x").front() == keys::kBucket);
    CHECK(keys::object("x", "y").front() == keys::kObject);
    CHECK(keys::uploadById("x").front() == keys::kUploadById);
    CHECK(keys::uploadByKey("x", "y", "z").front() == keys::kUploadByKey);
    CHECK(keys::part("x", 1).front() == keys::kPart);
    CHECK(keys::orphan("x").front() == keys::kOrphan);
    CHECK(keys::meta("x").front() == keys::kMeta);
}
