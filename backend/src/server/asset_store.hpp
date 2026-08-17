#pragma once

#include <cstddef>
#include <string_view>

namespace monobucket::assets {

/// One file from the SvelteKit build, baked into the binary's rodata section.
///
/// A compressible asset carries its pre-compressed forms alongside the
/// original. Compressing on every request would spend CPU re-deriving bytes
/// that never change; compressing once at build time spends binary size
/// instead, and the generator only pays it where the result is actually
/// smaller — so an already-compressed font or PNG has no variants at all.
struct Asset {
    std::string_view     path;  ///< Request path, e.g. "/_app/immutable/x.js"
    std::string_view     mime;
    const unsigned char* data;
    std::size_t          size;

    /// nullptr when no variant was generated for this asset.
    const unsigned char* gzip     = nullptr;
    std::size_t          gzipSize = 0;
    const unsigned char* brotli     = nullptr;
    std::size_t          brotliSize = 0;
};

// --- Provided by the generated translation unit ----------------------------
const Asset* tableData() noexcept;
std::size_t  tableSize() noexcept;

// --- Lookup helpers --------------------------------------------------------

/// False when the binary was built without MONOBUCKET_EMBED_FRONTEND, in which
/// case the dashboard routes are not registered at all.
bool embedded() noexcept;

/// Exact-path lookup. Returns nullptr when the path is not an embedded asset.
const Asset* find(std::string_view path) noexcept;

/// SPA entry point served for any unmatched console route.
const Asset* indexDocument() noexcept;

/// Total embedded payload size — surfaced on /metrics to make the binary's
/// static footprint visible. Counts the compressed variants too, because they
/// are equally part of what the binary costs.
std::size_t totalBytes() noexcept;

/// What to send for one asset given a request's `Accept-Encoding`.
struct Encoded {
    const unsigned char* data;
    std::size_t          size;

    /// Empty when the identity encoding was chosen, in which case no
    /// `Content-Encoding` header should be added.
    std::string_view encoding;
};

/// Brotli is preferred over gzip where both were generated and both are
/// acceptable: it is smaller, and the decode cost is the client's.
///
/// The header is matched by substring rather than parsed. A `q=0` on a token
/// that is present would be read as acceptance, which is the one way this can
/// be wrong — and no browser sends one, whereas every browser sends the
/// tokens themselves.
Encoded encodedFor(const Asset& asset, std::string_view acceptEncoding) noexcept;

}  // namespace monobucket::assets
