#include "world_fixture.hpp"

namespace {
const std::vector<RegionLayer> layers = {
    {RegionLayerType::Rock, 2, 2, 0, 0, 1},
    {RegionLayerType::Rock, 1, 1024, 2, 0, 1}};

void expectColumn(const Chunk& chunk, int localY, int block, LiquidType liquid,
                  int level) {
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int x = 0; x < Chunk::SIZE_X; ++x) {
            const Tile& tile = chunk.tiles[z][localY][x];
            const auto context = "tile (" + std::to_string(x) + "," +
                                 std::to_string(localY) + "," +
                                 std::to_string(z) + ")";
            EQUAL(int(tile.block_id), block, context + " block");
            EQUAL(int(tile.liquid), int(liquid), context + " liquid");
            EQUAL(int(tile.liquidLevel), level, context + " liquid level");
        }
}
}  // namespace

TEST(tile_classification) {
    Tile tile;
    CHECK(tile.isAir());
    CHECK(!tile.isSolid());
    CHECK(!tile.isLiquid());
    CHECK(!tile.isVisible());
    tile.liquid = LiquidType::Water;
    tile.liquidLevel = 255;
    CHECK(!tile.isAir());
    CHECK(!tile.isSolid());
    CHECK(tile.isLiquid());
    tile.liquid = LiquidType::Lava;
    CHECK(tile.isLiquid());
    tile.block_id = 1;
    CHECK(tile.isSolid());
    CHECK(tile.isVisible());
    CHECK(!tile.isAir());
}

TEST(generation_surface_layers) {
    auto region = test::regionAt(SEA_LEVEL_Y + 5);
    Chunk chunk;
    LocationGenerator{}.generateChunk(chunk, {0, 24, 0}, {}, region, layers);
    expectColumn(chunk, 6, 0, LiquidType::None, 0);
    expectColumn(chunk, 5, 2, LiquidType::None, 0);
    expectColumn(chunk, 4, 2, LiquidType::None, 0);
    expectColumn(chunk, 3, 1, LiquidType::None, 0);
    expectColumn(chunk, 0, 1, LiquidType::None, 0);
}

TEST(generation_sea_level) {
    auto region = test::regionAt(SEA_LEVEL_Y - 2);
    Chunk below, above;
    LocationGenerator gen;
    gen.generateChunk(below, {0, 23, 0}, {}, region, layers);
    gen.generateChunk(above, {0, 24, 0}, {}, region, layers);
    expectColumn(below, 14, 2, LiquidType::None, 0);
    expectColumn(below, 15, 0, LiquidType::Water, 255);
    expectColumn(above, 0, 0, LiquidType::Water, 255);
    expectColumn(above, 1, 0, LiquidType::None, 0);
}

TEST(generation_chunk_seam) {
    auto region = test::regionAt(SEA_LEVEL_Y + 16);
    Chunk lower, upper;
    LocationGenerator gen;
    gen.generateChunk(lower, {9, 24, 12}, {8, 0, 8}, region, layers);
    gen.generateChunk(upper, {9, 25, 12}, {8, 0, 8}, region, layers);
    expectColumn(lower, 14, 1, LiquidType::None, 0);
    expectColumn(lower, 15, 2, LiquidType::None, 0);
    expectColumn(upper, 0, 2, LiquidType::None, 0);
    expectColumn(upper, 1, 0, LiquidType::None, 0);
}

TEST(generation_reused_chunk) {
    Chunk chunk;
    test::fill(chunk, 3);
    chunk.setCell(Chunk::cell(3, 1, 1), chunk.tiles[1][1][3],
                  TileShape::make(TileShapeProfile::Ramp));
    CHECK(chunk.hasShapes());
    chunk.recordRoot(Chunk::cell(3, 1, 1), 18, chunk.tiles[1][1][3]);
    for (auto& plane : chunk.tiles)
        for (auto& row : plane)
            for (auto& tile : row) {
                tile.liquid = LiquidType::Lava;
                tile.liquidLevel = 128;
            }
    auto region = test::regionAt(SEA_LEVEL_Y + 5);
    LocationGenerator{}.generateChunk(chunk, {0, 24, 0}, {}, region, layers);
    CHECK(!chunk.hasShapes() && !chunk.rootTerrain);
    expectColumn(chunk, 3, 1, LiquidType::None, 0);
    expectColumn(chunk, 7, 0, LiquidType::None, 0);
}

TEST(generation_repeatable) {
    auto region = test::regionAt(SEA_LEVEL_Y + 5);
    Chunk a, b;
    LocationGenerator gen;
    gen.generateChunk(a, {3, 24, 4}, {}, region, layers);
    gen.generateChunk(b, {3, 24, 4}, {}, region, layers);
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int y = 0; y < Chunk::SIZE_Y; ++y)
            for (int x = 0; x < Chunk::SIZE_X; ++x) {
                const auto& first = a.tiles[z][y][x];
                const auto& second = b.tiles[z][y][x];
                CHECK(first.block_id == second.block_id);
                CHECK(first.liquid == second.liquid);
                CHECK(first.liquidLevel == second.liquidLevel);
            }
}

TEST(layers_cover_depth) {
    test::Assets assets;
    WorldGenSettings settings{};
    settings.feature_seed = 42;
    auto region = test::regionAt(389);
    LayersGenerator gen(settings, &assets);
    auto generated = gen.get(region);
    int depth = 0;
    for (const auto& layer : generated) {
        CHECK(layer.thickness_m > 0);
        EQUAL(layer.depth_from_surface_m, depth, "contiguous rock layer");
        EQUAL(int(layer.block_id), 1, "fixture base rock block");
        depth += layer.thickness_m;
    }
    EQUAL(depth, region.land_height_m, "seed 42: covered depth");
}

TEST(layers_depth_limit) {
    test::Assets assets;
    WorldGenSettings settings{};
    settings.feature_seed = 42;
    LayersGenerator gen(settings, &assets);
    const auto generated = gen.get(test::regionAt(389), 20);
    EQUAL(generated.size(), 1u, "limited layer count");
    EQUAL(generated.front().thickness_m, 20, "limited thickness");
    EQUAL(generated.front().depth_from_surface_m, 0, "surface layer");
}

TEST(layers_missing_rock) {
    test::Assets assets;
    WorldGenSettings settings{};
    auto region = test::regionAt(389);
    region.crust = CrustType::OCEANIC;
    LayersGenerator gen(settings, &assets);
    bool threw = false;
    try {
        gen.get(region);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(generation_feature_ice_and_snow_blocks) {
    auto region = test::regionAt(SEA_LEVEL_Y - 2);
    region.addFeature(FeatureMask::SEA_ICE);
    ChunkColumnInputs columns;
    for (auto& column : columns.columns) {
        column.surface_y = SEA_LEVEL_Y - 10;
        column.water = WaterSurface{SEA_LEVEL_Y + 1};
    }
    Chunk chunk;
    const unsigned cy = SEA_LEVEL_Y / Chunk::SIZE_Y;
    LocationGenerator{}.generateChunk(chunk, {0, cy, 0}, {}, region, layers, {},
                                      &columns, 7, 8);
    const int iceY = SEA_LEVEL_Y - int(cy) * Chunk::SIZE_Y;
    const int snowY = SEA_LEVEL_Y + 1 - int(cy) * Chunk::SIZE_Y;
    CHECK(chunk.tiles[0][iceY][0].block_id == 7);
    CHECK(chunk.tiles[0][iceY][0].liquid == LiquidType::None);
    CHECK(chunk.tiles[0][snowY][0].block_id == 8);
}
