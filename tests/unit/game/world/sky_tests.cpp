#include <climits>

#include "test.hpp"
#include "world/consts.hpp"
#include "world/save/chunk_codec.hpp"
#include "world/save/sky_codec.hpp"
#include "world/sky_column.hpp"

// Sky heightmap columns (docs/handoff/sky-light.md).

namespace {
constexpr uint8_t Stone = 1, Glass = 2;

sky::OpacityTable opacities() {
    sky::OpacityTable table{};
    table[Stone] = 15;
    table[Glass] = 3;
    return table;
}

// Direct light by the sweep's rule over explicit tile opacities (index y).
uint8_t sweep(const std::vector<int>& opacity, int y) {
    int light = 15;
    for (int above = int(opacity.size()) - 1; above > y; --above)
        light = std::max(0, light - opacity[size_t(above)]);
    return uint8_t(light);
}

size_t mismatches(const sky::Column& column, const std::vector<int>& tiles) {
    size_t wrong = 0;
    for (int y = 0; y < int(tiles.size()); ++y)
        wrong += column.direct(y) != sweep(tiles, y);
    return wrong;
}

// A column edit with one structure slab holding `blocks` at (x, z) = (0, 0).
std::shared_ptr<ColumnEdit> edit(
    int ground, uint8_t groundBlock, int water,
    const std::vector<std::pair<int, uint8_t>>& blocks) {
    auto e = std::make_shared<ColumnEdit>();
    e->groundY.fill(int16_t(ground));
    e->groundBlock.fill(groundBlock);
    e->waterY.fill(int16_t(water));
    std::map<int, Chunk> slabs;
    for (const auto& [y, block] : blocks)
        slabs[y / 16].tiles[0][y % 16][0].block_id = block;
    for (const auto& [slab, chunk] : slabs)
        e->structure.emplace_back(
            uint16_t(slab),
            std::make_shared<const Bytes>(chunk_codec::encode(chunk)));
    e->finish();
    return e;
}

sky::TileInputs flat(int surface) {
    sky::TileInputs in;
    in.surface.fill(surface);
    in.water.fill(WaterSurface::sea(surface));
    in.structureTop.fill(INT_MIN);
    return in;
}
}  // namespace

TEST(sky_column_scan_and_direct) {
    // Tiles: stone up to 400, glass at 410, two water tiles at 405-406,
    // glass 420-422.
    std::vector<int> tiles(WORLD_HEIGHT_Y, 0);
    for (int y = 0; y <= 400; ++y) tiles[size_t(y)] = 15;
    tiles[405] = tiles[406] = 8;
    tiles[410] = 3;
    for (int y = 420; y <= 422; ++y) tiles[size_t(y)] = 3;
    const auto column = sky::scanColumn(
        WORLD_HEIGHT_Y - 1, 0, [&](int y) { return tiles[size_t(y)]; });
    EQUAL(int(column.opaque), 400, "opaque top");
    EQUAL(column.spans.size(), size_t(3), "runs merge");
    EQUAL(column.top(), 422, "top");
    EQUAL(mismatches(column, tiles), size_t(0), "direct light");
    // More runs than kept: the floor goes up (darker, never brighter).
    std::vector<int> many(WORLD_HEIGHT_Y, 0);
    for (int i = 0; i < sky::MaxSpans + 3; ++i) many[size_t(600 + 2 * i)] = 2;
    const auto capped = sky::scanColumn(WORLD_HEIGHT_Y - 1, 0,
                                        [&](int y) { return many[size_t(y)]; });
    CHECK(capped.overflow);
    EQUAL(capped.spans.size(), size_t(sky::MaxSpans), "kept runs");
    for (int y = 0; y < WORLD_HEIGHT_Y; ++y)
        CHECK(capped.direct(y) <= sweep(many, y));
}

