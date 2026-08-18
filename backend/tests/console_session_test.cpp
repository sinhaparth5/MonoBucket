#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "server/console_session.hpp"

using monobucket::kLoginWindowSeconds;
using monobucket::kMaxFailedLogins;
using monobucket::LoginThrottle;
using monobucket::Role;
using monobucket::SessionStore;

namespace {

/// An arbitrary fixed point. Every assertion below is relative to it, so none
/// of them depend on the wall clock.
constexpr std::int64_t kNow = 1'760'000'000;

}  // namespace

TEST_CASE("a session resolves to the user that opened it", "[session]") {
    SessionStore sessions;

    const std::string token = sessions.openAt("admin", Role::Administrator, kNow);
    const auto        principal = sessions.resolveAt(token, kNow);
    REQUIRE(principal.has_value());
    CHECK(principal->username == "admin");

    // Never a credential — the whole separation rests on this.
    CHECK(principal->username != "MBAAAAAAAAAAAAAAAAAA");
}

TEST_CASE("a session carries the role it was opened with", "[session]") {
    SessionStore sessions;

    const std::string reader = sessions.openAt("sam", Role::ReadOnly, kNow);
    const std::string admin  = sessions.openAt("admin", Role::Administrator, kNow);

    // Two people signed in at once do not share an authority level, which is
    // the thing a single-account console never had to answer.
    REQUIRE(sessions.resolveAt(reader, kNow).has_value());
    REQUIRE(sessions.resolveAt(admin, kNow).has_value());
    CHECK(sessions.resolveAt(reader, kNow)->role == Role::ReadOnly);
    CHECK(sessions.resolveAt(admin, kNow)->role == Role::Administrator);
}

TEST_CASE("an unknown token resolves to nothing", "[session]") {
    SessionStore sessions;
    sessions.openAt("admin", Role::Administrator, kNow);

    CHECK_FALSE(sessions.resolveAt("", kNow).has_value());
    CHECK_FALSE(sessions.resolveAt("00000000", kNow).has_value());
}

TEST_CASE("tokens are unguessable and distinct", "[session]") {
    SessionStore          sessions;
    std::set<std::string> tokens;
    for (int i = 0; i < 100; ++i) {
        tokens.insert(sessions.openAt("admin", Role::Administrator, kNow));
    }

    CHECK(tokens.size() == 100);
    // 32 bytes of randomness, hex encoded.
    CHECK(tokens.begin()->size() == 64);
}

TEST_CASE("a session expires once its lifetime is up", "[session]") {
    SessionStore sessions(60);

    const std::string token = sessions.openAt("admin", Role::Administrator, kNow);
    REQUIRE(sessions.resolveAt(token, kNow + 59).has_value());
    CHECK(sessions.resolveAt(token, kNow + 59)->username == "admin");

    // Refused at the boundary and after it, without anything having swept.
    CHECK_FALSE(sessions.resolveAt(token, kNow + 60).has_value());
    CHECK_FALSE(sessions.resolveAt(token, kNow + 10'000).has_value());
}

TEST_CASE("expired sessions are collected when a new one opens", "[session]") {
    SessionStore sessions(60);

    for (int i = 0; i < 5; ++i) sessions.openAt("admin", Role::Administrator, kNow);
    CHECK(sessions.size() == 5);

    // Opening is the only thing that sweeps, which is why the leak this
    // guards against is bounded by the login rate rather than by uptime.
    sessions.openAt("admin", Role::Administrator, kNow + 61);
    CHECK(sessions.size() == 1);
}

TEST_CASE("signing out invalidates the token immediately", "[session]") {
    SessionStore sessions;

    const std::string token = sessions.openAt("admin", Role::Administrator, kNow);
    REQUIRE(sessions.resolveAt(token, kNow).has_value());

    sessions.close(token);
    CHECK_FALSE(sessions.resolveAt(token, kNow).has_value());

    // Idempotent: a second sign-out, or one for a token that never existed, is
    // not an error the caller has to handle.
    sessions.close(token);
    sessions.close("never-issued");
}

TEST_CASE("signing out one session leaves the others alone", "[session]") {
    SessionStore sessions;

    const std::string first  = sessions.openAt("admin", Role::Administrator, kNow);
    const std::string second = sessions.openAt("admin", Role::Administrator, kNow);

    sessions.close(first);
    CHECK_FALSE(sessions.resolveAt(first, kNow).has_value());
    REQUIRE(sessions.resolveAt(second, kNow).has_value());
    CHECK(sessions.resolveAt(second, kNow)->username == "admin");
}

TEST_CASE("disabling a user ends every session that user holds", "[session]") {
    SessionStore sessions;

    const std::string first  = sessions.openAt("sam", Role::Operator, kNow);
    const std::string second = sessions.openAt("sam", Role::Operator, kNow);
    const std::string other  = sessions.openAt("admin", Role::Administrator, kNow);

    CHECK(sessions.closeUser("sam") == 2);

    // Both of theirs, and only theirs. A session carries a copy of the role, so
    // a change to the account has to reach every copy or the tab keeps the
    // authority it was handed at sign-in.
    CHECK_FALSE(sessions.resolveAt(first, kNow).has_value());
    CHECK_FALSE(sessions.resolveAt(second, kNow).has_value());
    CHECK(sessions.resolveAt(other, kNow).has_value());
}

TEST_CASE("closing the sessions of an unknown user changes nothing", "[session]") {
    SessionStore sessions;
    const std::string token = sessions.openAt("admin", Role::Administrator, kNow);

    CHECK(sessions.closeUser("nobody") == 0);
    CHECK(sessions.closeUser("") == 0);
    CHECK(sessions.resolveAt(token, kNow).has_value());
}

TEST_CASE("repeated failures lock the login route", "[session]") {
    LoginThrottle throttle;

    for (int i = 0; i < kMaxFailedLogins - 1; ++i) {
        throttle.recordFailureAt(kNow);
        CHECK_FALSE(throttle.blockedAt(kNow));
    }

    throttle.recordFailureAt(kNow);
    CHECK(throttle.blockedAt(kNow));
}

TEST_CASE("the lockout lifts when the window rolls", "[session]") {
    LoginThrottle throttle;
    for (int i = 0; i < kMaxFailedLogins; ++i) throttle.recordFailureAt(kNow);
    REQUIRE(throttle.blockedAt(kNow));

    CHECK(throttle.blockedAt(kNow + kLoginWindowSeconds - 1));
    CHECK_FALSE(throttle.blockedAt(kNow + kLoginWindowSeconds));
}

TEST_CASE("a successful login clears the failure count", "[session]") {
    LoginThrottle throttle;
    for (int i = 0; i < kMaxFailedLogins - 1; ++i) throttle.recordFailureAt(kNow);

    throttle.recordSuccess();

    // Whoever mistyped their password nine times and then got it right is not
    // one keystroke away from locking themselves out.
    for (int i = 0; i < kMaxFailedLogins - 1; ++i) {
        throttle.recordFailureAt(kNow);
        CHECK_FALSE(throttle.blockedAt(kNow));
    }
}
