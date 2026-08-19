#include "storage/checksum.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>

#include <openssl/evp.h>

namespace monobucket {
namespace {

/// A reflected CRC table, built once per polynomial at first use.
///
/// Reflected rather than the textbook big-endian form because every CRC S3
/// uses is specified reflected in and out; writing it the other way round and
/// reversing the bits afterwards is the classic way to get CRC32C subtly wrong.
template <typename Word, Word kPolynomial>
const std::array<Word, 256>& crcTable() {
    static const std::array<Word, 256> table = [] {
        std::array<Word, 256> built{};
        for (std::size_t i = 0; i < built.size(); ++i) {
            Word value = static_cast<Word>(i);
            for (int bit = 0; bit < 8; ++bit) {
                value = (value >> 1) ^ ((value & 1) != 0 ? kPolynomial : Word{0});
            }
            built[i] = value;
        }
        return built;
    }();
    return table;
}

template <typename Word, Word kPolynomial>
Word crcUpdate(Word state, std::string_view data) {
    const auto& table = crcTable<Word, kPolynomial>();
    for (const char ch : data) {
        const auto byte = static_cast<unsigned char>(ch);
        state = table[(state ^ byte) & 0xFF] ^ (state >> 8);
    }
    return state;
}

/// Big-endian, which is how every `x-amz-checksum-*` value is laid out before
/// it is base64-encoded.
template <typename Word>
std::string toBigEndian(Word value) {
    std::string out(sizeof(Word), '\0');
    for (std::size_t i = 0; i < sizeof(Word); ++i) {
        out[sizeof(Word) - 1 - i] = static_cast<char>((value >> (i * 8)) & 0xFF);
    }
    return out;
}

constexpr std::uint32_t kCrc32Polynomial  = 0xEDB88320u;
constexpr std::uint32_t kCrc32cPolynomial = 0x82F63B78u;

/// CRC-64/NVME: the reflection of 0xAD93D23594C93659.
constexpr std::uint64_t kCrc64NvmePolynomial = 0x9A6C9329AC4BC9B5ull;

const EVP_MD* hashFor(ChecksumAlgorithm algorithm) {
    return algorithm == ChecksumAlgorithm::Sha1 ? EVP_sha1() : EVP_sha256();
}

}  // namespace

struct ChecksumComputer::Impl {
    /// One of these is live, decided by the algorithm. A variant would buy
    /// nothing: the discriminant is already stored beside it.
    std::uint32_t crc32 = 0;
    std::uint64_t crc64 = 0;
    EVP_MD_CTX*   hash  = nullptr;

