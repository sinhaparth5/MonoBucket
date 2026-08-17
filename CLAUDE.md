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

Running a subset of tests — the suite is one Catch2 binary, `build/<preset>/bin/monobucket_tests`,
registered with `catch_discover_tests`, so both work:

```bash
ctest --preset dev -R sigv4                      # by discovered test name
./build/dev/bin/monobucket_tests "[sigv4]"       # by Catch2 tag
./build/dev/bin/monobucket_tests "exact test case name"
./build/dev/bin/monobucket_tests --list-tests
```

Tags in use: `[assets] [blob] [cache] [codec] [config] [digest] [engine] [env] [io] [keyspace]
[metadata] [sigv4]`, plus the S3 suites (`s3_*_test.cpp`).

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

### Frontend (pnpm, in `frontend/`)

```bash
pnpm install
pnpm dev            # :5173, proxies to the C++ API
pnpm run check      # svelte-check — CI runs this
pnpm run lint       # prettier --check + eslint — CI runs this
pnpm run format
pnpm run test       # vitest, single run
pnpm run build      # static output in frontend/build/, consumed by the embed step
```

CI (`.github/workflows/ci.yml`) runs backend presets `dev` and `asan`, the frontend
check/lint/build, and an amd64 image smoke test that also fetches the console out of the running
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
as GET by Drogon, so `handleGetObject` serves both.

SigV4 is expressed over plain strings, not Drogon types, so it can be tested against the AWS
reference vectors without a socket. Keep it that way.

### Storage invariants (`storage/`)

These are correctness rules, not style:

- A payload is written and flushed **before** the metadata naming it is committed; a payload is
  unlinked only **after** that metadata is gone. An interruption leaves the old state or the new one.
- Every `MetadataStore` mutation that drops a payload reference *returns* the released blob id in the
  same call that committed the change; the caller must hand it to reclamation. The interface is shaped
  to make a leaked or prematurely deleted blob impossible to overlook.
- A blob is registered in the reclamation log (`trackBlob`) **before** its payload is written, so a
  crash leaves a trace collectable in time proportional to the leak, not to the object count.
- `listOrphans` takes an age cutoff. The grace period is load-bearing, not an optimisation:
  reclaiming an in-flight blob would drop its tracking record and leak it permanently. Startup
  recovery passes "now" because nothing is in flight then.

All records share one RocksDB column family with a one-byte type tag (`storage/keyspace.hpp`) — a
family per type would multiply the memtable floor. NUL separates key components (safe: bucket names
are DNS-restricted, keys are UTF-8) and sorts below everything, so `o<bucket>\0` iterates exactly one
bucket in the lexicographic order ListObjectsV2 specifies. Part numbers are stored big-endian for
ascending iteration.

On-disk layout under `MONOBUCKET_DATA_DIR`: `meta/` (RocksDB), `objects/<aa>/<bb>/<blobId>`, `tmp/`.

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

Console styling is daisyUI 5 over Tailwind 4. The two themes (`monobucket`, `monobucket-dark`) are
defined in `frontend/src/app.css`, not borrowed from daisyUI's built-ins; use semantic colour names
(`bg-primary`, `text-base-content/60`) so both themes stay correct, never `dark:` and never a raw
Tailwind palette colour for text. Icons come from `$lib/components/Icon.svelte` — one inline set, no
icon package and no emoji.

## Project conventions

- **Configuration is environment only.** Every knob is a `MONOBUCKET_*` variable parsed once at
  startup into the immutable `Config`, validated before the first listener opens. No config file
  format, nothing hot-reloaded. A malformed setting aborts startup with an actionable message rather
  than silently defaulting. Add new settings to `core/config.cpp`, `.env.example`, and `--help`.
- **Bounded memory is the product.** Resident memory must stay flat regardless of object size or
  concurrency. Anything above `MONOBUCKET_MAX_MEMORY_BODY_BYTES` streams in fixed-size chunks;
  queues are bounded; RocksDB's block cache and memtables share one budget.
- **S3 fidelity over convenience.** Where the spec and ergonomics disagree, match the spec.
- **Comments explain the decision, not the code.** The existing comments justify why an alternative
  was rejected (see `keyspace.hpp`, `io_executor.hpp`, `metadata_store.hpp`). Match that register;
  don't add narration.
- **Versioning is CalVer** `YYYY.0M.MICRO`, set in the top-level `CMakeLists.txt` in two places (the
  three `set()` lines and `project(VERSION)`). Don't edit them by hand: `scripts/cut-release.sh`
  bumps the version, promotes the `[Unreleased]` changelog block and tags. `release.yml` refuses to
  publish unless the tag, `CMakeLists.txt` and `CHANGELOG.md` all agree.
- **Every change ticks its `ROADMAP.md` checkbox and adds a `CHANGELOG.md` entry under
  `[Unreleased]`, in the same commit.** Run `ctest --preset dev` before pushing.

## Status vs. docs

Phases 1–6 are complete: the S3 protocol layer (`backend/src/s3/`), the dashboard and the signed
multi-arch image pipeline all exist and work. `ROADMAP.md` is the source of truth for what is *not*
yet done — notably `io_uring`, `fsck`, per-bucket durability, `rediss://` TLS, C++23 `#embed`, and
the conformance/benchmark suites of Phase 7, which is where any claim about throughput or RSS under
load has to come from. Where a doc and the source disagree, trust the source and fix the doc in the
same commit.
