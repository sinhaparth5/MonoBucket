#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/identity_migration.hpp"
#include "storage/records.hpp"
#include "storage/storage_engine.hpp"
#include "temporary_directory.hpp"

// The migration runs once per deployment, on data somebody else already has,
// and there is no second chance to get it right. It is exercised here against a
// store rather than through startup, which is why it is a free function.

using monobucket::adoptOwnerlessAccessKeys;
using monobucket::migrateLegacyIdentities;
using monobucket::Role;
using monobucket::StorageEngine;
using monobucket::testing::TemporaryDirectory;

namespace {

StorageEngine::Options optionsFor(const TemporaryDirectory& root) {
    StorageEngine::Options options;
    options.dataDir             = root.path();
    options.durability          = monobucket::Durability::None;
    options.metadataMemoryBytes = 8ull * 1024 * 1024;
    return options;
}

/// A store exactly as the previous release left it: one administrator record
/// and access keys with no owner.
void writeLegacyState(StorageEngine& storage) {
    monobucket::AdminRecord admin;
    admin.username     = "ops";
    admin.passwordHash = "pbkdf2-sha256$600000$aabb$ccdd";
    admin.createdAt    = 1000;
    admin.updatedAt    = 2000;
    storage.putAdmin(admin);

    for (const char* id : {"MBAAAAAAAAAAAAAAAAAA", "MBBBBBBBBBBBBBBBBBBB"}) {
        monobucket::AccessKeyRecord key;
        key.accessKeyId = id;
        key.secretKey   = "a-secret";
        key.createdAt   = 3000;
        storage.putAccessKey(key);
    }
}

}  // namespace

TEST_CASE("the single administrator becomes a user with the administrator role", "[identity]") {
    TemporaryDirectory root{"migrate-admin"};
    StorageEngine      storage(optionsFor(root));
    writeLegacyState(storage);

    const auto outcome = migrateLegacyIdentities(storage);

    CHECK(outcome.migratedAdministrator);
    CHECK(outcome.administrator == "ops");

    const auto user = storage.getUser("ops");
    REQUIRE(user.has_value());
    // Same name, same verifier, same creation date. Anything else would be a
    // migration that asked somebody to reset a password they still know.
    CHECK(user->passwordHash == "pbkdf2-sha256$600000$aabb$ccdd");
    CHECK(user->createdAt == 1000);
    CHECK_FALSE(user->disabled);
    // Administrator, because taking authority away from the only person who
    // had it is not a migration anybody can recover from.
    CHECK(user->role == Role::Administrator);
    CHECK(storage.countEnabledAdministrators() == 1);
}

TEST_CASE("the legacy record is dropped once it has been migrated", "[identity]") {
    TemporaryDirectory root{"migrate-drop"};
    StorageEngine      storage(optionsFor(root));
    writeLegacyState(storage);

    migrateLegacyIdentities(storage);
    CHECK_FALSE(storage.getAdmin().has_value());

    // Idempotent: a second start finds nothing to do and changes nothing.
    const auto again = migrateLegacyIdentities(storage);
    CHECK_FALSE(again.migratedAdministrator);
    CHECK(storage.listUsers().size() == 1);
}

TEST_CASE("keys issued before owners are adopted into the migrated account", "[identity]") {
    TemporaryDirectory root{"migrate-keys"};
    StorageEngine      storage(optionsFor(root));
    writeLegacyState(storage);

    const auto outcome = migrateLegacyIdentities(storage);
    CHECK(outcome.adoptedKeys == 2);

    // An unowned key cannot be authorised — the S3 path has no identity to ask
    // about — so leaving one behind would silently stop a working client.
    for (const auto& key : storage.listAccessKeys()) {
        INFO(key.accessKeyId);
        CHECK(key.owner == "ops");
    }
}

TEST_CASE("adoption leaves keys that already have an owner alone", "[identity]") {
    TemporaryDirectory root{"migrate-owned"};
    StorageEngine      storage(optionsFor(root));

    monobucket::AccessKeyRecord owned;
    owned.accessKeyId = "MBAAAAAAAAAAAAAAAAAA";
    owned.secretKey   = "a-secret";
    owned.owner       = "sam";
    storage.putAccessKey(owned);

    monobucket::AccessKeyRecord orphan;
    orphan.accessKeyId = "MBBBBBBBBBBBBBBBBBBB";
    orphan.secretKey   = "another-secret";
    storage.putAccessKey(orphan);

    CHECK(adoptOwnerlessAccessKeys(storage, "ops") == 1);
    CHECK(storage.getAccessKey("MBAAAAAAAAAAAAAAAAAA")->owner == "sam");
    CHECK(storage.getAccessKey("MBBBBBBBBBBBBBBBBBBB")->owner == "ops");
}

TEST_CASE("a leftover legacy record never overwrites existing users", "[identity]") {
    TemporaryDirectory root{"migrate-stale"};
    StorageEngine      storage(optionsFor(root));

    // Migrated, downgraded to a build that rewrote the legacy record, then
    // upgraded again. The user accounts are the newer truth.
    monobucket::UserRecord existing;
    existing.username     = "sam";
    existing.passwordHash = "the-current-verifier";
    existing.role         = Role::Operator;
    storage.putUser(existing);

    writeLegacyState(storage);

    const auto outcome = migrateLegacyIdentities(storage);
    CHECK_FALSE(outcome.migratedAdministrator);
    CHECK(outcome.droppedStaleAdministrator);

    // The stale record is gone and it promoted nobody on its way out.
    CHECK_FALSE(storage.getAdmin().has_value());
    CHECK_FALSE(storage.getUser("ops").has_value());
    CHECK(storage.getUser("sam")->role == Role::Operator);
}

TEST_CASE("a store that never had an administrator record is untouched", "[identity]") {
    TemporaryDirectory root{"migrate-none"};
    StorageEngine      storage(optionsFor(root));

    const auto outcome = migrateLegacyIdentities(storage);
    CHECK_FALSE(outcome.migratedAdministrator);
    CHECK_FALSE(outcome.droppedStaleAdministrator);
    CHECK(storage.listUsers().empty());
}
