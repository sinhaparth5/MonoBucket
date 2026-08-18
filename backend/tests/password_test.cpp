#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/password.hpp"

using monobucket::password::dummyHash;
using monobucket::password::hash;
using monobucket::password::verify;

namespace {

/// The production cost is deliberately several hundred milliseconds. Paying it
/// in every assertion below would turn this file into the slowest thing in the
/// suite for no extra coverage — the iteration count is a parameter of the
/// format, and that it is *read back* from the record is itself asserted.
constexpr std::uint32_t kFast = 1000;

}  // namespace

TEST_CASE("a password verifies against its own hash", "[password]") {
    const std::string stored = hash("correct horse battery staple", kFast);
    CHECK(verify("correct horse battery staple", stored));
}

TEST_CASE("a wrong password does not verify", "[password]") {
    const std::string stored = hash("correct horse battery staple", kFast);

    CHECK_FALSE(verify("Correct horse battery staple", stored));
    CHECK_FALSE(verify("correct horse battery stapl", stored));
    CHECK_FALSE(verify("correct horse battery staple ", stored));
    CHECK_FALSE(verify("", stored));
}

TEST_CASE("the same password hashes differently every time", "[password]") {
    // Two identical passwords must not produce identical records, or the store
    // answers "do these two accounts share a password" to anyone who reads it.
    const std::string first  = hash("a shared password", kFast);
    const std::string second = hash("a shared password", kFast);

    CHECK(first != second);
    CHECK(verify("a shared password", first));
    CHECK(verify("a shared password", second));
}

TEST_CASE("the record names its own cost", "[password]") {
    // Raising the iteration count must not strand records written at the old
    // one, which only works if the record carries it.
    const std::string cheap = hash("a password worth keeping", kFast);
    const std::string dear  = hash("a password worth keeping", kFast * 4);

    CHECK(cheap.find("$1000$") != std::string::npos);
    CHECK(dear.find("$4000$") != std::string::npos);
    CHECK(verify("a password worth keeping", cheap));
    CHECK(verify("a password worth keeping", dear));
}

TEST_CASE("an unusable record denies access rather than throwing", "[password]") {
    // Every one of these reaches verify() from the login route. A throw there
    // is a 500 that an attacker can produce at will; false is a 401.
    CHECK_FALSE(verify("anything", ""));
    CHECK_FALSE(verify("anything", "not a hash at all"));
    CHECK_FALSE(verify("anything", "pbkdf2-sha256$"));
    CHECK_FALSE(verify("anything", "pbkdf2-sha256$1000$"));
    CHECK_FALSE(verify("anything", "pbkdf2-sha256$1000$abcd"));
    CHECK_FALSE(verify("anything", "pbkdf2-sha256$0$abcd$ef01"));
    CHECK_FALSE(verify("anything", "pbkdf2-sha256$notanumber$abcd$ef01"));
    CHECK_FALSE(verify("anything", "pbkdf2-sha256$1000$zzzz$ef01"));
    CHECK_FALSE(verify("anything", "argon2id$1000$abcd$ef01"));
}

TEST_CASE("an absurd iteration count is refused rather than computed", "[password]") {
    // A corrupt record naming ten billion rounds would otherwise turn one
    // login attempt into an outage.
    CHECK_FALSE(verify("anything", "pbkdf2-sha256$999999999$abcd$ef01"));
}

TEST_CASE("a tampered digest does not verify", "[password]") {
    std::string stored = hash("the original password", kFast);
    stored.back()      = stored.back() == 'a' ? 'b' : 'a';
    CHECK_FALSE(verify("the original password", stored));
}

TEST_CASE("the placeholder hash matches nothing", "[password]") {
    // It exists to cost the same as a real verification when the username is
    // unknown. If anything ever verified against it, a login would succeed.
    CHECK_FALSE(verify("", dummyHash()));
    CHECK_FALSE(verify("admin", dummyHash()));
    CHECK_FALSE(verify("password", dummyHash()));
    CHECK(dummyHash().starts_with("pbkdf2-sha256$"));

    // Stable across calls: a new one per attempt would cost a hash to build.
    CHECK(dummyHash() == dummyHash());
}
