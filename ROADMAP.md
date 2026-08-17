# MonoBucket Roadmap

A phased blueprint for **MonoBucket**: a single-binary, ultra-low-memory, C++
S3-compatible object storage server with an embedded SvelteKit administration
dashboard.

**Status legend** — `[ ]` not started · `[~]` in progress · `[x]` done

**Current phase:** Phase 5 — SvelteKit Frontend & Asset Embedding
**Last updated:** 2026-08-17

---

## Guiding constraints

These hold across every phase. A change that violates one is a bug, not a
trade-off.

| Constraint | What it means in practice |
| --- | --- |
| **Single binary** | The dashboard, the S3 API and the storage engine ship as one executable. No sidecars, no external database. |
| **Bounded memory** | Resident memory stays flat regardless of object size or concurrency. Everything above `MONOBUCKET_MAX_MEMORY_BODY_BYTES` streams through fixed-size chunks. |
| **Configuration is environment** | No config file format to maintain. Every knob is a `MONOBUCKET_*` variable, parsed once at startup, validated before the first listener opens. |
| **Fail fast, fail loud** | A malformed setting aborts startup with an actionable message rather than falling back to a default nobody asked for. |
| **S3 fidelity over convenience** | Where the specification and ergonomics disagree, match the specification — clients depend on the wire format, not on our taste. |

---

## Phase 1 — Project Scaffolding & Core Architecture

*Goal: a repository that configures, builds, tests and runs an empty server.*

### Build system setup
- [x] CMake 3.25+ with C++20, warnings-as-signal, Release/Debug/ASan presets
- [x] Dependency acquisition via `find_package` first, `FetchContent` fallback
      (Drogon, nlohmann_json, Catch2, optional hiredis)
- [x] Directory layout: `/backend` (C++ engine), `/frontend` (SvelteKit UI),
      `/common` (shared definitions)
- [x] `CMakePresets.json` for dev / asan / release / release-redis
- [x] CalVer version header generated from CMake into `common/`
- [ ] CI workflow: configure, build, test, and publish the image

### Threading & core loop strategy
- [x] Asynchronous event loop on Drogon (Trantor / epoll)
- [x] Worker pool sized from `std::thread::hardware_concurrency()`, overridable
      and capped
- [x] Global configuration management: environment parsing, byte-size units,
      cross-field validation, redacted summary and JSON views
- [x] Graceful shutdown: `SIGTERM`/`SIGINT` set a `sig_atomic_t`, the loop polls
      it, ordered shutdown hooks run afterwards
- [x] Structured stderr logger, independent of the HTTP stack so core code stays
      unit-testable

**Exit criteria:** `ctest` is green, `monobucket --print-config` renders the
resolved configuration, and the process answers `/healthz` and shuts down
cleanly on `SIGTERM`.

---

## Phase 2 — Storage Layer & POSIX I/O Engine

*Goal: durable objects on disk with transactional metadata.*

### Metadata management
- [x] Embedded key-value store behind a `MetadataStore` interface — **RocksDB**,
      tuned so its block cache and memtables share one budget
      (`MONOBUCKET_METADATA_MEMORY_BYTES`) rather than being sized separately
- [x] Schema: buckets, object keys, ETags, SHA-256, content types, user
      metadata, public-read flag, multipart upload state. One column family
      with a type-tagged keyspace, because a family per record type multiplies
      the memtable floor for no ordering benefit
- [x] Atomic metadata updates — every mutation is one `WriteBatch`, and an
      object becomes visible only once its payload is fully durable
- [x] Crash recovery: temporaries swept and unreferenced payloads reclaimed on
      startup, in time proportional to the leak rather than the object count
- [ ] Full `fsck` mode that walks the payload tree to catch damage the
      reclamation log cannot see

### Disk storage engine
- [x] Directory hashing / sharded tree — `objects/<aa>/<bb>/<id>`, 65 536 leaf
      directories
- [x] Chunked streaming with fixed memory per transfer, on `pread`/`pwrite`
- [x] Blocking I/O moved off the event loop onto a bounded pool (`IoExecutor`);
      a full queue sheds load rather than growing
- [ ] `io_uring` backend on Linux — the thread-pooled path above is the
      sanctioned fallback; revisit once there is a benchmark that justifies the
      second I/O path
- [x] Integrity layer: MD5 (ETag) and SHA-256 computed during the stream, never
      by re-reading the object
- [~] `fsync` / durability policy — global (`MONOBUCKET_DURABILITY`), not yet
      per bucket. Per-bucket overrides land with bucket policies in Phase 4,
      where there is somewhere to put them
- [x] Free-space accounting for the dashboard's capacity metrics

**Exit criteria:** a multi-gigabyte object round-trips byte-identically with
resident memory flat to within a few MiB.
*Not yet demonstrated end to end — there is no HTTP path to an object until
Phase 4. The engine-level round trip is covered by tests, and the memory claim
is measured in Phase 7.*

---

## Phase 3 — Abstraction & Cache Layer

