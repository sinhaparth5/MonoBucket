#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Who a request is, and what that entitles it to.
//
// MonoBucket had one identity — the root pair — and then two kinds of identity:
// an administrator for the console and access keys for S3. Neither of those
// answered "what may this person do", because the answer was always
// "everything". This file is that answer, and it is a pure function of a role
// and a permission so the whole matrix can be asserted in a test rather than
// discovered one handler at a time.
//
// Roles are a fixed, closed set rather than a policy language. A policy
// document would be a second authorisation system beside the bucket policies
// that already exist, with its own evaluation order and its own way of being
// subtly wrong; three roles cover the deployments this server is for, and
// anything past them is S3's IAM, which is not a feature that fits in a
// single binary.

namespace monobucket {

/// What a user is allowed to be.
///
/// Ordered from most to least authority, and that order is load-bearing:
/// `atLeast()` compares them, so inserting a role in the middle changes what
/// existing comparisons mean. Add to the ends, or add a permission instead.
enum class Role {
    /// Everything, including managing other users and reading the audit log.
    /// The only role that can create or destroy an identity.
    Administrator,

    /// Full authority over buckets and objects, and over their own S3 access
    /// keys. Cannot see or change other people, and cannot read the audit log
    /// — an operator who could edit the record of what they did is not audited.
    Operator,

    /// Reads buckets, objects and the resolved configuration. Creates nothing,
    /// and that deliberately includes S3 access keys: a role whose name says it
    /// only reads should not be able to mint a credential, even one that could
    /// only read.
    ReadOnly,
};

/// One protected capability. Handlers name a permission, never a role — a
/// handler that asks "is this an administrator?" has to be revisited every time
/// the role set changes, and a handler that asks "may this identity write
/// buckets?" does not.
enum class Permission {
    BucketRead,       ///< List buckets, read their settings, list objects
    BucketWrite,      ///< Create, delete, and change access, CORS and policy
    ObjectRead,       ///< Read object data and metadata, and presign a GET
    ObjectWrite,      ///< Upload and delete objects
    SettingsRead,     ///< The resolved configuration and the server overview

    /// Change an existing bucket's storage allocation.
    ///
    /// Separate from BucketWrite because it is not a decision about one
    /// bucket: allocations are drawn from one instance-wide capacity, so
    /// raising this bucket's is taking it away from whoever asks next. Anyone
    /// who may create a bucket may size the one they are creating — that is
    /// bounded by what is unallocated — but moving capacity between buckets
    /// that already exist is an administrator's call.
    CapacityWrite,

    CredentialRead,   ///< List S3 access keys
    CredentialWrite,  ///< Issue, rotate and revoke S3 access keys
    UserRead,         ///< List users and their roles
    UserWrite,        ///< Create, update, disable and delete users
    AuditRead,        ///< Read the security event log
};

/// Every permission, in declaration order. Used to render a role's grants and
/// to drive the exhaustive tests — a permission added to the enum and forgotten
/// here fails those tests rather than silently going ungranted.
const std::vector<Permission>& allPermissions();

std::string_view toString(Role role) noexcept;
std::string_view toString(Permission permission) noexcept;

/// Nullopt rather than a default: a stored role this build does not recognise
/// is a record written by a newer version, and guessing at it would either
/// grant authority that was never granted or silently demote someone.
std::optional<Role> parseRole(std::string_view name) noexcept;

/// One sentence, for the role picker in the console. Kept beside the matrix so
/// the description and the grants cannot drift.
std::string_view describe(Role role) noexcept;

/// The whole authorisation decision, as a pure function.
bool allows(Role role, Permission permission) noexcept;

/// The permissions a role holds, in `allPermissions()` order. Sent to the
/// console so it can hide what it must not offer — which is a courtesy to the
/// person using it and never the enforcement. Every route checks for itself.
std::vector<Permission> permissionsFor(Role role);

/// Whether a username may be stored.
///
/// A username becomes a RocksDB key component and is compared against the
/// session subject, so it is held to what can be typed unambiguously and
/// round-tripped exactly: 1-64 characters of ASCII letters, digits, and
/// `. _ -`, starting with a letter or digit. That excludes NUL — which the
/// keyspace uses as its separator — but the rule is deliberately much narrower
/// than "no NUL", because two accounts whose names differ by an invisible
/// character are two accounts nobody can tell apart.
bool isValidUsername(std::string_view username) noexcept;

}  // namespace monobucket
