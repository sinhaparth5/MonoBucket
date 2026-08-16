#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Key encoding for the metadata store.
//
// Everything lives in one RocksDB column family, separated by a one-byte type
// tag. The alternative — a column family per record type — gives each family
// its own memtable set, multiplying the write-buffer floor by the number of
// families for no benefit here: RocksDB's keyspace is already ordered, and a
// tag byte partitions it just as cleanly.
//
// Records are separated from their variable-length components by NUL. That is
// safe and load-bearing rather than convenient: bucket names are restricted to
// DNS characters and S3 object keys are UTF-8, so neither can contain a NUL.
// Because NUL sorts below every other byte, `o<bucket>\0` is a prefix that
// captures exactly one bucket's objects, and iterating it yields keys in the
// lexicographic order ListObjectsV2 is specified to return.

namespace monobucket::keys {

inline constexpr char kSeparator = '\0';

// Type tags. Uppercase and lowercase are used deliberately for the two upload
// views so they occupy different regions of the keyspace.
inline constexpr char kBucket      = 'b';  ///< b<name>
inline constexpr char kObject      = 'o';  ///< o<bucket>\0<key>
inline constexpr char kUploadByKey = 'u';  ///< u<bucket>\0<key>\0<uploadId>
inline constexpr char kUploadById  = 'U';  ///< U<uploadId>
inline constexpr char kPart        = 'p';  ///< p<uploadId>\0<be32 partNumber>
inline constexpr char kOrphan      = 'x';  ///< x<blobId>
inline constexpr char kMeta        = 'm';  ///< m<name>

std::string bucket(std::string_view name);
std::string bucketPrefix();

std::string object(std::string_view bucketName, std::string_view key);
std::string objectPrefix(std::string_view bucketName);

std::string uploadByKey(std::string_view bucketName, std::string_view key,
                        std::string_view uploadId);
std::string uploadByKeyPrefix(std::string_view bucketName);
std::string uploadById(std::string_view uploadId);

/// Part numbers are stored big-endian so that lexicographic iteration returns
/// them in ascending numeric order, which is the order CompleteMultipartUpload
/// requires when concatenating.
std::string part(std::string_view uploadId, std::uint32_t partNumber);
std::string partPrefix(std::string_view uploadId);

std::string orphan(std::string_view blobId);
std::string orphanPrefix();

std::string meta(std::string_view name);

/// The exclusive upper bound for iterating `prefix`, for RocksDB's
/// `iterate_upper_bound`. Returns nothing when the prefix is all 0xFF bytes and
/// therefore has no representable successor; callers then fall back to checking
/// the prefix on each key.
std::optional<std::string> upperBound(std::string_view prefix);

/// Recovers the object key from a stored `o<bucket>\0<key>` entry. Returns
/// nothing when the stored key does not belong to this bucket.
///
/// The result borrows from `stored` and is valid only for as long as it is —
/// which, for an iterator's key, means until the next step of the iterator.
std::optional<std::string_view> objectKey(std::string_view stored, std::string_view bucketName);

/// Recovers `<key>` and `<uploadId>` from a stored `u<bucket>\0<key>\0<id>`
/// entry.
struct UploadKeyParts {
    std::string_view key;
    std::string_view uploadId;
};
std::optional<UploadKeyParts> uploadKeyParts(std::string_view stored, std::string_view bucketName);

/// Recovers the part number from a stored `p<uploadId>\0<be32>` entry.
std::optional<std::uint32_t> partNumber(std::string_view stored, std::string_view uploadId);

}  // namespace monobucket::keys