*Goal: a pluggable cache that never becomes the reason memory grows.*

### Abstract interface
- [x] `CacheProvider`: `get`, `set`, `del`, `evict`, `clear`, plus stats for
      `/metrics`. Values are handed out as `shared_ptr<const string>` so a
      reader is not racing eviction for the bytes it is still writing

### In-memory LRU backend (default)
- [x] `std::unordered_map` + intrusive doubly linked list, guarded by
      `std::shared_mutex` — **sharded**, because one lock over a cache this hot
      serialises the whole server
- [x] Reads take the shared lock and do not reorder the list. Promoting on
      every read would make `get()` a writer and reduce the `shared_mutex` to
      decoration; a hit sets an atomic flag and eviction gives a flagged entry
      one reprieve (CLOCK). Recency is approximate, concurrency is real
- [x] Byte-size budget, not per-entry counts, and charged per-entry overhead as
      well as payload — a million eight-byte values are not eight megabytes
- [x] The budget is held **on insert**; the periodic pass collects expired
      entries rather than enforcing the ceiling. A budget only checked on a
      timer is a target
- [x] Hit/miss/eviction/expiration/rejection counters exported to Prometheus

### Redis backend (optional)
- [x] `hiredis` wrapper conforming to `CacheProvider`, over `redisCommandArgv`
      so an object key containing `%` is data rather than syntax
- [x] Bounded connection pool with lazy connect — a Redis that is down at
      startup must not stop the container from starting
- [x] Reconnect with backoff, transparent fallback to in-memory when Redis is
      unreachable — a cache outage must never become a storage outage
- [x] Arranged as two tiers rather than an either/or, so the fallback is warm
      when it is needed instead of empty. `MONOBUCKET_CACHE_LOCAL_TTL_SECONDS`
      bounds how long the local copy of a shared value can go unnoticed
- [x] `clear()` scans our own key prefix; never `FLUSHDB`, since the database
      may not be ours alone
- [ ] TLS (`rediss://`) — refused at startup with the reason rather than left
      to fail as a connection timeout

**Exit criteria:** cache backend is swappable by environment variable alone, and
the in-memory backend holds its configured byte budget under stress.
*Both met and covered by tests. The budget is asserted on every insert across
500 inserts and under eight concurrent threads, and backend selection is
verified through `Config::fromEnvironment()`. What is not yet demonstrated is a
hit ratio under real traffic: nothing calls the cache until the S3 handlers land
in Phase 4, so `monobucket_cache_hits_total` is legitimately 0 today.*

---

## Phase 4 — S3 REST API Protocol

*Goal: real S3 clients work unmodified.*

### Router & signature engine
- [x] AWS Signature Version 4 validator on OpenSSL: canonical request, signed
      headers, query-string signing, chunked payload signing
- [x] Clock-skew rejection window
- [x] Presigned URL verification
- [x] Anonymous read paths for public buckets
- [x] S3-shaped XML error responses with request IDs

### Endpoints
- [x] **Service:** `GET /` (ListBuckets)
- [x] **Bucket:** `PUT /{bucket}`, `DELETE /{bucket}`, `HEAD /{bucket}`,
      `GET /{bucket}` (ListObjects + ListObjectsV2, prefix/delimiter/pagination)
- [x] **Object:** `GET` (incl. `Range`), `PUT`, `DELETE`, `HEAD`,
      `POST ?delete` (DeleteObjects)
- [x] **Multipart:** `CreateMultipartUpload`, `UploadPart`, `ListParts`,
      `CompleteMultipartUpload`, `AbortMultipartUpload`, `ListMultipartUploads`
- [x] Bucket policy / public-access endpoints backing the link generator

`CopyObject` is deliberately absent and answers 501 rather than silently
storing the header's value as an object. It is not on this phase's list; it
belongs with the dashboard's file operations.

**Exit criteria:** `aws s3 cp/sync`, `boto3` and `rclone` all pass a functional
smoke suite against a live instance.
*Met on 2026-08-17. `aws s3 cp/sync` (including a 17 MiB multipart transfer and
a repeat sync that correctly transfers nothing), boto3 (including the managed
transfer manager and its paginator), and `rclone copy/sync/check` all pass
against a live instance; `rclone check` verifies every object's hash. The raw
protocol surface — SigV4 header and presigned signing, `aws-chunked` streaming
signatures, ranged and conditional GETs, listing pagination, bucket policy —
is covered by a separate suite driven straight off the wire.*

*Reaching that took one dependency patch. Drogon's request parser refuses a
zero-length body sent with `Expect: 100-continue`, which is exactly how every
S3 client writes an empty object, so `boto3.put_object(Body=b"")` and
`aws s3 cp` of an empty file failed with a bare 400 raised before any
MonoBucket code ran. Fixed in `backend/cmake/patches/`; the bug is still on
upstream master, so a system Drogon is not equivalent and now has to be asked
for explicitly.*

---

## Phase 5 — SvelteKit Frontend & Asset Embedding

*Goal: an admin dashboard that ships inside the binary.*

