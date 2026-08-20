# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

MonoBucket is a single-binary, S3-compatible object storage server in C++20 (Drogon + RocksDB),
with a SvelteKit dashboard compiled into the executable as a static asset table. No sidecars, no
external database. GPL-3.0-or-later. Pre-alpha.

## Commands

### Backend (CMake presets — always use them; they set the build dir under `build/<preset>/`)

```bash
cmake --preset dev              # Debug, tests on, no embedded dashboard (first run fetches Drogon)
cmake --build --preset dev
ctest --preset dev
```

Other presets: `asan` (ASan+UBSan), `dev-redis` (debug + Redis backend), `release` (LTO, dashboard
embedded), `release-redis`.

Dependencies resolve `find_package` first and fall back to a pinned `FetchContent`
(`backend/cmake/Dependencies.cmake`). Drogon is the exception: it is fetched *and patched*
(`cmake/patches/drogon-expect-100-continue.patch`) because its parser rejects a zero-length body sent
with `Expect: 100-continue` — how every S3 client writes an empty object. A system Drogon cannot take
the patch, so configure refuses one unless `MONOBUCKET_ALLOW_SYSTEM_DROGON=ON`. Don't switch to a
system Drogon to save build time; it silently loses empty-object support.

Running a subset of tests — the suite is one Catch2 binary, `build/<preset>/bin/monobucket_tests`,
registered with `catch_discover_tests`, so both work:

```bash
ctest --preset dev -R sigv4                      # by discovered test name
./build/dev/bin/monobucket_tests "[sigv4]"       # by Catch2 tag
./build/dev/bin/monobucket_tests "exact test case name"
./build/dev/bin/monobucket_tests --list-tests
```

Tags are per-suite and there are ~30 of them (`[assets] [blob] [cache] [checksum] [chunked] [codec]
[config] [cors] [credentials] [digest] [engine] [env] [identity] [io] [keyspace] [metadata]
[multipart] [operation] [password] [policy] [quota] [reclaim] [recovery] [redis] [request] [session]
[sigv4] [uri] [xml]` and more); `--list-tests` is the authority, not this list.

Redis integration tests are skipped without a live server:

```bash
docker run -d --rm -p 63790:6379 redis:7-alpine
MONOBUCKET_TEST_REDIS_URL=redis://127.0.0.1:63790/0 ctest --preset dev-redis
```

### Running the server

```bash
export MONOBUCKET_DATA_DIR=$PWD/data MONOBUCKET_ROOT_SECRET_KEY=local-dev-secret
./build/dev/bin/monobucket
./build/dev/bin/monobucket --help | --print-config | --version
```
Port 9000 = S3 API, 9001 = console.

`monobucket --checkpoint <dir>` writes a consistent, startable copy of the store: RocksDB's
checkpoint API for `meta/`, hard links for `objects/`. The CLI form needs a *stopped* server —
one process may open the metadata store at a time — so backing up a **running** instance is the
console's job (`POST /_mb/api/backup`, `Permission::BackupWrite`, bounded by `MONOBUCKET_BACKUP_DIR`),
because the running process is the only thing that can copy its own store. Restore is: stop, point
`MONOBUCKET_DATA_DIR` at the checkpoint, start.

The ordering is load-bearing and so is the barrier. The metadata is snapshotted *first* and the
payloads linked second, so a payload written after the snapshot is merely absent from a copy that
never names it. That alone is not enough: `deleteObject` unlinks its payload immediately
(`reclaimNow`), so a delete landing between the two passes would strip a payload the snapshot still
names. `StorageEngine::checkpointing_` makes `reclaimNow`/`reclaimOlderThan` no-ops for the
duration; nothing leaks, because every payload is in the reclamation log before it is written and
the next pass collects what the window skipped. `backend/tests/checkpoint_test.cpp` asserts this
against a concurrent deleter — remove the barrier and that test fails.

