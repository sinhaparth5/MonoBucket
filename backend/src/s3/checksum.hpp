#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "storage/checksum.hpp"

// Where a checksum is asked for, and what it looks like on the wire.
//
// The algorithms themselves are in storage/checksum.hpp; everything here is
// about the request. Expressed over a header lookup rather than over Drogon so
// the rules — which header wins, what is refused, what may arrive late — are
// testable as a table instead of through a socket, the same reason sigv4.hpp
// deals in plain strings.

namespace monobucket::s3 {

/// `x-amz-checksum-crc32`, and so on. Lowercased, because it is compared
/// against trailer names that arrive lowercased.
std::string checksumHeaderName(ChecksumAlgorithm algorithm);

/// `ChecksumCRC32` — the element CompleteMultipartUpload and ListParts carry
/// the value in.
std::string checksumElementName(ChecksumAlgorithm algorithm);

/// Base64 of the raw digest, with `-N` appended when the value is a composite
/// over N parts. What a client recomputes and compares literally.
std::string encodeChecksum(const Checksum& checksum);

/// The inverse. Nullopt when the text is not base64 of the algorithm's digest
/// length, `-N` suffix included.
std::optional<Checksum> decodeChecksum(ChecksumAlgorithm algorithm, std::string_view text);

/// `COMPOSITE` for a checksum of part checksums, `FULL_OBJECT` for one over the
/// payload. Reported as `x-amz-checksum-type`, because the two cannot be
/// compared against each other and a client has no other way to tell them
/// apart.
std::string_view checksumTypeOf(const Checksum& checksum);

/// What a request asks to have verified.
struct ChecksumRequest {
    /// Nothing when the request named no checksum at all, which stays a
    /// perfectly ordinary upload — this must never become mandatory.
    std::optional<ChecksumAlgorithm> algorithm;

    /// Base64 as the client wrote it, empty when it has not arrived yet.
    std::string expected;

    /// The value travels in the trailing header block of an aws-chunked body,
    /// so it is only knowable once the payload has been read — which is why
    /// verification happens after the stream and before the commit, never
    /// before the stream.
    bool inTrailer = false;

    /// The lowercased trailer the value will arrive under.
    std::string trailerName;

    bool wanted() const noexcept { return algorithm.has_value(); }
};

/// The checksum-bearing headers of one request, names lowercased.
///
/// A table rather than a lookup by name, because the rules have to *enumerate*:
/// an `x-amz-checksum-*` header under an algorithm this build does not know has
/// to be found before it can be refused, and asking for the five names we
/// already thought of would find only those.
using ChecksumHeaders = std::map<std::string, std::string>;

/// Resolves `x-amz-checksum-*`, `x-amz-trailer` and
/// `x-amz-sdk-checksum-algorithm` into one answer.
///
/// Throws S3Exception(InvalidRequest) when the request names more than one
/// checksum, or names an algorithm this build does not compute. Refusing the
/// unknown algorithm is the point of the exercise: accepting it would be the
/// same silent 200 that made an unverified checksum look verified.
ChecksumRequest resolveChecksumRequest(const ChecksumHeaders& headers);

/// Compares a client-supplied base64 value against raw digest bytes.
///
/// Throws InvalidRequest when the value is not base64 of the right length —
/// a malformed header is not a corrupt body — and BadDigest when it decodes
/// but disagrees.
void verifyChecksum(ChecksumAlgorithm algorithm, std::string_view expected,
                    std::string_view actualRaw);

}  // namespace monobucket::s3
