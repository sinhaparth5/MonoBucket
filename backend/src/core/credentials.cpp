#include "core/credentials.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

#include <openssl/rand.h>

namespace monobucket::credentials {
namespace {

/// Base32's alphabet for ids: no lowercase, and no 0/1/8/9 to confuse with
/// O/I/B/g when a key is read off a screen.
constexpr std::string_view kIdAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

/// Full alphanumeric for secrets, which are copied rather than transcribed.
constexpr std::string_view kSecretAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

/// Uniform over `alphabet`. Rejection sampling rather than `% size`: a modulo
/// over 256 favours the first few characters, which is a small bias that a
/// credential generator has no reason to accept.
std::string draw(std::string_view alphabet, std::size_t length) {
    const auto size = static_cast<unsigned>(alphabet.size());

    // Widened deliberately. An alphabet whose size divides 256 leaves no
    // remainder, so the bound is 256 itself — which truncates to zero in a
    // narrower type and rejects every byte, spinning forever.
    const unsigned limit = 256U - (256U % size);

    std::string out;
    out.reserve(length);

    std::vector<unsigned char> buffer(length);
    while (out.size() < length) {
        if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
            throw std::runtime_error("the system random source is unavailable");
        }
        for (const unsigned char byte : buffer) {
            if (static_cast<unsigned>(byte) >= limit) continue;
            out.push_back(alphabet[byte % size]);
            if (out.size() == length) break;
        }
    }
    return out;
}

}  // namespace

std::string generateAccessKeyId() {
    std::string id(kAccessKeyIdPrefix);
    id += draw(kIdAlphabet, kAccessKeyIdLength - kAccessKeyIdPrefix.size());
    return id;
}

std::string generateSecretKey() { return draw(kSecretAlphabet, kSecretKeyLength); }

bool plausibleAccessKeyId(std::string_view id) {
    if (id.size() != kAccessKeyIdLength) return false;
    if (!id.starts_with(kAccessKeyIdPrefix)) return false;
    return std::all_of(id.begin() + kAccessKeyIdPrefix.size(), id.end(), [](const char ch) {
        return kIdAlphabet.find(ch) != std::string_view::npos;
    });
}

}  // namespace monobucket::credentials