The residue runs the other way and is accepted: an upload whose payload landed before the link
pass but whose metadata postdates the snapshot leaves an unreferenced file in the copy, so `--fsck`
on a backup taken under load exits 2 with `unreferenced-payload` findings while one taken quiet
exits 0. Skipping files by mtime would remove them and is provably safe on paper — a payload is
always written before the row naming it — but it stakes backup integrity on clock monotonicity,
and an extra file is recoverable where a missing one is not.

`monobucket --fsck [--deep]` checks a stopped store: it walks the metadata against the payload tree
and reports every disagreement — referenced-but-absent payloads, length mismatches, files nothing
references and reclamation does not know about, and names the store could not have written.
`--deep` additionally re-hashes every payload against the SHA-256 recorded at write time. It
reports and never repairs. Exit 0 is clean, 2 means it found something, 1 means it could not run.

`docker-compose.yml` is the deployment reference, and the annotated one: it is where the intended
value of every `MONOBUCKET_*` knob is written down. The console administrator's password is passed
as a Docker secret (`secrets/admin_password.txt`, gitignored, see `secrets/README.md`) rather than an
environment variable, because `environment:` values are readable from `docker inspect` and
`/proc/<pid>/environ`. That file is read only to create or *reset* the account — leaving it in place
resets the password on every restart.

### Frontend (pnpm, in `frontend/`)

```bash
pnpm install
pnpm dev            # :5173, proxies to the C++ API
pnpm run check      # svelte-check — CI runs this
pnpm run lint       # prettier --check + eslint — CI runs this
pnpm run test       # vitest, node environment — CI runs this
pnpm run format
pnpm run build      # static output in frontend/build/, consumed by the embed step
```

CI (`.github/workflows/ci.yml`) runs backend presets `dev` and `asan`, the frontend
check/lint/test/build, and an amd64 image smoke test that also fetches the console out of the running
container — the one place the embedded dashboard is exercised in its shipping form. On `master` it
publishes that same image as `edge`. Releases are `release.yml`: two native runners (amd64 and
`ubuntu-24.04-arm`), push by digest, merge into a manifest list, cosign it keylessly, publish. Never
build the arm64 leg under QEMU — it is a tenth the speed and cannot run its own smoke test.

C++ formatting is `.clang-format` (Google base, 100 cols, 4-space indent, `SortIncludes` on but
`IncludeBlocks: Preserve` — keep the existing include grouping).

## Architecture

### Layering

`main.cpp` → `Server` (owns event loop, listeners, `StorageEngine`, `IoExecutor`, `CacheProvider`,
`S3Metrics`) → route registration → handlers → `StorageEngine` → `MetadataStore` (RocksDB) +
`BlobStore` (POSIX files).

Everything except `main()` lives in the `monobucket_core` static library so the test binary can link
it; `main.cpp` is the only file in the `monobucket` executable target. New `.cpp` files must be added
to `MONOBUCKET_CORE_SOURCES` in `backend/CMakeLists.txt` (and new tests to `backend/tests/CMakeLists.txt`).

`common/` is a separate, header-only target (`MonoBucket::Common`, INTERFACE): `constants.hpp` plus
`version.hpp.in`, which CMake configures into the build tree. The version therefore reaches the code
from `CMakeLists.txt` and never from a hand-maintained header — nothing else should re-declare it.

### Threading rule

Drogon handlers run on the event loop. Any code touching RocksDB or the filesystem must be posted to
`IoExecutor` — a bounded thread pool. The bound is deliberate: a full queue sheds load (S3 `503
SlowDown`) rather than growing memory. Storage APIs are synchronous and unit-testable because of this
split; do not make them async.

### Routing (`s3/`)

One catch-all Drogon route serves both listeners, dispatching on
`http->getLocalAddr().toPort() == config.consolePort` (`s3/router.cpp`). Two independent catch-alls
would shadow each other by registration order. System routes (`/healthz`, `/readyz`, `/metrics`,
`/_mb/*`) are exact paths registered first, which is why those names are refused as bucket names.

