#pragma once

#include <cstddef>
#include <string_view>

namespace monobucket::assets {

/// One file from the SvelteKit build, baked into the binary's rodata section.
struct Asset {
    std::string_view     path;  ///< Request path, e.g. "/_app/immutable/x.js"
    std::string_view     mime;
    const unsigned char* data;
    std::size_t          size;
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
/// static footprint visible.
std::size_t totalBytes() noexcept;

}  // namespace monobucket::assets
