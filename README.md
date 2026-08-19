<p align="center">
  <img src="frontend/src/lib/assets/monobucket-logo.svg" width="176" alt="MonoBucket logo">
</p>

<h1 align="center">MonoBucket</h1>

<p align="center">
  Small S3-compatible object storage with a built-in admin dashboard.
</p>

MonoBucket packages a C++20 storage server and a Svelte dashboard into one
binary. It runs without sidecars or an external database.

## What you get

- An S3 API that works with AWS CLI, boto3, and rclone
- Multipart uploads, presigned URLs, bucket policies, CORS, and SigV4
- A browser dashboard for buckets, objects, uploads, metrics, and settings
- Console sign-in with a password, separate from the S3 keys you issue in it
- Named user accounts with three roles, and an audit log of what they did
- One container for `linux/amd64` and `linux/arm64`

## Two kinds of credential

MonoBucket keeps the person and the program apart:

- **User accounts** — a username and password — sign in to the dashboard. Each
  is stored as a PBKDF2-SHA256 verifier, never as a password. The first one is
  provisioned from the environment; the rest are created in the console.
- **S3 access keys** sign S3 requests. Issue, rotate and revoke them from the
  console's **Access keys** screen. They do not grant console access, and the
  console session does not sign S3 requests.

Revoking a key never locks you out of the dashboard; changing your password
never breaks a client.

There is no default administrator password. A deployment with none configured
and none provisioned **refuses to start** and says what to set — a shipped
default would be a published credential on every install.

## Users and roles

Every account holds one of three roles. The role decides what it may do in the
console *and* what any S3 access key it issues may do — a key acts as the person
who issued it and can never exceed them.

| | `administrator` | `operator` | `readonly` |
| --- | --- | --- | --- |
| Read buckets and objects | ✓ | ✓ | ✓ |
| Write buckets and objects | ✓ | ✓ | |
| Read the resolved settings | ✓ | ✓ | ✓ |
| Issue, rotate and revoke access keys | ✓ | own only | |
| Manage users | ✓ | | |
| Read the audit log | ✓ | | |

`readonly` deliberately cannot issue an access key at all, not even a read-only
one: a role whose name says it only reads should not mint a credential that
outlives the session it was minted from.

Enforcement is in the backend. Every console route names the permission it
needs and answers `403` without it, and every signed S3 request is checked
against the role of the identity behind its access key. The console hides what
you cannot use, which is a courtesy — calling the API directly gets the same
refusal.

**Disabling an account** ends every console session it holds immediately and
stops its S3 access keys on their next request; nothing is cached between
requests. The keys survive, so re-enabling restores them. **Deleting** an
account revokes them instead. The last enabled administrator cannot be deleted,
disabled or demoted, so the console cannot be locked out of itself.

**Activity** records sign-ins and refusals, user and credential changes, and
every denied authorisation check. It is a bounded ring of the most recent 5000
entries kept beside the metadata — a window on what just happened, not an
archive. Ship the server's stdout somewhere durable if you need one.

## Run it

Create a password first. Twelve characters minimum; a file keeps it out of
`docker inspect` and `/proc/<pid>/environ`.

```bash
umask 077
head -c 24 /dev/urandom | base64 > admin_password.txt
```

```bash
docker run --rm \
  -p 9000:9000 \
  -p 9001:9001 \
  -v "$PWD/admin_password.txt:/run/secrets/admin_password:ro" \
  -e MONOBUCKET_ADMIN_PASSWORD_FILE=/run/secrets/admin_password \
  -e MONOBUCKET_ROOT_ACCESS_KEY=monobucket \
  -e MONOBUCKET_ROOT_SECRET_KEY=change-me-please \
  -v monobucket-data:/data \
  ghcr.io/sinhaparth5/monobucket:2026.08.0
```

