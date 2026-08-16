#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

// A minimal length-prefixed record encoding for values stored in RocksDB.
//
// Why not JSON, when nlohmann is already linked: a ListObjectsV2 page decodes
// up to 1000 records per request, and metadata rows outnumber objects. Varints
// and length-prefixed strings decode with no allocation beyond the strings
// themselves and roughly halve the stored size, which is also cache-block
// occupancy. The format is versioned so a future field can be added without a
// migration.

namespace monobucket::codec {

/// Raised when a stored record cannot be decoded — corruption, or a record
/// written by a newer build than the one reading it.
class DecodeError : public std::runtime_error {
public:
    explicit DecodeError(const std::string& what) : std::runtime_error("corrupt record: " + what) {}
};

/// Appends to a growing buffer. Deliberately not a stream: every record is
/// small and built in one go.
class Writer {
public:
    explicit Writer(std::string& out) : out_(out) {}

    void u8(std::uint8_t value);
    void varint(std::uint64_t value);
    void string(std::string_view value);
    void boolean(bool value);

private:
    std::string& out_;
};

/// Reads back what Writer produced. Every accessor validates before consuming,
/// so a truncated or malformed value throws rather than reading past the end.
class Reader {
public:
    explicit Reader(std::string_view in) : in_(in) {}

    std::uint8_t     u8();
    std::uint64_t    varint();
    std::string      string();
    std::string_view stringView();
    bool             boolean();

    bool             exhausted() const noexcept { return offset_ >= in_.size(); }
    std::size_t      remaining() const noexcept { return in_.size() - offset_; }

    /// Skips any fields a newer writer appended that this build does not know
    /// about. Forward compatibility is the reason records carry a version byte.
    void skipRemaining() noexcept { offset_ = in_.size(); }

private:
    void require(std::size_t bytes) const;

    std::string_view in_;
    std::size_t      offset_ = 0;
};

}  // namespace monobucket::codec
