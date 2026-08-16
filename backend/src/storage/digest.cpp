#include "storage/digest.hpp"

#include <array>
#include <stdexcept>

#include <openssl/evp.h>

namespace monobucket {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

/// EVP_MD_CTX is an opaque heap type in OpenSSL 3; wrap it so the digest class
/// stays exception-safe without hand-written destructors everywhere.
struct EvpContext {
    EVP_MD_CTX* ctx = nullptr;

    explicit EvpContext(const EVP_MD* md) : ctx(EVP_MD_CTX_new()) {
        if (ctx == nullptr) throw std::runtime_error("EVP_MD_CTX_new failed");
        if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestInit_ex failed");
        }
    }

    ~EvpContext() {
        if (ctx != nullptr) EVP_MD_CTX_free(ctx);
    }

    EvpContext(const EvpContext&)            = delete;
    EvpContext& operator=(const EvpContext&) = delete;

    void update(const void* data, std::size_t len) {
        if (len == 0) return;
        if (EVP_DigestUpdate(ctx, data, len) != 1) {
            throw std::runtime_error("EVP_DigestUpdate failed");
        }
    }

    std::string finishHex() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
        unsigned                                   len = 0;
        if (EVP_DigestFinal_ex(ctx, out.data(), &len) != 1) {
            throw std::runtime_error("EVP_DigestFinal_ex failed");
        }
        return toHex(std::span<const unsigned char>(out.data(), len));
    }

    std::string finishRaw() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
        unsigned                                   len = 0;
        if (EVP_DigestFinal_ex(ctx, out.data(), &len) != 1) {
            throw std::runtime_error("EVP_DigestFinal_ex failed");
        }
        return std::string(reinterpret_cast<const char*>(out.data()), len);
    }
};

}  // namespace

const char* const kEmptySha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

struct Digest::Impl {
    EvpContext    md5{EVP_md5()};
    EvpContext    sha256{EVP_sha256()};
    std::uint64_t bytes = 0;
};

Digest::Digest() : impl_(std::make_unique<Impl>()) {}
Digest::~Digest()                           = default;
Digest::Digest(Digest&&) noexcept            = default;
Digest& Digest::operator=(Digest&&) noexcept = default;

void Digest::update(std::span<const std::byte> data) {
    if (data.empty()) return;
    impl_->md5.update(data.data(), data.size());
    impl_->sha256.update(data.data(), data.size());
    impl_->bytes += data.size();
}

void Digest::update(std::string_view data) {
    update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

Digest::Result Digest::finish() {
    Result result;
    result.md5    = impl_->md5.finishHex();
    result.sha256 = impl_->sha256.finishHex();
    result.bytes  = impl_->bytes;
    return result;
}

std::uint64_t Digest::bytes() const noexcept { return impl_->bytes; }

std::string toHex(std::span<const unsigned char> bytes) {
    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2]     = kHexDigits[bytes[i] >> 4];
        out[i * 2 + 1] = kHexDigits[bytes[i] & 0x0F];
    }
    return out;
}

std::string md5Hex(std::string_view data) {
    EvpContext ctx(EVP_md5());
    ctx.update(data.data(), data.size());
    return ctx.finishHex();
}

std::string sha256Hex(std::string_view data) {
    EvpContext ctx(EVP_sha256());
    ctx.update(data.data(), data.size());
    return ctx.finishHex();
}

std::string multipartETag(const std::vector<std::string>& partMd5Hex) {
    EvpContext ctx(EVP_md5());

    // The digests are concatenated in their raw 16-byte form, not as hex. This
    // is the single most commonly mis-implemented detail of the S3 ETag.
    for (const std::string& hex : partMd5Hex) {
        if (hex.size() != 32) {
            throw std::invalid_argument("multipart ETag: part digest is not 32 hex characters");
        }
        std::array<unsigned char, 16> raw{};
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const auto nibble = [&](std::size_t index) -> unsigned {
                const char c = hex[index];
                if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
                if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
                if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
                throw std::invalid_argument("multipart ETag: part digest is not hexadecimal");
            };
            raw[i] = static_cast<unsigned char>((nibble(i * 2) << 4) | nibble(i * 2 + 1));
        }
        ctx.update(raw.data(), raw.size());
    }

    const std::string raw = ctx.finishRaw();
    return toHex(std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(raw.data()),
                                                raw.size())) +
           '-' + std::to_string(partMd5Hex.size());
}

}  // namespace monobucket
