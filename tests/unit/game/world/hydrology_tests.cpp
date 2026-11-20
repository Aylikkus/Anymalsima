#include <filesystem>

#include "test.hpp"
#include "test_assets.hpp"
#include "world/generated_hydrology.hpp"
#include "world/location_generator.hpp"
#include "world/save/save_catalog.hpp"
#include "world/surface_sampler.hpp"
#include "world/terrain_lod.hpp"

namespace {
constexpr int Width = 12;
constexpr int Height = 9;
constexpr int Side = TerrainTuning::RegionX;
constexpr int First = 4 * Width + 3;
constexpr int Second = 4 * Width + 8;
constexpr int Dry = 6 * Width + 6;
constexpr int Diagonal = 3 * Width + 1;
constexpr int WrappedCoast = 2 * Width + Width - 1;

World basinWorld() {
    World world{};
    world.width = Width;
    world.height = Height;
    world.genSettings = WorldGenSettings::fromSeed(2024);
    world.genSettings.width = Width;
    world.genSettings.height = Height;
    world.regions.resize(Width * Height);
    const auto set = [&](int i, int y, LandType land) {
        auto& region = world.regions[i];
        region = test::regionAt(y);
        region.coord = {unsigned(i % Width), unsigned(i / Width)};
        region.land = land;
    };
    for (int i = 0; i < Width * Height; ++i) set(i, 500, LandType::PLAINS);
    for (int z = 0; z <= 3; ++z) {
        set(z * Width + 3, 414 + z * 6, LandType::PLAINS);
        set(z * Width + 8, 452 + z * 6, LandType::PLAINS);
    }
    // The only low outlets are just north of the lake centers.
    set(First, 407, LandType::PLAINS);
    set(Second, 449, LandType::PLAINS);
    set(Dry, 350, LandType::PLAINS);
    set(Diagonal, 372, LandType::WATER);
    for (int z = 0; z <= 2; ++z) set(z * Width, 330, LandType::DEEP_WATER);
    // The east coast is connected to the edge through west-side neighbors.
    set(WrappedCoast, 330, LandType::DEEP_WATER);
    auto& river = world.rivers.emplace_back();
    river.path = {{{3, 5}, 1}, {{3, 4}, 2}};
    return world;
}

Vec2i center(int i) {
    return {i % Width * Side + Side / 2, i / Width * Side + Side / 2};
}

WaterSurface water(const GeneratedHydrology& hydrology, int region) {
    const auto p = center(region);
    return hydrology.waterAt(p.x, p.y);
}
}  // namespace

TEST(hydrology_basin_outlets_and_wrap) {
    const auto world = basinWorld();
    const auto hydrology = GeneratedHydrology::build(world);
    const auto id = hydrology->candidateBody(First);
    const auto other = hydrology->candidateBody(Second);
    EQUAL(id, uint32_t(First + 2), "stable first basin");
    EQUAL(other, uint32_t(Second + 2), "stable second basin");
    EQUAL(hydrology->basin(id)->sillY, 433, "low sill");
    EQUAL(hydrology->basin(other)->sillY, 471, "high sill");
    EQUAL(hydrology->basin(id)->outlet, First - Width, "regional outlet");
    EQUAL(hydrology->catchment(First + Width), id, "uphill catchment");
    EQUAL(hydrology->candidateBody(Dry), 0u, "dry inland bowl");
    CHECK(!water(*hydrology, Dry).wet());
    CHECK(hydrology->candidateBody(Diagonal) > GeneratedHydrology::Sea);
    EQUAL(water(*hydrology, Diagonal).body, hydrology->candidateBody(Diagonal),
          "diagonal-only inland lake");
    EQUAL(water(*hydrology, WrappedCoast).body, GeneratedHydrology::Sea,
          "face-connected through X seam");
    const auto seam = center(WrappedCoast);
    EQUAL(hydrology->waterAt(seam.x - Side * Width, seam.y).surfaceY,
          WaterSurface::SeaBoundary, "wrapped sea level");

    const auto lake = hydrology->lake(id);
    const auto highLake = hydrology->lake(other);
    EQUAL(lake.sillY, 430, "sampled low sill");
    EQUAL(highLake.sillY, 465, "sampled high sill");
    EQUAL(lake.surfaceY, lake.sillY, "low lake fills to its sill");
    EQUAL(highLake.surfaceY, highLake.sillY, "high lake fills to its sill");
    CHECK(lake.outlet.has_value());
    EQUAL(water(*hydrology, First).surfaceY, lake.surfaceY,
          "refined first lake");
    EQUAL(water(*hydrology, Second).surfaceY, highLake.surfaceY,
          "refined second lake");
    const auto& beds = hydrology->riverBeds();
    EQUAL(beds.size(), size_t(1), "generated river baseline");
    EQUAL(beds.front().size(), size_t(2), "bed samples");
    EQUAL(beds.front().back().catchment, id, "river receiving basin");
}

