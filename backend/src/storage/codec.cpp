#include "storage/codec.hpp"

namespace monobucket::codec {

void Writer::u8(std::uint8_t value) { out_.push_back(static_cast<char>(value)); }

void Writer::varint(std::uint64_t value) {
    // LEB128: seven bits per byte, high bit set while more follow. Small values
    // — which is almost all of them: part numbers, timestamps, sizes — cost one
    // to five bytes instead of a fixed eight.
    while (value >= 0x80) {
        out_.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out_.push_back(static_cast<char>(value));
}

void Writer::string(std::string_view value) {
    varint(value.size());
    out_.append(value);
}

void Writer::boolean(bool value) { u8(value ? 1 : 0); }

void Reader::require(std::size_t bytes) const {
    if (offset_ + bytes > in_.size()) {
        throw DecodeError("truncated after " + std::to_string(offset_) + " of " +
                          std::to_string(in_.size()) + " bytes");
    }
}

std::uint8_t Reader::u8() {
    require(1);
    return static_cast<std::uint8_t>(in_[offset_++]);
}

std::uint64_t Reader::varint() {
    std::uint64_t value = 0;
    unsigned      shift = 0;
    while (true) {
        require(1);
        const auto byte = static_cast<std::uint8_t>(in_[offset_++]);
        // Ten groups of seven bits is the most a 64-bit value can occupy;
        // anything longer is corruption, and continuing would silently wrap.
        if (shift > 63) throw DecodeError("varint exceeds 64 bits");
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
    }
    return value;
}

std::string_view Reader::stringView() {
    const std::uint64_t length = varint();
    if (length > in_.size() - offset_) {
        throw DecodeError("string length " + std::to_string(length) + " exceeds the remaining " +
                          std::to_string(in_.size() - offset_) + " bytes");
    }
    const auto view = in_.substr(offset_, static_cast<std::size_t>(length));
    offset_ += static_cast<std::size_t>(length);
    return view;
}

std::string Reader::string() { return std::string(stringView()); }

bool Reader::boolean() { return u8() != 0; }

}  // namespace monobucket::codec
