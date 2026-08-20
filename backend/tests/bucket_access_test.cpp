#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>
#include <string>

#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include "core/identity.hpp"
#include "storage/codec.hpp"
#include "storage/keyspace.hpp"
#include "storage/metadata_store.hpp"
#include "storage/records.hpp"
#include "temporary_directory.hpp"

using monobucket::allows;
using monobucket::allPermissions;
using monobucket::BucketAccess;
using monobucket::BucketGrants;
using monobucket::parseBucketAccess;
using monobucket::Permission;
using monobucket::permits;
using monobucket::Role;
using monobucket::toString;
using monobucket::UserRecord;
using monobucket::testing::TemporaryDirectory;

namespace {

/// Which permissions each level leaves reachable, written out as data rather
/// than derived from `permits()`. A test that computed the expected answer the
/// way the implementation does would agree with any change, wrong ones
/// included — the same reason the role matrix is spelled out in
/// identity_test.cpp.
struct Expected {
    BucketAccess access;
    Permission   permission;
    bool         permitted;
};

constexpr Expected kMatrix[] = {
    // No access: the bucket is not readable, not writable, and not resizable.
    {BucketAccess::None, Permission::BucketRead, false},
    {BucketAccess::None, Permission::BucketWrite, false},
    {BucketAccess::None, Permission::ObjectRead, false},
    {BucketAccess::None, Permission::ObjectWrite, false},
    {BucketAccess::None, Permission::CapacityWrite, false},
    // Not about any one bucket, so this level has nothing to say about them.
    {BucketAccess::None, Permission::SettingsRead, true},
    {BucketAccess::None, Permission::SettingsWrite, true},
    {BucketAccess::None, Permission::CredentialRead, true},
    {BucketAccess::None, Permission::CredentialWrite, true},
    {BucketAccess::None, Permission::UserRead, true},
    {BucketAccess::None, Permission::UserWrite, true},
    {BucketAccess::None, Permission::AuditRead, true},

    // Read: the two reads and nothing that changes anything.
    {BucketAccess::Read, Permission::BucketRead, true},
    {BucketAccess::Read, Permission::BucketWrite, false},
    {BucketAccess::Read, Permission::ObjectRead, true},
    {BucketAccess::Read, Permission::ObjectWrite, false},
    {BucketAccess::Read, Permission::CapacityWrite, false},
    {BucketAccess::Read, Permission::SettingsRead, true},
    {BucketAccess::Read, Permission::SettingsWrite, true},
    {BucketAccess::Read, Permission::CredentialRead, true},
    {BucketAccess::Read, Permission::CredentialWrite, true},
    {BucketAccess::Read, Permission::UserRead, true},
    {BucketAccess::Read, Permission::UserWrite, true},
    {BucketAccess::Read, Permission::AuditRead, true},

    // Write: everything, which is what an account that was never narrowed has.
    {BucketAccess::Write, Permission::BucketRead, true},
    {BucketAccess::Write, Permission::BucketWrite, true},
    {BucketAccess::Write, Permission::ObjectRead, true},
    {BucketAccess::Write, Permission::ObjectWrite, true},
    {BucketAccess::Write, Permission::CapacityWrite, true},
    {BucketAccess::Write, Permission::SettingsRead, true},
    {BucketAccess::Write, Permission::SettingsWrite, true},
    {BucketAccess::Write, Permission::CredentialRead, true},
    {BucketAccess::Write, Permission::CredentialWrite, true},
    {BucketAccess::Write, Permission::UserRead, true},
    {BucketAccess::Write, Permission::UserWrite, true},
    {BucketAccess::Write, Permission::AuditRead, true},
};

BucketGrants onlyReports() {
    BucketGrants grants;
    grants.fallback = BucketAccess::None;
    grants.exceptions.emplace("reports", BucketAccess::Write);
    grants.exceptions.emplace("archive", BucketAccess::Read);
    return grants;
}

monobucket::MetadataStoreOptions storeOptions(const TemporaryDirectory& root) {
    monobucket::MetadataStoreOptions options;
    options.path              = (root.path() / "meta").string();
    options.memoryBudgetBytes = 8ull * 1024 * 1024;
    return options;
}

}  // namespace

// --- The matrix -------------------------------------------------------------

TEST_CASE("each access level permits exactly the permissions it should",
          "[bucket-access][identity]") {
    for (const Expected& expected : kMatrix) {
        INFO(std::string(toString(expected.access))
             << " should " << (expected.permitted ? "permit " : "refuse ")
             << toString(expected.permission));
        CHECK(permits(expected.access, expected.permission) == expected.permitted);
    }
}

