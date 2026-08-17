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

1. Update `MONOBUCKET_VERSION_{YEAR,MONTH,MICRO}` at the top of `CMakeLists.txt`.
2. Move the `[Unreleased]` entries below under a new dated CalVer heading.
3. Tick the corresponding boxes in `ROADMAP.md`.
4. Commit, then tag: `git tag -a v2026.08.0 -m "MonoBucket 2026.08.0"`.
5. Push the tag; CI builds the multi-arch image and pushes the tag set above.

---

## [Unreleased]

### Added

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
- **Documentation.** `ROADMAP.md` tracking all seven phases, `.env.example`
  documenting every setting, and this changelog.

### Fixed

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

---

<!--
Template for the next release — copy, do not edit in place.

## [YYYY.0M.MICRO] — YYYY-MM-DD

### Breaking
### Added
### Changed
### Deprecated
### Removed
### Fixed
### Security
-->