Request flow: `parseRequest()` (`s3/request.cpp`) → `classify()` (`s3/operation.cpp`, a pure function
mapping method + path + subresource to an `Operation` enum — the entire routing table, exhaustively
testable) → SigV4 verification (`s3/sigv4.cpp`) → `authorize()` → `dispatch()` → a handler in
`bucket_handlers.cpp` / `object_handlers.cpp` / `multipart_handlers.cpp`.

Handlers throw `S3Exception(S3ErrorCode)`; the router converts every throw into an S3 XML error
document. Error code ⇄ HTTP status pairs live in one table in `s3_error.cpp` and are never assembled
at a call site — clients branch on the code string.

`S3Context` (config, storage, cache, metrics) is passed by reference and never owned. HEAD is routed
as GET by Drogon, so `handleGetObject` serves both. `CopyObject` deliberately answers 501 rather than
storing the header's value as an object — that is a decision, not a gap.

`OPTIONS` preflight (`s3/cors.cpp`) is answered *before* signature verification, since a browser
preflight carries no credentials by definition.

SigV4 is expressed over plain strings, not Drogon types, so it can be tested against the AWS
reference vectors without a socket. Keep it that way.

### Console backend (`server/console_api.cpp`)

The dashboard is not served by the S3 code path. `registerConsoleApi()` mounts a JSON API under
`/_mb/api/` (login, logout, session, overview, series, audit, buckets, buckets/access, buckets/cors,
buckets/policy, buckets/quota, config, objects, object, upload, upload-limit, presign, credentials,
credentials/rotate, users, users/password) that answers **only** on the console
listener — a `/_mb/api/...` route on port 9000 would collide with a bucket named `_mb`. Both it and
`registerSystemRoutes()` must run *before* the S3 catch-all: Drogon prefers exact paths over regex
handlers, but only when they were registered first.

Console auth is a session cookie (`mb_session`, `SameSite=Strict`, 12 h), not SigV4. The subject is
a **user account** — a username, a PBKDF2-SHA256 verifier and a role, in the metadata store — and
never an S3 credential. Keeping them apart is the point: revoking every access key leaves the
session working, and signing out leaves every S3 client working. `SessionStore` and `LoginThrottle`
live in `server/console_session.cpp` with the clock injected, so expiry and lockout are testable
without a listener.

A session carries a *copy* of the role and of the bucket grants rather than re-reading them per
request, so every change to a user's role, status or grants calls `SessionStore::closeUser()`. That
is the whole reason such a change signs somebody out: the alternative is a RocksDB lookup on the
event loop for every console request. `SessionStore::open()` takes a whole `Principal` for the same
reason — a signature taking the parts would let a caller capture the role and default the grants,
and the default is unrestricted.

Authorisation is `allows(Role, Permission)` in `core/identity.cpp` — a pure function, a closed set of
three roles, no policy language. Console routes pass the permission they need to `guard()`; a route
that forgets does not compile. Signed S3 requests map operation → permission through
`s3::permissionFor()` and are checked against the role of the key's owner, read fresh every request.
Both mapping tables are exhaustive switches with no default arm, so a new `Operation` or `Permission`
is a compile error rather than a silent hole. `backend/tests/identity_test.cpp` asserts the whole
matrix as literal data — never derived from `allows()`, which would agree with any change.

A second, narrower axis sits under the role: `BucketAccess` (`none`/`read`/`write`) per bucket,
stored on the `UserRecord` as a `BucketGrants` — one fallback plus a map of exceptions, so a
denylist and an allowlist are one mechanism. `allows(Role, const BucketGrants&, bucket, Permission)`
ANDs the two, and the direction is load-bearing: grants only ever *narrow*, so a `readonly` account
with `write` access to a bucket still cannot write. `permits(BucketAccess, Permission)` classifies
which permissions are bucket-scoped at all — the ones that are not (users, credentials, settings,
audit) return true rather than being denied, because this function has no opinion about a question
it was not asked. **An administrator is never narrowed** and the console refuses to store a
narrowing for one: an administrator can rewrite their own grants, so enforcing them would be a lock
whose key hangs on the door, and the failure mode is stranding the only account that could repair
it. Promotion clears whatever the record carried.

