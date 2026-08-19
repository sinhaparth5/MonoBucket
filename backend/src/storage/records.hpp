#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/identity.hpp"
#include "storage/checksum.hpp"
#include "storage/codec.hpp"
#include "storage/durability.hpp"

// The metadata schema, as plain structs. These cross the boundary between the
// storage engine and the S3 protocol layer, so field names track S3 vocabulary
// rather than RocksDB vocabulary.

namespace monobucket {

/// Milliseconds since the Unix epoch. S3 renders timestamps to second
/// precision, but multipart part ordering and the dashboard both benefit from
/// keeping the finer resolution we already have.
using TimestampMs = std::int64_t;

TimestampMs nowMs() noexcept;

/// Renders a timestamp as ISO-8601 in UTC, the format S3 responses use.
std::string toIso8601(TimestampMs ms);

/// User-supplied `x-amz-meta-*` headers, minus the prefix. Ordered so that a
/// round-trip through the store is byte-stable, which keeps ETags of the
/// metadata itself comparable and makes tests deterministic.
using UserMetadata = std::map<std::string, std::string>;

/// One rule of a bucket's CORS configuration.
///
/// Stored decomposed rather than as the XML it arrived in: a rule is consulted
/// on every cross-origin request, and re-parsing a document to answer a header
/// question would put an XML parse on the read path. GetBucketCors renders it
/// back, which is also what makes the round trip normalising rather than
/// byte-preserving — as S3's own is.
struct CorsRule {
    /// Optional client-supplied label, echoed back untouched.
    std::string id;

    /// Each entry is an exact origin or a pattern with at most one `*`.
    std::vector<std::string> allowedOrigins;

    /// Uppercased. S3 accepts only GET, PUT, POST, DELETE and HEAD here.
    std::vector<std::string> allowedMethods;

    /// Lowercased, because the header names they are matched against are
    /// case-insensitive and the comparison should not have to keep saying so.
    std::vector<std::string> allowedHeaders;

    /// Response headers a browser script is allowed to read. Without these a
    /// cross-origin fetch can see the body but not the ETag.
    std::vector<std::string> exposeHeaders;

    /// Negative means the rule sets no max age and the browser picks.
    std::int32_t maxAgeSeconds = -1;
};

/// The CORS rules' wire form, shared by the RocksDB bucket record and the
/// request-path bucket cache. One encoding rather than two: the cache is a
/// second copy of the same record, and a cached bucket that had quietly lost
/// its rules would answer preflights differently depending on whether the last
/// write had aged out.
void                  encodeCorsRules(codec::Writer& writer, const std::vector<CorsRule>& rules);
std::vector<CorsRule> decodeCorsRules(codec::Reader& reader);

struct BucketRecord {
    std::string name;
    TimestampMs createdAt = 0;

    /// Anonymous GETs of objects are allowed when set — `s3:GetObject` for
    /// Principal `*`, and nothing wider.
    bool publicRead = false;

    /// Anonymous listing of the bucket's keys is allowed when set —
    /// `s3:ListBucket`.
    ///
    /// Separate from `publicRead` because they are different exposures and a
    /// policy says which it means: publishing objects somebody already has the
    /// name of is not publishing the names. They were once one flag, which
    /// meant a policy granting only `s3:GetObject` also opened the bucket to
    /// anonymous enumeration — a grant no document had asked for.
    bool publicList = false;

    /// The policy document as written, or empty for no policy. Stored verbatim
    /// so `GetBucketPolicy` returns what was put; the flags above are what is
    /// actually enforced, and a document that cannot be reduced to them is
    /// refused at write time rather than stored.
    std::string policy;

    /// Empty means CORS is not enabled, which is not the same as enabled with
    /// no rules: S3 answers a preflight against the former with 403 and the
    /// latter cannot occur, because an empty CORSConfiguration is refused.
    std::vector<CorsRule> cors;

    /// The bucket's storage allocation, in bytes. Zero means unlimited.
    ///
    /// Only the allocation is stored, never what the bucket currently holds:
    /// usage is re-derived from the objects themselves at startup and
    /// maintained in memory from then on. A usage counter written beside the
    /// records it counts is a counter that can disagree with them after any
    /// interrupted write, and nothing could then say which of the two is right.
    std::uint64_t quotaBytes = 0;

