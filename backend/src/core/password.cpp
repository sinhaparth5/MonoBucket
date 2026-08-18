#include "core/password.hpp"

#include <array>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace monobucket::password {
namespace {

constexpr std::string_view kPrefix    = "pbkdf2-sha256$";
constexpr std::size_t      kSaltBytes = 16;
constexpr std::size_t      kHashBytes = 32;

/// A ceiling on what a stored record may ask us to compute. Without it a
/// corrupt or hostile record naming ten billion iterations turns one login
/// attempt into an outage.
constexpr std::uint32_t kMaxIterations = 10'000'000;

std::string toHex(const unsigned char* bytes, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string           out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 0x0F]);
    }
    return out;
}

bool fromHex(std::string_view text, std::vector<unsigned char>& out) {
    if (text.size() % 2 != 0) return false;
    out.clear();
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        int value = 0;
        for (std::size_t half = 0; half < 2; ++half) {
            const char ch = text[i + half];
            value <<= 4;
            if (ch >= '0' && ch <= '9')      value |= ch - '0';
            else if (ch >= 'a' && ch <= 'f') value |= ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') value |= ch - 'A' + 10;
            else                             return false;
        }
        out.push_back(static_cast<unsigned char>(value));
    }
    return true;
}

std::vector<unsigned char> derive(std::string_view password, const unsigned char* salt,
                                  std::size_t saltSize, std::uint32_t iterations,
                                  std::size_t length) {
    std::vector<unsigned char> out(length);
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt,
                          static_cast<int>(saltSize), static_cast<int>(iterations), EVP_sha256(),
                          static_cast<int>(length), out.data()) != 1) {
        throw std::runtime_error("password hashing failed");
    }
    return out;
}

/// Splits on '$'. Returns false rather than throwing: every caller is on the
/// login path, where an unparseable record is a denial, not an exception.
bool split(std::string_view stored, std::uint32_t& iterations, std::string_view& salt,
           std::string_view& hash) {
    if (!stored.starts_with(kPrefix)) return false;
    stored.remove_prefix(kPrefix.size());

    const std::size_t firstSeparator = stored.find('$');
    if (firstSeparator == std::string_view::npos) return false;
    const std::size_t secondSeparator = stored.find('$', firstSeparator + 1);
    if (secondSeparator == std::string_view::npos) return false;

    const std::string_view iterationText = stored.substr(0, firstSeparator);
    if (iterationText.empty() || iterationText.size() > 9) return false;

    std::uint32_t parsed = 0;
    for (const char ch : iterationText) {
        if (ch < '0' || ch > '9') return false;
        parsed = parsed * 10 + static_cast<std::uint32_t>(ch - '0');
    }
    if (parsed == 0 || parsed > kMaxIterations) return false;

    iterations = parsed;
    salt       = stored.substr(firstSeparator + 1, secondSeparator - firstSeparator - 1);
    hash       = stored.substr(secondSeparator + 1);
    return !salt.empty() && !hash.empty();
}

}  // namespace

std::string hash(std::string_view password, std::uint32_t iterations) {
    if (iterations == 0 || iterations > kMaxIterations) {
        throw std::runtime_error("password iteration count out of range");
    }

    std::array<unsigned char, kSaltBytes> salt{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        throw std::runtime_error("the system random source is unavailable");
    }

    const auto digest = derive(password, salt.data(), salt.size(), iterations, kHashBytes);

    std::string out(kPrefix);
    out += std::to_string(iterations);
    out += '$';
    out += toHex(salt.data(), salt.size());
    out += '$';
    out += toHex(digest.data(), digest.size());
    return out;
}

bool verify(std::string_view password, std::string_view stored) {
    std::uint32_t    iterations = 0;
    std::string_view saltHex;
    std::string_view hashHex;
    if (!split(stored, iterations, saltHex, hashHex)) return false;

    std::vector<unsigned char> salt;
    std::vector<unsigned char> expected;
    if (!fromHex(saltHex, salt) || !fromHex(hashHex, expected)) return false;
    if (salt.empty() || expected.empty()) return false;

    try {
        const auto actual = derive(password, salt.data(), salt.size(), iterations, expected.size());
        return CRYPTO_memcmp(actual.data(), expected.data(), expected.size()) == 0;
    } catch (const std::exception&) {
        return false;
    }
}

const std::string& dummyHash() {
    // Built once, from randomness, at first use. A hard-coded constant would
    // work equally well for the timing, but a random one cannot be recognised
    // in a memory dump as "the placeholder", which is a small thing that costs
    // nothing.
    static const std::string kDummy = [] {
        std::array<unsigned char, kSaltBytes> noise{};
        if (RAND_bytes(noise.data(), static_cast<int>(noise.size())) != 1) {
            // Not fatal: this value never authorises anything. It only has to
            // cost the same as a real one.
            noise.fill(0x5A);
        }
        return hash(toHex(noise.data(), noise.size()));
    }();
    return kDummy;
}

}  // namespace monobucket::password
