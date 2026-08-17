# MonoBucket

Single-binary, low-memory, S3-compatible object storage written in C++20, with a
SvelteKit administration dashboard compiled into the executable.

One binary. One container. No sidecars, no external database.

> **Status: pre-alpha.** Phases 1–4 (scaffolding and lifecycle, the storage
> engine, the cache layer, the S3 REST API) and the Phase 5 embedding pipeline
> are in place. `aws s3`, `boto3` and `rclone` all work against it. The
> dashboard itself does not exist yet — the console port serves an empty SPA.
> See [ROADMAP.md](ROADMAP.md). Do not put data you cannot lose in it.

---

## Layout

```
MonoBucket/
├── backend/            C++ engine
│   ├── cmake/          dependency resolution + frontend embedding
│   ├── src/
│   │   ├── core/       config, env parsing, logging, lifecycle
│   │   ├── server/     event loop, listeners, system routes, asset store
│   │   ├── storage/    metadata store + POSIX I/O engine
│   │   ├── cache/      CacheProvider, sharded LRU, optional Redis
│   │   └── s3/         Phase 4 — SigV4, routing, XML responses
│   └── tests/          Catch2 suite
├── common/include/     version + protocol constants shared across layers
├── frontend/           SvelteKit dashboard (static build, embedded at compile time)
├── Dockerfile          Node → Alpine toolchain → minimal runtime
└── docker-compose.yml
```

---

## Build from source

### Prerequisites

Three things come from the system: jsoncpp and libuuid, which Drogon links
against, and RocksDB, which holds the metadata. Everything else is fetched and
pinned by CMake.

Drogon is always built from source, even when one is installed. It is the one
dependency we patch — its request parser refuses the zero-length body that every
S3 client sends when writing an empty object, so an unpatched build cannot store
one (`backend/cmake/patches/`). `-DMONOBUCKET_ALLOW_SYSTEM_DROGON=ON` uses an
installed copy instead and warns about what that costs.

RocksDB is not vendored deliberately — it is a ~40 MB static archive that pulls
in gflags, snappy, lz4, zstd and bz2, and building it from source would dominate
both the image build and the toolchain surface.

```bash
# Debian / Ubuntu
sudo apt install -y build-essential cmake ninja-build git \
                    libjsoncpp-dev uuid-dev libssl-dev zlib1g-dev \
                    libbrotli-dev libc-ares-dev librocksdb-dev

# Alpine
apk add build-base cmake ninja git jsoncpp-dev util-linux-dev \
        openssl-dev zlib-dev brotli-dev c-ares-dev rocksdb-dev

# Fedora / RHEL
sudo dnf install gcc-c++ cmake ninja-build git jsoncpp-devel libuuid-devel \
                 openssl-devel zlib-devel brotli-devel c-ares-devel rocksdb-devel

# macOS
brew install cmake ninja jsoncpp ossp-uuid openssl brotli c-ares rocksdb
```

CMake 3.25+ and a compiler with C++20 support (GCC 12+, Clang 15+) are required.

### Configure, build, test

```bash
cmake --preset dev          # first run also fetches Drogon; expect a few minutes
cmake --build --preset dev
ctest --preset dev
```

Other presets: `asan` (AddressSanitizer + UBSan), `release` (LTO, dashboard
embedded), `release-redis` (adds the Redis cache backend), `dev-redis` (debug
build with the Redis backend, for the integration suite).

The Redis integration tests need a real server and are skipped without one:

```bash
docker run -d --rm -p 63790:6379 redis:7-alpine
MONOBUCKET_TEST_REDIS_URL=redis://127.0.0.1:63790/0 ctest --preset dev-redis
```

### Run

```bash
export MONOBUCKET_DATA_DIR=$PWD/data
export MONOBUCKET_ROOT_SECRET_KEY=local-dev-secret
./build/dev/bin/monobucket
```

