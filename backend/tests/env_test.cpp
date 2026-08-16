#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#include "core/env.hpp"

using monobucket::env::formatBytes;
using monobucket::env::parseBoolean;
using monobucket::env::parseBytes;
using monobucket::env::parseNumber;

TEST_CASE("boolean parsing accepts the usual spellings", "[env]") {
    for (const char* truthy : {"1", "true", "TRUE", "yes", "On", " true "}) {
        CHECK(parseBoolean(truthy));
    }
    for (const char* falsy : {"0", "false", "NO", "off"}) {
        CHECK_FALSE(parseBoolean(falsy));
    }
    CHECK_THROWS_AS(parseBoolean("maybe"), std::invalid_argument);
    CHECK_THROWS_AS(parseBoolean(""), std::invalid_argument);
}

TEST_CASE("number parsing rejects anything that is not a plain integer", "[env]") {
    CHECK(parseNumber("0") == 0);
    CHECK(parseNumber(" 9000 ") == 9000);
    CHECK_THROWS_AS(parseNumber("-1"), std::invalid_argument);
    CHECK_THROWS_AS(parseNumber("9000x"), std::invalid_argument);
    CHECK_THROWS_AS(parseNumber(""), std::invalid_argument);
}

TEST_CASE("byte sizes distinguish binary from SI units", "[env]") {
    CHECK(parseBytes("512") == 512);
    CHECK(parseBytes("512B") == 512);

    // Binary
    CHECK(parseBytes("1K") == 1024);
    CHECK(parseBytes("1KiB") == 1024);
    CHECK(parseBytes("1Ki") == 1024);
    CHECK(parseBytes("64MiB") == 64ull * 1024 * 1024);
    CHECK(parseBytes("2GiB") == 2ull * 1024 * 1024 * 1024);
    CHECK(parseBytes("1TiB") == 1024ull * 1024 * 1024 * 1024);

    // SI
    CHECK(parseBytes("1KB") == 1000);
    CHECK(parseBytes("200MB") == 200ull * 1000 * 1000);
    CHECK(parseBytes("1GB") == 1000ull * 1000 * 1000);
}

TEST_CASE("byte sizes accept decimals and spacing", "[env]") {
    CHECK(parseBytes("1.5GiB") == 1610612736ull);
    CHECK(parseBytes("64 MiB") == 64ull * 1024 * 1024);
    CHECK(parseBytes(" 8m ") == 8ull * 1024 * 1024);
}

TEST_CASE("malformed byte sizes are rejected rather than silently defaulted", "[env]") {
    CHECK_THROWS_AS(parseBytes("MiB"), std::invalid_argument);
    CHECK_THROWS_AS(parseBytes("10PB"), std::invalid_argument);
    CHECK_THROWS_AS(parseBytes("1.2.3M"), std::invalid_argument);
    CHECK_THROWS_AS(parseBytes(""), std::invalid_argument);
}

TEST_CASE("formatBytes renders a compact binary string", "[env]") {
    CHECK(formatBytes(512) == "512 B");
    CHECK(formatBytes(1024) == "1.0 KiB");
    CHECK(formatBytes(64ull * 1024 * 1024) == "64.0 MiB");
    CHECK(formatBytes(1610612736ull) == "1.5 GiB");
}

TEST_CASE("environment lookups treat empty as unset", "[env]") {
    ::setenv("MONOBUCKET_TEST_EMPTY", "", 1);
    CHECK(monobucket::env::string("MONOBUCKET_TEST_EMPTY", "fallback") == "fallback");

    ::setenv("MONOBUCKET_TEST_SIZE", "32MiB", 1);
    CHECK(monobucket::env::bytes("MONOBUCKET_TEST_SIZE", 0) == 32ull * 1024 * 1024);

    ::setenv("MONOBUCKET_TEST_SIZE", "not-a-size", 1);
    CHECK_THROWS_AS(monobucket::env::bytes("MONOBUCKET_TEST_SIZE", 0),
                    monobucket::env::ParseError);

    ::unsetenv("MONOBUCKET_TEST_EMPTY");
    ::unsetenv("MONOBUCKET_TEST_SIZE");
}
