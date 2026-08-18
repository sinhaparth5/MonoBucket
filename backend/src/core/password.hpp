#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Password hashing for the console administrator account.
//
// PBKDF2-HMAC-SHA256 rather than Argon2 or scrypt: OpenSSL is already a
// dependency and already provides it, whereas a memory-hard KDF would mean
// vendoring a second crypto library into a project whose whole shape is "one
// binary, no sidecars". PBKDF2 is the weaker primitive against a GPU attacker
// and is chosen with that known — the mitigation is the iteration count below
// plus the rate limiter in front of the login route, not a claim that the two
// are equivalent.

namespace monobucket::password {

/// OWASP's 2023 floor for PBKDF2-HMAC-SHA256. Roughly 200 ms on a laptop core
/// and closer to a second on a small ARM board, which is why verification is
/// posted to an I/O thread rather than run on the event loop.
inline constexpr std::uint32_t kDefaultIterations = 600'000;

/// The shortest password this accepts. Long rather than complex on purpose:
/// there is one account, it is reachable from a browser, and a composition rule
/// buys less than four more characters do.
inline constexpr std::size_t kMinimumLength = 12;

/// Derives a verifier for `password`, salted with fresh randomness.
///
/// Returns `pbkdf2-sha256$<iterations>$<hex salt>$<hex hash>` — self-describing
/// so that raising the iteration count later does not strand records written at
/// the old one. Throws std::runtime_error if the system random source fails;
/// falling back to a weaker source would produce a hash that looks fine.
std::string hash(std::string_view password, std::uint32_t iterations = kDefaultIterations);

/// True when `password` produced `stored`. Constant-time in the comparison and
/// false — never throwing — for a stored string this build cannot parse, so a
/// corrupt record denies access rather than crashing the login route.
bool verify(std::string_view password, std::string_view stored);

/// The cost of a verify against a record that does not exist.
///
/// Login must take the same time whether or not the username is known,
/// otherwise the response time answers a question the error text refuses to.
/// Callers hand this to verify() when the lookup missed.
const std::string& dummyHash();

}  // namespace monobucket::password
