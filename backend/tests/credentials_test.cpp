#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>

#include "core/credentials.hpp"

namespace credentials = monobucket::credentials;

TEST_CASE("a generated access key id has the advertised shape", "[credentials]") {
    const std::string id = credentials::generateAccessKeyId();

    CHECK(id.size() == credentials::kAccessKeyIdLength);
    CHECK(id.starts_with(credentials::kAccessKeyIdPrefix));
    CHECK(credentials::plausibleAccessKeyId(id));
}

TEST_CASE("a generated secret has the advertised shape", "[credentials]") {
    const std::string secret = credentials::generateSecretKey();

    CHECK(secret.size() == credentials::kSecretKeyLength);

    // Alphanumeric throughout, so pasting one into a shell, a URL or a YAML
    // file needs no quoting and no escaping.
    CHECK(std::all_of(secret.begin(), secret.end(), [](const unsigned char ch) {
        return std::isalnum(ch) != 0;
    }));
}

TEST_CASE("generated credentials do not repeat", "[credentials]") {
    // Not a randomness test — a real one belongs to OpenSSL. This catches the
    // failure that actually happens: a generator seeded once, or not at all.
    std::set<std::string> ids;
    std::set<std::string> secrets;
    for (int i = 0; i < 200; ++i) {
        ids.insert(credentials::generateAccessKeyId());
        secrets.insert(credentials::generateSecretKey());
    }
    CHECK(ids.size() == 200);
    CHECK(secrets.size() == 200);
}

TEST_CASE("only ids this build could have issued are plausible", "[credentials]") {
    // The S3 router asks this before it touches RocksDB, so it is what keeps a
    // client's arbitrary bytes out of a database key.
    CHECK_FALSE(credentials::plausibleAccessKeyId(""));
    CHECK_FALSE(credentials::plausibleAccessKeyId("MB"));
    CHECK_FALSE(credentials::plausibleAccessKeyId("monobucket"));
    CHECK_FALSE(credentials::plausibleAccessKeyId("AKIAIOSFODNN7EXAMPLE"));

    // Right length and prefix, wrong alphabet: lowercase, and the digits the
    // id alphabet leaves out.
    CHECK_FALSE(credentials::plausibleAccessKeyId("MBabcdefghijklmnopqr"));
    CHECK_FALSE(credentials::plausibleAccessKeyId("MBAAAAAAAAAAAAAAAAA0"));
    CHECK_FALSE(credentials::plausibleAccessKeyId("MBAAAAAAAAAAAAAAAAA1"));
    CHECK_FALSE(credentials::plausibleAccessKeyId("MBAAAAAAAAAAAAAAAA/A"));

    // One character short and one over.
    CHECK_FALSE(credentials::plausibleAccessKeyId("MBAAAAAAAAAAAAAAAA"));
    CHECK_FALSE(credentials::plausibleAccessKeyId("MBAAAAAAAAAAAAAAAAAAA"));

    CHECK(credentials::plausibleAccessKeyId("MBAAAAAAAAAAAAAAAAAA"));
    CHECK(credentials::plausibleAccessKeyId("MB234567234567234567"));
}
