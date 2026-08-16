# syntax=docker/dockerfile:1.7
# ---------------------------------------------------------------------------
# MonoBucket — three-stage build producing one binary in a minimal runtime.
#
#   1. frontend : Node builds the SvelteKit dashboard to static files
#   2. backend  : Alpine toolchain compiles the C++ engine, embedding (1)
#   3. runtime  : the stripped binary plus only the shared libraries it needs
#
# Build:
#   docker build -t monobucket:dev .
# ---------------------------------------------------------------------------

# --- Stage 1: dashboard ----------------------------------------------------
FROM node:22-alpine AS frontend

WORKDIR /src
COPY . .

# Playwright is a devDependency of the vitest browser tests and would pull
# ~150 MB of browsers this stage never runs. pnpm's `allowBuilds` in
# pnpm-workspace.yaml already blocks the postinstall; this is belt and braces.
ENV PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1

# The dashboard is optional during early phases: if frontend/ has not been
# scaffolded yet, emit a placeholder so the backend stage still has something
# to embed and the image stays buildable.
RUN set -eu; \
    if [ ! -f frontend/package.json ]; then \
        mkdir -p frontend/build; \
        printf '<!doctype html><meta charset="utf-8"><title>MonoBucket</title><p>Dashboard not built.\n' \
            > frontend/build/index.html; \
    else \
        npm install -g pnpm@11; \
        cd frontend && pnpm install --frozen-lockfile && pnpm run build; \
    fi

# adapter-static must have produced an SPA entry point, otherwise the console
# would ship as an empty asset table and 404 at runtime.
RUN test -f frontend/build/index.html

# --- Stage 2: engine -------------------------------------------------------
FROM alpine:3.21 AS backend

RUN apk add --no-cache \
        build-base \
        cmake \
        ninja \
        git \
        linux-headers \
        openssl-dev \
        zlib-dev \
        brotli-dev \
        jsoncpp-dev \
        util-linux-dev \
        c-ares-dev

WORKDIR /src
COPY . .
COPY --from=frontend /src/frontend/build ./frontend/build

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DMONOBUCKET_BUILD_TESTS=OFF \
        -DMONOBUCKET_EMBED_FRONTEND=ON \
        -DMONOBUCKET_ENABLE_LTO=ON \
        -DMONOBUCKET_STATIC_LINK=ON \
 && cmake --build build --parallel "$(nproc)" \
 && strip --strip-all build/bin/monobucket \
 && build/bin/monobucket --version

# --- Stage 3: runtime ------------------------------------------------------
FROM alpine:3.21 AS runtime

ARG MONOBUCKET_VERSION=dev
ARG VCS_REF=unknown
ARG BUILD_DATE=unknown

LABEL org.opencontainers.image.title="MonoBucket" \
      org.opencontainers.image.description="Single-binary S3-compatible object storage with an embedded admin dashboard" \
      org.opencontainers.image.version="${MONOBUCKET_VERSION}" \
      org.opencontainers.image.revision="${VCS_REF}" \
      org.opencontainers.image.created="${BUILD_DATE}" \
      org.opencontainers.image.licenses="GPL-3.0-or-later" \
      org.opencontainers.image.source="https://github.com/sinhaparth5/MonoBucket"

RUN apk add --no-cache \
        libstdc++ \
        openssl \
        jsoncpp \
        brotli-libs \
        c-ares \
        libuuid \
 && addgroup -S -g 1000 monobucket \
 && adduser  -S -u 1000 -G monobucket -H -h /data monobucket \
 && mkdir -p /data \
 && chown monobucket:monobucket /data

COPY --from=backend /src/build/bin/monobucket /usr/local/bin/monobucket

ENV MONOBUCKET_DATA_DIR=/data \
    MONOBUCKET_HOST=0.0.0.0 \
    MONOBUCKET_PORT=9000 \
    MONOBUCKET_CONSOLE_PORT=9001 \
    MONOBUCKET_LOG_LEVEL=info

VOLUME ["/data"]
EXPOSE 9000 9001
USER monobucket

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD wget -q -O- "http://127.0.0.1:${MONOBUCKET_PORT}/healthz" >/dev/null || exit 1

ENTRYPOINT ["/usr/local/bin/monobucket"]
