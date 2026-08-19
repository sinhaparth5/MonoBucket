#include "s3/checksum.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

#include "s3/base64.hpp"
#include "s3/s3_error.hpp"
#include "s3/sigv4.hpp"

namespace monobucket::s3 {
namespace {

constexpr std::string_view kChecksumPrefix = "x-amz-checksum-";

/// Suffixes of `x-amz-checksum-` that name something other than a value.
/// Without this list `x-amz-checksum-algorithm` — which every SDK sends on
/// CreateMultipartUpload — would be read as a checksum under an algorithm
/// called "algorithm" and refused.
constexpr std::string_view kReservedSuffixes[] = {"algorithm", "mode", "type"};

std::string toLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[noreturn]] void refuse(const std::string& message) {
    throw S3Exception(S3ErrorCode::InvalidRequest, message);
}

std::optional<ChecksumAlgorithm> algorithmFromHeaderName(std::string_view name,
                                                         bool             refuseUnknown) {
    if (name.size() <= kChecksumPrefix.size()) return std::nullopt;
    if (name.compare(0, kChecksumPrefix.size(), kChecksumPrefix) != 0) return std::nullopt;

    const std::string_view suffix = name.substr(kChecksumPrefix.size());
    if (std::find(std::begin(kReservedSuffixes), std::end(kReservedSuffixes), suffix) !=
        std::end(kReservedSuffixes)) {
        return std::nullopt;
    }

    const auto algorithm = checksumAlgorithmFromString(suffix);
    if (!algorithm && refuseUnknown) {
        refuse("This request carries a " + std::string(name) +
               " header. MonoBucket verifies CRC32, CRC32C, CRC64NVME, SHA1 and SHA256, and "
               "refuses a checksum it cannot check rather than storing the object as though it "
               "had.");
    }
    return algorithm;
}

}  // namespace

std::string checksumHeaderName(ChecksumAlgorithm algorithm) {
    return std::string(kChecksumPrefix) + toLower(toString(algorithm));
}

std::string checksumElementName(ChecksumAlgorithm algorithm) {
    return "Checksum" + std::string(toString(algorithm));
}

std::string encodeChecksum(const Checksum& checksum) {
    if (!checksum.present()) return {};

    std::string out = base64Encode(checksum.value);
    if (checksum.parts != 0) {
        out += '-';
        out += std::to_string(checksum.parts);
    }
    return out;
}

std::optional<Checksum> decodeChecksum(ChecksumAlgorithm algorithm, std::string_view text) {
    if (text.empty()) return std::nullopt;

    std::uint32_t parts = 0;
    if (const std::size_t dash = text.rfind('-'); dash != std::string_view::npos) {
        const std::string_view count = text.substr(dash + 1);
        std::uint32_t          parsed = 0;
        const auto [end, error] =
            std::from_chars(count.data(), count.data() + count.size(), parsed);
        if (count.empty() || error != std::errc() || end != count.data() + count.size() ||
            parsed == 0) {
            return std::nullopt;
        }
        parts = parsed;
        text  = text.substr(0, dash);
    }

    const auto raw = base64Decode(text);
    if (!raw || raw->size() != checksumLength(algorithm)) return std::nullopt;

    Checksum checksum;
    checksum.algorithm = algorithm;
    checksum.value     = *raw;
    checksum.parts     = parts;
    return checksum;
}

std::string_view checksumTypeOf(const Checksum& checksum) {
    return checksum.parts != 0 ? "COMPOSITE" : "FULL_OBJECT";
}

ChecksumRequest resolveChecksumRequest(const ChecksumHeaders& headers) {
    ChecksumRequest wanted;

    // Every `x-amz-checksum-*` header is examined, not just the ones we know
    // how to compute. A value header under an unrecognised algorithm is the
    // exact shape of the defect this exists to close — accepted, discarded, and
    // answered 200 as though it had been checked.
    //
    // There may be only one: two checksums under different algorithms describe
    // two different uploads, and choosing between them would be a guess about
    // which the client meant.
    for (const auto& [name, value] : headers) {
        const auto algorithm = algorithmFromHeaderName(name, true);
        if (!algorithm || value.empty()) continue;

        if (wanted.algorithm) {
            refuse("This request carries checksums under both " +
                   std::string(toString(*wanted.algorithm)) + " and " +
                   std::string(toString(*algorithm)) + ". Send exactly one.");
        }
        wanted.algorithm = algorithm;
        wanted.expected  = value;
    }
    if (wanted.algorithm) return wanted;

    // `x-amz-trailer` names the trailing header the value will arrive in. It
    // has to be read now even though the value cannot be: the algorithm has to
    // be chosen before the first byte is hashed, or the payload would need a
    // second pass once the trailer finally said which one it was.
    if (const auto found = headers.find("x-amz-trailer"); found != headers.end()) {
        const std::string trailer = toLower(found->second);

        // At most one trailer is defined for an upload, and a list is not
        // something S3 accepts here.
        if (trailer.find(',') != std::string::npos) {
            refuse("x-amz-trailer names more than one trailing header.");
        }

        const auto algorithm = algorithmFromHeaderName(trailer, true);
        if (!algorithm) {
            refuse("x-amz-trailer names '" + trailer +
                   "', which is not a checksum this build understands.");
        }
        wanted.algorithm   = algorithm;
        wanted.inTrailer   = true;
        wanted.trailerName = trailer;
        return wanted;
    }

    // Last, the algorithm on its own. The SDKs send it beside the trailer they
    // are about to write, and CreateMultipartUpload sends it with no value at
    // all — there is no payload there to checksum, only a decision about what
    // the parts will carry.
    for (const char* name : {"x-amz-sdk-checksum-algorithm", "x-amz-checksum-algorithm"}) {
        const auto found = headers.find(name);
        if (found == headers.end() || found->second.empty()) continue;

        const auto algorithm = checksumAlgorithmFromString(found->second);
        if (!algorithm) {
            refuse(std::string(name) + " names '" + found->second +
                   "', which is not an algorithm MonoBucket computes.");
        }
        wanted.algorithm = algorithm;
        return wanted;
    }

    return wanted;
}

void verifyChecksum(ChecksumAlgorithm algorithm, std::string_view expected,
                    std::string_view actualRaw) {
    const auto decoded = decodeChecksum(algorithm, expected);
    if (!decoded || decoded->parts != 0) {
        throw S3Exception(S3ErrorCode::InvalidRequest,
                          "The " + checksumHeaderName(algorithm) +
                              " value is not base64 of a " + std::string(toString(algorithm)) +
                              " digest.");
    }

    if (!secureEquals(decoded->value, actualRaw)) {
        throw S3Exception(S3ErrorCode::BadDigest,
                          "The " + std::string(toString(algorithm)) +
                              " checksum you specified did not match what was received.");
    }
}

}  // namespace monobucket::s3