Bucket-scoped console routes use `guardBucket()` rather than `guard()`. It hands the handler the
bucket name, which is the *only* way a handler can get one — reading the name and checking it are
the same act, so a route cannot do the first and forget the second. The name is extracted by one
function that knows all four places the routes carry it (`?bucket=`, `?name=`, and the `bucket` or
`name` field of a JSON body); four per-route extractors would eventually check a different field
from the one the handler reads. Routes that name *no* bucket — the bucket list, the allocation
list, S3 `ListBuckets` — are narrowed by **filtering** rather than refusing, because one bucket
somebody is not entitled to must not cost them every bucket they are.

The audit log (`kAudit` records, `MetadataStore::appendAudit`) is a fixed ring of `kAuditCapacity`
entries keyed by sequence alone, so lexicographic order is insertion order and a clock that steps
backwards cannot reorder history. It is written without an fsync and drops entries when the I/O queue
is full: a log that could refuse a sign-in, or that an unauthenticated client could grow, would be a
participant rather than a record. S3 *signature* failures are deliberately not logged — only role
refusals, which require a valid credential and are therefore bounded by who holds one.

What is recorded is everything that destroys data or changes who can reach it: bucket create and
delete, access, CORS and policy changes, console object uploads and deletes, plus the session,
user and credential events. The access and policy entries record the *resulting* visibility read
back from the record, not the request — the log has to answer "who made this public" without the
reader re-evaluating a stored policy document. Session events name the peer address and never
`X-Forwarded-For`, which is written by whoever spoke to us last. `kAuditCapacity` is sized for that
volume: object events arrive per click, so the window in *time* is what the number is really
choosing, and S3 object writes are excluded to keep it bounded by what a person can do in a browser.

Consequences that are easy to break: login is posted to `IoExecutor` because the verifier is
deliberately expensive and the event loop must not pay it; a failed login returns one message for
every cause *and* runs the verifier against `password::dummyHash()` when the username misses, so
neither the text nor the timing distinguishes an unknown user from a wrong password; failed logins
are rate-limited *globally* rather than per-IP (one account, so a per-IP bucket just tells an
attacker to rotate source addresses); and the cookie's `SameSite=Strict` is why `pnpm dev` proxies
`/_mb` to `:9001` in `vite.config.ts` instead of calling an absolute URL. The session TTL, sampler
cadence and rate-limit window are deliberately constants, not `MONOBUCKET_*` knobs.

Startup refuses to open a listener when the console is enabled, no administrator is provisioned and
no password is configured (`Server::provisionAdministrator`). That is a decision, not an oversight:
the alternatives are a documented default password or a console nobody can enter.

Startup also runs two record-rewriting passes over the store — `core/identity_migration.cpp` (adopts
ownerless access keys into the administrator account) and `server/policy_reconcile.cpp` (narrows
bucket access to what this build actually enforces, and names any stored policy it will not
evaluate). Both are free functions, not `Server` members, for the same reason: they change records
an operator already has, so they must be testable against a bare store rather than a live listener.
An unenforceable policy document is left byte-for-byte as written — it is the operator's text, and
deleting it would destroy the only record of the intent — but it grants nothing.

S3 credentials are issued from `/_mb/api/credentials`. A generated secret crosses the wire exactly
once, in the response that created or rotated it — `toJson(const AccessKeyRecord&)` has no
`secretKey` field, so a later handler cannot leak one by reusing it. Revocation deletes the record;
`s3/router.cpp` resolves the secret from the store on every signed request and caches nothing, which
is what makes revocation take effect on the next request rather than the next restart. Secrets are
stored recoverable because SigV4 is symmetric — there is no verifier that authenticates a signature
without reproducing the secret, and that deviation is recorded in *Known limitations*.