TEST(hydrology_saved_baseline_matches_new_chunk) {
    test::Assets assets;
    auto world = basinWorld();
    world.hydrology = GeneratedHydrology::build(world);
    const auto p = center(First);
    const auto h = water(*world.hydrology, First).surfaceY;
    const auto root =
        std::filesystem::path(TEST_SCRATCH_DIR) / "hydrology_baseline";
    std::filesystem::remove_all(root);
    auto slot = save_catalog::create(world, world.regions[First].coord, root);
    auto reopened = save_catalog::readWorldMap(slot);
    CHECK(reopened->hydrology != nullptr);
    SurfaceSampler unloaded(*reopened, assets);
    const auto sample = unloaded.column(p.x, p.y);
    EQUAL(sample.water.body, world.hydrology->candidateBody(First),
          "stable body after reload");
    EQUAL(sample.water.surfaceY, h, "refined height after reload");
    EQUAL(unloaded.waterAt(p.x, p.y).surfaceY, h, "unloaded water query");
    EQUAL(reopened->hydrology->riverBeds().front().back().groundY,
          sample.surfaceY, "river bed uses the baseline terrain");

    const auto& region = reopened->regions[First];
    const auto terrain =
        TerrainNeighborhood::capture(*reopened, region.coord, assets);
    const ChunkCoord top{unsigned(p.x / Chunk::SIZE_X),
                         unsigned((h - 1) / Chunk::SIZE_Y),
                         unsigned(p.y / Chunk::SIZE_Z)};
    const auto columns = TerrainSampler(terrain).columns(top.x, top.z);
    CHECK(int(top.y) * Chunk::SIZE_Y > SEA_LEVEL_Y);
    for (const auto& column : columns.columns)
        CHECK(column.surface_y < int(top.y) * Chunk::SIZE_Y);
    LocationGenerator generator;
    CHECK(!generator.isAirChunk(top, columns));
    Chunk chunk{};
    const auto layers =
        LayersGenerator(reopened->genSettings, &assets).get(region);
    generator.generateChunk(chunk, top,
                            {region.coord.x * CHUNKS_PER_REGION, 0,
                             region.coord.y * CHUNKS_PER_REGION},
                            region, layers, {}, &columns);
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int x = 0; x < Chunk::SIZE_X; ++x) {
            const auto column = unloaded.column(top.x * Chunk::SIZE_X + x,
                                                top.z * Chunk::SIZE_Z + z);
            for (int y = 0; y < Chunk::SIZE_Y; ++y) {
                const int wy = top.y * Chunk::SIZE_Y + y;
                CHECK(chunk.tiles[z][y][x].isLiquid() ==
                      column.water.fills(wy, column.surfaceY));
            }
        }
    CHECK(chunk.tiles[0][(h - 1) % Chunk::SIZE_Y][0].isLiquid());

    const std::atomic_bool cancelled{false};
    for (int spacing : {1, 2}) {
        const TerrainLod::Patch patch{p.x - 16, p.y - 16, spacing};
        const auto lod = TerrainLod::build(patch, unloaded, cancelled, 4);
        CHECK(!lod.water.vertices.empty());
        for (const auto& v : lod.water.vertices)
            EQUAL(v.y, float(h), "water mesh height");
    }

    // The same saved inputs opened as v13 keep the old generated water and
    // baseline over the explored frontier, even with a lake presentation tag.
    slot.meta.generatorVersion = WorldMeta::HydrologyVersion - 1;
    save_catalog::writeMeta(slot);
    const auto oldSlot = save_catalog::list(root).front();
    const auto legacy = save_catalog::readWorldMap(oldSlot);
    CHECK(!legacy->hydrology);
    SurfaceSampler oldWater(*legacy, assets);
    EQUAL(oldWater.height(p.x, p.y), sample.surfaceY, "same generated terrain");
    CHECK(!oldWater.waterAt(p.x, p.y).wet());
    const auto dry = center(Dry);
    CHECK(oldWater.height(dry.x, dry.y) < SEA_LEVEL_Y);
    EQUAL(oldWater.waterAt(dry.x, dry.y).surfaceY, WaterSurface::SeaBoundary,
          "unexplored legacy columns keep the original sea fill");
    CHECK(!unloaded.waterAt(dry.x, dry.y).wet());
}

TEST(hydrology_lod_fallback_refines_visible_water) {
    test::Assets assets;
    auto world = basinWorld();
    world.hydrology = GeneratedHydrology::build(world);
    SurfaceSampler preview(world, assets, nullptr, nullptr,
                           SurfaceSampler::WaterDetail::CertifiedSea);
    SurfaceSampler exact(world, assets);
    const auto p = center(First);
    CHECK(!preview.waterAt(p.x, p.y).wet());
    CHECK(!exact.submerged(p.x, p.y, 500.f));
    CHECK(exact.submerged(p.x, p.y, 428.f));

    const std::atomic_bool cancelled{false};
    const TerrainLod::Patch patch{p.x - 32, p.y - 32, 2};
    const auto first = TerrainLod::build(patch, preview, cancelled, 4);
    const auto refined = TerrainLod::build(patch, exact, cancelled, 4);
    CHECK(first.provisionalWater);
    CHECK(!first.vertices.empty());
    CHECK(first.water.vertices.empty());
    CHECK(!refined.provisionalWater);
    CHECK(!refined.water.vertices.empty());
    CHECK(first.indices == refined.indices);
    EQUAL(first.vertices.size(), refined.vertices.size(), "ground mesh size");
    for (size_t i = 0; i < first.vertices.size(); ++i)
        EQUAL(first.vertices[i].y, refined.vertices[i].y,
              "ground stays drawable");
    for (const auto& v : refined.water.vertices)
        EQUAL(v.y, 430.f, "refined lake height");

    // The fast depth predicate must agree with the full terrain surface
    // where a warped beach crosses the canonical wrap seam.
    for (unsigned rx : {0u, unsigned(Width - 1)}) {
        const auto data = TerrainNeighborhood::capture(world, {rx, 2}, assets);
        TerrainSampler terrain(data);
        for (int z = 2 * Side; z < 3 * Side; z += 5)
            for (int x = int(rx) * Side; x < int(rx + 1) * Side; x += 3)
                CHECK(terrain.belowSea(x, z) ==
                      (terrain.surface(x, z) < SEA_LEVEL_Y));
    }
}
