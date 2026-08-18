#include "core/identity_migration.hpp"

#include "core/identity.hpp"
#include "core/logging.hpp"
#include "storage/records.hpp"

namespace monobucket {

std::size_t adoptOwnerlessAccessKeys(StorageEngine& storage, const std::string& owner) {
    std::size_t adopted = 0;
    for (auto key : storage.listAccessKeys()) {
        if (!key.owner.empty()) continue;
        key.owner = owner;
        storage.putAccessKey(key);
        ++adopted;
    }
    if (adopted > 0) {
        log::info("adopted ", adopted, " S3 access key", adopted == 1 ? "" : "s",
                  " issued before keys had owners into '", owner, "'");
    }
    return adopted;
}

IdentityMigration migrateLegacyIdentities(StorageEngine& storage) {
    IdentityMigration outcome;

    const auto legacy = storage.getAdmin();
    if (!legacy) return outcome;

    if (storage.listUsers().empty()) {
        UserRecord admin;
        admin.username     = legacy->username;
        admin.passwordHash = legacy->passwordHash;
        // The account that was the console is the account that can administer
        // it. Any other choice would silently take authority away from the only
        // person who had it.
        admin.role              = Role::Administrator;
        admin.createdAt         = legacy->createdAt;
        admin.updatedAt         = nowMs();
        admin.passwordChangedAt = legacy->updatedAt;
        storage.putUser(admin);

        outcome.migratedAdministrator = true;
        outcome.administrator         = admin.username;
        outcome.adoptedKeys           = adoptOwnerlessAccessKeys(storage, admin.username);

        log::info("migrated the console administrator '", admin.username,
                  "' to a user account with the administrator role");
    } else {
        outcome.droppedStaleAdministrator = true;
        log::warn("dropping a leftover single-administrator record for '", legacy->username,
                  "'; this data directory already has user accounts");
    }

    storage.deleteAdmin();
    return outcome;
}

}  // namespace monobucket