Graph data comes from `MetricsHistory` (`server/metrics_history.cpp`), a fixed ring of 240 samples
at 5 s — 20 minutes, allocated once. It stores *deltas* between consecutive readings, so the browser
never sees cumulative counters, and the first reading only establishes a baseline. Longer retention
is `/metrics` plus a scraper's job; do not grow the ring.

### Storage invariants (`storage/`)

These are correctness rules, not style:

- A payload is written and flushed **before** the metadata naming it is committed; a payload is
  unlinked only **after** that metadata is gone. An interruption leaves the old state or the new one.
- Every `MetadataStore` mutation that drops a payload reference *returns* the released blob id in the
  same call that committed the change; the caller must hand it to reclamation. The interface is shaped
  to make a leaked or prematurely deleted blob impossible to overlook.
- A blob is registered in the reclamation log (`trackBlob`) **before** its payload is written, so a
  crash leaves a trace collectable in time proportional to the leak, not to the object count.
- `MONOBUCKET_DURABILITY` (`none` / `relaxed` / `strict`, `storage/durability.hpp`) decides how much
  of a write reaches stable storage before it is acknowledged. It is global, not per-bucket, and an
  unrecognised value fails startup rather than quietly weakening durability.
- `listOrphans` takes an age cutoff. The grace period is load-bearing, not an optimisation:
  reclaiming an in-flight blob would drop its tracking record and leak it permanently. Startup
  recovery passes "now" because nothing is in flight then.

`ObjectRecord` and `UploadRecord` carry the five stored response headers
(`Cache-Control`, `Content-Disposition`, `Content-Encoding`, `Content-Language`,
`Expires`) as a `ContentHeaders`. Adding a field to `ObjectRecord` touches three
encodings, not one: the stored record (`rocksdb_metadata_store.cpp`, appended
and read only if the row is long enough, so no version bump), the cached record
(`s3/handlers.cpp`, which has its own `kCachedObjectVersion` and *must* be
bumped — a warm cache that dropped a field would answer differently from a cold
one), and whatever emits it. `resolveContentHeaders` (`s3/content_headers.cpp`)
decides stored-value-versus-`response-*`-override as a pure function over
`S3Request`, deliberately away from Drogon so the precedence presigned links
depend on is testable without a socket.

All records share one RocksDB column family with a one-byte type tag (`storage/keyspace.hpp`) — a
family per type would multiply the memtable floor. NUL separates key components (safe: bucket names
are DNS-restricted, keys are UTF-8) and sorts below everything, so `o<bucket>\0` iterates exactly one
bucket in the lexicographic order ListObjectsV2 specifies. Part numbers are stored big-endian for
ascending iteration.

On-disk layout under `MONOBUCKET_DATA_DIR`: `meta/` (RocksDB), `objects/<aa>/<bb>/<blobId>`, `tmp/`.

### Storage allocations (`storage/quota.cpp`)

Every bucket carries a `quotaBytes` in its record; zero means unlimited, which is
what a bucket created before this existed and a bucket created by plain S3
`CreateBucket` (with no `MONOBUCKET_DEFAULT_BUCKET_QUOTA_BYTES` set) both read
back as.

`QuotaLedger` — not RocksDB — is the authority on whether a write is admitted.
Checking a bucket's charge and then committing has to happen without another
writer slipping between them, RocksDB has no compare-and-commit spanning a
payload write, and there is exactly one writer process by design; a mutex over an
in-memory tally is the only thing here that is actually atomic. Nothing in the
ledger is persisted: allocations live in the bucket records, and the charges are
re-derived at startup from the scan `seedCounters()` already does over every
object and part.

A write takes a `QuotaLedger::Reservation` — RAII, because every write path
throws — before the body is read, and `finishWrite`/`finishPart` raise it to what
actually arrived and settle it. Settlement releases the claim and applies the
charge *under one lock*: releasing first would let a second writer into space the
first is about to occupy. Every delta (`replacedBytes`, `releasedBytes`,
`releasedPartBytes`) is returned by the commit that caused it, never read
separately, so an overwrite cannot drift.

