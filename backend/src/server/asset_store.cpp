#include "server/asset_store.hpp"

#include <algorithm>
#include <numeric>

namespace monobucket::assets {

bool embedded() noexcept { return tableSize() > 0; }

const Asset* find(std::string_view path) noexcept {
    const Asset* const first = tableData();
    const std::size_t count = tableSize();
    if (first == nullptr || count == 0) return nullptr;

    // The generator sorts entries by path, so a binary search is valid.
    const Asset* const last = first + count;
    const auto hit = std::lower_bound(first, last, path,
                                      [](const Asset& a, std::string_view p) { return a.path < p; });
    if (hit == last || hit->path != path) return nullptr;
    return hit;
}

const Asset* indexDocument() noexcept {
    if (const Asset* root = find("/index.html")) return root;
    // adapter-static can be configured with a different fallback name.
    return find("/200.html");
}

std::size_t totalBytes() noexcept {
    const Asset* const first = tableData();
    const std::size_t count = tableSize();
    if (first == nullptr) return 0;
    return std::accumulate(first, first + count, std::size_t{0}, [](std::size_t sum, const Asset& a) {
        return sum + a.size + a.gzipSize + a.brotliSize;
    });
}

Encoded encodedFor(const Asset& asset, std::string_view acceptEncoding) noexcept {
    const auto accepts = [acceptEncoding](std::string_view token) {
        return acceptEncoding.find(token) != std::string_view::npos;
    };

    if (asset.brotli != nullptr && accepts("br")) {
        return {asset.brotli, asset.brotliSize, "br"};
    }
    if (asset.gzip != nullptr && accepts("gzip")) {
        return {asset.gzip, asset.gzipSize, "gzip"};
    }
    return {asset.data, asset.size, {}};
}

}  // namespace monobucket::assets