    /// Overrides `MONOBUCKET_DURABILITY` for writes to this bucket.
    ///
    /// Unset means "follow the server", which is deliberately not the same as
    /// storing today's server setting: an operator who raises the global level
    /// expects every bucket that never asked for something else to rise with
    /// it. Only a bucket that explicitly asked stays where it is.
    std::optional<Durability> durability;
};

/// One console account.
///
/// Separate from the S3 credential on purpose: a person signing in to a browser
/// and a program signing a request are different acts with different lifetimes,
/// and collapsing them means the only way to revoke a colleague's console
/// access is to break every S3 client at the same time.
struct UserRecord {
    std::string username;

    /// A password verifier, never the password. Produced by
    /// `password::hash()`; the format identifies its own parameters so the
    /// iteration count can be raised without stranding existing records.
    std::string passwordHash;

    Role role = Role::ReadOnly;

    /// A disabled account still exists and still owns its access keys. That is
    /// the difference between disabling and deleting: disabling is reversible
    /// and keeps the audit trail attributable, deleting takes the keys with it.
    bool disabled = false;

    TimestampMs createdAt = 0;
    TimestampMs updatedAt = 0;

    /// When the verifier was last replaced. Distinct from `updatedAt`, which
    /// also moves for a role change — "when did this password last change" is a
    /// question a role change is not an answer to.
    TimestampMs passwordChangedAt = 0;
};

/// The single administrator account that predates per-user identities.
///
/// Only ever read, and only once: startup converts it into a UserRecord with
/// the administrator role and deletes it. Kept as a type rather than folded
/// into UserRecord because its stored layout has no role and no disabled flag,
/// and a decoder that guessed at them would be inventing authority.
struct AdminRecord {
    std::string username;
    std::string passwordHash;
    TimestampMs createdAt = 0;
    TimestampMs updatedAt = 0;
};

/// One security-relevant thing that happened.
///
/// The log is a bounded ring in the metadata store, not a file and not a
/// stream: it answers "who changed this, and when" for an operator looking at
/// the console, and it is explicitly not a substitute for shipping logs
/// somewhere durable. Denied authorisation checks are recorded alongside the
/// changes, because the attempt is the interesting half.
struct AuditRecord {
    /// Assigned by the store, strictly increasing, and what the log is ordered
    /// by. Not the timestamp: two events in the same millisecond are ordinary,
    /// and a clock that steps backwards must not reorder history.
    std::uint64_t sequence = 0;

    TimestampMs atMs = 0;

    /// The username that acted, or empty when the request never got as far as
    /// naming one.
    std::string actor;

    /// A dotted verb — `user.create`, `credential.revoke`, `authz.denied`.
    /// Matched by prefix in the console filter, so the first component is the
    /// subject and the second is what happened to it.
    std::string action;

    /// What was acted on: a username, an access key id, a bucket name.
    std::string target;

    /// False for a refusal. A log that only recorded successes would answer
    /// "what was done" and never "what was tried".
    bool allowed = true;

    /// Free text, already safe to render. Never a secret and never a password.
    std::string detail;
};

/// One S3 credential pair, issued and revoked from the console.
struct AccessKeyRecord {
    std::string accessKeyId;

    /// Stored recoverable, not hashed, and that is forced rather than chosen:
    /// SigV4 is a symmetric HMAC construction, so verifying a signature means
    /// re-deriving the signing key from the secret itself. There is no verifier
    /// that authenticates a signature without reproducing the secret. Treat the
    /// data directory accordingly — see "Known limitations" in README.md.
    std::string secretKey;

    /// Free text from whoever minted it, so a key can be recognised months
    /// later without a separate note somewhere else.
    std::string description;

    /// The username this key acts as. A signed request is authorised with the
    /// owner's role, so a key can never do more than the person who issued it —
    /// and disabling that person stops the key on its next request.
    ///
    /// Empty only for records written before keys had owners. Startup adopts
    /// those into the migrated administrator account rather than leaving them
    /// unattributable; nothing issues an ownerless key any more.
    std::string owner;

    TimestampMs createdAt = 0;

