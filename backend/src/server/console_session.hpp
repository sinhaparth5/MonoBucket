#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// Console session tokens and the rate limiter in front of the login route.
//
// Split out of console_api.cpp so that expiry, sign-out and lockout are
// testable without standing up a listener. The clock is injected for the same
// reason it is injected into SigV4: a test for "the session expires after
// twelve hours" that waits twelve hours is not a test anyone runs.

namespace monobucket {

/// Twelve hours: long enough to survive a working day with the tab open, short
/// enough that a forgotten session on a shared machine expires the same day.
/// Not an environment knob — a console session is not part of the deployment
/// shape, and every extra variable is one more thing to get wrong.
inline constexpr std::int64_t kSessionTtlSeconds = 12 * 60 * 60;

/// A wrong password is cheap to try, so cap how often it can be tried. The
/// window is global rather than per-IP on purpose: there is exactly one
/// account, so a per-IP bucket would only tell an attacker to change source
/// addresses.
inline constexpr int          kMaxFailedLogins    = 10;
inline constexpr std::int64_t kLoginWindowSeconds = 60;

/// Unix seconds, from the system clock.
std::int64_t nowSeconds() noexcept;

/// Session tokens held in memory only. A restart logs everyone out, which is
/// the correct trade for a single-binary server: persisting them would mean a
/// stolen data directory is also a stolen login, and signing in again costs one
/// round trip.
///
/// A session names the administrator, never a credential. That is the whole
/// separation: revoking every S3 key leaves this session working, and ending
/// this session leaves every S3 client working.
class SessionStore {
public:
    explicit SessionStore(std::int64_t ttlSeconds = kSessionTtlSeconds) : ttlSeconds_(ttlSeconds) {}

    /// Returns an unguessable token naming `username`.
    std::string open(const std::string& username);
    std::string openAt(const std::string& username, std::int64_t atSeconds);

    /// The administrator the token names, or empty when it is unknown or has
    /// expired. Expiry is judged on every use rather than by a sweeper, so a
    /// tab left open past the TTL is refused without waiting for one.
    std::string resolve(const std::string& token) const;
    std::string resolveAt(const std::string& token, std::int64_t atSeconds) const;

    /// Signing out. Idempotent, and silent about whether the token was live —
    /// nothing useful follows from telling the caller.
    void close(const std::string& token);

    /// Live sessions, expired ones excluded. For tests and for nothing else.
    std::size_t size() const;

private:
    struct Session {
        std::string  username;
        std::int64_t expiresAt = 0;
    };

    /// Called on every open rather than on a timer: expired entries are only a
    /// leak if logins keep happening, and if they do this collects them.
    void sweep(std::int64_t atSeconds);

    mutable std::mutex                       mutex_;
    std::unordered_map<std::string, Session> sessions_;
    std::int64_t                             ttlSeconds_;
};

/// A fixed window rather than a token bucket. The window resets wholesale,
/// which means a determined attacker gets kMaxFailedLogins tries per window
/// instead of a smoothly limited rate — accepted, because the cost of one
/// attempt is a PBKDF2 verification and the account is one password.
class LoginThrottle {
public:
    bool blocked();
    bool blockedAt(std::int64_t atSeconds);

    void recordFailure();
    void recordFailureAt(std::int64_t atSeconds);

    void recordSuccess();

private:
    void roll(std::int64_t atSeconds);

    std::mutex   mutex_;
    std::int64_t windowStart_ = 0;
    int          failures_    = 0;
};

}  // namespace monobucket
