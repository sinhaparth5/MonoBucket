# Changelog

All notable changes to MonoBucket are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## Versioning: CalVer `YYYY.0M.MICRO`

MonoBucket releases on **calendar versioning**, not semantic versioning. The
release cadence is what the version communicates; API compatibility is
communicated separately (see *Compatibility* below).

```
        2026.08.3
        ────┬─── ─┬─ ┬
            │     │  └── MICRO  incrementing release number within the month,
            │     │             starting at 0 and reset each month
            │     └───── 0M     zero-padded month of release  (01–12)
            └─────────── YYYY   full year of release
```

| Segment | Rule |
| --- | --- |
| `YYYY` | Four-digit year the artefact was published. |
| `0M` | Zero-padded month, always two digits. `2026.8.0` is never valid; `2026.08.0` is. |
| `MICRO` | Starts at `0` for the first release of a month and increments for every subsequent release that month, whatever its size. Resets to `0` when the month rolls over. |

**Why CalVer.** MonoBucket is deployed as a container image, and the question an
operator actually asks is "how old is what I am running?". A calendar version
answers that at a glance. It also removes the recurring argument over whether a
change is "minor" or "major" in a project whose external contract is the S3 API,
not our own.

### Compatibility

CalVer says nothing about breaking changes, so they are called out explicitly:

- Every entry that changes an environment variable, an on-disk layout, or an
  HTTP contract appears under a **Breaking** heading in this file.
- On-disk formats carry their own storage-format version, independent of the
  release version. A release that requires a migration says so under
  **Breaking** and ships the migration.
- The S3 wire protocol is fixed by AWS; we only ever move toward more of it.

### Pre-1.0 status

Until Phase 4 is complete, MonoBucket is not a durable store. Assume any
pre-release build may require wiping `MONOBUCKET_DATA_DIR` on upgrade.
Pre-release builds are tagged `-alpha.N` / `-beta.N`, e.g. `2026.08.0-alpha.1`.

### Docker image tags

Published to `ghcr.io/sinhaparth5/monobucket`:

| Tag | Points at |
| --- | --- |
| `2026.08.3` | One exact, immutable release. Use this in production. |
| `2026.08` | The newest MICRO within that month. |
| `2026` | The newest release within that year. |
| `latest` | The newest stable release overall. |
| `edge` | Every push to `master`. Not for production. |

### Cutting a release

```bash
scripts/cut-release.sh --dry-run     # what it would do
scripts/cut-release.sh               # next MICRO for the current month
scripts/cut-release.sh 2026.09.0     # or an explicit version
```

It rewrites the version in `CMakeLists.txt`, promotes the `[Unreleased]` block
below under a dated CalVer heading, commits and tags.

Pushing the tag is what publishes. `release.yml` refuses to go on unless the
tag, `CMakeLists.txt` and this file agree, then builds both architectures on
native runners, smoke-tests each on its own hardware, merges them into one
manifest, signs it with cosign and creates the GitHub release from the section
below matching the version.

Each heading's entries become the release notes verbatim, so nothing may follow
the last one — which is why the template for the next release lives here, above
them, rather than at the foot of the file:

<!--
## [YYYY.0M.MICRO] — YYYY-MM-DD

### Breaking
### Added
### Changed
### Deprecated
### Removed
### Fixed
### Security
-->

---

## [Unreleased]

### Added

- **Objects now keep `Cache-Control`, `Content-Disposition`, `Content-Encoding`,
  `Content-Language` and `Expires`.** All five are read from `PutObject` and
  from `CreateMultipartUpload`, stored with the object, and returned unchanged
  by `GetObject` and `HeadObject`. Until now only `Content-Type` and the
  `x-amz-meta-*` headers survived a write, which had three consequences worth
  naming: a CDN in front of a public bucket had nothing to cache on and
  revalidated every asset on every request; a pre-compressed object stored with
  `Content-Encoding: gzip` came back as undecodable bytes labelled as the
  original type, so there was no way to serve one correctly; and a bucket could
  not offer a file as a download with a filename.
- The five are accepted on the console's upload endpoint as well, so an object
  put through the dashboard is not a second-class one a CDN has to be told
  about separately, and they are reported by `/_mb/api/object`.
- `CreateMultipartUpload` is the only request in a multipart upload that can
  name them — `UploadPart` describes a part and completion has no
  representation of its own — so they are carried on the upload record and
  applied to the object at completion.

### Changed

- The `response-cache-control`, `response-content-disposition`,
  `response-content-encoding`, `response-content-language` and
  `response-expires` query parameters still override the stored value for that
  one response, which is what presigned download links rely on. A parameter
  that is present but blank is not an override: it says nothing and the stored
  value stands.
- A `304 Not Modified` now carries `Cache-Control` and `Expires`, per RFC 9110
  §15.4.5. A 304 is what a CDN gets when it revalidates, and one that came back
  without them left the edge with nothing to say how long the copy it had just
  confirmed could be kept.
- An object written before this release reads back with the five absent and no
  error. The fields are appended to the stored record and read only if the row
  is long enough to hold them, so no format version changed and nothing needs
  migrating. Cached object entries carry them too — the cache's own version byte
  went from 2 to 3, which makes a rolling upgrade discard the older entries
  rather than serve an object without its cache directives.
- `Content-Encoding: aws-chunked` is no longer stored. It describes the upload's
  framing, which is gone by the time the object exists; a client that gzipped
  its object and let the SDK frame the upload sends `aws-chunked,gzip`, and the
  `gzip` is what is kept.

### Security

- A stored response header is refused at write time if it contains a control
  character — CR and LF above all — or exceeds 1024 bytes. These values are
  emitted on every read of the object, so one stored CR would split every
  subsequent response for that key. The check is enforced in the storage engine
  as well as in the S3 layer, so no write path can skip it. Recorded under
  *Known limitations* in `README.md` as the third deliberate deviation from S3.

### Fixed

- The console build is warning-free again. `registerConsoleApi()` discarded the
  `[[nodiscard]]` result of `IoExecutor::post` when writing an audit entry; the
  drop was always deliberate — a saturated queue must shed the record rather
  than delay the request it describes — but the call site never said so and now
  logs the dropped action at debug level.
- Removed `secretsMatch()` from `console_api.cpp`. It was the timing-safe
  comparison console sign-in used before authentication moved to a PBKDF2 user
  verifier, and has been unreachable since; `<openssl/crypto.h>` went with it as
  the file's only user.

---

## [2026.08.9] — 2026-08-19

### Changed

- **The console now uses MonoBucket's Sulu green visual system.** Both DaisyUI
  themes have been rebuilt around the same eleven-step palette, with separate
  light and dark surface mappings and accessible foreground pairs. Buttons,
  navigation, panels, charts, focus states and status treatments now share one
  brand language instead of the previous indigo, cyan and magenta mix.
- Headings and product names use the self-hosted Space Grotesk variable font;
  Inter remains the body face for dense controls, tables and object metadata.
  Neither font requires a request outside the MonoBucket binary.
- The MonoBucket mark has been redrawn as a reusable SVG for the console,
  favicon and documentation. The login hero, dashboard banner, bucket states,
  workspace and settings illustrations have also been regenerated in the Sulu
  palette and kept as size-matched WebP assets so the new artwork does not
  shift the layouts that contain it.

---

## [2026.08.8] — 2026-08-19

### Security

- **The `x-amz-checksum-*` family is verified instead of discarded.** Every
  current AWS SDK computes a checksum on upload and sends it; MonoBucket parsed
  it, threw it away, and answered `200`. A client whose object was corrupted in
  transit was told the upload had been verified end to end. CRC32, CRC32C,
  CRC64NVME, SHA1 and SHA256 are now checked on `PutObject` and `UploadPart`,
  whether the value arrives as a header or as an `aws-chunked` trailer, and a
  mismatch is refused with `BadDigest` before anything is committed. An
  algorithm this build cannot compute is refused rather than accepted and
  ignored.
