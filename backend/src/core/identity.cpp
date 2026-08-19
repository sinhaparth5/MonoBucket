#include "core/identity.hpp"

#include <algorithm>
#include <cctype>

namespace monobucket {

const std::vector<Permission>& allPermissions() {
    static const std::vector<Permission> kAll{
        Permission::BucketRead,      Permission::BucketWrite,     Permission::ObjectRead,
        Permission::ObjectWrite,     Permission::SettingsRead,    Permission::SettingsWrite,
        Permission::CapacityWrite,   Permission::CredentialRead,  Permission::CredentialWrite,
        Permission::UserRead,        Permission::UserWrite,       Permission::AuditRead,
    };
    return kAll;
}

std::string_view toString(Role role) noexcept {
    switch (role) {
        case Role::Administrator: return "administrator";
        case Role::Operator:      return "operator";
        case Role::ReadOnly:      return "readonly";
    }
    return "readonly";
}

std::string_view toString(Permission permission) noexcept {
    switch (permission) {
        case Permission::BucketRead:      return "bucket:read";
        case Permission::BucketWrite:     return "bucket:write";
        case Permission::ObjectRead:      return "object:read";
        case Permission::ObjectWrite:     return "object:write";
        case Permission::SettingsRead:    return "settings:read";
        case Permission::SettingsWrite:   return "settings:write";
        case Permission::CapacityWrite:   return "capacity:write";
        case Permission::CredentialRead:  return "credential:read";
        case Permission::CredentialWrite: return "credential:write";
        case Permission::UserRead:        return "user:read";
        case Permission::UserWrite:       return "user:write";
        case Permission::AuditRead:       return "audit:read";
    }
    return "";
}

std::optional<Role> parseRole(std::string_view name) noexcept {
    if (name == "administrator") return Role::Administrator;
    if (name == "operator") return Role::Operator;
    if (name == "readonly") return Role::ReadOnly;
    return std::nullopt;
}

std::string_view describe(Role role) noexcept {
    switch (role) {
        case Role::Administrator:
            return "Everything, including managing users and reading the audit log.";
        case Role::Operator:
            return "Buckets, objects and their own access keys. No user management.";
        case Role::ReadOnly:
            return "Reads buckets, objects and settings. Changes nothing and issues no keys.";
    }
    return "";
}

bool allows(Role role, Permission permission) noexcept {
    // Written as a full matrix rather than as a hierarchy with exceptions.
    // A hierarchy reads well until the first exception, and then every reader
    // has to hold both the ladder and the list of rungs it does not apply to.
    switch (role) {
        case Role::Administrator:
            // The only role with no gaps, which is the one thing about it worth
            // stating: an administrator is defined by holding everything, not
            // by a list that has to be extended alongside the enum.
            return true;

        case Role::Operator:
            switch (permission) {
                case Permission::BucketRead:
                case Permission::BucketWrite:
                case Permission::ObjectRead:
                case Permission::ObjectWrite:
                case Permission::SettingsRead:
                case Permission::CredentialRead:
                case Permission::CredentialWrite:
                    return true;
                case Permission::SettingsWrite:
                case Permission::CapacityWrite:
                case Permission::UserRead:
                case Permission::UserWrite:
                case Permission::AuditRead:
                    return false;
            }
            return false;

        case Role::ReadOnly:
            switch (permission) {
                case Permission::BucketRead:
                case Permission::ObjectRead:
                case Permission::SettingsRead:
                case Permission::CredentialRead:
                    return true;
                case Permission::BucketWrite:
                case Permission::ObjectWrite:
                case Permission::SettingsWrite:
                case Permission::CapacityWrite:
                case Permission::CredentialWrite:
                case Permission::UserRead:
                case Permission::UserWrite:
                case Permission::AuditRead:
                    return false;
            }
            return false;
    }
    return false;
}

std::vector<Permission> permissionsFor(Role role) {
    std::vector<Permission> granted;
    for (const Permission permission : allPermissions()) {
        if (allows(role, permission)) granted.push_back(permission);
    }
    return granted;
}

bool isValidUsername(std::string_view username) noexcept {
    if (username.empty() || username.size() > 64) return false;

    const auto isBase = [](unsigned char ch) {
        return std::isalnum(ch) != 0;
    };
    if (!isBase(static_cast<unsigned char>(username.front()))) return false;

    return std::all_of(username.begin(), username.end(), [&](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return isBase(byte) || byte == '.' || byte == '_' || byte == '-';
    });
}

}  // namespace monobucket
