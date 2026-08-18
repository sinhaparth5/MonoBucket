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
- One container for `linux/amd64` and `linux/arm64`

## Two kinds of credential

MonoBucket keeps the person and the program apart:

- **An administrator account** — a username and password — signs in to the
  dashboard. It is provisioned from the environment on first start and stored
  as a PBKDF2-SHA256 verifier, never as a password.
- **S3 access keys** sign S3 requests. Issue, rotate and revoke them from the
  console's **Access keys** screen. They do not grant console access, and the
  console session does not sign S3 requests.

Revoking a key never locks you out of the dashboard; changing your password
never breaks a client.

There is no default administrator password. A deployment with none configured
and none provisioned **refuses to start** and says what to set — a shipped
default would be a published credential on every install.

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
- **Every valid credential has full authority.** An issued access key can do
  anything the root pair can, including issuing nothing — there is no per-key
  scoping, no read-only key and no bucket restriction. What issued keys buy over
  the root pair is that they can be rotated and revoked individually.
- **One administrator account, and it cannot be changed from the console.**
  There are no additional users, no roles, and no password-change screen; a
  reset means restarting with the password variable set. The root S3 key pair
  likewise cannot be revoked from the console — it comes from the environment.
- **Object versioning, lifecycle rules, server-side encryption and IAM-style
  policies** beyond the credentials described above do not exist.

Two deliberate deviations from S3, both refused at write time rather than stored
as data nothing can later address: object keys containing control characters,
and keys containing a path traversal segment.

## Project links

[Changelog](CHANGELOG.md)

MonoBucket is licensed under [GPL-3.0-or-later](LICENSE).