The S3 API listens on [localhost:9000](http://localhost:9000). The dashboard is
at [localhost:9001](http://localhost:9001) — sign in as `admin` with that
password, then issue an S3 key from **Access keys**.

```bash
AWS_ACCESS_KEY_ID=<the issued key id> \
AWS_SECRET_ACCESS_KEY=<the secret, shown once> \
aws --endpoint-url http://localhost:9000 s3 ls
```

The account is provisioned once. After the first start the verifier lives in
the data volume, so you can drop the password variable entirely and the server
still comes up. Leaving it set means every restart resets the password back to
that value — which is how you recover a lost one, and not what you want
otherwise.

`MONOBUCKET_ADMIN_USERNAME` chooses the name (default `admin`), and
`MONOBUCKET_ADMIN_PASSWORD` is the inline alternative to the file. Setting both
forms is an error rather than a silent precedence rule.

### Docker Compose

[docker-compose.yml](docker-compose.yml) wires the password in as a Compose
secret. Create `secrets/admin_password.txt`, then `docker compose up`. See
[secrets/README.md](secrets/README.md).

### Directly from the binary

```bash
export MONOBUCKET_DATA_DIR=/var/lib/monobucket
export MONOBUCKET_ADMIN_PASSWORD_FILE=/etc/monobucket/admin_password
export MONOBUCKET_ROOT_SECRET_KEY=...
monobucket
```

Under systemd, `LoadCredential=` puts the password in the unit's private
credential directory and `MONOBUCKET_ADMIN_PASSWORD_FILE` points at it. Keep
the file `0600` and owned by the service user.

If the console is served over HTTPS — directly or behind a proxy that sets
`X-Forwarded-Proto` — the session cookie is marked `Secure` automatically.
`MONOBUCKET_CONSOLE_COOKIE_SECURE` forces the question either way; forcing it
on behind a plain-HTTP hop makes the browser accept the cookie and then refuse
to send it, which looks like a login that silently does nothing.

The full list of settings is in [.env.example](.env.example).

## Upgrading from 2026.08.0

Console sign-in used to take the root S3 access key and secret. It no longer
does, and no S3 key of any kind signs you in.

1. **Before upgrading**, pick an administrator password and put it in
   `MONOBUCKET_ADMIN_PASSWORD_FILE` (or `MONOBUCKET_ADMIN_PASSWORD`). Without
   one, the upgraded server will not start — deliberately, so the change cannot
   be discovered as a console that silently accepts nothing.
2. **Start it.** The account is provisioned on first boot and logged by name.
   Sign in with the username and password.
3. **Existing S3 clients keep working untouched.**
   `MONOBUCKET_ROOT_ACCESS_KEY` and `MONOBUCKET_ROOT_SECRET_KEY` still sign S3
   requests exactly as before. Nothing about the S3 API changed.
4. **Optionally, move clients off the root pair.** Issue a key per client from
   **Access keys**, swap it in, and confirm. Issued keys can be rotated and
   revoked individually; the root pair can only be changed by restarting the
   server, which is why it is best kept as a break-glass credential.

Nothing is migrated automatically and nothing is removed. Step 4 can happen
whenever, or never.

## Upgrading from 2026.08.1

The single administrator account became one of many user accounts with roles.
Nothing has to be done by hand:

1. **Start it.** The old administrator record is converted in place into a user
   with the `administrator` role, keeping its username, password and creation
   date, and the old record is dropped. Access keys issued before this release
   are adopted into that account, so they keep working and become revocable by
   name.
2. **Add the people who need access** under **Users**, giving each the narrowest
   role that does their job. They sign in with their own username and password.
3. **Reissue keys per identity** if you want a program's authority bounded — a
   key issued by an `operator` cannot manage users, and one issued by a
   `readonly` account cannot write.

The root S3 pair is untouched and still administrator-equivalent. Downgrading to
2026.08.1 after this will not find the migrated account, because it reads the
record this release deleted; set `MONOBUCKET_ADMIN_PASSWORD` on that start to
re-establish one.

## Known limitations

Every project has them; these are MonoBucket's, and they are worth reading
before you rely on it:

- **Concurrent single-PUT uploads are not memory-bounded.** Peak resident memory
  tracks concurrency × object size — four simultaneous 32 MiB PUTs reach about
  151 MiB. MonoBucket's own write path streams the body into the payload tree in
  fixed-size chunks; the residency is below it, in Drogon, which spills a body
  past `MONOBUCKET_MAX_MEMORY_BODY_BYTES` to a temporary file and then hands the
  handler a memory map of the whole file. The pages are file-backed and
  evictable rather than anonymous, so this is a weaker hazard than holding the
  object on the heap, but it is resident. Multipart uploads are unaffected —
  each part is bounded by the client's part size, which is why the AWS CLI and
  rclone moving large objects stay flat. Fixing it means adopting Drogon's
  request-streaming API.
- **`CopyObject` is not implemented** and answers 501 rather than silently
  storing the header's value as an object.
- **No `io_uring` backend.** Blocking I/O runs on a bounded thread pool. Under
  the workloads measured so far the request path, not the I/O pool, is the
  limit.
- **No TLS to Redis.** `rediss://` is refused at startup with the reason;
  terminate TLS in front of Redis or use a private network.
- **S3 secrets are stored recoverable, not hashed.** SigV4 is a symmetric HMAC
  scheme: verifying a signature means re-deriving the signing key from the
  secret, so there is no verifier that authenticates a request without
  reproducing the secret. Anyone who can read the data directory can read every
  issued S3 secret. Console passwords are not stored this way — those are
  PBKDF2-SHA256 verifiers, and a stolen data directory does not yield a login.
- **The upload limit is instance-wide only.** One maximum object size applies to
  every bucket and every client; there is no per-bucket or per-user limit, and
  no precedence rule to learn. A bucket's allocation bounds how much it holds,
  not how large one object in it may be.
- **Bucket policies are a small, closed grammar, not IAM.** What is evaluated:
  `Allow` statements granting `s3:GetObject` or `s3:ListBucket` to
  `Principal "*"` over a whole bucket (`arn:aws:s3:::<bucket>`,
  `arn:aws:s3:::<bucket>/*`, or `*`). That is the entire language. A document
  containing anything else — a `Deny`, a `Condition`, a `NotAction`, a named
  principal, an action other than those two, or a resource scoped to a key
  prefix — is **refused** at `PutBucketPolicy`, naming the element, rather than
  stored and ignored. Signed requests never consult the policy at all: they are
  authorised by the role of the access key's owner. A policy is therefore only
  ever a statement about clients carrying no credentials.
- **Checksums are verified, but not every shape of them.** The
  `x-amz-checksum-*` family is checked on `PutObject` and `UploadPart` —
  CRC32, CRC32C, CRC64NVME, SHA1 and SHA256, sent as a header or as an
  `aws-chunked` trailer — and a value that does not match the bytes received is
  refused with `BadDigest`. Three gaps remain. A multipart object always gets
  the **composite** checksum (a checksum of the parts' checksums, `-N`
  suffixed); `x-amz-checksum-type: FULL_OBJECT` is refused at
  `CreateMultipartUpload` rather than answered with a composite the client did
  not ask for. Per-part `<ChecksumCRC32>` elements inside a
  `CompleteMultipartUpload` manifest are not compared — each part's ETag already
  pins it, and the composite is checked against the completion's own
  `x-amz-checksum-*` header when one is sent. And `ListObjectsV2` does not report
  a per-object `ChecksumAlgorithm`. An algorithm this build cannot compute is
  refused rather than stored unchecked: a checksum accepted and discarded is
  indistinguishable, to the client, from one that was verified.
- **Authorisation is per identity, not per bucket or per key.** A key inherits
  its owner's role and nothing narrower: there is no per-key scoping, no bucket
  restriction and no way to give one program read access to one bucket. An
  operator who may delete an object may delete any object. Narrowing that would
  mean a second policy system beside the bucket policies that already exist,
  with the two having to agree about which one denies.
- **The root S3 key pair cannot be revoked from the console.** It comes from the
  environment, is not attached to any user account, and is administrator-
  equivalent. Treat it as a break-glass credential and issue per-client keys for
  everything else; changing it means restarting with different values.
- **The audit log is a bounded ring, not an archive.** The most recent 5000
  entries, oldest dropped on write, stored beside the object metadata and lost
  with the data directory. Anything that has to be kept belongs wherever the
  server's stdout is collected. Entries are written without an fsync, so a power
  cut can lose the last few.
- **A role change reaches an open console tab only by ending its session.** The
  session carries a copy of the role, so changing a user's role or status signs
  them out rather than silently updating the page. S3 access keys need no such
  step — the owner's record is read on every signed request.
- **Storage allocations are logical, not physical, and are enforced in one
  process.** A bucket's allocation counts the object bytes it holds; it does not
  count the copy a multipart completion makes while concatenating parts, the
  RocksDB metadata beside them, or a payload still in `tmp/`. The allocatable
  capacity carries a 10% reserve for exactly that, and the reserve is a
  heuristic rather than a measurement. The tally itself is in memory, rebuilt
  from the objects at startup — correct for the single writer MonoBucket is, and
  not a scheme that would survive a second process writing the same directory.
- **An allocation is not a reservation on disk.** Allocating 500 GiB to a bucket
  reserves nothing from the filesystem; it only caps what that bucket may store.
  Allocating more in total than the disk holds is refused, but a disk shared with
  anything else can still fill under MonoBucket without any bucket reaching its
  allocation. Set `MONOBUCKET_ALLOCATABLE_BYTES` when the data directory is not
  alone on its volume.
- **`ListMultipartUploads` ignores `delimiter`.** The parameter is echoed back
  but no keys are rolled up, so the response carries no `CommonPrefixes`.
  `prefix`, `key-marker` and `upload-id-marker` all work, so a listing is
  complete and resumable — it is only the delimiter rollup that is missing.
- **Object versioning, lifecycle rules, server-side encryption and IAM-style
  policies** beyond the credentials described above do not exist. Abandoned
  multipart uploads are expired on a timer
  (`MONOBUCKET_MULTIPART_EXPIRY_HOURS`), which is the one piece of lifecycle
  behaviour that does exist, because without it they leak.

Two deliberate deviations from S3, both refused at write time rather than stored
as data nothing can later address: object keys containing control characters,
and keys containing a path traversal segment.

## Project links

[Changelog](CHANGELOG.md)

MonoBucket is licensed under [GPL-3.0-or-later](LICENSE).