TEST_CASE("the access matrix covers every permission", "[bucket-access][identity]") {
    // A permission added to the enum and not classified above would otherwise
    // pass this suite by never being asserted about at all.
    for (const BucketAccess access :
         {BucketAccess::None, BucketAccess::Read, BucketAccess::Write}) {
        std::set<Permission> covered;
        for (const Expected& expected : kMatrix) {
            if (expected.access == access) covered.insert(expected.permission);
        }
        for (const Permission permission : allPermissions()) {
            INFO(std::string(toString(access)) << " / " << toString(permission));
            CHECK(covered.count(permission) == 1);
        }
    }
}

TEST_CASE("access levels round-trip through their names", "[bucket-access][identity]") {
    for (const BucketAccess access :
         {BucketAccess::None, BucketAccess::Read, BucketAccess::Write}) {
        CHECK(parseBucketAccess(toString(access)) == access);
    }

    // A level a newer build might have. Nullopt rather than a guess, for the
    // reason an unrecognised role is.
    CHECK_FALSE(parseBucketAccess("admin").has_value());
    CHECK_FALSE(parseBucketAccess("").has_value());
    CHECK_FALSE(parseBucketAccess("WRITE").has_value());
}

// --- Resolution -------------------------------------------------------------

TEST_CASE("a bucket falls to the default unless it is named", "[bucket-access][identity]") {
    const BucketGrants grants = onlyReports();

    CHECK(grants.forBucket("reports") == BucketAccess::Write);
    CHECK(grants.forBucket("archive") == BucketAccess::Read);
    // Not named, so the fallback answers — including for a bucket created
    // after the grants were written, which is the case that matters.
    CHECK(grants.forBucket("payroll") == BucketAccess::None);
    CHECK(grants.forBucket("") == BucketAccess::None);
}

TEST_CASE("an untouched grant set is unrestricted", "[bucket-access][identity]") {
    CHECK(BucketGrants{}.unrestricted());
    CHECK(BucketGrants{}.forBucket("anything") == BucketAccess::Write);

    BucketGrants narrowed;
    narrowed.exceptions.emplace("payroll", BucketAccess::None);
    CHECK_FALSE(narrowed.unrestricted());

    BucketGrants byDefault;
    byDefault.fallback = BucketAccess::Read;
    CHECK_FALSE(byDefault.unrestricted());
}

// --- The combined decision --------------------------------------------------

TEST_CASE("bucket access narrows a role and never widens it", "[bucket-access][identity]") {
    BucketGrants everything;
    everything.fallback = BucketAccess::Write;

    // A readonly account handed write access to a bucket still cannot write to
    // it. This is the whole reason the two are ANDed rather than consulted in
    // order, and the property that makes the feature safe to add to a store
    // full of existing accounts.
    CHECK_FALSE(allows(Role::ReadOnly, everything, "reports", Permission::ObjectWrite));
    CHECK(allows(Role::ReadOnly, everything, "reports", Permission::ObjectRead));

    // And an operator narrowed to nothing loses what the role would have given.
    const BucketGrants grants = onlyReports();
    CHECK(allows(Role::Operator, grants, "reports", Permission::ObjectWrite));
    CHECK(allows(Role::Operator, grants, "archive", Permission::ObjectRead));
    CHECK_FALSE(allows(Role::Operator, grants, "archive", Permission::ObjectWrite));
    CHECK_FALSE(allows(Role::Operator, grants, "payroll", Permission::BucketRead));
    CHECK_FALSE(allows(Role::Operator, grants, "payroll", Permission::ObjectRead));
}

TEST_CASE("an administrator is never narrowed", "[bucket-access][identity]") {
    // The console refuses to store this, but the decision must not depend on
    // the console having refused: a record from an older build, or one written
    // by hand, must not be able to lock the last administrator out of a bucket
    // only an administrator could unlock.
    const BucketGrants grants = onlyReports();

    for (const Permission permission : allPermissions()) {
        INFO(toString(permission));
        CHECK(allows(Role::Administrator, grants, "payroll", permission));
    }
}

TEST_CASE("permissions that name no bucket are left to the role",
          "[bucket-access][identity]") {
    const BucketGrants grants = onlyReports();

    // An operator shut out of every bucket still issues their own access keys,
    // and still cannot read the audit log. Bucket access has no opinion either
    // way; the role has both.
    CHECK(allows(Role::Operator, grants, "payroll", Permission::CredentialWrite));
    CHECK_FALSE(allows(Role::Operator, grants, "payroll", Permission::AuditRead));
    CHECK(allows(Role::ReadOnly, grants, "payroll", Permission::SettingsRead));
    CHECK_FALSE(allows(Role::ReadOnly, grants, "payroll", Permission::CredentialWrite));
}

