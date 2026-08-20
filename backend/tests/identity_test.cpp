#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "core/identity.hpp"
#include "s3/operation.hpp"

using monobucket::allows;
using monobucket::allPermissions;
using monobucket::isValidUsername;
using monobucket::parseRole;
using monobucket::Permission;
using monobucket::permissionsFor;
using monobucket::Role;
using monobucket::toString;

namespace {

/// The whole matrix, written out as data rather than derived from `allows()`.
/// A test that computed the expected answer the same way the implementation
/// does would agree with any change, including a wrong one.
struct Expected {
    Role       role;
    Permission permission;
    bool       allowed;
};

constexpr Expected kMatrix[] = {
    // An administrator holds everything, and the test says so one line at a
    // time so that a permission added without thinking about this role fails
    // the exhaustiveness check below rather than passing by inheritance.
    {Role::Administrator, Permission::BucketRead, true},
    {Role::Administrator, Permission::BucketWrite, true},
    {Role::Administrator, Permission::ObjectRead, true},
    {Role::Administrator, Permission::ObjectWrite, true},
    {Role::Administrator, Permission::SettingsRead, true},
    {Role::Administrator, Permission::SettingsWrite, true},
    {Role::Administrator, Permission::CapacityWrite, true},
    {Role::Administrator, Permission::CredentialRead, true},
    {Role::Administrator, Permission::CredentialWrite, true},
    {Role::Administrator, Permission::UserRead, true},
    {Role::Administrator, Permission::UserWrite, true},
    {Role::Administrator, Permission::AuditRead, true},
    {Role::Administrator, Permission::BackupWrite, true},

    // An operator runs the storage and cannot touch the people using it.
    {Role::Operator, Permission::BucketRead, true},
    {Role::Operator, Permission::BucketWrite, true},
    {Role::Operator, Permission::ObjectRead, true},
    {Role::Operator, Permission::ObjectWrite, true},
    {Role::Operator, Permission::SettingsRead, true},
    // Reading the resolved configuration is part of running the storage;
    // changing an instance-wide limit that every bucket is held to is not.
    {Role::Operator, Permission::SettingsWrite, false},
    // Sizing the bucket you are creating is BucketWrite; moving capacity
    // between buckets that already exist is not an operator's decision.
    {Role::Operator, Permission::CapacityWrite, false},
    {Role::Operator, Permission::CredentialRead, true},
    {Role::Operator, Permission::CredentialWrite, true},
    {Role::Operator, Permission::UserRead, false},
    {Role::Operator, Permission::UserWrite, false},
    {Role::Operator, Permission::AuditRead, false},
    // A backup carries every S3 secret in the instance out of the building.
    {Role::Operator, Permission::BackupWrite, false},

    // Read-only creates nothing at all, and that includes credentials.
    {Role::ReadOnly, Permission::BucketRead, true},
    {Role::ReadOnly, Permission::BucketWrite, false},
    {Role::ReadOnly, Permission::ObjectRead, true},
    {Role::ReadOnly, Permission::ObjectWrite, false},
    {Role::ReadOnly, Permission::SettingsRead, true},
    {Role::ReadOnly, Permission::SettingsWrite, false},
    {Role::ReadOnly, Permission::CapacityWrite, false},
    {Role::ReadOnly, Permission::CredentialRead, true},
    {Role::ReadOnly, Permission::CredentialWrite, false},
    {Role::ReadOnly, Permission::UserRead, false},
    {Role::ReadOnly, Permission::UserWrite, false},
    {Role::ReadOnly, Permission::AuditRead, false},
    {Role::ReadOnly, Permission::BackupWrite, false},
};

constexpr Role kRoles[] = {Role::Administrator, Role::Operator, Role::ReadOnly};

}  // namespace

TEST_CASE("every role is decided for every permission", "[identity]") {
    for (const Expected& expected : kMatrix) {
        INFO(std::string(toString(expected.role))
             << " / " << std::string(toString(expected.permission)));
        CHECK(allows(expected.role, expected.permission) == expected.allowed);
    }

    // The matrix above covers the enum exactly. A permission added to
    // `allPermissions()` and forgotten here leaves the product short.
    CHECK(std::size(kMatrix) == allPermissions().size() * std::size(kRoles));
}

TEST_CASE("only the administrator can manage users or read the audit log", "[identity]") {
    for (const Role role : kRoles) {
        const bool isAdmin = role == Role::Administrator;
        CHECK(allows(role, Permission::SettingsWrite) == isAdmin);
        CHECK(allows(role, Permission::CapacityWrite) == isAdmin);
        CHECK(allows(role, Permission::UserRead) == isAdmin);
        CHECK(allows(role, Permission::UserWrite) == isAdmin);
        CHECK(allows(role, Permission::AuditRead) == isAdmin);
    }
}

TEST_CASE("read-only writes nothing", "[identity]") {
    CHECK_FALSE(allows(Role::ReadOnly, Permission::BucketWrite));
    CHECK_FALSE(allows(Role::ReadOnly, Permission::ObjectWrite));
    // Including a credential, which would otherwise be a way to mint something
    // that outlives the session it was minted from.
    CHECK_FALSE(allows(Role::ReadOnly, Permission::CredentialWrite));
}

TEST_CASE("a role's permission list is exactly what it is allowed", "[identity]") {
    for (const Role role : kRoles) {
        const auto granted = permissionsFor(role);
        for (const Permission permission : allPermissions()) {
            const bool listed =
                std::find(granted.begin(), granted.end(), permission) != granted.end();
            INFO(std::string(toString(role)) << " / " << std::string(toString(permission)));
            CHECK(listed == allows(role, permission));
        }
    }
}