    ~Impl() {
        if (hash != nullptr) EVP_MD_CTX_free(hash);
    }
};

ChecksumComputer::ChecksumComputer(ChecksumAlgorithm algorithm)
    : algorithm_(algorithm), impl_(std::make_unique<Impl>()) {
    switch (algorithm) {
        case ChecksumAlgorithm::Crc32:
        case ChecksumAlgorithm::Crc32c:
            impl_->crc32 = 0xFFFFFFFFu;
            break;
        case ChecksumAlgorithm::Crc64Nvme:
            impl_->crc64 = 0xFFFFFFFFFFFFFFFFull;
            break;
        case ChecksumAlgorithm::Sha1:
        case ChecksumAlgorithm::Sha256:
            impl_->hash = EVP_MD_CTX_new();
            if (impl_->hash == nullptr) throw std::runtime_error("EVP_MD_CTX_new failed");
            if (EVP_DigestInit_ex(impl_->hash, hashFor(algorithm), nullptr) != 1) {
                throw std::runtime_error("EVP_DigestInit_ex failed");
            }
            break;
    }
}

ChecksumComputer::~ChecksumComputer()                                    = default;
ChecksumComputer::ChecksumComputer(ChecksumComputer&&) noexcept            = default;
ChecksumComputer& ChecksumComputer::operator=(ChecksumComputer&&) noexcept = default;

void ChecksumComputer::update(std::string_view data) {
    if (data.empty()) return;

    switch (algorithm_) {
        case ChecksumAlgorithm::Crc32:
            impl_->crc32 = crcUpdate<std::uint32_t, kCrc32Polynomial>(impl_->crc32, data);
            return;
        case ChecksumAlgorithm::Crc32c:
            impl_->crc32 = crcUpdate<std::uint32_t, kCrc32cPolynomial>(impl_->crc32, data);
            return;
        case ChecksumAlgorithm::Crc64Nvme:
            impl_->crc64 = crcUpdate<std::uint64_t, kCrc64NvmePolynomial>(impl_->crc64, data);
            return;
        case ChecksumAlgorithm::Sha1:
        case ChecksumAlgorithm::Sha256:
            if (EVP_DigestUpdate(impl_->hash, data.data(), data.size()) != 1) {
                throw std::runtime_error("EVP_DigestUpdate failed");
            }
            return;
    }
}

std::string ChecksumComputer::finish() {
    switch (algorithm_) {
        case ChecksumAlgorithm::Crc32:
        case ChecksumAlgorithm::Crc32c:
            return toBigEndian<std::uint32_t>(impl_->crc32 ^ 0xFFFFFFFFu);
        case ChecksumAlgorithm::Crc64Nvme:
            return toBigEndian<std::uint64_t>(impl_->crc64 ^ 0xFFFFFFFFFFFFFFFFull);
        case ChecksumAlgorithm::Sha1:
        case ChecksumAlgorithm::Sha256:
            break;
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
    unsigned                                   length = 0;
    if (EVP_DigestFinal_ex(impl_->hash, out.data(), &length) != 1) {
        throw std::runtime_error("EVP_DigestFinal_ex failed");
    }
    return std::string(reinterpret_cast<const char*>(out.data()), length);
}

std::string_view toString(ChecksumAlgorithm algorithm) {
    switch (algorithm) {
        case ChecksumAlgorithm::Crc32:     return "CRC32";
        case ChecksumAlgorithm::Crc32c:    return "CRC32C";
        case ChecksumAlgorithm::Crc64Nvme: return "CRC64NVME";
        case ChecksumAlgorithm::Sha1:      return "SHA1";
        case ChecksumAlgorithm::Sha256:    return "SHA256";
    }
    return "CRC32";
}

std::optional<ChecksumAlgorithm> checksumAlgorithmFromString(std::string_view name) {
    std::string upper(name);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (upper == "CRC32")     return ChecksumAlgorithm::Crc32;
    if (upper == "CRC32C")    return ChecksumAlgorithm::Crc32c;
    if (upper == "CRC64NVME") return ChecksumAlgorithm::Crc64Nvme;
    if (upper == "SHA1")      return ChecksumAlgorithm::Sha1;
    if (upper == "SHA256")    return ChecksumAlgorithm::Sha256;
    return std::nullopt;
}

std::size_t checksumLength(ChecksumAlgorithm algorithm) {
    switch (algorithm) {
        case ChecksumAlgorithm::Crc32:
        case ChecksumAlgorithm::Crc32c:    return 4;
        case ChecksumAlgorithm::Crc64Nvme: return 8;
        case ChecksumAlgorithm::Sha1:      return 20;
        case ChecksumAlgorithm::Sha256:    return 32;
    }
    return 4;
}

std::string checksumOf(ChecksumAlgorithm algorithm, std::string_view data) {
    ChecksumComputer computer(algorithm);
    computer.update(data);
    return computer.finish();
}

Checksum compositeChecksum(ChecksumAlgorithm               algorithm,
                           const std::vector<std::string>& rawPartChecksums) {
    ChecksumComputer computer(algorithm);
    for (const std::string& part : rawPartChecksums) {
        if (part.size() != checksumLength(algorithm)) {
            throw std::invalid_argument("composite checksum: a part digest is the wrong length");
        }
        computer.update(part);
    }

    Checksum result;
    result.algorithm = algorithm;
    result.value     = computer.finish();
    result.parts     = static_cast<std::uint32_t>(rawPartChecksums.size());
    return result;
}

}  // namespace monobucket
