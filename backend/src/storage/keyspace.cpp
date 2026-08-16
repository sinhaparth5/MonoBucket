#include "storage/keyspace.hpp"

namespace monobucket::keys {
namespace {

std::string tagged(char tag, std::string_view rest) {
    std::string out;
    out.reserve(1 + rest.size());
    out.push_back(tag);
    out.append(rest);
    return out;
}

void appendBigEndian32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

}  // namespace

std::string bucket(std::string_view name) { return tagged(kBucket, name); }

std::string bucketPrefix() { return std::string(1, kBucket); }

std::string object(std::string_view bucketName, std::string_view key) {
    std::string out = objectPrefix(bucketName);
    out.append(key);
    return out;
}

std::string objectPrefix(std::string_view bucketName) {
    std::string out;
    out.reserve(2 + bucketName.size());
    out.push_back(kObject);
    out.append(bucketName);
    out.push_back(kSeparator);
    return out;
}

std::string uploadByKey(std::string_view bucketName, std::string_view key,
                        std::string_view uploadId) {
    std::string out = uploadByKeyPrefix(bucketName);
    out.append(key);
    out.push_back(kSeparator);
    out.append(uploadId);
    return out;
}

std::string uploadByKeyPrefix(std::string_view bucketName) {
    std::string out;
    out.reserve(2 + bucketName.size());
    out.push_back(kUploadByKey);
    out.append(bucketName);
    out.push_back(kSeparator);
    return out;
}

std::string uploadById(std::string_view uploadId) { return tagged(kUploadById, uploadId); }

std::string part(std::string_view uploadId, std::uint32_t partNumber) {
    std::string out = partPrefix(uploadId);
    appendBigEndian32(out, partNumber);
    return out;
}

std::string partPrefix(std::string_view uploadId) {
    std::string out;
    out.reserve(2 + uploadId.size());
    out.push_back(kPart);
    out.append(uploadId);
    out.push_back(kSeparator);
    return out;
}

std::string orphan(std::string_view blobId) { return tagged(kOrphan, blobId); }

std::string orphanPrefix() { return std::string(1, kOrphan); }

std::string meta(std::string_view name) { return tagged(kMeta, name); }

std::optional<std::string> upperBound(std::string_view prefix) {
    std::string out(prefix);
    while (!out.empty()) {
        auto& last = out.back();
        if (static_cast<unsigned char>(last) != 0xFF) {
            last = static_cast<char>(static_cast<unsigned char>(last) + 1);
            return out;
        }
        // Trailing 0xFF has no successor at this position; carry left.
        out.pop_back();
    }
    return std::nullopt;
}

std::optional<std::string_view> objectKey(std::string_view stored, std::string_view bucketName) {
    const std::string prefix = objectPrefix(bucketName);
    if (!stored.starts_with(prefix)) return std::nullopt;
    return stored.substr(prefix.size());
}

std::optional<UploadKeyParts> uploadKeyParts(std::string_view stored,
                                             std::string_view bucketName) {
    const std::string prefix = uploadByKeyPrefix(bucketName);
    if (!stored.starts_with(prefix)) return std::nullopt;

    const std::string_view rest = stored.substr(prefix.size());

    // The object key may not contain NUL, so the last separator is the one that
    // introduces the upload id — searching from the right is unambiguous.
    const auto split = rest.rfind(kSeparator);
    if (split == std::string_view::npos) return std::nullopt;

    return UploadKeyParts{rest.substr(0, split), rest.substr(split + 1)};
}

std::optional<std::uint32_t> partNumber(std::string_view stored, std::string_view uploadId) {
    const std::string prefix = partPrefix(uploadId);
    if (!stored.starts_with(prefix)) return std::nullopt;

    const std::string_view encoded = stored.substr(prefix.size());
    if (encoded.size() != 4) return std::nullopt;

    return (static_cast<std::uint32_t>(static_cast<unsigned char>(encoded[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(encoded[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(encoded[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(encoded[3]));
}

}  // namespace monobucket::keys