    /// When the secret was last replaced, or 0 if it never was. Rotation keeps
    /// the id and changes the secret, so this is the only record that the value
    /// a client holds may have stopped working.
    TimestampMs rotatedAt = 0;
};

struct ObjectRecord {
    std::string key;

    /// Identifies the payload in the blob store. An object row and its blob are
    /// written in that order and torn down in the reverse, which is what makes
    /// a half-finished PUT invisible rather than corrupt.
    std::string blobId;

    std::uint64_t size = 0;

    /// MD5 hex for a single-part object, `<hex>-<parts>` for a completed
    /// multipart upload. Returned verbatim in the ETag header.
    std::string etag;

    /// Full-payload SHA-256. Lets a future fsck verify integrity without
    /// trusting the filesystem, and satisfies clients that ask for it back.
    std::string sha256;

    std::string  contentType = "application/octet-stream";
    TimestampMs  lastModified = 0;
    UserMetadata userMetadata;

    /// The `x-amz-checksum-*` value the client asked to have verified, kept so
    /// a reader can be given the same guarantee the writer got. Absent for an
    /// object uploaded without one, which stays the ordinary case — S3 reports
    /// no checksum for those and neither do we.
    Checksum checksum;
};

struct PartRecord {
    std::uint32_t partNumber = 0;
    std::string   blobId;
    std::uint64_t size = 0;
    std::string   etag;  ///< MD5 hex of this part alone
    TimestampMs   uploadedAt = 0;

    /// This part's own checksum. Retained after the part is consumed only in
    /// the sense that the completed object's composite is computed from it —
    /// a composite cannot be recovered once the parts are gone, so it is
    /// computed at completion and the parts are then free to go.
    Checksum checksum;
};

struct UploadRecord {
    std::string  uploadId;
    std::string  bucket;
    std::string  key;
    std::string  contentType = "application/octet-stream";
    TimestampMs  createdAt   = 0;
    UserMetadata userMetadata;

    /// The algorithm named at CreateMultipartUpload, if any. Every part is
    /// then checksummed under it whether or not the client sends a value,
    /// because the composite at completion needs all of them and a part
    /// uploaded without one cannot be re-read cheaply to supply it.
    std::optional<ChecksumAlgorithm> checksumAlgorithm;
};

/// One page of ListMultipartUploads.
///
/// The upload rows are keyed `u<bucket>\0<key>\0<uploadId>`, so a listing
/// resumes from the pair S3 pages on without a token of our own: the key
/// marker and the upload-id marker together name the last row returned, and
/// the next page starts immediately after it.
struct ListUploadsRequest {
    std::string prefix;

    /// Exclusive lower bound, both halves. An upload-id marker without a key
    /// marker is meaningless — S3 refuses that combination and so do we.
    std::string keyMarker;
    std::string uploadIdMarker;

    std::uint32_t maxUploads = 1000;
};

struct ListUploadsResult {
    std::vector<UploadRecord> uploads;

    bool truncated = false;

    /// What resumes this listing. Only meaningful when truncated.
    std::string nextKeyMarker;
    std::string nextUploadIdMarker;
};

/// One page of ListObjects/ListObjectsV2.
struct ListObjectsRequest {
    std::string prefix;
    std::string delimiter;

    /// Exclusive lower bound. Serves both `start-after` (V2) and `marker` (V1),
    /// and is what a continuation token resolves to.
    std::string startAfter;

    std::uint32_t maxKeys = 1000;
};

struct ListObjectsResult {
    std::vector<ObjectRecord> objects;

    /// Keys rolled up by the delimiter, each ending with the delimiter itself.
    std::vector<std::string> commonPrefixes;

    bool truncated = false;

    /// The `startAfter` that resumes this listing. Only meaningful when
    /// truncated; a common prefix can be the resumption point, not just a key.
    std::string nextStartAfter;
};

/// Aggregate counters for `/metrics` and the dashboard's capacity panel.
struct UsageStats {
    std::uint64_t buckets      = 0;
    std::uint64_t objects      = 0;
    std::uint64_t bytes        = 0;
    std::uint64_t uploads      = 0;  ///< multipart uploads currently in progress
    std::uint64_t orphanBlobs  = 0;  ///< payloads awaiting reclamation
};

}  // namespace monobucket
