#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Integrity digests computed *during* the stream. Nothing here ever re-reads
// an object: a multi-gigabyte upload is hashed exactly once, as its bytes pass
// through the write path, which is what keeps a PUT to O(size) I/O instead of
// O(2 x size).

namespace monobucket {

/// Incremental MD5 and SHA-256 over one byte stream.
///
/// Both are needed and neither is optional: MD5 *is* the S3 ETag for a
/// single-part object, and SHA-256 is what SigV4 payload signing compares
/// against. Computing them in one pass costs one traversal of the data.
class Digest {
public:
    Digest();
    ~Digest();

    Digest(Digest&&) noexcept;
    Digest& operator=(Digest&&) noexcept;
    Digest(const Digest&)            = delete;
    Digest& operator=(const Digest&) = delete;

    void update(std::span<const std::byte> data);
    void update(std::string_view data);

    struct Result {
        std::string   md5;     ///< 32 lowercase hex chars
        std::string   sha256;  ///< 64 lowercase hex chars
        std::uint64_t bytes = 0;
    };

    /// Finalises both digests. The object must not be updated afterwards.
    Result finish();

    /// Bytes consumed so far. Valid before and after finish().
    std::uint64_t bytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Lowercase hex, the encoding every S3 header uses.
std::string toHex(std::span<const unsigned char> bytes);

std::string md5Hex(std::string_view data);
std::string sha256Hex(std::string_view data);

/// SHA-256 of the empty string. SigV4 sends this literal for bodyless requests,
/// so it is worth not recomputing.
extern const char* const kEmptySha256;

/// The ETag S3 reports for a completed multipart upload: the MD5 of the
/// concatenated *raw* part digests, then `-` and the part count.
///
/// This is part of the wire format, not an implementation detail — clients
/// compare the string literally, and some infer the original part size from it.
/// `partMd5Hex` must be in ascending part-number order.
std::string multipartETag(const std::vector<std::string>& partMd5Hex);

}  // namespace monobucket