```bash
./build/dev/bin/monobucket --help           # every setting, with defaults
./build/dev/bin/monobucket --print-config   # the resolved configuration as JSON
curl -s localhost:9000/healthz
curl -s localhost:9000/metrics
```

---

## Frontend

The dashboard lives in `frontend/` (SvelteKit 2 + Svelte 5, TypeScript, pnpm)
and is compiled into the binary as a static asset table.

```bash
cd frontend
pnpm install
pnpm dev                      # dev server on :5173, proxying to the C++ API
pnpm run build                # static output in frontend/build/
```

Building the backend with the dashboard embedded:

```bash
cd frontend && pnpm install --frozen-lockfile && pnpm run build && cd ..
cmake --preset release
cmake --build --preset release
```

Without `MONOBUCKET_EMBED_FRONTEND=ON` the binary compiles with an empty asset
table and the console port returns 404 — useful while working on the engine.

### How it was scaffolded

```bash
npx sv create frontend        # SvelteKit, TypeScript
cd frontend
pnpm add -D @sveltejs/adapter-static
```

This SvelteKit version carries its Kit config in `vite.config.ts` rather than a
separate `svelte.config.js`. The adapter is configured for SPA fallback:

```ts
adapter: adapter({
  pages: 'build',
  assets: 'build',
  fallback: 'index.html',
  precompress: false
})
```

and `src/routes/+layout.ts` turns off SSR and prerendering, because the binary
has no Node runtime beside it:

```ts
export const ssr = false;
export const prerender = false;
```

---

## Docker

```bash
docker build -t monobucket:dev .
docker run --rm -p 9000:9000 -p 9001:9001 \
  -e MONOBUCKET_ROOT_ACCESS_KEY=monobucket \
  -e MONOBUCKET_ROOT_SECRET_KEY=change-me-please \
  -v monobucket-data:/data \
  monobucket:dev
```

Or with compose:

```bash
cp .env.example .env      # edit the root credentials first
docker compose up --build
docker compose --profile redis up --build    # with the Redis cache backend
```

| Port | Purpose |
| --- | --- |
| 9000 | S3 REST API |
| 9001 | Admin dashboard |

---

## Configuration

Everything is read from the environment at startup; nothing is hot-reloaded.
See [.env.example](.env.example) for the annotated list, or run
`monobucket --help`.

Size values accept unit suffixes. Binary units (`K`, `Ki`, `KiB`, `M`, `MiB`,
`G`, `GiB`) are powers of 1024; SI units (`KB`, `MB`, `GB`) are powers of 1000.
A bare number is bytes.

The settings that govern memory behaviour:

- `MONOBUCKET_MAX_MEMORY_BODY_BYTES` — request bodies larger than this spill to
  disk instead of being buffered in RAM.
- `MONOBUCKET_STREAM_CHUNK_BYTES` — the fixed chunk size used by the streaming
  I/O engine, which is what keeps resident memory flat during multi-gigabyte
  transfers.
- `MONOBUCKET_METADATA_MEMORY_BYTES` — one budget covering RocksDB's block
  cache, its write buffers and its index/filter blocks together. Sized
  separately, as RocksDB does by default, the sum is what the container sees.
- `MONOBUCKET_IO_QUEUE_LIMIT` — how much storage work may queue before requests
  are refused. Bounded deliberately; an unbounded queue turns a slow disk into
  unbounded memory.
- `MONOBUCKET_CACHE_MAX_BYTES` — the cache budget, counted as stored bytes plus
  per-entry overhead and enforced on every insert rather than trimmed on a
  timer. `0` disables caching.

---

## Cache

One `CacheProvider` interface, selected by `MONOBUCKET_CACHE_BACKEND`.

**`memory`** (default) is a sharded LRU. Each shard has its own map, intrusive
list and `shared_mutex`, and holds an equal slice of the budget. Reads take the
shared lock and deliberately do *not* reorder the list — promoting on every read
would make `get()` a writer and reduce the `shared_mutex` to decoration. A hit
sets an atomic flag instead, and eviction gives a flagged entry one reprieve
before taking it. Recency is approximate; concurrency is real.