TEST(sky_tile_build_sources) {
    const auto table = opacities();
    // Plain ground.
    {
        auto in = flat(420);
        in.opacity = &table;
        const auto t = sky::build(in);
        EQUAL(t.top(0), 420, "ground");
        EQUAL(int(t.direct(0, 420)), 15, "surface lit");
        EQUAL(int(t.direct(0, 419)), 0, "below dark");
    }
    // Sea above ground below sea level.
    {
        auto in = flat(SEA_LEVEL_Y - 3);
        in.opacity = &table;
        const auto t = sky::build(in);
        std::vector<int> tiles(WORLD_HEIGHT_Y, 0);
        for (int y = 0; y <= SEA_LEVEL_Y - 3; ++y) tiles[size_t(y)] = 15;
        for (int y = SEA_LEVEL_Y - 2; y <= SEA_LEVEL_Y; ++y)
            tiles[size_t(y)] = 8;
        EQUAL(mismatches(t.column(0), tiles), size_t(0), "sea");
    }
    // Wood of a tree over the ground.
    {
        auto in = flat(420);
        in.opacity = &table;
        in.structureTop[5] = 470;
        const auto t = sky::build(in);
        EQUAL(t.top(5), 470, "wood top");
        EQUAL(int(t.direct(5, 469)), 0, "under the limb");
        EQUAL(t.top(4), 420, "next column");
        EQUAL(t.highest(), 470, "highest");
    }
    // Edits: a dug pit with a pond, a glass roof and a stone slab above.
    {
        auto in = flat(420);
        in.opacity = &table;
        in.edit =
            edit(412, Stone, 415,
                 {{430, Glass}, {431, Glass}, {440, Stone}, {450, Glass}});
        const auto t = sky::build(in);
        std::vector<int> tiles(WORLD_HEIGHT_Y, 0);
        for (int y = 0; y <= 412; ++y) tiles[size_t(y)] = 15;
        for (int y = 413; y <= 414; ++y) tiles[size_t(y)] = 8;
        tiles[430] = tiles[431] = tiles[450] = 3;
        tiles[440] = 15;
        // Column 0 holds the structure; the others only the pit and pond.
        const auto c = t.column(0);
        EQUAL(int(c.opaque), 440, "stone slab");
        EQUAL(mismatches(c, tiles), size_t(0), "edited column");
        tiles[430] = tiles[431] = tiles[450] = tiles[440] = 0;
        EQUAL(mismatches(t.column(1), tiles), size_t(0), "pit and pond");
    }
    // A glass floor dug down to: what lies below it counts as opaque.
    {
        auto in = flat(420);
        in.opacity = &table;
        in.edit = edit(410, Glass, -1, {});
        const auto c = sky::build(in).column(0);
        EQUAL(int(c.top()), 410, "glass floor");
        EQUAL(int(c.direct(410)), 15, "floor lit");
        EQUAL(int(c.direct(409)), 12, "below the glass");
        EQUAL(int(c.direct(408)), 0, "assumed opaque");
    }
}

TEST(sky_tile_overrides) {
    const auto table = opacities();
    auto in = flat(420);
    in.opacity = &table;
    auto t = sky::build(in);
    sky::Column hole;
    hole.opaque = 400;
    hole.spans.push_back({int16_t(430), int16_t(428), uint8_t(3)});
    t.set(17, hole);
    CHECK(t.column(17) == hole);
    EQUAL(t.top(16), 420, "neighbour kept");
    EQUAL(int(t.direct(17, 427)), 15 - 9, "under the run");
}

TEST(sky_codec_roundtrip) {
    sky::Overrides overrides;
    sky::Column a;
    a.opaque = 500;
    sky::Column b;
    b.opaque = sky::None;
    b.overflow = true;
    b.spans = {{int16_t(700), int16_t(690), uint8_t(2)},
               {int16_t(600), int16_t(600), uint8_t(8)}};
    overrides[(uint64_t(3) << 32) | 9][0] = a;
    overrides[(uint64_t(3) << 32) | 9][255] = b;
    overrides[(uint64_t(70) << 32) | 1][40] = a;
    const auto bytes = sky_codec::encode(overrides);
    const auto file = sky_codec::decode(bytes);
    CHECK(file.overrides == overrides);
    // Malformed: truncated data, a span above the world, unsorted spans.
    bool threw = false;
    try {
        sky_codec::decode(ByteView(bytes.data(), bytes.size() - 3));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    for (const auto& bad :
         {sky::Span{int16_t(WORLD_HEIGHT_Y), int16_t(10), uint8_t(2)},
          sky::Span{int16_t(10), int16_t(20), uint8_t(2)}}) {
        sky::Overrides broken;
        sky::Column c;
        c.spans = {bad};
        broken[1][0] = c;
        threw = false;
        try {
            sky_codec::decode(sky_codec::encode(broken));
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }
}
