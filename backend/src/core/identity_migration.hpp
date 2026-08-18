#pragma once

#include <cstddef>
#include <string>

#include "storage/storage_engine.hpp"

// Bringing a data directory written before per-user identities up to date.
//
// Free functions rather than Server members so that the one part of startup
// that rewrites existing records can be tested against a store instead of
// against a listener. A migration only ever runs once per deployment, on
// somebody else's data, which is exactly the code that has to be right the
// first time.

namespace monobucket {

/// What a migration did, for the log and for a test to assert on.
struct IdentityMigration {
    /// True when a single-administrator record was converted into a user.
    bool migratedAdministrator = false;

    /// Set when a legacy record was found but user accounts already existed —
    /// a store that was migrated, downgraded and upgraded again. The user
    /// records are the newer truth, so the leftover is dropped rather than
    /// allowed to overwrite them.
    bool droppedStaleAdministrator = false;

    /// The account the legacy record became, when it became one.
    std::string administrator;

    std::size_t adoptedKeys = 0;
};

/// Attributes access keys issued before credentials had owners.
///
/// Leaving them unowned would leave the S3 path with a credential it cannot
/// attribute and therefore cannot authorise — which, given the rule that a key
/// never exceeds its owner, would mean refusing every request they sign.
/// Adopting them keeps them working exactly as they did and makes them
/// revocable by name, which is the point of ownership.
std::size_t adoptOwnerlessAccessKeys(StorageEngine& storage, const std::string& owner);

/// Converts the single-administrator record into a user with the administrator
/// role and drops it. A no-op on a store that never had one.
IdentityMigration migrateLegacyIdentities(StorageEngine& storage);

}  // namespace monobucket
