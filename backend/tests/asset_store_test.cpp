#include <catch2/catch_test_macros.hpp>

#include "server/asset_store.hpp"

namespace assets = monobucket::assets;

TEST_CASE("the asset table is consistent with itself", "[assets]") {
    const std::size_t count = assets::tableSize();

    if (count == 0) {
        // Default build: no frontend embedded. Every accessor must still be
        // safe to call — the console routes rely on this to stay unregistered.
        CHECK_FALSE(assets::embedded());
        CHECK(assets::tableData() == nullptr);
        CHECK(assets::find("/index.html") == nullptr);
        CHECK(assets::indexDocument() == nullptr);
        CHECK(assets::totalBytes() == 0);
        return;
    }

    CHECK(assets::embedded());
    CHECK(assets::tableData() != nullptr);
    CHECK(assets::totalBytes() > 0);

    const assets::Asset* const first = assets::tableData();
    for (std::size_t i = 0; i < count; ++i) {
        const auto& asset = first[i];
        INFO("asset " << i << ": " << asset.path);
        CHECK(asset.path.starts_with("/"));
        CHECK_FALSE(asset.mime.empty());
        CHECK(asset.data != nullptr);

        // find() binary-searches, which is only valid on a sorted table.
        if (i > 0) CHECK(first[i - 1].path < asset.path);
        CHECK(assets::find(asset.path) == &asset);
    }

    CHECK(assets::indexDocument() != nullptr);
    CHECK(assets::find("/definitely-not-an-asset") == nullptr);
}
