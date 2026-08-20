#include "server/console_session.hpp"

#include <chrono>
#include <iterator>
#include <stdexcept>

#include <openssl/rand.h>

namespace monobucket {
namespace {

std::string randomToken() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        throw std::runtime_error("the system random source is unavailable");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string           out;
    out.reserve(sizeof(bytes) * 2);
    for (const unsigned char byte : bytes) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

}  // namespace

std::int64_t nowSeconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// --- SessionStore ----------------------------------------------------------

std::string SessionStore::open(const Principal& principal) {
    return openAt(principal, nowSeconds());
}

std::string SessionStore::openAt(const Principal& principal, std::int64_t atSeconds) {
    std::string                       token = randomToken();
    const std::lock_guard<std::mutex> guard(mutex_);
    sweep(atSeconds);
    sessions_.emplace(token, Session{principal, atSeconds + ttlSeconds_});
    return token;
}

std::optional<Principal> SessionStore::resolve(const std::string& token) const {
    return resolveAt(token, nowSeconds());
}

std::optional<Principal> SessionStore::resolveAt(const std::string& token,
                                                 std::int64_t       atSeconds) const {
    if (token.empty()) return std::nullopt;
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto                        it = sessions_.find(token);
    if (it == sessions_.end() || it->second.expiresAt <= atSeconds) return std::nullopt;
    return it->second.principal;
}

void SessionStore::close(const std::string& token) {
    const std::lock_guard<std::mutex> guard(mutex_);
    sessions_.erase(token);
}

std::size_t SessionStore::closeUser(std::string_view username) {
    if (username.empty()) return 0;

    const std::lock_guard<std::mutex> guard(mutex_);
    std::size_t                       closed = 0;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.principal.username == username) {
            it = sessions_.erase(it);
            ++closed;
        } else {
            it = std::next(it);
        }
    }
    return closed;
}

std::size_t SessionStore::size() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return sessions_.size();
}

void SessionStore::sweep(std::int64_t atSeconds) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        it = it->second.expiresAt <= atSeconds ? sessions_.erase(it) : std::next(it);
    }
}

// --- LoginThrottle ---------------------------------------------------------

bool LoginThrottle::blocked() { return blockedAt(nowSeconds()); }

bool LoginThrottle::blockedAt(std::int64_t atSeconds) {
    const std::lock_guard<std::mutex> guard(mutex_);
    roll(atSeconds);
    return failures_ >= kMaxFailedLogins;
}

void LoginThrottle::recordFailure() { recordFailureAt(nowSeconds()); }

void LoginThrottle::recordFailureAt(std::int64_t atSeconds) {
    const std::lock_guard<std::mutex> guard(mutex_);
    roll(atSeconds);
    ++failures_;
}

void LoginThrottle::recordSuccess() {
    const std::lock_guard<std::mutex> guard(mutex_);
    failures_ = 0;
}

void LoginThrottle::roll(std::int64_t atSeconds) {
    if (atSeconds - windowStart_ >= kLoginWindowSeconds) {
        windowStart_ = atSeconds;
        failures_    = 0;
    }
}

}  // namespace monobucket
