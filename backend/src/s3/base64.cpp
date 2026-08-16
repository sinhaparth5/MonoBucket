#include "s3/base64.hpp"

namespace monobucket::s3 {
namespace {

constexpr const char* kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int valueOf(unsigned char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::string base64Encode(std::string_view data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t triple = (static_cast<unsigned char>(data[i]) << 16) |
                                     (static_cast<unsigned char>(data[i + 1]) << 8) |
                                     static_cast<unsigned char>(data[i + 2]);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
    }

    if (const std::size_t remaining = data.size() - i; remaining > 0) {
        std::uint32_t triple = static_cast<unsigned char>(data[i]) << 16;
        if (remaining == 2) triple |= static_cast<unsigned char>(data[i + 1]) << 8;

        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(remaining == 2 ? kAlphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back('=');
    }

    return out;
}

std::optional<std::string> base64Decode(std::string_view text) {
    if (text.empty() || text.size() % 4 != 0) return std::nullopt;

    std::string out;
    out.reserve(text.size() / 4 * 3);

    for (std::size_t i = 0; i < text.size(); i += 4) {
        int values[4]{};
        int padding = 0;

        for (int j = 0; j < 4; ++j) {
            const char ch = text[i + j];
            if (ch == '=') {
                // Padding is legal only in the final group, and only in the
                // last two positions.
                if (i + 4 != text.size() || j < 2) return std::nullopt;
                ++padding;
                continue;
            }
            if (padding > 0) return std::nullopt;  // data after padding
            values[j] = valueOf(static_cast<unsigned char>(ch));
            if (values[j] < 0) return std::nullopt;
        }

        const std::uint32_t triple = (static_cast<std::uint32_t>(values[0]) << 18) |
                                     (static_cast<std::uint32_t>(values[1]) << 12) |
                                     (static_cast<std::uint32_t>(values[2]) << 6) |
                                     static_cast<std::uint32_t>(values[3]);

        out.push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (padding < 2) out.push_back(static_cast<char>((triple >> 8) & 0xFF));
        if (padding < 1) out.push_back(static_cast<char>(triple & 0xFF));
    }

    return out;
}

}  // namespace monobucket::s3
