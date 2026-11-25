#include <climits>

#include "test.hpp"
#include "world/consts.hpp"
#include "world/save/chunk_codec.hpp"
#include "world/save/sky_codec.hpp"
#include "world/sky_column.hpp"
#include "world/structure_overrides.hpp"

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
    // A receding river leaves a partly filled top cell.
    {
        auto in = flat(420);
        in.opacity = &table;
        in.water[0] = {423, 0, 1, {1, 0}, false, 96};
        const auto t = sky::build(in);
        EQUAL(int(t.direct(0, 421)), 11, "partial river top");
        EQUAL(int(t.direct(0, 420)), 3, "river bed after drawdown");
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

TEST(sky_shape_transmission_and_overrides) {
    using enum TileShapeProfile;
    const auto table = opacities();
    auto in = flat(400);
    in.opacity = &table;
    auto edit = std::make_shared<ColumnEdit>();
    edit->groundY.fill(400);
    edit->groundBlock.fill(1);
    edit->waterY.fill(-1);
    const auto wall = TileShape::make(Wall10);
    const auto slab = TileShape::make(Slab);
    const auto ramp = TileShape::make(Ramp);
    edit->groundShape[5] = wall;
    Chunk roof;
    constexpr int Y = 800;
    for (const auto& [x, shape] :
         {std::pair{1, slab}, std::pair{2, ramp}, std::pair{3, wall},
          std::pair{4, wall.rotated(1)}}) {
        ::Tile tile;
        tile.block_id = x == 4 ? 2 : 1;
        roof.setCell(Chunk::cell(x, Y % Chunk::SIZE_Y, 0), tile, shape);
    }
    edit->structure.emplace_back(
        Y / Chunk::SIZE_Y,
        std::make_shared<const Bytes>(chunk_codec::encode(roof)));
    edit->finish();
    in.edit = edit;
    const auto tiles = sky::build(in);
    EQUAL(int(tiles.direct(0, Y - 1)), 15, "empty control");
    EQUAL(int(tiles.direct(1, Y - 1)), 0, "roof slab blocks overhead light");
    EQUAL(int(tiles.direct(2, Y)), 15, "ramp top receives overhead light");
    EQUAL(int(tiles.direct(3, Y - 1)), 13, "thin wall subtracts two levels");
    EQUAL(int(tiles.direct(4, Y - 1)), 15, "thin glass wall rounds down");
    EQUAL(int(tiles.direct(5, 399)), 13,
          "shaped ground projects its footprint");
    CHECK(tiles.column(1).cellAt(Y).shape == slab);
    CHECK(tiles.column(4).cellAt(Y).shape == wall.rotated(1));
    sky::Overrides overrides;
    for (int i = 1; i <= 5; ++i) overrides[1][i] = tiles.column(i);
    CHECK(sky_codec::decode(sky_codec::encode(overrides)).overrides ==
          overrides);

    // The v1 payloads carried no profile codes.
    BinaryWriter legacy;
    legacy.write(uint32_t(1));
    legacy.write(uint32_t(0));
    legacy.write(uint32_t(1));
    legacy.write(uint16_t(1));
    legacy.write(uint8_t(0));
    legacy.write(int16_t(500));
    legacy.write(uint8_t(0));
    legacy.write(uint8_t(1));
    legacy.write(int16_t(600));
    legacy.write(int16_t(600));
    legacy.write(uint8_t(3));
    SectionContainer container;
    container.put("SKYO", 1, legacy.take());
    const auto old =
        sky_codec::decode(container.encode()).overrides.at(1).at(0);
    CHECK(!old.cellAt(500).shape.code() && !old.cellAt(600).shape.code());
    EQUAL(int(old.direct(599)), 12, "legacy cube opacity");
}

TEST(compound_sky_and_save) {
    using namespace structure_voxels;
    using tile_geometry::LayerThickness;
    using tile_geometry::TilePiece;
    constexpr uint64_t Key = 19;
    Override layout;
    layout.mode = OverrideMode::Replace;
    layout.anchorX = 8;
    layout.anchorZ = 8;
    layout.landmark = true;
    const auto wall = TileShape::make(TileShapeProfile::Wall25);
    OverrideVoxel roof(Stone);
    roof.pieces.push_back({{1,
                            Stone,
                            wall,
                            {0, .75f, 0},
                            tile_geometry::PieceFill::BelowProfile,
                            .25f},
                           {PieceOrigin::Structure, Key}});
    layout.voxels[{8, 443, 8}] = roof;
    const auto generated =
        applyOverrides({}, {{"compound_test", {{Key, layout}}}});
    Column sources(0, 0);
    generated[0].addTo(sources, {});
    sources.orderByKey();
    auto inputs = flat(420);
    const auto table = opacities();
    inputs.opacity = &table;
    inputs.structures = &sources;
    inputs.structureTop = sources.skyTops();
    const auto sky = sky::build(inputs);
    const auto column = sky.column(sky::Tile::index(8, 8));
    EQUAL(column.top(), 443, "generated roof top");
    EQUAL(int(column.direct(442)), 11, "sun through the 75% open footprint");
    const auto roofCell = column.cellAt(443);
    EQUAL(int(voxel_light::faceLoss({}, roofCell, Face::PosY)), 0,
          "opening below raised timber");
    EQUAL(int(voxel_light::faceLoss({}, roofCell, Face::NegY)), 4,
          "roof at the upper boundary");
    const auto loaded =
        sky_codec::decode(sky_codec::encode({{3, {{10, column}}}}));
    CHECK(loaded.overrides.at(3).at(10) == column);

    Chunk exact;
    const auto slab = sources.rasterize(443 / 16);
    CHECK(slab && stamp(exact, *slab) == 1);
    const auto stamped = exact.solidCellAt(Chunk::cell(8, 443 % 16, 8));
    const auto exactCell =
        voxel_light::Cell(voxel_light::coverPieces(stamped, table));
    CHECK(exactCell == roofCell);

    const auto full =
        TilePiece::layer(1, 15, LayerThickness::Cm25, {0, .75f, 0});
    const auto overlappingFootprint =
        TilePiece::layer(2, 15, LayerThickness::Cm25, {0, .25f, 0});
    const auto layers = std::array{full, overlappingFootprint};
    const auto closed = voxel_light::Cell(voxel_light::coverPieces(layers));
    EQUAL(int(voxel_light::verticalLoss(closed)), 15,
          "projected union counts once");
    CHECK(!closed.solidOpaque());
}
