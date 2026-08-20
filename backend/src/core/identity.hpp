#pragma once

#include <functional>
#include <map>
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

    /// Change an instance-wide limit that is stored rather than configured.
    ///
    /// Separate from SettingsRead because almost everything on the settings
    /// panel is environment-only and therefore unwritable by anyone; this is
    /// the permission for the handful of values that are not, starting with
    /// the maximum object-upload size. It is an instance-wide policy — one
    /// number that every bucket and every client is held to — so it is an
    /// administrator's call for the same reason CapacityWrite is.
    SettingsWrite,

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

/// What a user may do in one particular bucket.
///
/// A ceiling, never a grant. The role still has to allow the operation: a
/// ReadOnly user with Write access to a bucket still cannot write to it,
/// because the two are ANDed and the narrower answer wins. That is what makes
/// this safe to add to a store full of existing accounts — it can only ever
/// take authority away, never hand it out.
///
/// Ordered least to most, unlike Role, so that a comparison reads the way it
/// sounds. Nothing compares these yet; the order is stated so that the first
/// thing to do so does not have to guess.
enum class BucketAccess {
    /// The bucket is not listed and every request naming it is refused, which
    /// together is what "cannot see it" means. Not a 404: the bucket exists,
    /// and pretending otherwise would make "no such bucket" and "not yours"
    /// indistinguishable to the operator reading the audit log.
    None,

    /// Read the bucket's settings and its objects, and nothing else.
    Read,

    /// Whatever the role allows. The default, and what every account had
    /// before bucket access existed.
    Write,
};

std::string_view toString(BucketAccess access) noexcept;

/// Nullopt for a name this build does not know, for the reason parseRole gives.
std::optional<BucketAccess> parseBucketAccess(std::string_view name) noexcept;

/// One sentence, for the picker in the console.
std::string_view describe(BucketAccess access) noexcept;

/// How a level reads in a refusal — "no access", "read-only access". Beside
/// the enum because the S3 router and the console both record one, and two
/// phrasings of the same refusal is one more thing for an operator reading the
/// audit log to have to reconcile.
std::string_view describeHolding(BucketAccess access) noexcept;

/// Whether `access` leaves `permission` reachable.
///
/// Only the permissions that name a bucket or an object are constrained.
/// Managing users, issuing credentials and reading the audit log are not about
/// any one bucket, so bucket access has nothing to say about them and says so
/// by returning true — the role is the only thing deciding those, as before.
///
/// Exhaustive with no default arm, for the same reason `allows()` is: a
/// permission added to the enum has to be classified here rather than
/// inheriting whichever answer the default happened to be.
bool permits(BucketAccess access, Permission permission) noexcept;

/// A user's bucket access, as stored on their account.
///
/// Two parts because both questions get asked and one mechanism should answer
/// them: "this person works on these buckets and nothing else" is a fallback of
/// None with exceptions, and "this person works on everything except the one
/// holding the backups" is a fallback of Write with one exception. A record
/// written before this existed decodes as a Write fallback with no exceptions,
/// which is precisely what every account already had.
struct BucketGrants {
    /// What a bucket not named in `exceptions` resolves to.
    BucketAccess fallback = BucketAccess::Write;

    /// Buckets named explicitly. A std::map rather than a hash so that a round
    /// trip through the store is byte-stable, for the reason UserMetadata is;
    /// `std::less<>` so a lookup can be made from a string_view without
    /// materialising the key.
    std::map<std::string, BucketAccess, std::less<>> exceptions;

    /// The access this grants in `bucket`.
    BucketAccess forBucket(std::string_view bucket) const;

    /// Compared whole when deciding whether an update actually changed
    /// anything, so that a PATCH restating what is already stored does not
    /// close every session the account has open.
    friend bool operator==(const BucketGrants&, const BucketGrants&) = default;

    /// True when this is what every account had before bucket access existed:
    /// everything, decided by the role alone. The console renders that as "all
    /// buckets" rather than as a fallback and an empty list.
    bool unrestricted() const noexcept;
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

/// The whole authorisation decision for a request that names a bucket.
///
/// Administrators are deliberately not narrowed. An administrator can rewrite
/// their own grants, so enforcing them would be a lock whose key hangs on the
/// door; and the one failure worse than an unenforced rule here is one that
/// strands the only account able to repair it. The console refuses to store
/// grants for an administrator for the same reason, rather than storing a
/// restriction it would then ignore.
bool allows(Role role, const BucketGrants& grants, std::string_view bucket,
            Permission permission) noexcept;

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
