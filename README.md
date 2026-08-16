# MonoBucket

Single-binary, low-memory, S3-compatible object storage written in C++20, with a
SvelteKit administration dashboard compiled into the executable.

One binary. One container. No sidecars, no external database.

> **Status: pre-alpha.** Phase 1 (scaffolding, event loop, configuration,
> lifecycle) and the Phase 5 embedding pipeline are in place. The storage
> engine and the S3 protocol layer are not implemented yet — see
> [ROADMAP.md](ROADMAP.md). Do not put data in it.

---

## Layout

```
MonoBucket/
├── backend/            C++ engine
│   ├── cmake/          dependency resolution + frontend embedding
│   ├── src/
│   │   ├── core/       config, env parsing, logging, lifecycle
│   │   ├── server/     event loop, listeners, system routes, asset store
│   │   ├── storage/    Phase 2 — metadata store + POSIX I/O engine
│   │   ├── cache/      Phase 3 — CacheProvider, LRU, Redis
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
embedded), `release-redis` (adds the Redis cache backend).

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

| Endpoint | Listener | Purpose |
| --- | --- | --- |
| `GET /healthz` | both | Liveness |
| `GET /readyz` | both | Readiness |
| `GET /metrics` | both | Prometheus text format |
| `GET /_mb/version` | both | Version and build info |
| `GET /_mb/config` | console | Resolved configuration, secrets redacted |

The S3 API itself arrives in Phase 4.

---

## Versioning

CalVer, `YYYY.0M.MICRO`. See [CHANGELOG.md](CHANGELOG.md) for the full scheme,
the Docker tag set and the release procedure.

## Contributing

Tick the box in `ROADMAP.md` and add a `CHANGELOG.md` entry in the same commit
as the work. Run `ctest --preset dev` before pushing.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).