**`redis`** (optional, `-DMONOBUCKET_ENABLE_REDIS=ON`) is a shared tier for
several instances, and is arranged as two tiers rather than an either/or: the
local cache sits in front of it and is written on every `set`, so it is already
warm if Redis goes away. After a few consecutive failures a circuit breaker
stops calling Redis entirely, backing off exponentially and letting a single
probe through per window — retrying a dead socket on every request would satisfy
"the cache never fails" while still stalling every request.

The cost of the local tier is coherence: another instance can change a value
this process has already copied. `MONOBUCKET_CACHE_LOCAL_TTL_SECONDS` (default
5) caps how long that can go unnoticed, and applies even to entries stored with
no expiry.

A cache outage is never a storage outage. `/readyz` reports the backend and its
health, but a degraded cache does not take the container out of rotation.

---

## Storage layout

Everything lives under `MONOBUCKET_DATA_DIR`:

```
/data
├── meta/      RocksDB: buckets, objects, multipart uploads, reclamation log
├── objects/   payloads, sharded as <aa>/<bb>/<blobId>
└── tmp/       in-flight writes, linked into objects/ only once durable
```

Two rules make the pair recoverable. A payload is written and flushed **before**
the metadata naming it is committed, and unlinked only **after** that metadata
is gone — so an interruption leaves either the old state or the new one. And a
payload is registered in the reclamation log before it is written, so a crash at
any point leaves a trace that startup can collect in time proportional to what
leaked rather than to what is stored.

`MONOBUCKET_DURABILITY` decides how hard the first of those rules pushes:
`relaxed` (the default) survives a process crash, `strict` survives a power cut
and costs an fsync per commit, `none` is for CI.

The on-disk format carries its own version, independent of the release version.
A binary that does not recognise it refuses to open the directory rather than
misreading it.

---

## Endpoints available today

System routes, on both listeners unless noted:

| Endpoint | Listener | Purpose |
| --- | --- | --- |
| `GET /healthz` | both | Liveness |
| `GET /readyz` | both | Readiness |
| `GET /metrics` | both | Prometheus text format |
| `GET /_mb/version` | both | Version and build info |
| `GET /_mb/config` | console | Resolved configuration, secrets redacted |

The S3 API, on port 9000:

| Operation | Request |
| --- | --- |
| ListBuckets | `GET /` |
| CreateBucket / DeleteBucket / HeadBucket | `PUT` / `DELETE` / `HEAD` `/{bucket}` |
| ListObjects, ListObjectsV2 | `GET /{bucket}[?list-type=2]`, with `prefix`, `delimiter`, `max-keys`, `continuation-token` |
| GetObject, HeadObject | `GET` / `HEAD` `/{bucket}/{key}`, with `Range` and conditional headers |
| PutObject | `PUT /{bucket}/{key}` |
| DeleteObject, DeleteObjects | `DELETE /{bucket}/{key}`, `POST /{bucket}?delete` |
| Multipart | `?uploads`, `?uploadId=`, `?partNumber=` — create, upload, list, complete, abort |
| Bucket policy / ACL / location / versioning | `GET`, `PUT`, `DELETE` on `/{bucket}?policy`, `?acl`, `?location`, `?versioning` |

Authentication is SigV4, in both header and presigned-query form, including
`aws-chunked` streaming signatures. Path-style addressing always works;
virtual-host style (`bucket.s3.example.com`) requires `MONOBUCKET_S3_DOMAIN`.
Anonymous reads are served only for a bucket whose policy grants them.

`CopyObject` and object versioning are not implemented and answer `501`.

---

## Versioning

CalVer, `YYYY.0M.MICRO`. See [CHANGELOG.md](CHANGELOG.md) for the full scheme,
the Docker tag set and the release procedure.

## Contributing

Tick the box in `ROADMAP.md` and add a `CHANGELOG.md` entry in the same commit
as the work. Run `ctest --preset dev` before pushing.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).
