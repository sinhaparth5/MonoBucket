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

TEST_CASE("a pre-compressed variant is served only to a client that accepts it",
          "[assets]") {
    static constexpr unsigned char kRaw[]    = {'h', 'e', 'l', 'l', 'o'};
    static constexpr unsigned char kGzip[]   = {0x1f, 0x8b, 0x00};
    static constexpr unsigned char kBrotli[] = {0xce, 0xb2};

    assets::Asset asset{"/app.js", "text/javascript", kRaw, sizeof(kRaw)};
    asset.gzip       = kGzip;
    asset.gzipSize   = sizeof(kGzip);
    asset.brotli     = kBrotli;
    asset.brotliSize = sizeof(kBrotli);

    // Brotli wins where both are on offer: it is smaller, and the decode cost
    // belongs to the client.
    const auto both = assets::encodedFor(asset, "gzip, deflate, br");
    CHECK(both.encoding == "br");
    CHECK(both.data == kBrotli);

    const auto gzipOnly = assets::encodedFor(asset, "gzip, deflate");
    CHECK(gzipOnly.encoding == "gzip");
    CHECK(gzipOnly.size == sizeof(kGzip));

    // No header at all is the case that matters: an ancient client, a health
    // probe or a curl one-liner must get bytes it can read, not a 200 full of
    // deflate stream.
    const auto identity = assets::encodedFor(asset, "");
    CHECK(identity.encoding.empty());
    CHECK(identity.data == kRaw);
    CHECK(identity.size == sizeof(kRaw));

    // An asset the generator refused to compress — a font, a PNG — has no
    // variants, and asking for one must not hand back a null pointer.
    const assets::Asset plain{"/font.woff2", "font/woff2", kRaw, sizeof(kRaw)};
    const auto          served = assets::encodedFor(plain, "gzip, br");
    CHECK(served.encoding.empty());
    CHECK(served.data == kRaw);
}