// --- The encoding -----------------------------------------------------------

TEST_CASE("bucket grants survive a round trip", "[bucket-access][codec]") {
    const BucketGrants grants = onlyReports();

    std::string               buffer;
    monobucket::codec::Writer writer(buffer);
    monobucket::encodeBucketGrants(writer, grants);

    monobucket::codec::Reader reader(buffer);
    const BucketGrants        decoded = monobucket::decodeBucketGrants(reader);

    CHECK(decoded == grants);
    CHECK(reader.exhausted());
}

TEST_CASE("an unrestricted grant set encodes to almost nothing", "[bucket-access][codec]") {
    std::string               buffer;
    monobucket::codec::Writer writer(buffer);
    monobucket::encodeBucketGrants(writer, BucketGrants{});

    // The name "write", length-prefixed, and a zero count. Every account that
    // was never narrowed pays this and no more.
    CHECK(buffer.size() == 7);

    monobucket::codec::Reader reader(buffer);
    CHECK(monobucket::decodeBucketGrants(reader).unrestricted());
}

TEST_CASE("an access level this build does not know is refused", "[bucket-access][codec]") {
    // What a record written by a newer build would look like. Refused rather
    // than guessed at: every guess available is either "grant more than was
    // granted" or "silently lock somebody out".
    std::string               buffer;
    monobucket::codec::Writer writer(buffer);
    writer.string("append");
    writer.varint(0);

    monobucket::codec::Reader reader(buffer);
    CHECK_THROWS_AS(monobucket::decodeBucketGrants(reader), monobucket::codec::DecodeError);

    std::string               inException;
    monobucket::codec::Writer other(inException);
    other.string("write");
    other.varint(1);
    other.string("reports");
    other.string("append");

    monobucket::codec::Reader second(inException);
    CHECK_THROWS_AS(monobucket::decodeBucketGrants(second), monobucket::codec::DecodeError);
}

// --- The store --------------------------------------------------------------

TEST_CASE("a user's bucket grants survive the metadata store", "[bucket-access][storage]") {
    TemporaryDirectory root("bucket-access-store");
    auto               store = monobucket::openRocksMetadataStore(storeOptions(root));

    UserRecord user;
    user.username     = "sam";
    user.passwordHash = "pbkdf2$fake";
    user.role         = Role::Operator;
    user.buckets      = onlyReports();
    user.createdAt    = monobucket::nowMs();
    user.updatedAt    = user.createdAt;
    store->putUser(user);

    const auto read = store->getUser("sam");
    REQUIRE(read.has_value());
    CHECK(read->role == Role::Operator);
    CHECK(read->buckets == onlyReports());
    CHECK(read->buckets.forBucket("payroll") == BucketAccess::None);
}

TEST_CASE("an account written before bucket access existed reads unrestricted",
          "[bucket-access][storage]") {
    TemporaryDirectory root("bucket-access-legacy");
    const auto         options = storeOptions(root);

    // Written byte for byte the way the encoder wrote a user before this
    // change: version, verifier, role name, disabled, three timestamps — and
    // then nothing. Spelled out literally rather than produced by the current
    // encoder, because the point is what an older build left on disk, and a
    // test that asked today's code what that was would agree with any answer.
    std::string               legacy;
    monobucket::codec::Writer writer(legacy);
    writer.u8(1);  // kRecordVersion
    writer.string("pbkdf2$fake");
    writer.string("operator");
    writer.boolean(false);
    writer.varint(1'700'000'000'000);  // createdAt
    writer.varint(1'700'000'000'000);  // updatedAt
    writer.varint(0);                  // passwordChangedAt

    {
        rocksdb::DB*     raw = nullptr;
        rocksdb::Options open;
        open.create_if_missing = true;
        REQUIRE(rocksdb::DB::Open(open, options.path, &raw).ok());
        const std::unique_ptr<rocksdb::DB> db(raw);
        REQUIRE(db->Put(rocksdb::WriteOptions(), monobucket::keys::user("sam"), legacy).ok());
    }

    auto       store = monobucket::openRocksMetadataStore(options);
    const auto read  = store->getUser("sam");

    REQUIRE(read.has_value());
    CHECK(read->role == Role::Operator);
    // Everything, decided by the role alone — which is exactly what that
    // account already had — and no error on the way to finding out.
    CHECK(read->buckets.unrestricted());
    CHECK(allows(read->role, read->buckets, "payroll", Permission::ObjectWrite));
}
