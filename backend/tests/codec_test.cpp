#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>

#include "storage/codec.hpp"

using monobucket::codec::DecodeError;
using monobucket::codec::Reader;
using monobucket::codec::Writer;

TEST_CASE("values survive a round trip", "[codec]") {
    std::string buffer;
    Writer      writer(buffer);

    writer.u8(1);
    writer.varint(0);
    writer.varint(127);
    writer.varint(128);
    writer.varint(std::numeric_limits<std::uint64_t>::max());
    writer.string("");
    writer.string("an object key with spaces and Ünicode");
    writer.boolean(true);
    writer.boolean(false);

    Reader reader(buffer);
    CHECK(reader.u8() == 1);
    CHECK(reader.varint() == 0);
    CHECK(reader.varint() == 127);
    CHECK(reader.varint() == 128);
    CHECK(reader.varint() == std::numeric_limits<std::uint64_t>::max());
    CHECK(reader.string().empty());
    CHECK(reader.string() == "an object key with spaces and Ünicode");
    CHECK(reader.boolean());
    CHECK_FALSE(reader.boolean());
    CHECK(reader.exhausted());
}

TEST_CASE("varints use the fewest bytes that fit", "[codec]") {
    const auto encodedSize = [](std::uint64_t value) {
        std::string buffer;
        Writer      writer(buffer);
        writer.varint(value);
        return buffer.size();
    };

    // The whole point of the encoding: a size or timestamp costs a few bytes
    // rather than a fixed eight, across every stored record.
    CHECK(encodedSize(0) == 1);
    CHECK(encodedSize(127) == 1);
    CHECK(encodedSize(128) == 2);
    CHECK(encodedSize(std::numeric_limits<std::uint64_t>::max()) == 10);
}

TEST_CASE("a string containing NUL round-trips intact", "[codec]") {
    // Values are length-prefixed rather than NUL-terminated, so binary content
    // in user metadata cannot truncate a record.
    const std::string awkward("before\0after", 12);

    std::string buffer;
    Writer      writer(buffer);
    writer.string(awkward);

    Reader reader(buffer);
    CHECK(reader.string() == awkward);
}

TEST_CASE("a truncated record is rejected rather than guessed at", "[codec]") {
    std::string buffer;
    Writer      writer(buffer);
    writer.string("a reasonably long object key");

    // Every prefix of a valid record must fail cleanly; silently returning a
    // short read here would surface as a corrupt object much later.
    for (std::size_t length = 1; length < buffer.size(); ++length) {
        Reader reader(std::string_view(buffer).substr(0, length));
        CHECK_THROWS_AS(reader.string(), DecodeError);
    }
}

TEST_CASE("a bogus length is rejected before it is trusted", "[codec]") {
    std::string buffer;
    Writer      writer(buffer);
    writer.varint(1'000'000);  // claims a megabyte that is not there

    Reader reader(buffer);
    CHECK_THROWS_AS(reader.string(), DecodeError);
}

TEST_CASE("an over-long varint is rejected rather than silently wrapping", "[codec]") {
    // Eleven continuation bytes: more groups of seven bits than 64 bits holds.
    const std::string corrupt(11, static_cast<char>(0xFF));

    Reader reader(corrupt);
    CHECK_THROWS_AS(reader.varint(), DecodeError);
}

TEST_CASE("reading past the end throws instead of returning garbage", "[codec]") {
    Reader reader(std::string_view{});
    CHECK(reader.exhausted());
    CHECK_THROWS_AS(reader.u8(), DecodeError);
}