### Console API

The dashboard needs a surface of its own: the S3 listener speaks SigV4, and a
browser holding an S3 secret cannot have console access revoked separately.

- [x] `/_mb/api/*` on the console listener only, session-authenticated, with
      every storage call posted to the I/O pool (`backend/src/server/console_api.cpp`)
- [x] Bounded metrics ring sampled on a timer, so the graphs need no time-series
      store and cost the same after a day as after a minute
      (`backend/src/server/metrics_history.cpp`)

### Dashboard
- [x] Visual language: two high-contrast daisyUI themes (`corporate` / `night`),
      Inter latin subset self-hosted from the binary, one sanctioned type scale
- [x] Overview: storage capacity, request rate, throughput, cache hit/miss,
      resident memory and storage-queue graphs
- [ ] Active connections on the overview — needs a listener-level counter that
      does not exist yet
- [x] Bucket list with create / delete and the anonymous-read toggle
- [ ] Bucket policy editing
- [x] File browser: delimiter-based folder walk, continuation pagination, object
      metadata viewer
- [ ] Drag-and-drop chunked uploader with progress
- [ ] Presigned link generator
- [ ] Settings panel: cache backend selection, RAM thresholds, API key
      management
- [x] Auth for the console, separate from S3 credentials: the root key pair is
      exchanged for an HttpOnly session cookie, so the browser never holds an S3
      secret

### Embedding pipeline
- [x] SvelteKit 2 / Svelte 5 project on pnpm, scaffolded with `sv create`
- [x] `@sveltejs/adapter-static` with SPA fallback, SSR and prerendering off
      (configured in `vite.config.ts` — this SvelteKit version has no
      `svelte.config.js`)
- [x] CMake pre-build step converting `frontend/build/` into a generated C++
      translation unit (`backend/cmake/EmbedFrontend.cmake`)
- [x] Sorted asset table with binary-search lookup and MIME detection
- [x] SPA fallback routing on the console listener; the S3 listener never serves
      console assets
- [x] Long-lived cache headers for fingerprinted `/_app/immutable/` assets
- [ ] Pre-compressed `gzip`/`brotli` variants selected by `Accept-Encoding`
- [ ] Migrate the generator to C++23 `#embed` once the toolchain floor allows

**Exit criteria:** `docker run` serves the dashboard on the console port with no
external assets and no network fetches.

---

## Phase 6 — Single-Container Build & Packaging

*Goal: one small image, correct lifecycle behaviour.*

- [x] Multi-stage Dockerfile: Node → Alpine toolchain → minimal runtime
- [x] Release flags: `-O3`, `--gc-sections`, `-ffunction-sections`, stripped
      symbols
- [~] LTO — enabled on glibc, automatically disabled on musl because Alpine's
      `fortify-headers` and LTO cannot coexist (see `CMakeLists.txt`). Revisit
      if the size/throughput gain justifies a glibc build stage.
- [x] Non-root runtime user, `/data` volume, read-only root filesystem
- [x] `docker-compose.yml` with an optional Redis profile and a memory cap
- [x] `/healthz`, `/readyz`, container `HEALTHCHECK`
- [x] `/metrics` in Prometheus text format
- [x] Graceful shutdown flushing state and closing descriptors
- [ ] Multi-architecture images (`linux/amd64`, `linux/arm64`)
- [ ] Automated CalVer tagging and publication to GHCR
- [ ] SBOM and image signing

**Exit criteria:** a tagged commit produces a published, signed, multi-arch
image whose runtime footprint is documented.

---

## Phase 7 — Testing, Profiling & Benchmarking

*Goal: evidence, not assertions.*

### Conformance
- [ ] `boto3` and AWS CLI compatibility suite in CI
- [ ] `rclone` and Go SDK cross-checks
- [ ] `s3-tests`-style edge cases: unicode keys, deep prefixes, empty objects,
      zero-byte multipart parts

### Performance & memory
- [ ] Valgrind and ASan/UBSan clean on the full test suite
- [ ] `wrk` and `k6` scenarios: concurrent small-object reads, large uploads,
      mixed workloads
- [ ] Published baseline: RSS under load vs. the configured budget
- [ ] Regression gate in CI on both throughput and peak RSS

**Exit criteria:** documented numbers for RAM, throughput and latency, with CI
failing on regression.

---

## Beyond 1.0 — under consideration

Not committed. Listed so they are not accidentally designed out.

- Object versioning and lifecycle rules
- Server-side encryption (SSE-S3, SSE-C)
- Bucket replication between MonoBucket instances
- Erasure coding / multi-disk backends
- Event notifications (webhook, NATS)
- IAM-style policy documents beyond the root credential

---

## How progress is recorded

1. Tick the checkbox in this file as part of the same commit that lands the work.
2. Add a line to `CHANGELOG.md` under `[Unreleased]`.
3. On release, move the `[Unreleased]` block under a new CalVer heading and tag
   the commit. See [CHANGELOG.md](CHANGELOG.md) for the versioning scheme.