TEST_CASE("roles round-trip through their stored names", "[identity]") {
    for (const Role role : kRoles) {
        const auto parsed = parseRole(toString(role));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == role);
    }
}

TEST_CASE("an unrecognised role name is refused rather than defaulted", "[identity]") {
    // A record written by a newer build must not silently become the weakest
    // role — nor, far worse, the strongest.
    CHECK_FALSE(parseRole("").has_value());
    CHECK_FALSE(parseRole("Administrator").has_value());
    CHECK_FALSE(parseRole("superuser").has_value());
    CHECK_FALSE(parseRole("read-only").has_value());
}

TEST_CASE("permission names are distinct and stable", "[identity]") {
    // They cross the wire to the console and appear in the audit log, so a
    // collision would make two different refusals indistinguishable.
    std::set<std::string> names;
    for (const Permission permission : allPermissions()) {
        names.insert(std::string(toString(permission)));
    }
    CHECK(names.size() == allPermissions().size());
    CHECK(names.count("user:write") == 1);
}

TEST_CASE("usernames are held to what can be typed and stored", "[identity]") {
    CHECK(isValidUsername("admin"));
    CHECK(isValidUsername("a"));
    CHECK(isValidUsername("ops.team_2"));
    CHECK(isValidUsername("0day"));
    CHECK(isValidUsername(std::string(64, 'a')));

    CHECK_FALSE(isValidUsername(""));
    CHECK_FALSE(isValidUsername(std::string(65, 'a')));
    // Must not start with punctuation: a name that sorts oddly in the key space
    // is a name nobody can find in the list.
    CHECK_FALSE(isValidUsername("-admin"));
    CHECK_FALSE(isValidUsername(".hidden"));
    CHECK_FALSE(isValidUsername("_root"));
    // A NUL separates key components, so a name carrying one could address
    // another record entirely.
    CHECK_FALSE(isValidUsername(std::string("ad\0min", 6)));
    CHECK_FALSE(isValidUsername("ad min"));
    CHECK_FALSE(isValidUsername("admin@example.com"));
    CHECK_FALSE(isValidUsername("../../etc/passwd"));
}

// --- S3 operations ---------------------------------------------------------

using monobucket::s3::Operation;
using monobucket::s3::permissionFor;

TEST_CASE("every read-only S3 operation needs only a read permission", "[identity]") {
    // The two tables are written independently — `isReadOnly` for the anonymous
    // path and `permissionFor` for the signed one — and this is what keeps them
    // from disagreeing about what counts as a read.
    constexpr Operation kAll[] = {
        Operation::ListBuckets,        Operation::CreateBucket,
        Operation::DeleteBucket,       Operation::HeadBucket,
        Operation::ListObjectsV1,      Operation::ListObjectsV2,
        Operation::ListMultipartUploads, Operation::DeleteObjects,
        Operation::GetBucketLocation,  Operation::GetBucketVersioning,
        Operation::GetBucketPolicy,    Operation::PutBucketPolicy,
        Operation::DeleteBucketPolicy, Operation::GetBucketAcl,
        Operation::PutBucketAcl,       Operation::GetBucketCors,
        Operation::PutBucketCors,      Operation::DeleteBucketCors,
        Operation::GetObject,          Operation::HeadObject,
        Operation::PutObject,          Operation::DeleteObject,
        Operation::CreateMultipartUpload, Operation::UploadPart,
        Operation::ListParts,          Operation::CompleteMultipartUpload,
        Operation::AbortMultipartUpload,
    };

    for (const Operation operation : kAll) {
        const Permission needed = permissionFor(operation);
        const bool       isRead =
            needed == Permission::BucketRead || needed == Permission::ObjectRead;
        INFO(std::string(toString(operation)));
        CHECK(isRead == monobucket::s3::isReadOnly(operation));
    }
}

TEST_CASE("a read-only identity cannot mutate anything over S3", "[identity]") {
    constexpr Operation kWrites[] = {
        Operation::CreateBucket,   Operation::DeleteBucket,
        Operation::PutBucketPolicy, Operation::DeleteBucketPolicy,
        Operation::PutBucketAcl,   Operation::PutBucketCors,
        Operation::DeleteBucketCors, Operation::PutObject,
        Operation::DeleteObject,   Operation::DeleteObjects,
        Operation::CreateMultipartUpload, Operation::UploadPart,
        Operation::CompleteMultipartUpload, Operation::AbortMultipartUpload,
    };

    for (const Operation operation : kWrites) {
        INFO(std::string(toString(operation)));
        CHECK_FALSE(allows(Role::ReadOnly, permissionFor(operation)));
        CHECK(allows(Role::Operator, permissionFor(operation)));
        CHECK(allows(Role::Administrator, permissionFor(operation)));
    }
}

TEST_CASE("a read-only identity can still read over S3", "[identity]") {
    constexpr Operation kReads[] = {
        Operation::ListBuckets,   Operation::HeadBucket,  Operation::ListObjectsV2,
        Operation::GetObject,     Operation::HeadObject,  Operation::ListParts,
        Operation::GetBucketCors, Operation::GetBucketPolicy,
    };

    for (const Operation operation : kReads) {
        INFO(std::string(toString(operation)));
        CHECK(allows(Role::ReadOnly, permissionFor(operation)));
    }
}

TEST_CASE("an unroutable operation defaults to the permission fewest hold", "[identity]") {
    // Neither is ever dispatched, but if the order of the checks in the router
    // ever changed, the safe answer is the one nobody but a writer holds.
    CHECK_FALSE(allows(Role::ReadOnly, permissionFor(Operation::Unsupported)));
    CHECK_FALSE(allows(Role::ReadOnly, permissionFor(Operation::MethodNotAllowed)));
}