Parts are charged as `pendingBytes` the moment they are stored and released in
full on abort; completion moves them to `usedBytes` without a fresh admission,
because a bucket at its allocation must still be able to finish an upload it was
already charged for. An overwrite is admitted as if the key were new — both
payloads are on disk until the commit.

`MONOBUCKET_ALLOCATABLE_BYTES` unset means "derive from the filesystem, less
`MONOBUCKET_CAPACITY_RESERVE_PERCENT`". Changing an existing bucket's allocation
is `Permission::CapacityWrite` (administrator only); sizing the bucket you are
creating is `BucketWrite`, because that is bounded by what is unallocated.

### Cache (`cache/`)

One `CacheProvider` interface selected by `MONOBUCKET_CACHE_BACKEND`. `memory` is a sharded LRU where
reads take the shared lock and deliberately do *not* reorder the list (promoting on read would make
`get()` a writer); a hit sets an atomic flag and eviction grants a flagged entry one reprieve. `redis`
(optional, `-DMONOBUCKET_ENABLE_REDIS=ON`) sits *behind* the local tier, not instead of it, with a
circuit breaker. A cache outage must never become a storage outage. The budget is enforced on every
insert, not trimmed on a timer.

### Frontend embedding

`backend/cmake/EmbedFrontend.cmake` turns `frontend/build/` into a generated C++ translation unit
(sorted asset table, binary-search lookup). With `MONOBUCKET_EMBED_FRONTEND=OFF` (the `dev` preset) a
stub with an empty table is generated instead, so the backend always builds without running pnpm; the
console port then returns 404. SSR and prerendering are off (`src/routes/+layout.ts`) and Kit config
lives in `vite.config.ts` — this SvelteKit version has no `svelte.config.js`.

The generator also shells out to `gzip` and `brotli` to pre-compress text assets at build time,
storing the variants in the same table entry; `assets::encodedFor()` picks one from the request's
`Accept-Encoding`. Both tools are optional — absent, the table simply has no variants. Nothing
compresses per request.

The frontend suite is `pnpm run test` (vitest, node environment, `src/**/*.test.ts`). It covers
`$lib/api.ts` against a stubbed `fetch` — which endpoint is called, with what body, and what is done
with the answer — because that is where the console's logic actually is. A browser runner would drag
Playwright into CI to assert the same things through three more layers. CI runs `check`, `lint`,
`test` and `build`.

The console is a client-rendered SPA: `adapter-static` with `fallback: 'index.html'`, and the C++
asset store hands `index.html` to any unmatched console route so SvelteKit resolves it client-side.
Routes live under the `(app)` group (dashboard, `buckets`, `buckets/[name]`, `activity`,
`credentials`, `users`, `settings`) with `login` outside it; all API calls go through `$lib/api.ts`.
Runes mode is forced for everything outside `node_modules` in `vite.config.ts` — write Svelte 5
runes, not the legacy reactive syntax.

Console styling is daisyUI 5 over Tailwind 4. The two themes (`monobucket`, `monobucket-dark`) are
defined in `frontend/src/app.css`, not borrowed from daisyUI's built-ins; use semantic colour names
(`bg-primary`, `text-base-content/60`) so both themes stay correct, never `dark:` and never a raw
Tailwind palette colour for text; `app.css` records the measured contrast ratio of every semantic
pair, so a colour change means re-checking them. Icons come from `$lib/components/Icon.svelte` — one
inline set, no icon package and no emoji. Charts are LayerChart over the same daisyUI variables
(`SeriesChart`, `ThroughputChart`), which is why they follow the theme without a second palette.

## Project conventions

- **Allocations are logical and single-process.** A bucket's allocation counts
  object bytes, not the concatenation copy a multipart completion makes, the
  RocksDB files beside them, or a payload still in `tmp/`. The reserve percent
  covers those and is a heuristic. Recorded in *Known limitations*.
- **Two credential kinds, kept apart.** User accounts are for people and the console; S3 access
  keys are for programs and the S3 listener. Neither authenticates against the other's surface. A
  change that lets one do the other's job is a regression regardless of how convenient it is.
