#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The `x-amz-checksum-*` family, computed during the write stream.
//
// Separate from digest.hpp rather than folded into it, because the two answer
// different questions. MD5 and SHA-256 are computed for every object whether or
// not anybody asked — one *is* the ETag, the other is what fsck re-verifies
// against — while a checksum here exists only because a client requested it,
// under an algorithm the client chose. Merging them would mean paying for five
// digests on every write to satisfy the one request in a hundred that names a
// checksum.
//
// Raw digest bytes throughout. Base64 and the composite `-N` suffix are wire
// encodings and belong to the S3 layer; a stored checksum that had already been
// rendered for the wire could not be composed with another one.

namespace monobucket {

/// The algorithms S3 defines. CRC64NVME is here because current SDKs default
/// to it, and an algorithm we refuse is an upload that fails.
enum class ChecksumAlgorithm { Crc32, Crc32c, Crc64Nvme, Sha1, Sha256 };

/// `CRC32`, `CRC32C`, `CRC64NVME`, `SHA1`, `SHA256`. The same spelling serves
/// `x-amz-checksum-algorithm` and, lowercased, the `x-amz-checksum-*` suffix.
std::string_view toString(ChecksumAlgorithm algorithm);

/// Accepts either case, since the name arrives uppercased in one header and
/// lowercased in the other. Nullopt for anything this build does not compute —
/// which the caller must refuse rather than ignore, since the whole point is
/// not to answer 200 to a checksum nobody checked.
std::optional<ChecksumAlgorithm> checksumAlgorithmFromString(std::string_view name);

/// Raw digest length in bytes. What an incoming value is validated against
/// before it is compared, so a truncated header is a clear refusal rather than
/// a mismatch.
std::size_t checksumLength(ChecksumAlgorithm algorithm);

/// A checksum as it is stored: raw bytes plus the algorithm that produced them.
struct Checksum {
    std::optional<ChecksumAlgorithm> algorithm;

    /// Raw digest bytes, never base64.
    std::string value;

    /// Non-zero when `value` is a checksum over that many part checksums
    /// rather than over the payload. It is what the `-N` on the wire reports,
    /// and it is stored rather than derived from the ETag because a multipart
    /// object need not have a composite checksum.
    std::uint32_t parts = 0;

    bool present() const noexcept { return algorithm.has_value() && !value.empty(); }
};

/// Incremental, so the payload is checksummed in the pass that already writes
/// it. A second walk over a five-gigabyte object to compute a CRC32 would cost
/// more than the write.
class ChecksumComputer {
public:
    explicit ChecksumComputer(ChecksumAlgorithm algorithm);
    ~ChecksumComputer();

    ChecksumComputer(ChecksumComputer&&) noexcept;
    ChecksumComputer& operator=(ChecksumComputer&&) noexcept;
    ChecksumComputer(const ChecksumComputer&)            = delete;
    ChecksumComputer& operator=(const ChecksumComputer&) = delete;

    void update(std::string_view data);

    /// Raw digest bytes. The computer must not be updated afterwards.
    std::string finish();

    ChecksumAlgorithm algorithm() const noexcept { return algorithm_; }

private:
    struct Impl;
    ChecksumAlgorithm     algorithm_;
    std::unique_ptr<Impl> impl_;
};

/// One-shot, for values already in memory.
std::string checksumOf(ChecksumAlgorithm algorithm, std::string_view data);

/// S3's composite form for a multipart object: the parts' raw checksums
/// concatenated in ascending part order and checksummed again under the same
/// algorithm. `parts` is set to the count, which the wire form appends as `-N`.
///
/// This mirrors multipartETag() exactly, and for the same reason: clients
/// recompute it themselves and compare the string, so the construction is part
/// of the wire format rather than an implementation detail.
Checksum compositeChecksum(ChecksumAlgorithm                algorithm,
                           const std::vector<std::string>& rawPartChecksums);

}  // namespace monobucket
