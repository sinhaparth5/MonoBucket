#pragma once

#include <string>
#include <string_view>

// Minting and validating S3 credential pairs issued from the console.
//
// Kept out of the console handler so the generator is testable without a
// socket, for the same reason SigV4 is: the properties that matter here are
// "unpredictable" and "the right shape", and neither is observable through an
// HTTP response.

namespace monobucket::credentials {

/// Length of a generated access key id, matching AWS's. Long enough that the
/// keyspace is not enumerable, short enough to read out loud.
inline constexpr std::size_t kAccessKeyIdLength = 20;

/// Length of a generated secret. Forty alphanumeric characters is about 238
/// bits — the same shape as an AWS secret, without the `+` and `/` that turn a
/// copy-paste into a shell-quoting problem.
inline constexpr std::size_t kSecretKeyLength = 40;

/// Prefix on every generated id, so a key found in someone's environment can
/// be traced back to which system issued it.
inline constexpr std::string_view kAccessKeyIdPrefix = "MB";

/// Draws from the system CSPRNG. Throws std::runtime_error if it is
/// unavailable — a predictable credential is worse than no credential.
std::string generateAccessKeyId();
std::string generateSecretKey();

/// Whether `id` could have been issued by generateAccessKeyId().
///
/// Used to refuse a lookup before it reaches RocksDB, and to keep a client's
/// arbitrary bytes out of a database key. It is deliberately not applied to the
/// root credential from the environment, which predates this scheme and is
/// whatever the operator chose.
bool plausibleAccessKeyId(std::string_view id);

}  // namespace monobucket::credentials