- **The trailer signature on a signed streaming upload is verified.**
  `STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER` bodies carry an
  `x-amz-trailer-signature` chained from the final chunk's signature; it was
  parsed and dropped. An unverified trailer is a checksum anything on the path
  can rewrite to match bytes it also rewrote, so a declared trailer block
  without a valid signature is now `SignatureDoesNotMatch`.
- **`Content-MD5` is now checked on `PutObject` and `UploadPart`.** It was only
  ever verified on `DeleteObjects`; on an upload the header was accepted and
  never compared. A request that sends both `Content-MD5` and an
  `x-amz-checksum-*` value has both checked.

### Added

- Checksums are stored with the object and returned on `GetObject`/`HeadObject`
  when the request carries `x-amz-checksum-mode: ENABLED`, alongside
  `x-amz-checksum-type`. Whole-object reads only — the stored value does not
  describe a range.
- `CreateMultipartUpload` honours `x-amz-checksum-algorithm`: every part is then
  checksummed under it, and a part naming a different algorithm is refused.
- `CompleteMultipartUpload` returns the composite checksum (a checksum of the
  parts' raw checksums, `-N` suffixed) and validates the client's own
  `x-amz-checksum-*` header against it *before* the object is published, so a
  disagreement leaves the parts intact and nothing visible. `ListParts` reports
  each part's checksum and the upload's algorithm.
### Changed

- The object cache entry format carries the checksum, so a warm read and a cold
  read answer a checksum-mode `GET` identically.
- `x-amz-checksum-type: FULL_OBJECT` is refused at `CreateMultipartUpload`.
  Multipart objects get the composite form; answering a `FULL_OBJECT` request
  with a composite would hand back a number computed the other way from the one
  the client is about to compare it against.

---

## [2026.08.7] — 2026-08-19

### Security

- **A bucket policy containing a `Deny` no longer grants anonymous access.**
  `Deny` statements were skipped entirely while evaluating a document, so a
  policy publishing a bucket *except* one prefix published the prefix too, and
  nothing in the response, the stored document or the logs said the exclusion
  had not taken. Policies are now held to a closed grammar and anything outside
  it — `Deny`, `Condition`, `NotAction`/`NotResource`/`NotPrincipal`, a named
  principal, an unsupported action, or a resource scoped to a key prefix — is
  refused at `PutBucketPolicy` with a message naming the element. A policy that
  is stored is a policy that is enforced.
- **Granting `s3:GetObject` no longer opens a bucket to anonymous listing.**
  Anonymous access was gated on one `publicRead` flag against every read-only
  operation, so a policy granting object reads also permitted anonymous
  `ListObjectsV2`, `GetBucketPolicy`, `GetBucketCors` and `ListMultipartUploads`.
  Object reads and key listing are now separate grants, matched against the
  operation, and the bucket-configuration reads are never anonymous.
- **Policies stored by an earlier build are re-read at startup.** The enforced
  flags are derived data, so a policy whose `Deny` was silently ignored is
  stored with the bucket marked public; the flags are re-derived on every start
  and a document this build will not evaluate now grants nothing. The document
  itself is left untouched. Affected buckets are named in the log.

### Changed

- The console's policy editor shows object reads and key listing separately and
  states which subset of the policy language is enforced.
- The console's public-access toggle grants anonymous object reads only. Key
  listing is the wider exposure and is now asked for explicitly, with a policy.

---

## [2026.08.6] — 2026-08-19

### Added

- **Abandoned multipart uploads are now reclaimed.** A new
  `MONOBUCKET_MULTIPART_EXPIRY_HOURS` (default 168, `0` disables) aborts an
  upload that has gone that long without a part arriving, releasing its parts
  and the allocation they were charged against. Previously nothing expired
  them: an upload's parts are properly referenced, so the payload sweeper never
  treated them as garbage, and a client that crashed mid-transfer left bytes on
  disk and charged to the bucket for good. What decides is the upload's most
  recent part rather than when it began, so a slow client is safe for as long
  as it is still making progress. The sweep rides the existing reclamation
  tick and is bounded per pass.
- `ListMultipartUploads` now paginates. `key-marker` and `upload-id-marker` are
  honoured, `NextKeyMarker`/`NextUploadIdMarker` are returned, and
  `IsTruncated` reports the truth — it was previously hardcoded to `false`,
  which left more than `max-uploads` uploads impossible to enumerate. An
  `upload-id-marker` without a `key-marker` is refused, as S3 refuses it.

### Security

- **A bucket policy containing a `Deny` no longer grants anonymous access.**
  `Deny` statements were skipped entirely while evaluating a document, so a
  policy publishing a bucket *except* one prefix published the prefix too, and
  nothing in the response, the stored document or the logs said the exclusion
  had not taken. Policies are now held to a closed grammar and anything outside
  it — `Deny`, `Condition`, `NotAction`/`NotResource`/`NotPrincipal`, a named
  principal, an unsupported action, or a resource scoped to a key prefix — is
  refused at `PutBucketPolicy` with a message naming the element. A policy that
  is stored is a policy that is enforced.
- **Granting `s3:GetObject` no longer opens a bucket to anonymous listing.**
  Anonymous access was gated on one `publicRead` flag against every read-only
  operation, so a policy granting object reads also permitted anonymous
  `ListObjectsV2`, `GetBucketPolicy`, `GetBucketCors` and `ListMultipartUploads`.
  Object reads and key listing are now separate grants, matched against the
  operation, and the bucket-configuration reads are never anonymous.
- **Policies stored by an earlier build are re-read at startup.** The enforced
  flags are derived data, so a policy whose `Deny` was silently ignored is
  stored with the bucket marked public; the flags are re-derived on every start
  and a document this build will not evaluate now grants nothing. The document
  itself is left untouched. Affected buckets are named in the log.

### Changed

- The console's policy editor shows object reads and key listing separately and
  states which subset of the policy language is enforced.
- The console's public-access toggle grants anonymous object reads only. Key
  listing is the wider exposure and is now asked for explicitly, with a policy.
- `--fsck` pages through a bucket's in-progress uploads rather than requesting
  them all at once.

---

## [2026.08.5] — 2026-08-19

### Added

- **A configurable maximum object-upload size.** One limit for the whole
  instance, applied to every object whatever path it arrives by: the console
  uploader, a signed `PutObject`, a streaming (`aws-chunked`) body, and a
  multipart upload measured across all of its parts. An object over it is
  refused before any of it is committed — `EntityTooLarge` on the S3 listener,
  413 on the console API — and leaves no object and no orphaned upload state.
- `MONOBUCKET_MAX_UPLOAD_BYTES` — the limit a store that has never carried one
  starts at. **A seed, not an override:** it is written to the metadata store
  on first start, and from then on the console owns the figure, so a redeploy
  carrying a stale value cannot silently undo a limit somebody set.
- `MONOBUCKET_MAX_UPLOAD_CEILING_BYTES` — the most the console may raise the
  limit to. Environment only, so an administrator cannot lift their own
  ceiling. Defaults to 5 TiB, S3's own maximum object size.
- `GET`/`POST /_mb/api/upload-limit`, and a **Maximum object size** control on
  the settings panel. Reading it is `settings:read`; changing it is a new
  `settings:write` permission held by administrators alone. Changes are
  recorded in the audit log as `settings.upload-limit`.
- The console file picker refuses an oversized selection before sending
  anything, naming the file and the limit. A courtesy — the backend checks
  again and is what decides.
- `monobucket_max_upload_bytes` and `monobucket_max_upload_ceiling_bytes` on
  `/metrics`.

### Changed

- **Objects assembled by multipart are now capped at 5 GiB by default.** Single
  PUTs were already bounded by `MONOBUCKET_MAX_BODY_BYTES`; multipart was
  bounded by nothing. If you store objects larger than that, raise
  `MONOBUCKET_MAX_UPLOAD_BYTES` before upgrading, or raise the limit from the
  settings panel afterwards — which needs no restart.

### Removed

- `deploy/` — the compose file and environment template added in 2026.08.4. The
  repository already ships `docker-compose.yml`, and a second one drifts.

### Notes

- **When a change takes effect.** Every admission decision uses the limit in
  force at the moment it is taken. A transfer already under way is not
  interrupted; the next part of a multipart upload is held to the new figure;
  and a completion is judged against the object it is about to produce, so
  lowering the limit can refuse an upload whose parts were all accepted under
  the old one. That upload's parts are left untouched, so the client can abort
  it or complete a smaller subset.
- **Scope is the instance, deliberately.** Not per-bucket and not per-user: a
  limit with more than one scope needs a precedence rule, and a precedence rule
  is the start of a policy language this server has refused to grow. A bucket's
  allocation already answers "how much may this bucket hold"; this answers "how
  large may one object be".
- `CopyObject` remains 501, so there is no copy path for the limit to bound.

---

## [2026.08.4] — 2026-08-18

### Fixed

- **The console built S3 links against its own hostname**, gluing it to
  `MONOBUCKET_PORT` — behind a reverse proxy that is an address which exists
  nowhere (`https://console.example.com:9000/...`). The same string was signed
  as the host of every presigned URL, so correcting the link by hand then
  failed with `SignatureDoesNotMatch`: SigV4 covers the host, and the one that
  was signed was never the one the request arrived at.

### Added

- `MONOBUCKET_S3_PUBLIC_URL` — the origin S3 clients reach this deployment at,
  as a full origin (`https://s3.example.com`). Only the console reads it, to
  write object links and to sign presigned URLs. Behind a proxy the browser
  cannot derive it: the console is served from a different hostname on a
  different port, and `MONOBUCKET_HOST` is `0.0.0.0`. Unset, the previous
  behaviour is kept, which is correct for a deployment clients reach directly.
- `deploy/` — a compose file and environment template for running behind a
  TLS-terminating reverse proxy, with both listeners bound to loopback.

### Compatibility

- Nothing changes for a deployment without `MONOBUCKET_S3_PUBLIC_URL` set.
- The console session payload gains `s3PublicUrl`; it is empty when unset.

---

## [2026.08.3] — 2026-08-18

### Added

- **Per-bucket storage allocations.** Every bucket carries an allocation, and
  writes are refused once it is exhausted. The console requires one when a
  bucket is created and shows what the instance has left to hand out; the S3
  listener, which has no field to ask for one, gives new buckets
  `MONOBUCKET_DEFAULT_BUCKET_QUOTA_BYTES`.
- `MONOBUCKET_ALLOCATABLE_BYTES`, `MONOBUCKET_CAPACITY_RESERVE_PERCENT` and
  `MONOBUCKET_DEFAULT_BUCKET_QUOTA_BYTES`. Left unset, the allocatable capacity
  is the filesystem holding the data directory less a 10% operational reserve —
  set it explicitly when the data directory shares its disk.
- `POST /_mb/api/buckets` now requires `quotaBytes`; `GET` returns each bucket's
  allocation, usage and remainder alongside an instance `capacity` block.
- `GET`/`POST /_mb/api/buckets/quota` reads the whole division of capacity and
  changes one bucket's allocation. Reading it is `settings:read`; changing it is
  the new `capacity:write` permission, which only an administrator holds.
- `/metrics` gains `monobucket_allocatable_bytes`, `monobucket_allocated_bytes`,
  `monobucket_unallocated_buckets`, and per-bucket
  `monobucket_bucket_quota_bytes`, `monobucket_bucket_used_bytes` and
  `monobucket_bucket_pending_bytes`.
- Two S3 error codes: `QuotaExceeded` (403) when a write does not fit the
  bucket's allocation, and `InsufficientCapacity` (507) when the instance cannot
  back a new bucket's allocation at all.

### Changed

- Multipart parts are charged to their bucket's allocation as soon as they are
  stored, and released in full when the upload is aborted. An open upload holds
  bytes on disk, so it holds allocation; a failed one never becomes usage.
- An overwrite is admitted against the bucket's remaining allocation as if the
  key were new. Both payloads are on disk until the metadata commits, so the
  bytes the old object will release are not available to the new one yet — the
  settled figure afterwards is the true delta.

### Compatibility

- The bucket record gains a trailing allocation field. Older records read back
  with no allocation, which is what they had; a bucket written by this release
  is read by 2026.08.2 as one whose trailing bytes it ignores.
- Nothing about an existing deployment starts refusing writes on upgrade:
  buckets that predate allocations are unlimited, and
  `MONOBUCKET_DEFAULT_BUCKET_QUOTA_BYTES` defaults to zero.

---

## [2026.08.2] — 2026-08-18

### Breaking

- **The single administrator account is now one of many user accounts.** The
  console's identity is a `UserRecord` with a role, not the one-of-a-kind admin
  record. Startup migrates the old record in place — it becomes a user with the
  `administrator` role, keeping its username, password and creation date — and
  then drops it. Nothing has to be done by hand and no password changes. A
  downgrade to 2026.08.1 after upgrading will not see the migrated account,
  because it reads the record this release deleted; set
  `MONOBUCKET_ADMIN_PASSWORD` on the downgraded start to re-establish it.
- **S3 access keys belong to a user and inherit that user's role.** A key issued
  by a `readonly` account can only read. Keys issued before this release had no
  owner; startup adopts them into the administrator account, so they keep
  working exactly as they did and become revocable by name. The root pair from
  `MONOBUCKET_ROOT_ACCESS_KEY` is unchanged and remains administrator-equivalent
  — it comes from the environment and is the break-glass credential.
- **`GET /_mb/api/session` and `POST /_mb/api/login` answer with `role` and
  `permissions`.** Existing fields are untouched.

### Added

- **Users, roles and per-role authorisation.** Three roles, defined in
  `core/identity.hpp` as a pure `allows(Role, Permission)` function:
  `administrator` (everything, including user management and the audit log),
  `operator` (buckets, objects and their own access keys), and `readonly`
  (reads, and creates nothing at all — including credentials).
- **`/_mb/api/users`** — list, create, update role and status, and delete.
  Deleting an account revokes the S3 keys it owns; disabling keeps them and is
  reversible. The last enabled administrator cannot be deleted, disabled or
  demoted, and an account cannot delete itself.
- **`/_mb/api/users/password`** — change your own password with the current one,
  or reset somebody else's with `user:write`. Either way every session that
  account holds ends; a self-change hands back a fresh cookie so the tab that
  made it keeps working.
- **`/_mb/api/audit`** — the security event log: sign-ins and refusals, user and
  credential changes, and every denied authorisation check. A bounded ring of
  the most recent 5000 entries in the metadata store, oldest dropped on write.
- **Console screens for both**, under **Users** and **Activity**, plus a
  password-change form on **Settings**. Navigation is filtered by the signed-in
  role — as a courtesy, never as the enforcement.

### Changed

- **Every console route names the permission it requires**, checked before the
  handler runs. A request that lacks it gets a `403` naming the permission, in
  one shape for every route and every role.
- **Signed S3 requests are authorised against the key owner's role.** The owner
  record is read on every request rather than cached, so a role change or a
  disabled account takes effect on the next request rather than the next
  restart. Refused requests are recorded, unlike signature failures — reaching
  that point requires a valid credential, which is what bounds the log.
- **The credentials screen shows only your own keys** unless you can manage
  users, and a key can only be rotated or revoked by its owner or an
  administrator. A key that belongs to somebody else answers `404`, the same as
  one that does not exist.
- Startup now refuses to open a listener when no *enabled administrator* exists,
  rather than when no administrator record exists.

### Security

- Disabling a user ends every console session they hold and stops their S3
  access keys on the next request. Deleting one additionally revokes those keys.
- A disabled account fails sign-in at the same point and the same cost as a
  wrong password, so response timing does not distinguish the two.

---

## [2026.08.1] — 2026-08-18

### Breaking

- **The console no longer accepts S3 credentials as a login.** Sign-in takes an
  administrator username and password. `POST /_mb/api/login` now expects
  `{username, password}` instead of `{accessKey, secretKey}`, and
  `GET /_mb/api/session` reports `username` in place of `accessKey`.
- **A deployment with no administrator provisioned and no password configured
  refuses to start** (exit 78) rather than defaulting to anything. Set
  `MONOBUCKET_ADMIN_PASSWORD_FILE` or `MONOBUCKET_ADMIN_PASSWORD` — at least 12
  characters — once; the account then lives in the data directory and the
  variable can be removed. A console-less deployment
  (`MONOBUCKET_CONSOLE_ENABLED=false`) is exempt.
- No change to the S3 API. `MONOBUCKET_ROOT_ACCESS_KEY` and
  `MONOBUCKET_ROOT_SECRET_KEY` sign S3 requests exactly as before, which is the
  documented migration path — see *Upgrading from 2026.08.0* in `README.md`.

### Added

- **S3 access keys issued from the console.** A new *Access keys* screen
  creates, lists, rotates and revokes credential pairs, backed by
  `/_mb/api/credentials` and `/_mb/api/credentials/rotate`. A generated secret
  is returned exactly once, by the call that created or rotated it; no later
  read returns it. Revocation deletes the record, so it takes effect on the
  next signed request without a restart.
- **A console administrator account.** `MONOBUCKET_ADMIN_USERNAME` (default
  `admin`) with `MONOBUCKET_ADMIN_PASSWORD` or `MONOBUCKET_ADMIN_PASSWORD_FILE`;
  setting both forms is an error. Stored as a PBKDF2-HMAC-SHA256 verifier at
  600,000 iterations with a per-record salt, so a stolen data directory does not
  yield a console login. Verification runs on an I/O thread — the cost is the
  point, and the event loop must not pay it.
- `MONOBUCKET_CONSOLE_COOKIE_SECURE` (`auto` | `true` | `false`). `auto` marks
  the session cookie `Secure` when the request arrived over TLS, directly or via
  a proxy's `X-Forwarded-Proto`.
- Compose now delivers the administrator password as a Docker secret rather
  than an environment variable.

### Changed

- Sign-in failures return one message for every cause, so response text cannot
  distinguish an unknown username from a wrong password. The verifier runs
  against a placeholder record when the username misses, so response *time*
  cannot either.
- SigV4 verification resolves the secret through a callback rather than
  comparing against one pair, so the root credential and every issued key are
  checked by the same path. A revoked key reports `InvalidAccessKeyId`, the same
  as one that never existed.

### Fixed

- The settings panel named `MONOBUCKET_S3_PORT` as the variable behind the S3
  port. It is `MONOBUCKET_PORT`; the panel exists to tell an operator which
  variable to set, so naming the wrong one defeated it.
- The access-key generator's rejection-sampling bound truncated to zero for an
  alphabet whose size divides 256, rejecting every byte and looping forever.
  Caught before release by the generator's own test.

---

## [2026.08.0] — 2026-08-18

### Added

<!-- Storage & protocol -->

- **`monobucket --fsck [--deep]`** — the consistency check the reclamation log
  cannot perform. Walks the metadata and the payload tree and reports every way
  they disagree: payloads a live row references but that are absent, files whose
  length does not match the row, files no row names and no reclamation record
  covers, and names the store could not have written. `--deep` re-hashes every
  payload against the SHA-256 recorded at write time, which is the only way to
  see a payload rewritten to the same length. Reports and never repairs — acting
  on a scan that raced a live writer is how a checker becomes the outage. Exits
  0 when clean and 2 when it found something, so a caller can tell a damaged
  store from a check that could not run.
- **Per-bucket durability override**, settable through the console API
  (`POST /_mb/api/buckets/access` with `durability`) and reported on the bucket
  record. Unset means "follow `MONOBUCKET_DURABILITY`", which is not the same as
  storing today's value: raising the global level lifts every bucket that never
  asked for something else. The level reaches both the payload fsync and the
  metadata log sync, because a bucket promised `strict` whose object row sat
  unflushed would in fact only have been getting `relaxed`. The write-ahead log
  is shared, so a strict write also flushes what its neighbours had pending —
  every bucket gets at least what it asked for.

<!-- Phase 6 — Packaging -->

- **Signed, multi-architecture container images** on GHCR for `linux/amd64` and
  `linux/arm64`. They are built one job per architecture on a runner of that
  architecture rather than both on one under QEMU. Emulated compilation of
  Drogon and the engine runs roughly an order of magnitude slower, which would
  put the arm64 leg within reach of the job timeout — but the reason that
  matters less than the other one: a native runner can *execute* the image it
  just produced, so the arm64 artefact is smoke-tested on arm64 instead of being
  assumed to work because the amd64 one did. Each leg pushes by digest and names
  nothing; the manifest list is what binds digests to tags, so a smoke test that
  fails leaves an unreferenced blob rather than a `latest` that points at a
  broken image.
- **Keyless signing with cosign**, over the manifest list, so one signature
  covers every architecture beneath it. There is no key to distribute or leak:
  the signature is bound to this workflow's GitHub OIDC identity and recorded in
  the public Rekor log, which makes it a statement about *which workflow at
  which commit* produced the image rather than about who held a file. It is
  verified in the same run that creates it, because a signature nobody checks is
  a file. `edge` is deliberately left unsigned — a signature is what marks a
  release, and signing every push to `master` would reduce it to a note that CI
  ran.
- **A software bill of materials**, twice over and on purpose: BuildKit attaches
  an SPDX attestation per architecture, and the release carries the same
  inventory as a plain `.spdx.json` file. The attestation is what a policy
  engine admits an image on; the file is what a person reads or diffs without
  putting a registry client in the way.
- **`edge` published from CI** on every push to `master`, and pushed from the
  image that just passed the smoke test rather than rebuilt afterwards — a tag
  that names a *different* build from the one that was exercised is worse than
  no tag. It is amd64 only: it exists for trying out what is coming, and the
  cross-architecture matrix runs where it earns its cost, on a tag.
- **`scripts/cut-release.sh`.** `release.yml` refuses to publish when the tag,
  `CMakeLists.txt` and `CHANGELOG.md` disagree; this is what makes them agree.
  It derives the next version — including the MICRO reset that a month rollover
  needs and that nobody remembers — rewrites the version in both of the places
  CMake keeps it, promotes the `[Unreleased]` block under a dated heading,
  commits and tags. It refuses a dirty tree, a tag that already exists and an
  `[Unreleased]` section with nothing in it, on the grounds that a release with
  empty notes is worse than a late one.
- **The published runtime footprint** the packaging phase was supposed to
  produce, in [README.md](README.md#runtime-footprint): ~14 MB to pull, ~33 MB
  unpacked, a 6.6 MB stripped binary with the dashboard inside it, 23.7 MB
  resident when idle, and under half a second from `docker run` to `/readyz`.
  Throughput and RSS *under load* are Phase 7's and are not claimed here.

<!-- Phase 5 — Dashboard -->

- **A console API at `/_mb/api/*`**, answering on the console listener only. It
  is a separate surface from S3 on purpose: the browser exchanges the root key
  pair for an HttpOnly, `SameSite=Strict` session cookie and never holds an S3
  secret, which is what lets console access be revoked without rotating storage
  credentials. Sessions live in memory, so a restart signs everyone out — the
  alternative is a stolen data directory also being a stolen login. Failed
  logins are rate-limited in one global window rather than per source address,
  because there is exactly one account and a per-IP bucket only advises an
  attacker to change addresses.
- **A bounded metrics ring** sampled every five seconds and holding twenty
  minutes of history in roughly 30 KB. Counters are stored as differences
  between consecutive readings, so the browser never has to reason about a
  reset, and the ring is allocated once — a dashboard left open overnight costs
  what one opened a second ago costs. Longer windows remain Prometheus's job.
- **The dashboard itself**: an overview with live graphs for request rate,
  throughput in and out, cache hit ratio, resident memory and storage-queue
  depth; a disk capacity gauge; a bucket list with create, delete and the
  anonymous-read toggle; and a file browser that walks prefixes as folders with
  continuation pagination and an object metadata viewer.
- **The object URL in the metadata viewer**, in a read-only field with a copy
  button, alongside a badge saying whether the bucket answers an unsigned GET —
  the difference between a link that opens in a browser and one that returns
  403. The URL is assembled in the browser because `MONOBUCKET_HOST` is normally
  `0.0.0.0`: the only hostname known to reach the deployment is the one the
  console was loaded from, so the server contributes the S3 port and endpoint
  domain and the client contributes the host. Copying falls back to selecting
  the field when `navigator.clipboard` is unavailable, which is every console
  served over plain HTTP to anything but localhost.
- **A presigned link generator**, in the same metadata viewer: pick a lifetime
  (15 minutes to 7 days, S3's own ceiling) and get a URL that grants an
  unauthenticated GET of one private object until it expires. Signing happens on
  the server — the console holds a session cookie, never an S3 secret, and
  handing the browser one to sign with would undo the reason the two are kept
  apart. `s3::presignQuery` shares `buildCanonicalRequest` and
  `deriveSigningKey` with the verifier rather than reimplementing the canonical
  form, and is tested against the same AWS worked example the verification path
  uses: it reproduces the documented query string byte for byte. The endpoint
  refuses a key that does not exist, because a link to a missing object is
  indistinguishable from a wrong one until after it has been sent to someone.
- **Bucket CORS**, per bucket and to S3's rules: `PUT`, `GET` and
  `DELETE /<bucket>?cors`, an unauthenticated `OPTIONS` preflight, and
  `Access-Control-*` decoration of ordinary responses that carry an `Origin`.
  Rules are evaluated in document order and the first that permits the origin,
  the method *and* every header the preflight asked about is the one that
  answers; a rule that permits two of the three does not match at all, because
  replying with a partial header list would let the browser send a request the
  configuration never allowed. The preflight runs before signature verification
  — a browser never attaches credentials to one, so requiring a signature would
  refuse every cross-origin request to a private bucket before the real, signed
  one was ever sent — and answers a missing bucket exactly as it answers a
  bucket with no rules, so an unauthenticated caller cannot use it to discover
  which buckets exist. `Access-Control-Allow-Origin` echoes the origin rather
  than sending `*`, because a browser refuses `*` together with credentials.
  Error responses are decorated too: a script that cannot read the 403 it got
  back reports a network failure instead, which sends whoever is debugging it
  looking at the wrong layer. Rules are stored decomposed rather than as the XML
  they arrived in, appended to the bucket record behind a length check so that
  buckets written before this release load unchanged.
- **A CORS editor in the console**, on the bucket page, and a rule count beside
  the bucket in the list. It reads and writes the same rules through the same
  validation as the S3 API — one feature with two front doors, not two
  implementations that can disagree about what a valid rule is.
- **A drag-and-drop uploader** on the bucket page, with per-file progress, a
  queue three transfers wide and cancellation that leaves nothing behind. Files
  go to `PUT /_mb/api/upload` one request each and are streamed into the payload
  tree in fixed-size chunks; Drogon spills anything above
  `MONOBUCKET_MAX_MEMORY_BODY_BYTES` to a file and hands back a mapping of it,
  so a five-gigabyte upload is five gigabytes of page cache and one chunk of
  heap. The console does not chunk the request itself: a resumable console
  protocol would be a second upload session to design and get wrong, and the S3
  listener already has the one S3 clients use. Progress comes from
  `XMLHttpRequest` because `fetch` still cannot report how much of a request
  body has gone out, and a progress bar that cannot is a spinner wearing a
  percentage.
- **A bucket policy editor**, sharing `validateBucketPolicy` and
  `policyGrantsAnonymousRead` with the S3 `?policy` endpoint so the console and
  the wire cannot disagree about what a document means. It is a textarea rather
  than a builder: a policy has a published grammar, it is usually pasted in from
  somewhere that already has one, and a form limited to the shapes we thought of
  would quietly refuse the rest. What the console adds is the server's own
  reading — whether the document grants an unauthenticated `GetObject` —
  recomputed on every write, so removing the statement removes the access.
- **A settings panel**, read-only and behind the session. It shows the
  configuration this process resolved at startup, grouped the way someone asks
  about it rather than the way `Config` is declared, with the `MONOBUCKET_*`
  variable behind each value and a byte or duration reading beside the raw
  number. Nothing is editable on purpose: configuration is environment only,
  parsed once and validated before the first listener opens, and a panel with
  save buttons would either lie about taking effect or invent a reload path that
  does not exist.
- **Open connections** on the overview, as a tile and a graph, and as
  `monobucket_connections` on `/metrics`. Drogon counts connections per process
  rather than per listener, so the number covers both and says so instead of
  claiming a split that would have to be invented.
- **Pre-compressed dashboard assets.** The embedder now runs `gzip` and `brotli`
  over every text asset at build time and bakes the variants in beside the
  original, and the console serves whichever the request's `Accept-Encoding`
  allows, with `Vary: Accept-Encoding`. Compressing per request would spend CPU
  re-deriving bytes that never change; this spends binary size instead, and only
  where it pays — a variant is kept only if it saves at least a tenth, so fonts
  and images have none and are served untouched. Both compressors are optional:
  without them the table carries no variants and every response is the original.
  The console stylesheet goes out at 21 KB instead of 147 KB.
- **A visual language for the console.** Two purpose-built daisyUI themes,
  `monobucket` and `monobucket-dark`, replacing the built-in pair the console
  started on. The built-ins were neutral to the point of having no identity, and
  a console whose only colours are its alert states gives a reader nothing to
  navigate by. Both sit on one hue axis so flipping to dark changes the light
  level and nothing else; surfaces carry a trace of that hue rather than being
  grey; and every semantic pair clears WCAG AA against the surface it is used
  on. The theme choice is stored and replayed before first paint rather than
  held in a checkbox, so it survives a reload — with "follow the system" as the
  absence of a choice rather than a third value. Icons are one inline set drawn
  on a single grid, and Inter is self-hosted from the latin subset alone — 48 KB
  rather than the ~400 KB of the full fontsource package — because the console
  must fetch nothing from the network.

<!-- Phase 4 — S3 REST API Protocol -->

- **AWS Signature Version 4 verification** on OpenSSL: canonical request,
  signed-header selection, query-string (presigned) signing and `aws-chunked`
  payload signing, with a clock-skew window and constant-time comparison of
  both the signature and the access key id. The whole engine is expressed over
  plain strings rather than framework types, because it is the one part of the
  server where a subtle mistake is a security hole rather than a bug — and
  because that is what makes it testable against the AWS reference vectors
  without a socket.
- **Presigned URL verification**, bounded to S3's own 1-to-604800-second
  lifetime. Zero is treated as a URL that was already invalid when it was
  minted, not as "never expires".
- **Anonymous read for public buckets**, gated on a deliberately narrow reading
  of one shape of bucket policy. A document the evaluator does not recognise is
  stored and echoed back unchanged but grants nothing: refusing to guess is the
  only safe direction to be wrong in. Listing every bucket is never public, and
  no anonymous request can reach a mutating operation.
- **The S3 endpoint surface.** ListBuckets; bucket create / delete / head and
  both listing versions with prefix, delimiter and pagination; object GET
  (including `Range` and RFC 7232 preconditions), PUT, DELETE, HEAD and
  `POST ?delete`; the six multipart operations; and the bucket policy, ACL,
  location and versioning subresources the dashboard's link generator needs.
- **Operation classification as one pure function.** S3 does not route on the
  path alone — `GET /bucket`, `GET /bucket?uploads` and `GET /bucket?list-type=2`
  are three different operations — so the whole routing table lives in
  `classify()` where it can be tested exhaustively, rather than spread across
  handlers as a chain of header and query checks.
- **S3-shaped XML errors with request ids.** The error code string is part of
  the protocol: the AWS CLI backs off on `SlowDown`, boto3 raises a distinct
  exception per code, rclone treats `NoSuchKey` differently from `AccessDenied`.
  Codes and their HTTP statuses therefore travel together in one table and are
  never assembled at a call site. Unexpected exception text goes to the log, not
  to the client, since it is as likely to name a path on our disk as anything
  the caller could act on.
- **One catch-all route serving both listeners**, dispatching on the listener
  port. Drogon takes the first matching regex handler, so a separate catch-all
  for the console and for S3 would shadow each other by registration order;
  deciding inside a single handler removes the ordering from the question. The
  bounded I/O pool's rejection now has its wire form: `503 SlowDown` with
  `Retry-After`, counted separately from 5xx because shedding load is the queue
  working, not the server failing.
- **Reserved bucket names.** `healthz`, `readyz`, `metrics` and `_mb` are
  refused at creation. Drogon matches an exact path before the catch-all, so a
  bucket with one of those names would exist in the metadata store and be
  permanently unaddressable — a clear error now beats an unexplainable 404
  later.

### Changed

- **`GET /_mb/config` is gone; the resolved configuration is now
  `GET /_mb/api/config`, behind the console session.** The old route answered
  anyone who could reach the console port, and what it answered names the data
  directory and the root access key. It was added for a settings panel that did
  not exist yet; the panel exists now, it authenticates like every other console
  call, and the response carries the environment variable behind each setting
  rather than the bare value. Nothing outside the dashboard is known to have
  used the old path, but it was reachable, so it is called out here.

- **Drogon is now always built from source unless a system copy is explicitly
  requested.** It is the one dependency MonoBucket patches (see *Fixed*), and a
  system Drogon cannot carry the patch — so preferring one silently, as
  `find_package` first did, would make whether the server can store an empty
  object depend on which machine built it. `find_package(Drogon)` is now tried
  only under `-DMONOBUCKET_ALLOW_SYSTEM_DROGON=ON`, which warns about exactly
  what is given up. Every other dependency keeps the find-first policy.

<!-- Phase 3 — Abstraction & Cache Layer -->

- **`CacheProvider` interface.** `get` / `set` / `del` / `evict` / `clear` plus
  stats, with three backends behind it: the in-memory LRU, an optional Redis
  tier, and a null backend for `MONOBUCKET_CACHE_MAX_BYTES=0`. Values are handed
  out as `shared_ptr<const std::string>`, which is what lets a reader take a
  value under a shared lock without copying its bytes and keep it valid if
  eviction removes the entry while the response is still being written.
- **Sharded in-memory LRU.** Independent map, intrusive list and `shared_mutex`
  per shard, each holding `maxBytes / shards`. Shard count is derived from the
  worker count rather than exposed as a knob, and is reduced automatically when
  the budget is too small to give each shard useful room — fewer, larger shards
  beat many that cannot hold anything.
- **Reads that do not write.** A textbook LRU promotes on every read, which
  makes `get()` a writer and reduces the `shared_mutex` to decoration. A hit
  instead sets an atomic flag, and eviction gives a flagged entry exactly one
  reprieve before taking it. Recency becomes approximate; concurrency becomes
  real, which is the trade a read cache wants.
- **A budget that is actually a budget.** The ceiling is enforced on every
  insert, not by the sweeper, and per-entry overhead is charged alongside the
  payload — otherwise a million eight-byte values would report themselves as
  eight megabytes while occupying well over a hundred. A value larger than its
  shard's whole budget is refused rather than accepted and then evicted, which
  would empty the cache to make room for something that could never fit.
- **Redis backend (optional, `-DMONOBUCKET_ENABLE_REDIS=ON`).** A bounded,
  lazily-connected pool over hiredis. Commands go through `redisCommandArgv`, so
  an object key containing `%` is data rather than syntax. `clear()` scans our
  own key prefix and never issues `FLUSHDB`: the database may not be ours alone.
- **Fallback with a circuit breaker.** After a few consecutive failures the
  shared tier stops being called at all, with exponential backoff and a single
  probe per window. Retrying a dead socket on every request would satisfy "the
  cache never fails" on paper while still stalling every request.
- **Two tiers rather than an either/or.** The local cache sits in front of Redis
  and is written on every `set`, so it is already warm when the shared tier
  disappears instead of starting empty at the worst possible moment. The cost is
  coherence, and `MONOBUCKET_CACHE_LOCAL_TTL_SECONDS` (default 5) bounds it: a
  local copy of a shared value expires even when it was stored with no TTL,
  because another instance can change a value this process has no other way to
  learn is stale.
- **Redis URL parsing** for `redis://[user[:password]@]host[:port][/db]`, with
  percent-decoded credentials (a generated password containing `@` or `:` is
  otherwise unrepresentable), bracketed IPv6 literals, and `rediss://` refused
  at startup with the reason rather than left to fail as a connection timeout.
  Validated in `Config::validate()`, so `--print-config` rejects a typo instead
  of the container starting and quietly running without the cache.
- **Cache metrics.** `monobucket_cache_{hits,misses,evictions,expirations,
  rejections,errors}_total`, plus `_hit_ratio`, `_entries`, `_bytes`,
  `_limit_bytes` and `_healthy`, all labelled by backend. `/readyz` reports the
  backend and its health but never answers 503 for a degraded cache — that is
  the entire point of the fallback.
- **Settings.** `MONOBUCKET_CACHE_TTL_SECONDS`,
  `MONOBUCKET_CACHE_LOCAL_TTL_SECONDS`, `MONOBUCKET_REDIS_POOL_SIZE`, and
  `MONOBUCKET_CACHE_MAX_BYTES=0` to disable caching outright.
- **`dev-redis` preset** and an opt-in integration suite that runs against a
  real server when `MONOBUCKET_TEST_REDIS_URL` is set, and skips otherwise.
  Mocking hiredis would test the mock; what is in question is whether a real
  connection fails the way `FallbackCache` assumes.

<!-- Phase 2 — Storage Layer & POSIX I/O Engine -->

- **Metadata store.** RocksDB behind a `MetadataStore` interface, holding
  buckets, objects, multipart uploads and their parts. Everything lives in one
  column family under a type-tagged keyspace: a family per record type would
  give each its own memtable and multiply the write-buffer floor, and RocksDB's
  keyspace is already ordered. Records use a small varint codec rather than
  JSON, because a `ListObjectsV2` page decodes up to 1000 of them per request.
- **Bounded metadata memory.** The block cache and the `WriteBufferManager`
  share one budget (`MONOBUCKET_METADATA_MEMORY_BYTES`), and index and filter
  blocks are charged to the cache instead of growing outside it. Left to its
  defaults RocksDB sizes these independently, and the container only ever sees
  the sum.
- **Payload store.** A sharded tree at `objects/<aa>/<bb>/<blobId>` — 65 536
  leaf directories, so a million objects sit at roughly fifteen entries each.
  Payloads are written to `tmp/`, flushed, then linked into place by `rename`,
  which is what makes a partially written object invisible rather than corrupt.
- **Streaming I/O.** Fixed-size chunks over `pread`/`pwrite`, so memory per
  transfer is constant regardless of object size. Range reads are served from
  the same path, and an over-long range is clamped rather than rejected, as S3
  specifies.
- **Integrity.** MD5 (the ETag) and SHA-256 computed in one pass as bytes stream
  through the write path — never by re-reading the object. Multipart ETags use
  the real algorithm: MD5 over the concatenated raw part digests, suffixed with
  the part count.
- **Durability policy.** `MONOBUCKET_DURABILITY` selects `none`, `relaxed` or
  `strict`, controlling payload `fsync`, directory `fsync` and whether the
  metadata log is synced per commit.
- **Crash recovery.** Startup sweeps interrupted writes out of `tmp/` and
  reclaims payloads nothing references. Reclamation is driven by a log rather
  than a tree walk, so recovery costs what was leaked, not what is stored.
- **Blob reclamation.** Every mutation that drops a reference to a payload
  returns the released blob id from the same call that committed the metadata,
  so a leak requires ignoring a return value rather than forgetting a step.
  Payloads are registered *before* they are written, which is what makes a crash
  mid-upload recoverable at all.
- **Bounded I/O pool.** `IoExecutor` keeps blocking filesystem work off the
  event loop. Its queue is bounded on purpose: an unbounded one converts a disk
  that cannot keep up into unbounded memory growth. A full queue refuses work,
  which Phase 4 will surface as `503 SlowDown`.
- **Listing.** Prefix, delimiter and pagination in the exact binary order S3
  specifies. A delimiter group is skipped with one seek rather than iterated, so
  a prefix with a million keys under it costs the same as one with ten.
- **Storage metrics.** Bucket, object, byte, upload and pending-reclaim counts,
  filesystem capacity, RocksDB's own memory gauges, and I/O queue depth,
  throughput and rejections. `/readyz` now touches the engine, so a data
  directory that has become unreadable takes the container out of rotation.
- **Build system.** CMake 3.25+ / C++20 project split into `backend/` (C++
  engine), `frontend/` (SvelteKit dashboard) and `common/` (shared definitions).
  Dependencies resolve through `find_package` first and fall back to
  `FetchContent` with pinned tags, so local builds reuse an installed toolchain
  while CI and Docker stay hermetic. `CMakePresets.json` provides `dev`, `asan`,
  `release` and `release-redis`.
- **Version header.** CalVer string generated by CMake into
  `common/include/monobucket/version.hpp`, consumed by `--version`, the `Server`
  response header, `/metrics` and `/_mb/version`.
- **Protocol constants.** `common/include/monobucket/constants.hpp` centralises
  S3 limits (key length, bucket naming, part sizes, `max-keys`, signature skew)
  so the storage engine and the protocol layer cannot drift apart.
- **Configuration.** Full `MONOBUCKET_*` environment parsing with a byte-size
  grammar that distinguishes binary units (`64MiB`) from SI units (`200MB`),
  cross-field validation, derived-value resolution, a redacted startup summary
  and a redacted JSON view for the settings panel. Malformed input aborts
  startup instead of silently defaulting.
- **Event loop and worker pool.** Drogon/Trantor event loop sized from
  `std::thread::hardware_concurrency()`, overridable via
  `MONOBUCKET_WORKER_THREADS` and capped at 256. Body-size limits are wired so
  requests above `MONOBUCKET_MAX_MEMORY_BODY_BYTES` spill to disk rather than
  buffering in RAM.
- **Dual listeners.** The S3 API and the admin console bind separate ports;
  configuration refuses to let them share one, because bucket names at the URL
  root would otherwise shadow console routes.
- **Lifecycle.** `SIGTERM`/`SIGINT` handlers touch only a `sig_atomic_t`; the
  event loop polls it and runs ordered shutdown hooks in reverse registration
  order. `SIGPIPE` is ignored so a peer disconnecting mid-transfer surfaces as a
  write error rather than killing the process.
- **Logging.** Small thread-safe stderr logger with ISO-8601 timestamps, kept
  independent of the HTTP stack so core and storage code stay unit-testable.
- **System endpoints.** `/healthz`, `/readyz`, `/metrics` (Prometheus text
  format) and `/_mb/version`; `/_mb/config` is restricted to the console
  listener.
- **Asset embedding pipeline.** `backend/cmake/EmbedFrontend.cmake` turns the
  SvelteKit static build into a generated C++ translation unit with MIME
  detection and a sorted, binary-searchable table. Builds without
  `MONOBUCKET_EMBED_FRONTEND` generate an empty table so the backend always
  compiles; console routes stay unregistered in that case.
- **Container packaging.** Three-stage Dockerfile (Node → Alpine toolchain →
  minimal Alpine runtime) producing a stripped, LTO-optimised binary running as
  a non-root user with a read-only root filesystem and a `/data` volume.
  `docker-compose.yml` adds a 256 MB memory cap and an optional Redis profile.
  The frontend stage builds the dashboard with pnpm and falls back to a
  placeholder page when `frontend/` is absent, so the image stays buildable at
  any point in the roadmap.
- **Dashboard project.** SvelteKit 2 / Svelte 5 on pnpm, configured for
  `adapter-static` with SPA fallback and SSR/prerendering disabled. This
  SvelteKit version carries its Kit configuration inside `vite.config.ts`; there
  is no `svelte.config.js`.
- **Tests.** Catch2 suite covering the environment grammar, configuration
  validation and the asset table's sortedness invariant.
- **Documentation.** `.env.example` documenting every setting, `README.md`
  covering what works and what does not, and this changelog.

### Fixed

- A **412 from a failed `If-Match`** was sent without a body, leaving a client
  no error code to branch on. It now carries the S3 `PreconditionFailed`
  document; 304 stays bodyless, as RFC 9110 requires, and keeps its validators.
- **Creating a bucket named for a system route** (`healthz`, `readyz`,
  `metrics`, `_mb`) returned a bare 405 from Drogon's exact-path router, which
  matches before the S3 catch-all — so `handleCreateBucket`'s `InvalidBucketName`
  refusal was unreachable and the client got an unparseable response. Those
  paths now claim the write verbs and answer with the S3 error document.

- **The release workflow would have failed on its own first tag.** It derived
  the image name from `github.repository`, which is `sinhaparth5/MonoBucket` —
  and GHCR rejects an uppercase path component. Nothing had ever pushed an
  image, so the error was waiting behind the one event that was supposed to
  produce a release. The name is now lowercased once, where it is resolved.

- **A manually triggered release published one tag instead of four.** The
  rolling `2026.08`, `2026` and `latest` tags were derived from the git ref with
  `type=match`, which matches nothing when the workflow is dispatched by hand
  rather than by a tag push — so the fallback path for "the tag push failed,
  publish it manually" quietly produced a release nobody's `latest` would ever
  see. Tags now come from the resolved version, and a manual run produces
  exactly what a tag push does.

- **Images were stamped with the wrong build date.** `BUILD_DATE` was
  `github.event.repository.updated_at`, which is when the repository record last
  changed — a description edit moves it, a rebuild of the same commit does not.
  It now comes from the push that triggered the build.

- **The container smoke test never looked at the dashboard.** It checked
  `/healthz`, `/metrics` and a clean exit code, all of which pass just as well
  with an empty asset table — which is exactly what a build with
  `MONOBUCKET_EMBED_FRONTEND=OFF` produces. The console being *inside the
  binary* is the whole premise, and it was the one thing the image was not
  asked about. CI and the release now fetch the console from a running
  container, assert it comes back compressed, and assert the S3 listener does
  not serve it.

- **The Docker build context carried the repository's root `node_modules/`.**
  `.dockerignore` excluded `frontend/node_modules/` but not the one beside it.

- **A field in the create-bucket dialog grew straight through the side of it.**
  A `fieldset` carries `min-inline-size: min-content` from the user agent, and
  daisyUI's `.label` is `white-space: nowrap`; together, one line of helper text
  set a floor the dialog could not meet, so the input and its guidance ran 88px
  past the modal on a desktop and 114px past it on a phone. Helper text now uses
  `.fieldset-label`, which wraps, and `fieldset { min-inline-size: 0 }` is set
  globally — the trap is in the element, so every fieldset anyone ever writes
  falls into it.

- **Deleting an object in the console left it readable over S3.** The console's
  delete removed the record but not the cached metadata the S3 read path answers
  HEAD from, so the object went on existing for a whole cache TTL as far as any
  client was concerned. The same gap as the anonymous-read one below, found
  while auditing the rest of the console's writes; console uploads invalidate
  the same key.

- **Turning off anonymous read in the console took a cache TTL to take effect.**
  The console wrote the flag straight to the store, but the S3 read path answers
  from a cached bucket record, so unsigned GETs kept succeeding against a bucket
  the dashboard already showed as private. Both console writes now drop the
  cached record, as the S3 handlers already did.

- **The console's SPA fallback swallowed unknown `/_mb/` paths.** Any
  extension-less path the asset table does not recognise is answered with
  `index.html` so client-side routing works, and `/_mb/api/anything` fitted that
  rule — a retired or mistyped endpoint returned 200 with an HTML body where the
  caller expected JSON, which surfaces at the far end as a parse error rather
  than the 404 that would have named the problem. `/_mb/` is the server's own
  namespace and is now excluded from the fallback.

- **Zero-byte objects could not be written by any AWS SDK.** Drogon's request
  parser rejects an HTTP/1.1 request carrying `Expect: 100-continue` together
  with `Content-Length: 0`, answering 400 before the request reaches a handler
  — and that is exactly how boto3, the AWS CLI and the SDKs write an empty
  object. `put_object(Body=b"")` failed, `aws s3 cp` of an empty file failed,
  and `aws s3 sync` failed on any tree containing one, all with a bare 400 that
  named no cause. With no body to withhold there is nothing for 100-continue to
  negotiate and the request is already complete, so the only correct action is
  to handle it. Patched in `backend/cmake/patches/`, applied to the fetched
  source at configure time. Reported upstream; still present on master.

- `StorageEngine::recover()` documented itself as reclaiming every unreferenced
  payload regardless of age, but ran through the ordinary sweep and so honoured
  the one-hour grace period. Startup therefore left abandoned payloads on disk
  until the first periodic sweep an hour later. Recovery now passes its own
  cutoff, and the two paths are separate functions rather than one with an
  implied caveat. Caught by a test asserting the documented behaviour.

- `Config`'s byte limits defaulted to `0` while their real defaults lived only
  inside `fromEnvironment()`, so a default-constructed `Config` could never pass
  `validate()`. Defaults now live once, as in-class initialisers, and
  `fromEnvironment()` overrides them. Covered by a regression test.
- Shutdown signals were handled twice and wrongly: Drogon installs its own
  `SIGTERM`/`SIGINT` handlers inside `run()`, overwriting ours, so the
  `sig_atomic_t` flag was never set and the 0.2 s polling timer never fired.
  The process still exited 0, but through Drogon's path rather than the one the
  code described. Shutdown callbacks are now registered via
  `setTermSignalHandler`/`setIntSignalHandler`, and `Lifecycle` keeps only the
  `SIGPIPE` ignore that Drogon has no opinion about.
- The asset embedder derived MIME types with CMake's `EXT`, which returns the
  longest extension — Vite's fingerprinted `app.EZ5uSd-D.js` was classified as
  `application/octet-stream`, and browsers refuse to execute ES modules served
  that way. Now uses `LAST_EXT`.
- The generated version header emitted the zero-padded month as a bare integer
  (`08`), which is an invalid octal literal. Integer uses now take an unpadded
  copy while the CalVer string keeps its padding.

- Build warnings: Trantor's pre-3.10 policy floor produced a CMake deprecation
  warning, and setting `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE` globally
  pushed LTO onto vendored targets that cannot accept it (CMP0069). LTO is now
  applied per-target to our own code only, and Drogon/Trantor are configured
  with deprecation warnings suppressed and their headers marked `SYSTEM`.

### Removed

- **`ROADMAP.md`**. Its open items now live under *Known limitations* in
  [README.md](README.md), where someone deciding whether to use MonoBucket will
  actually meet them.
- The SvelteKit scaffold's example component and unit tests, and the vitest
  configuration that existed only to run them.

### Security

- Startup now warns when the built-in demo credentials (`monobucket` /
  `monobucket`) are in effect, so an unconfigured deployment is noisy rather
  than silently open.

### Notes

- **The cache is not yet exercised by traffic.** Nothing calls it until the S3
  handlers land in Phase 4, so `monobucket_cache_hits_total` is legitimately 0
  on a running server and `monobucket_cache_healthy` stays 1 even after Redis is
  stopped — the breaker cannot trip on calls nobody makes. The Phase 3 exit
  criteria are met and tested (the budget is asserted on every insert across 500
  inserts and under eight concurrent threads; backend selection goes through
  `Config::fromEnvironment()`), but the hit ratio under load belongs to Phase 7.
- **Redis is optional and off by default.** A binary built without it warns and
  uses the in-memory backend when `MONOBUCKET_CACHE_BACKEND=redis`, rather than
  refusing to start. When it is compiled in, hiredis comes from the system if
  installed and is otherwise fetched at a pinned tag; the two lay their headers
  out differently (`<hiredis/hiredis.h>` versus flat), which the build resolves
  explicitly rather than by guessing.
- **Measured cost of the cache layer:** idle RSS 22.6 MB → 23.5 MB with the
  Redis backend active and the cache empty. The configured budget is on top of
  that and is only occupied once something is cached.
- **`monobucket_cache_limit_bytes` is the budget held in this process**, not
  `MONOBUCKET_CACHE_MAX_BYTES`. With the Redis backend most of the budget lives
  in Redis and the gauge reports the local tier — the part that counts against
  container memory.
- **RocksDB is a required system dependency**, not vendored: it is a ~40 MB
  static archive pulling in gflags, snappy, lz4, zstd and bz2, and building it
  from source would dominate the image build. The build prefers the shared
  target (`RocksDB::rocksdb-shared`), whose transitive compression dependencies
  are already resolved, and falls back to a plain header/library search for
  distributions that ship no CMake config package. Install
  `librocksdb-dev` / `rocksdb-dev` / `rocksdb-devel`.
- Choosing RocksDB costs about 18 MB of image and roughly 11 MB of resident
  memory over the Phase 1 baseline — the image is 43.5 MB and idle RSS 22.6 MB,
  against 25.1 MB and 11.2 MB before. The `MetadataStore` interface is what
  keeps that a reversible decision.
- **The storage format carries its own version**, independent of the CalVer
  release version, and a mismatch refuses to open the data directory rather than
  misreading it. It is currently `1`.
- The Phase 2 exit criterion — a multi-gigabyte object round-tripping with flat
  memory — is not yet demonstrated end to end, because no HTTP path reaches an
  object until Phase 4. The engine-level round trip is covered by tests.
- Drogon does not vendor jsoncpp or libuuid. The build now fails early with the
  per-distribution install command instead of a `FindJsoncpp` stack trace.
- CMake 4.x rejects the `cmake_minimum_required(3.5)` that Drogon and Trantor
  still declare; the build lowers the compatibility floor for vendored
  subprojects only.
- **LTO is off on musl.** Alpine's `fortify-headers` define the
  `_FORTIFY_SOURCE` wrappers as `always_inline`; under LTO, GCC re-compiles
  callers like libstdc++'s `__to_xstring` (behind `std::to_string`) and cannot
  honour `always_inline` for a body it treats as replaceable at link time, so
  the link fails on `vsnprintf`. The commonly cited fix is to disable
  `_FORTIFY_SOURCE`, which trades hardening for optimisation on a
  network-facing server. The build detects musl and drops LTO instead; the
  container image is therefore `-O3` and section-GC'd but not LTO'd.