- **An access key never exceeds its owner.** Every key carries an `owner` and is authorised with
  that user's role *and* that user's bucket grants, both read from the one record the signed request
  already fetches. The one exception is the root pair from the environment, which is not a user and
  is administrator-equivalent by design — it is the break-glass credential and is documented as one.
  Startup adopts ownerless keys (written before this existed) into the administrator account.
- **The last enabled administrator is protected.** Delete, disable and demote all consult
  `countEnabledAdministrators()` first. A console that cannot be administered cannot be repaired
  from the console.
- **Configuration is environment only.** Every knob is a `MONOBUCKET_*` variable parsed once at
  startup into the immutable `Config`, validated before the first listener opens. No config file
  format, nothing hot-reloaded. A malformed setting aborts startup with an actionable message rather
  than silently defaulting. Add new settings to `core/config.cpp`, `.env.example`, and `--help`.
- **Bounded memory is the product.** Resident memory must stay flat regardless of object size or
  concurrency. Anything above `MONOBUCKET_MAX_MEMORY_BODY_BYTES` streams in fixed-size chunks;
  queues are bounded; RocksDB's block cache and memtables share one budget.
- **S3 fidelity over convenience.** Where the spec and ergonomics disagree, match the spec.
- **Deviations from S3 are deliberate and documented.** MonoBucket refuses two things S3 accepts:
  object keys containing control characters, and keys containing a path traversal segment
  (`isValidObjectKey`, `s3/request.cpp`). Both are refused at write time rather than stored as data
  nothing can later list or address. Record any new deviation in *Known limitations* in
  `README.md`; an undocumented one is indistinguishable from a bug.
- **Comments explain the decision, not the code.** The existing comments justify why an alternative
  was rejected (see `keyspace.hpp`, `io_executor.hpp`, `metadata_store.hpp`). Match that register;
  don't add narration.
- **Versioning is CalVer** `YYYY.0M.MICRO`, set in the top-level `CMakeLists.txt` in two places (the
  three `set()` lines and `project(VERSION)`). Don't edit them by hand: `scripts/cut-release.sh`
  bumps the version, promotes the `[Unreleased]` changelog block and tags. `release.yml` refuses to
  publish unless the tag, `CMakeLists.txt` and `CHANGELOG.md` all agree.
- **Every change adds a `CHANGELOG.md` entry under `[Unreleased]` in the same commit.** A change
  that alters what the project can or cannot do also updates *Known limitations* in `README.md` —
  that section is now the only record of what is unfinished, so letting it go stale loses the
  information entirely. Run `ctest --preset dev` before pushing.

## Status vs. docs

The S3 protocol layer, the dashboard and the signed multi-arch image pipeline all exist and work.
*Known limitations* in `README.md` is the source of truth for what is **not** done — there is no
roadmap file. The one that most often trips people up:

**Concurrent single-PUT uploads are not memory-bounded.** Peak RSS tracks concurrency × object size
(four concurrent 32 MiB PUTs reach roughly 151 MiB). MonoBucket's write path is correctly chunked;
the residency is Drogon's, which spills a body past `MONOBUCKET_MAX_MEMORY_BODY_BYTES` to a temp
file and then hands the handler an `mmap` of the whole thing. Multipart is unaffected — each part
is bounded by the client's part size. The fix is Drogon's `enableRequestStream`, which turns the
PUT handler push-based and drags the SigV4 chunked verifier with it. Don't claim flat memory for
single PUTs until that lands.

Also unfinished: `io_uring` (the thread-pooled path is the sanctioned one), `rediss://` TLS
(refused at startup with the reason), C++23 `#embed` for the asset generator (blocked on the Alpine
toolchain floor), and LTO on musl.

There is no automated conformance or benchmark suite in this repo; `ctest --preset dev` and
`--preset asan` are what CI runs. Any claim about throughput or resident memory has to be measured
before it is made.

Where a doc and the source disagree, trust the source and fix the doc in the same commit.
