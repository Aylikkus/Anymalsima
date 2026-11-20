#include <filesystem>
#include <queue>
#include <set>

#include "lod_scene.hpp"
#include "serialization/file_io.hpp"
#include "test.hpp"
#include "test_assets.hpp"
#include "world/chunk_mesher.hpp"
#include "world/generated_hydrology.hpp"
#include "world/location_generator.hpp"
#include "world/save/chunk_codec.hpp"
#include "world/save/save_catalog.hpp"
#include "world/surface_sampler.hpp"
#include "world/terrain_edit_tracker.hpp"
#include "world/terrain_lod.hpp"
#include "world/world_editor.hpp"

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
    world.hydrology = GeneratedHydrology::build(world, {}, false);
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
    const auto waterOnly =
        TerrainLod::build(patch, exact, cancelled, 4, false, true);
    CHECK(waterOnly.vertices.empty());
    CHECK(waterOnly.covers.empty());
    CHECK(waterOnly.waterLevels == refined.waterLevels);
    CHECK(waterOnly.water.indices == refined.water.indices);
    EQUAL(waterOnly.water.vertices.size(), refined.water.vertices.size(),
          "water-only job emits the same lake");
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

TEST(hydrology_wrapped_river_profiles_and_meshes) {
    auto world = riverLodScene(32);
    world.hydrology = GeneratedHydrology::build(world);
    const auto& hydro = *world.hydrology;
    const auto& reaches = hydro.riverReaches();
    EQUAL(reaches.size(), size_t(2), "one river and one tributary");
    const auto& main = reaches.front();
    const auto& branch = reaches.back();
    CHECK(main.end == GeneratedHydrology::RiverEnd::Sea);
    EQUAL(main.receiver, GeneratedHydrology::Sea, "sea outlet");
    CHECK(branch.end == GeneratedHydrology::RiverEnd::Confluence);
    EQUAL(branch.receiver, main.id, "tributary flows into the main reach");
    CHECK(main.spring.unitsPerStep > 0);
    CHECK(!main.falls.empty());

    test::Assets assets;
    SurfaceSampler sampler(world, assets);
    const auto spring = sampler.column(main.spring.tile.x, main.spring.tile.y);
    EQUAL(spring.surfaceY, main.spring.rockY, "spring exposes rock");
    EQUAL(spring.soil.count, 0, "spring surface is bare rock");
    EQUAL(spring.water.depth(spring.surfaceY), 1, "one-block headwater");

    const int z = main.spring.tile.y;
    const int period = world.wrap().tiles;
    const auto left = sampler.waterAt(-1, z), right = sampler.waterAt(0, z);
    EQUAL(left.reach, main.id, "west side of the seam");
    EQUAL(right.reach, main.id, "east side of the seam");
    CHECK(left.surfaceY <= right.surfaceY);
    EQUAL(sampler.waterAt(period - 1, z).surfaceY, left.surfaceY,
          "canonical mouth profile");
    CHECK(left.flow.x < 0 && right.flow.x < 0);
    int deepest = 0;
    for (int x = main.spring.tile.x, prev = WORLD_HEIGHT_Y;
         x >= main.outlet.x - period; --x) {
        const auto column = sampler.column(x, z);
        CHECK(column.water.wet());
        CHECK(column.water.surfaceY <= prev);
        prev = column.water.surfaceY;
        if (column.water.reach == main.id)
            deepest = std::max(deepest, column.water.depth(column.surfaceY));
    }
    EQUAL(deepest, GeneratedHydrology::MaxRiverDepth, "deep channel depth");

    const auto& fall = main.falls.front();
    CHECK(fall.drop() >= GeneratedHydrology::MinFallDrop);
    CHECK(fall.drop() <= GeneratedHydrology::MaxFallDrop);
    EQUAL(sampler.waterAt(fall.lip.x, fall.lip.y).surfaceY, fall.lipY,
          "stable lip");
    EQUAL(sampler.waterAt(fall.landing.x, fall.landing.y).surfaceY,
          fall.landingY, "stable landing");
    CHECK(hydro.hasChannel(fall.lip.x, fall.lip.y, fall.lip.x + 1,
                           fall.lip.y + 1));
    CHECK(sampler.submerged(fall.lip.x, fall.lip.y, fall.lipY - .25f));
    CHECK(!sampler.submerged(fall.lip.x, fall.lip.y, fall.lipY + .25f));

    const std::atomic_bool cancelled{false};
    const auto isFallFace = [&](const BlockVertex& v) {
        return v.layer == -3 && v.nx < -.9f &&
               std::abs(v.x - fall.lip.x) < .01 &&
               std::abs(v.z - fall.lip.y) <= 1 && v.u < -.9f;
    };
    for (int spacing : {32, 1}) {
        // Position the boundary of the stepped patch on the lip itself.
        const TerrainLod::Patch patch{spacing == 1 ? fall.lip.x : 0,
                                      spacing == 1 ? fall.lip.y - 8 : 0,
                                      spacing};
        const auto mesh = TerrainLod::build(patch, sampler, cancelled, 4, true);
        bool lip = false, landing = false;
        for (const auto& v : mesh.water.vertices) {
            if (!isFallFace(v)) continue;
            lip |= v.y == fall.lipY;
            landing |= v.y == fall.landingY;
        }
        CHECK(lip);
        CHECK(landing);
        CHECK(mesh.water.bounds.minY <= fall.landingY);
        CHECK(mesh.water.bounds.maxY >= fall.lipY);
    }

    // The narrow channel falls between coarse sample rows. Both fallback
    // patches still reach the common X boundary at tile resolution.
    constexpr int seamSpacing = 64;
    const auto reachesSeam = [&](const TerrainLod::Patch patch, float edge,
                                 int expectedY) {
        SurfaceSampler fallback(world, assets, nullptr, nullptr,
                                SurfaceSampler::WaterDetail::CertifiedSea);
        const auto mesh =
            TerrainLod::build(patch, fallback, cancelled, 4, true);
        return std::any_of(
            mesh.water.vertices.begin(), mesh.water.vertices.end(),
            [&](const BlockVertex& v) {
                return v.x == edge && v.y == expectedY && v.z >= z &&
                       v.z <= z + 1 && v.u < -.9f && v.ny > .9f;
            });
    };
    CHECK(reachesSeam({0, 0, seamSpacing}, 0, right.surfaceY));
    CHECK(
        reachesSeam({period - TerrainLod::Cells * seamSpacing, 0, seamSpacing},
                    period, left.surfaceY));

    const auto& region =
        world.regions[fall.lip.y / Side * world.width + fall.lip.x / Side];
    const auto input =
        TerrainNeighborhood::capture(world, region.coord, assets);
    const unsigned cx = fall.lip.x / Chunk::SIZE_X;
    const unsigned cz = fall.lip.y / Chunk::SIZE_Z;
    const auto columns = TerrainSampler(input).columns(cx, cz);
    const auto layers = LayersGenerator(world.genSettings, &assets).get(region);
    const auto baseline = [&](int x, int y, int z) -> const Tile* {
        static const Tile solid{.block_id = 1, .lightLevel = {}};
        static const Tile water{
            .liquid = LiquidType::Water, .liquidLevel = 255, .lightLevel = {}};
        static const Tile air{};
        const auto column = sampler.column(x, z);
        if (y <= column.surfaceY) return &solid;
        return column.water.fills(y, column.surfaceY) ? &water : &air;
    };
    const auto waterColumn = [&](int x, int z) {
        return std::pair{sampler.height(x, z), sampler.waterAt(x, z)};
    };
    bool lip = false, landing = false;
    LocationGenerator generator;
    for (int slab = (fall.lipY - 1) / Chunk::SIZE_Y;
         slab >= fall.landingY / Chunk::SIZE_Y; --slab) {
        Chunk chunk{};
        const ChunkCoord coord{cx, unsigned(slab), cz};
        generator.generateChunk(chunk, coord, {}, region, layers, {}, &columns);
        const auto mesh = ChunkMesher(assets).build(chunk, coord, baseline, {},
                                                    baseline, waterColumn);
        for (auto v : mesh.water.vertices) {
            v.x += cx * Chunk::SIZE_X;
            v.z += cz * Chunk::SIZE_Z;
            v.y += slab * Chunk::SIZE_Y;
            if (!isFallFace(v)) continue;
            lip |= v.y == fall.lipY;
            landing |= v.y == fall.landingY;
        }
    }
    CHECK(lip);
    CHECK(landing);

    const auto root =
        std::filesystem::path(TEST_SCRATCH_DIR) / "river_baseline";
    std::filesystem::remove_all(root);
    auto slot = save_catalog::create(world, region.coord, root);
    slot.meta.generatorVersion = WorldMeta::RiverVersion;
    auto loaded = save_catalog::readWorldMap(slot);
    SurfaceSampler reloaded(*loaded, assets);
    EQUAL(reloaded.waterAt(fall.landing.x, fall.landing.y).surfaceY,
          fall.landingY, "reloaded landing level");
    EQUAL(reloaded.height(fall.lip.x, fall.lip.y),
          sampler.height(fall.lip.x, fall.lip.y), "reloaded lip bed");
    slot.meta.generatorVersion = WorldMeta::HydrologyVersion;
    auto v14 = save_catalog::readWorldMap(slot);
    CHECK(v14->hydrology->riverReaches().empty());
    // Preserve the wrapped outlet selected by the original full survey.
    const auto& outlet = main.outlet;
    EQUAL(v14->hydrology->waterAt(outlet.x, outlet.y).body,
          GeneratedHydrology::Sea, "first river column in the sea footprint");
    EQUAL(outlet.x, (world.width - 2) * Side + Side / 2,
          "stable outlet selected from the coastal survey");
    SurfaceSampler previous(*v14, assets);
    CHECK(previous.height(main.spring.tile.x, main.spring.tile.y) >
          spring.surfaceY);
    CHECK(!previous.waterAt(main.spring.tile.x, main.spring.tile.y).wet());
    std::filesystem::remove_all(root);
}

TEST(hydrology_natural_reach_profiles_and_coastal_preview) {
    auto world = riverLodScene(32);
    world.hydrology = GeneratedHydrology::build(world, {}, true, true);
    const auto& reaches = world.hydrology->riverReaches();
    CHECK(!reaches.empty());
    size_t falls = 0;
    for (const auto& reach : reaches) {
        CHECK(reach.falls.size() <= 1);
        falls += reach.falls.size();
        for (const auto& fall : reach.falls)
            CHECK(fall.drop() <= GeneratedHydrology::MaxFallDrop);
    }
    EQUAL(falls, size_t(0), "ordinary grades stay smooth");
    auto steep = riverLodScene(32);
    for (int row = 0; row < steep.height; ++row) {
        auto& ledge = steep.regions[row * steep.width + 1];
        ledge.land_height_m += 55;
        ledge.addFeature(FeatureMask::CLIFF);
    }
    steep.hydrology = GeneratedHydrology::build(steep, {}, true, true);
    size_t steepFalls = 0;
    for (const auto& reach : steep.hydrology->riverReaches()) {
        CHECK(reach.falls.size() <= 1);
        steepFalls += reach.falls.size();
    }
    CHECK(steepFalls > 0);
    const auto& main = reaches.front();
    test::Assets assets;
    SurfaceSampler exact(world, assets);
    SurfaceSampler fallback(world, assets, nullptr, nullptr,
                            SurfaceSampler::WaterDetail::Preview);
    bool curved = false;
    const auto head = main.spring.tile;
    for (int x = head.x - 96; x < head.x - 20; ++x)
        for (int dz : {-11, -10, -9, 9, 10, 11})
            curved |= exact.waterAt(x, head.y + dz).reach == main.id;
    CHECK(curved);
    CHECK(main.end == GeneratedHydrology::RiverEnd::Sea);
    const auto mouth = main.outlet;
    EQUAL(fallback.waterAt(mouth.x, mouth.y).surfaceY,
          WaterSurface::SeaBoundary, "ocean at the mouth in the first patch");
    const auto patch =
        TerrainLod::Patch{mouth.x / 128 * 128, mouth.y / 128 * 128, 4};
    const std::atomic_bool cancel{false};
    const auto preview = TerrainLod::build(patch, fallback, cancel, 8, true);
    const auto all = TerrainLod::build(patch, exact, cancel, 8, true);
    CHECK(preview.indices == all.indices);
    EQUAL(preview.vertices.size(), all.vertices.size(), "stable terrain count");
    for (size_t i = 0; i < all.vertices.size(); ++i)
        EQUAL(preview.vertices[i].y, all.vertices[i].y, "stable terrain Y");
    CHECK(std::any_of(preview.water.vertices.begin(),
                      preview.water.vertices.end(), [](const BlockVertex& v) {
                          return v.ny > .9f && v.layer == -2 &&
                                 v.y == WaterSurface::SeaBoundary;
                      }));
}

TEST(hydrology_river_banks_close_dry_faces) {
    auto world = riverLodScene(32);
    world.hydrology = GeneratedHydrology::build(world, {}, true, true);
    auto bounded = riverLodScene(32);
    bounded.hydrology =
        GeneratedHydrology::build(bounded, {}, true, true, true);
    test::Assets assets;
    SurfaceSampler before(world, assets);
    SurfaceSampler after(bounded, assets);
    constexpr std::array<Vec2i, 4> sides{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
    std::optional<Vec2i> bank;
    int count = 0;
    for (int z = 399; z < 500; ++z)
        for (int x = -240; x < 208; ++x) {
            const auto old = before.waterAt(x, z);
            const auto water = after.waterAt(x, z);
            for (const auto [dx, dz] : sides) {
                const int nx = x + dx, nz = z + dz;
                if (old.reach && !before.waterAt(nx, nz).wet() &&
                    before.height(nx, nz) < old.topTile())
                    bank = Vec2i{nx, nz};
                if (!water.reach || after.waterAt(nx, nz).wet()) continue;
                CHECK(after.height(nx, nz) >= water.topTile());
                ++count;
            }
        }
    CHECK(bank.has_value());
    CHECK(count > 0);

    // The raised bank is actual solid terrain in generated voxels.
    const int bx = after.wrapX(bank->x), bz = bank->y;
    const auto coord = Vec2u{unsigned(bx / Side), unsigned(bz / Side)};
    const auto& region = bounded.regions[coord.y * bounded.width + coord.x];
    const auto input = TerrainNeighborhood::capture(bounded, coord, assets);
    const int top = after.height(bx, bz);
    CHECK(top > before.height(bx, bz));
    const ChunkCoord slab{unsigned(bx / Chunk::SIZE_X),
                          unsigned(top / Chunk::SIZE_Y),
                          unsigned(bz / Chunk::SIZE_Z)};
    const auto columns = TerrainSampler(input).columns(slab.x, slab.z);
    const auto layers =
        LayersGenerator(bounded.genSettings, &assets).get(region);
    Chunk chunk{};
    LocationGenerator{}.generateChunk(chunk, slab, {}, region, layers, {},
                                      &columns);
    CHECK(
        chunk.tiles[bz % Chunk::SIZE_Z][top % Chunk::SIZE_Y][bx % Chunk::SIZE_X]
            .isSolid());
}

TEST(hydrology_river_sediment_matches_local_soil) {
    const auto scene = riverLodScene(32);
    const auto root =
        std::filesystem::path(TEST_SCRATCH_DIR) / "river_sediment";
    std::filesystem::remove_all(root);
    auto slot = save_catalog::create(scene, {1, 3}, root);
    const auto world = save_catalog::readWorldMap(slot);
    slot.meta.generatorVersion = WorldMeta::BoundedRiverVersion;
    const auto legacy = save_catalog::readWorldMap(slot);
    std::filesystem::remove_all(root);

    for (bool sand : {false, true}) {
        const uint8_t sedimentBlock = sand ? 3 : 2;
        Soil soil{};
        soil.id = 1;
        soil.name = sand ? "sand" : "loam";
        soil.block_name = sand ? "block3" : "block2";
        if (!sand) soil.topsoil_block_name = "block3";
        soil.depth_m = {4, 4};
        SoilPlacement placement;
        placement.allowed_land = {LandType::HILLS};
        soil.placement = {placement};
        test::Assets assets;
        assets.setData<Soil>({soil});
        SurfaceSampler before(*legacy, assets);
        SurfaceSampler after(*world, assets);

        const auto head = world->hydrology->riverReaches().front().spring;
        EQUAL(after.column(head.tile.x, head.tile.y).material, 7,
              "the aquifer outlet stays rock");
        std::optional<Vec2i> bed, bank;
        constexpr std::array<Vec2i, 4> sides{
            {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
        for (int x = head.tile.x - 90; x < head.tile.x - 20; ++x)
            for (int z = head.tile.y - 20; z <= head.tile.y + 20; ++z) {
                if (!before.waterAt(x, z).reach) continue;
                if (!bed && before.column(x, z).material == 7 &&
                    after.column(x, z).soil.depth_m >= 2)
                    bed = Vec2i{x, z};
                for (const auto [dx, dz] : sides) {
                    const int nx = x + dx, nz = z + dz;
                    if (!bank && !before.waterAt(nx, nz).wet() &&
                        before.column(nx, nz).material == 7 &&
                        after.column(nx, nz).soil.depth_m >= 2)
                        bank = Vec2i{nx, nz};
                }
            }
        CHECK(bed.has_value());
        CHECK(bank.has_value());
        for (const auto& point : {bed, bank}) {
            const auto [x, z] = *point;
            const bool wet = point == bed;
            const auto column = after.column(x, z);
            EQUAL(column.surfaceY, before.height(x, z), "same bed and bank Y");
            EQUAL(column.water.surfaceY, before.waterAt(x, z).surfaceY,
                  "same water profile");
            EQUAL(column.material, 9, "LOD top shows local sediment");
            EQUAL(column.rockMaterial, 9, "smooth LOD sides show sediment");
            EQUAL(after.materialAt(x, column.surfaceY - 1, z), 9,
                  "stepped LOD sides show the actual sediment layer");
            if (wet) {
                CHECK(!column.soil.layers[0].topsoil);
                EQUAL(column.soil.cover, 0, "no underwater vegetation");
            }
            const Vec2u regionCoord{unsigned(x / Side), unsigned(z / Side)};
            const auto& region =
                world->regions[regionCoord.y * world->width + regionCoord.x];
            const auto input =
                TerrainNeighborhood::capture(*world, regionCoord, assets);
            const auto layers =
                LayersGenerator(world->genSettings, &assets).get(region);
            const auto blockAt = [&](int y) {
                const ChunkCoord coord{unsigned(x / Chunk::SIZE_X),
                                       unsigned(y / Chunk::SIZE_Y),
                                       unsigned(z / Chunk::SIZE_Z)};
                const auto columns =
                    TerrainSampler(input).columns(coord.x, coord.z);
                Chunk chunk{};
                LocationGenerator{}.generateChunk(chunk, coord, {}, region,
                                                  layers, {}, &columns);
                return chunk
                    .tiles[z % Chunk::SIZE_Z][y % Chunk::SIZE_Y]
                          [x % Chunk::SIZE_X]
                    .block_id;
            };
            EQUAL(blockAt(column.surfaceY), wet ? sedimentBlock : 3,
                  "chunk surface uses the local parent soil or topsoil");
            EQUAL(blockAt(column.surfaceY - 1), sedimentBlock,
                  "chunk bank face uses the local parent soil");
            EQUAL(blockAt(column.surfaceY - column.soil.depth_m), 1,
                  "rock remains below the sediment");
        }
    }
}

TEST(hydrology_river_continues_from_lake) {
    auto scene = basinWorld();
    for (int z : {0, 1})
        for (int x : {2, 3, 4}) {
            auto& region = scene.regions[z * Width + x];
            region.land = LandType::DEEP_WATER;
            region.land_height_m = 330;
        }
    scene.rivers.front().path = {{{3, 5}, 12}, {{3, 4}, 16}, {{3, 3}, 16},
                                 {{3, 2}, 16}, {{3, 1}, 16}, {{3, 0}, 16}};
    scene.rivers.push_back(scene.rivers.front());
    // The route begins in the lake and shares its downstream course.
    auto& duplicate = scene.rivers.back().path;
    duplicate.erase(duplicate.begin());
    // A separate inland lake has no saved downstream path.
    scene.rivers.emplace_back().path = {{{8, 5}, 8}, {{8, 4}, 12}};

    const auto root = std::filesystem::path(TEST_SCRATCH_DIR) / "lake_outlet";
    std::filesystem::remove_all(root);
    auto slot = save_catalog::create(scene, scene.regions[First].coord, root);
    const auto world = save_catalog::readWorldMap(slot);
    slot.meta.generatorVersion = WorldMeta::RiverSedimentVersion;
    const auto legacy = save_catalog::readWorldMap(slot);
    std::filesystem::remove_all(root);

    const auto& hydro = *world->hydrology;
    const uint32_t body = hydro.candidateBody(First);
    const auto lake = hydro.lake(body);
    CHECK(lake.surfaceY > WaterSurface::SeaBoundary);
    const auto& reaches = hydro.riverReaches();
    CHECK(std::any_of(reaches.begin(), reaches.end(), [&](const auto& reach) {
        return reach.end == GeneratedHydrology::RiverEnd::Lake &&
               reach.receiver == body && reach.spring.unitsPerStep > 0;
    }));
    CHECK(std::none_of(legacy->hydrology->riverReaches().begin(),
                       legacy->hydrology->riverReaches().end(),
                       [](const auto& reach) { return reach.lakeSource; }));
    const auto outlet =
        std::find_if(reaches.begin(), reaches.end(),
                     [](const auto& r) { return r.lakeSource; });
    CHECK(outlet != reaches.end());
    EQUAL(std::count_if(reaches.begin(), reaches.end(),
                        [](const auto& r) { return bool(r.lakeSource); }),
          1, "one outlet for the lake with a downstream route");
    EQUAL(outlet->lakeSource->body, body, "source basin");
    EQUAL(outlet->spring.unitsPerStep, 0u, "the lake supplies the reach");
    CHECK(outlet->end == GeneratedHydrology::RiverEnd::Sea);
    EQUAL(outlet->receiver, GeneratedHydrology::Sea, "outlet drains to sea");
    test::Assets assets;
    SurfaceSampler sampler(*world, assets);
    const auto source = outlet->lakeSource->tile;
    EQUAL(sampler.waterAt(source.x, source.y).body, body,
          "outlet begins in the surveyed lake");

    std::queue<Vec2i> queue;
    std::set<std::pair<int, int>> seen;
    queue.push(source);
    seen.emplace(source.x, source.y);
    constexpr std::array<Vec2i, 4> sides{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
    bool connected = false;
    std::optional<Vec2i> channel;
    while (!queue.empty() && !connected) {
        const auto [x, z] = queue.front();
        queue.pop();
        const auto water = sampler.waterAt(x, z);
        for (const auto [dx, dz] : sides) {
            const Vec2i next{x + dx, z + dz};
            const auto neighbor = sampler.waterAt(next.x, next.y);
            if (neighbor.reach != outlet->id &&
                neighbor.body != GeneratedHydrology::Sea)
                continue;
            const int solid =
                std::max(sampler.height(x, z), sampler.height(next.x, next.y));
            if (solid + 1 >= std::min(water.surfaceY, neighbor.surfaceY))
                continue;
            if (neighbor.body == GeneratedHydrology::Sea) {
                connected = true;
                break;
            }
            if (!channel) channel = next;
            if (seen.emplace(next.x, next.y).second) queue.push(next);
        }
    }
    CHECK(connected);
    CHECK(channel.has_value());
    const auto [x, z] = *channel;
    SurfaceSampler fallback(*world, assets, nullptr, nullptr,
                            SurfaceSampler::WaterDetail::Preview);
    EQUAL(fallback.waterAt(x, z).reach, outlet->id,
          "fallback LOD includes the lake outlet");
    const int y = sampler.waterAt(x, z).topTile();
    const ChunkCoord slab{unsigned(x / Chunk::SIZE_X),
                          unsigned(y / Chunk::SIZE_Y),
                          unsigned(z / Chunk::SIZE_Z)};
    const auto& region = world->regions[z / Side * Width + x / Side];
    const auto input =
        TerrainNeighborhood::capture(*world, region.coord, assets);
    const auto columns = TerrainSampler(input).columns(slab.x, slab.z);
    const auto layers =
        LayersGenerator(world->genSettings, &assets).get(region);
    Chunk chunk{};
    LocationGenerator{}.generateChunk(chunk, slab, {}, region, layers, {},
                                      &columns);
    const auto& tile =
        chunk.tiles[z % Chunk::SIZE_Z][y % Chunk::SIZE_Y][x % Chunk::SIZE_X];
    CHECK(tile.liquid == LiquidType::Water);
}

TEST(hydrology_coastal_basin_fills_at_sea_level) {
    World world{};
    world.width = 8;
    world.height = 7;
    world.genSettings = WorldGenSettings::fromSeed(2);
    world.genSettings.width = world.width;
    world.genSettings.height = world.height;
    for (int z = 0; z < world.height; ++z)
        for (int x = 0; x < world.width; ++x) {
            // The coarse water regions touch at a corner. Coast warping
            // opens a face-connected passage at tile resolution.
            const bool low =
                (x == 3 && z <= 2) || (x == 4 && z == 3) || (x == 6 && z == 4);
            auto& region = world.regions.emplace_back();
            region = test::regionAt(low ? 374 : 405);
            region.coord = {unsigned(x), unsigned(z)};
            region.land = low ? LandType::WATER : LandType::PLAINS;
        }
    constexpr Vec2u regionCoord{4, 3};
    constexpr int x = regionCoord.x * Side + Side / 2;
    constexpr int z = regionCoord.y * Side + Side / 2;
    const auto legacy = GeneratedHydrology::build(world, {}, false);
    world.hydrology = GeneratedHydrology::build(world, {}, false, false, false,
                                                false, false, true);
    const auto& hydro = *world.hydrology;
    CHECK(hydro.candidateBody(3 * world.width + 4) > GeneratedHydrology::Sea);
    CHECK(!legacy->waterAt(x, z, 370).wet());

    test::Assets assets;
    SurfaceSampler sampler(world, assets);
    const auto col = sampler.column(x, z);
    EQUAL(col.water.body, GeneratedHydrology::Sea, "coastal basin joins sea");
    EQUAL(col.water.surfaceY, WaterSurface::SeaBoundary, "same sea level");
    CHECK(hydro.uniformWater(x - 2, z - 2, x + 3, z + 3,
                             GeneratedHydrology::Sea));
    CHECK(!hydro.uniformWater(x - 2, z - 2, x + 3, z + 3, 0));
    SurfaceSampler preview(world, assets, nullptr, nullptr,
                           SurfaceSampler::WaterDetail::Preview);
    EQUAL(preview.column(x, z).water.body, GeneratedHydrology::Sea,
          "cached coastal footprint also supplies distant water");
    CHECK(sampler.waterAt(6 * Side + Side / 2, 4 * Side + Side / 2).body >
          GeneratedHydrology::Sea);

    const int y = col.water.topTile();
    const ChunkCoord slab{unsigned(x / Chunk::SIZE_X),
                          unsigned(y / Chunk::SIZE_Y),
                          unsigned(z / Chunk::SIZE_Z)};
    const auto& region = world.regions[3 * world.width + 4];
    const auto inputs =
        TerrainNeighborhood::capture(world, regionCoord, assets);
    const auto columns = TerrainSampler(inputs).columns(slab.x, slab.z);
    const auto layers = LayersGenerator(world.genSettings, &assets).get(region);
    Chunk chunk{};
    LocationGenerator{}.generateChunk(chunk, slab, {}, region, layers, {},
                                      &columns);
    CHECK(chunk.tiles[z % Chunk::SIZE_Z][y % Chunk::SIZE_Y][x % Chunk::SIZE_X]
              .liquid == LiquidType::Water);
}

TEST(hydrology_sea_and_high_lake_share_region) {
    World world{};
    world.width = 8;
    world.height = 7;
    world.genSettings = WorldGenSettings::fromSeed(24);
    world.genSettings.width = world.width;
    world.genSettings.height = world.height;
    for (int z = 0; z < world.height; ++z)
        for (int x = 0; x < world.width; ++x) {
            const bool ocean = x == 3 && z <= 2;
            auto& region = world.regions.emplace_back();
            region = test::regionAt(ocean ? 375 : 414);
            if (x == 4 && (z == 3 || z == 4)) region.land_height_m = 394;
            if (x == 4 && z == 2) region.land_height_m = 405;
            region.coord = {unsigned(x), unsigned(z)};
            region.land = ocean ? LandType::WATER : LandType::PLAINS;
        }
    constexpr Vec2u regionCoord{4, 3};
    constexpr int x = regionCoord.x * Side, z = regionCoord.y * Side;
    const auto legacy = GeneratedHydrology::build(world, {}, false, false,
                                                  false, false, false, true);
    world.hydrology = GeneratedHydrology::build(world, {}, false, false, false,
                                                false, false, true, true);
    const auto& hydro = *world.hydrology;
    const uint32_t lakeId = hydro.candidateBody(3 * world.width + 4);
    CHECK(lakeId > GeneratedHydrology::Sea);

    test::Assets assets;
    SurfaceSampler sampler(world, assets);
    const auto shore = sampler.column(x, z);
    CHECK(shore.surfaceY < SEA_LEVEL_Y);
    CHECK(!legacy->waterAt(x, z, shore.surfaceY).wet());
    EQUAL(shore.water.body, GeneratedHydrology::Sea, "sea crosses lake region");
    EQUAL(shore.water.surfaceY, WaterSurface::SeaBoundary, "shore water level");
    CHECK(hydro.uniformWater(x, z, x + 1, z + 1, GeneratedHydrology::Sea));
    CHECK(!hydro.uniformWater(x, z, x + 1, z + 1, lakeId));
    const auto lake = sampler.column(x + Side / 2, z + Side / 2);
    EQUAL(lake.water.body, lakeId, "the inland lake has its own basin");
    EQUAL(lake.water.surfaceY, legacy->lake(lakeId).surfaceY,
          "terrain still supports the original lake level");
    CHECK(lake.water.surfaceY > shore.water.surfaceY);

    const auto columns =
        TerrainSampler(TerrainNeighborhood::capture(world, regionCoord, assets))
            .columns(x / Chunk::SIZE_X, z / Chunk::SIZE_Z);
    EQUAL(columns.columns.front().water.body, shore.water.body,
          "chunk generation agrees with distant water");
    EQUAL(columns.columns.front().water.surfaceY, shore.water.surfaceY,
          "chunk generation agrees with the distant surface height");
    const auto mesh =
        TerrainLod::build({x, z, 1}, sampler, std::atomic_bool{false}, 8, true);
    CHECK(std::any_of(mesh.water.vertices.begin(), mesh.water.vertices.end(),
                      [&](const BlockVertex& vertex) {
                          return vertex.x == x && vertex.z == z &&
                                 vertex.y == shore.water.surfaceY &&
                                 vertex.ny > .9f;
                      }));
}

TEST(hydrology_reservoir_reconciles_saved_and_unloaded_water) {
    auto world = basinWorld();
    // Extend the wrapped coast into a broad sea, with a shallow shore
    // next to the existing inland terrain.
    for (int z = 0; z < Height; ++z)
        for (int x : {0, 1, Width - 2, Width - 1}) {
            auto& r = world.regions[z * Width + x];
            r.land = LandType::DEEP_WATER;
            r.land_height_m = 325;
        }
    world.rivers.clear();
    world.hydrology = GeneratedHydrology::build(world);
    constexpr auto sea = GeneratedHydrology::Sea;
    constexpr int top = WaterSurface::SeaBoundary;
    constexpr auto layer = 4 * WaterUnitsPerBlock;
    const auto addLedger = [&](World& w) {
        w.reservoirs = std::make_shared<Reservoirs>(w.hydrology);
        w.reservoirs->define(sea, StorageCurve::constantArea(4, 315, top));
    };
    addLedger(world);

    test::Assets assets;
    LocationGenerator generator;
    ThreadPool pool{2};
    ChunkMesher mesher(assets);
    const auto root = std::filesystem::path(TEST_SCRATCH_DIR) / "live_water";
    std::filesystem::remove_all(root);
    const auto slot = save_catalog::create(world, {0, 2}, root);
    const auto cx = unsigned(2), cz = unsigned(20);
    const ChunkCoord upper{cx, unsigned((top - 1) / 16), cz};
    const ChunkCoord lower{cx, upper.y - 1, cz};
    const ChunkCoord west{cx - 1, lower.y, cz};
    const ChunkCoord east{cx + 1, lower.y, cz};
    const ChunkCoord north{cx, lower.y, cz - 1};
    const ChunkCoord newSlab{cx + 2, lower.y, cz};
    constexpr int x = int(cx) * 16 + 3, z = int(cz) * 16 + 5;
    const TilePos dry{x, top - 1, z};
    const TilePos pocket{x, 310, z};
    const ChunkCoord cavity{cx, unsigned(pocket.y / 16), cz};
    SurfaceSampler initial(world, assets);
    EQUAL(initial.waterAt(x, z).surfaceY, top, "initial sea surface");
    int shoreX = -1, shoreZ = -1;
    for (int dz = 0; dz < 64 && shoreX < 0; ++dz)
        for (int dx = 200; dx < 320 && shoreX < 0; ++dx)
            if (initial.height(dx, z + dz) == top - 2 &&
                initial.waterAt(dx, z + dz).body == sea) {
                shoreX = dx;
                shoreZ = z + dz;
            }
    CHECK(shoreX >= 0);

    const auto registerSlab = [&](ChunkManager& chunks, const World& w,
                                  ChunkCoord coord, ChunkBlob blob = {}) {
        const auto rx = coord.x / CHUNKS_PER_REGION;
        const auto rz = coord.z / CHUNKS_PER_REGION;
        chunks.registerChunk(coord, {rx * 8, 0, rz * 8},
                             &w.regions[rz * w.width + rx], std::move(blob));
    };
    const auto ready = [&](ChunkManager& chunks, ChunkCoord c) {
        for (int i = 0; i < 12 && !chunks.request(c); ++i) {
            chunks.update();
            CHECK(pool.wait_for(std::chrono::seconds(5)));
        }
        CHECK(chunks.isReady(c));
    };

    RegionData disk;
    const auto regionPath = region_file::path(slot.directory, {0, 2});
    FileIO::ensureDirectory(regionPath.parent_path());
    const auto saveChunks = [&](ChunkManager& chunks) {
        disk = region_file::merge(disk, chunks.snapshotRegion({0, 2}, true));
        FileIO::writeBytesAtomic(regionPath, region_file::encode(disk));
    };
    const auto detailedTop = [&](ChunkManager& chunks, ChunkCoord c, int wx,
                                 int wz) {
        const auto mesh = mesher.build(
            *chunks.getChunk(c), c,
            [&](int x, int y, int z) { return chunks.tileAt(x, y, z); });
        CHECK(!mesh.waterTops.empty());
        return int(c.y) * 16 + mesh.waterTops[(wz % 16) * 16 + wx % 16];
    };
    const auto lodTop = [&](const World& w, int level) {
        SurfaceSampler sampler(w, assets);
        const auto mesh = TerrainLod::build({16, 288, 2}, sampler,
                                            std::atomic_bool{false}, 4);
        CHECK(!mesh.water.vertices.empty());
        for (const auto& v : mesh.water.vertices)
            EQUAL(v.y, float(level), "coarse sea height");
    };
    {
        ChunkManager chunks(generator, pool, nullptr, world, &assets);
        for (auto c : {upper, lower, west, east, north, cavity})
            registerSlab(chunks, world, c);
        for (unsigned y = cavity.y + 1; y < lower.y; ++y)
            registerSlab(chunks, world, {cx, y, cz});
        registerSlab(chunks, world, {cx, 0, cz});
        for (auto c : {upper, lower, west, north, cavity}) ready(chunks, c);
        for (unsigned y = cavity.y + 1; y < lower.y; ++y)
            ready(chunks, {cx, y, cz});
        // A deliberate dry cell and an independent liquid in the sea band.
        WorldEditor editor(chunks, nullptr, assets.getStorage<Block>());
        TerrainEdits edits;
        TerrainEditTracker tracker(chunks, edits);
        tracker.watch(editor);
        CHECK(editor.removeBlock(pocket) == EditStatus::Applied);
        CHECK(editor.placeBlock(dry, 1) == EditStatus::Applied);
        CHECK(editor.removeBlock(dry) == EditStatus::Applied);
        chunks.withChunkLocked(upper, [&](Chunk& chunk) {
            auto& lava = chunk.tiles[z % 16][0][(x + 1) % 16];
            lava.liquid = LiquidType::Lava;
            lava.liquidLevel = 63;
            lava.waterOrigin = WaterOrigin::Independent;
        });
        tracker.update();
        SurfaceSampler edited(world, assets, nullptr, edits.snapshot());
        EQUAL(edited.waterAt(dry.x, dry.z).surfaceY, top - 1,
              "unloaded summary preserves the dry top cell");
        save_catalog::writeTerrainEdits(slot, *edits.snapshot());
        EQUAL(detailedTop(chunks, upper, x + 2, z), float(top),
              "generated detailed water");
        saveChunks(chunks);
        chunks.unregister(west);
        CHECK(!chunks.isRegistered(west));
        const auto start = world.reservoirs->volume(sea);
        EQUAL(chunks.divertWater(sea, pocket, 200), 200, "atomic local credit");
        EQUAL(chunks.tileAt(pocket.x, pocket.y, pocket.z)->liquidLevel, 200,
              "local fill immediately visible");
        EQUAL(chunks.transferWater(sea, -(layer - 201),
                                   ReservoirExchange::Disposal),
              -(layer - 201), "sub-layer removal");
        CHECK(chunks.tileAt(x + 2, top - 1, z)->liquid == LiquidType::Water);
        EQUAL(chunks.transferWater(sea, -1, ReservoirExchange::Disposal), -1,
              "cross first layer");
        CHECK(chunks.tileAt(x + 2, top - 1, z)->isAir());
        CHECK(!SurfaceSampler(world, assets).waterAt(shoreX, shoreZ).wet());
        EQUAL(initial.waterAt(shoreX, shoreZ).surfaceY, top,
              "captured snapshot keeps its original surface");
        EQUAL(detailedTop(chunks, lower, x + 2, z), float(top - 1),
              "detailed water at slab boundary");
        chunks.request(east);
        chunks.update();  // Dispatch the slab but leave publication pending.
        EQUAL(chunks.transferWater(sea, -layer, ReservoirExchange::Disposal),
              -layer, "cross second layer");
        ready(chunks, east);
        EQUAL(detailedTop(chunks, east, x + 16, z), float(top - 2),
              "in-flight generation publishes the current surface");
        EQUAL(detailedTop(chunks, lower, x + 2, z), float(top - 2),
              "ready slab reconciled");
        EQUAL(detailedTop(chunks, north, x + 2, z - 16), float(top - 2),
              "second ready shore column reconciled");
        EQUAL(world.reservoirs->volume(sea), start - 2 * layer,
              "volume ledger");
        EQUAL(world.reservoirs->exchanged(ReservoirExchange::Disposal),
              200 - 2 * layer, "external debit recorded once");
        CHECK(chunks.tileAt(x + 1, top - 1, z)->liquid == LiquidType::Lava);
        CHECK(!chunks.isRegistered(newSlab));
        size_t readyCount = 0;
        chunks.forEachReady([&](const ChunkCoord& c, Chunk&) {
            CHECK(c.x >= cx - 1 && c.x <= cx + 1);
            CHECK(c.z >= cz - 1 && c.z <= cz);
            ++readyCount;
        });
        EQUAL(readyCount, size_t(5 + lower.y - cavity.y - 1),
              "transfer does not stream any other slabs");
        SurfaceSampler lowered(world, assets, nullptr, edits.snapshot());
        EQUAL(lowered.waterAt(dry.x, dry.z).surfaceY, top - 2,
              "dry-cell cap follows the lowered reservoir");

        // Commit the new ledger while the region still holds its old snapshot.
        save_catalog::writeReservoirs(slot, *world.reservoirs);
        lodTop(world, top - 2);
    }

    auto reopened = save_catalog::readWorldMap(slot);
    addLedger(*reopened);
    save_catalog::readReservoirs(slot, *reopened->reservoirs);
    disk = region_file::decode(FileIO::readBytes(regionPath));
    ChunkManager chunks(generator, pool, nullptr, *reopened, &assets);
    for (auto c : {upper, lower, west, east, newSlab, cavity}) {
        const auto saved = disk.chunks.find(RegionData::localIndex(c));
        registerSlab(chunks, *reopened, c,
                     saved == disk.chunks.end() ? nullptr : saved->second);
        ready(chunks, c);
    }
    for (auto c : {west, east, newSlab})
        EQUAL(detailedTop(chunks, c, int(c.x) * 16, z), float(top - 2),
              "evicted and new slabs match after restart");
    EQUAL(chunks.tileAt(pocket.x, pocket.y, pocket.z)->liquidLevel, 200,
          "local credit restored alongside the debit");
    EQUAL(chunks.transferWater(sea, 2 * layer, ReservoirExchange::Input),
          2 * layer, "refill to baseline");
    CHECK(chunks.tileAt(dry.x, dry.y, dry.z)->isAir());
    CHECK(chunks.tileAt(x + 1, top - 1, z)->liquid == LiquidType::Lava);
    EQUAL(detailedTop(chunks, upper, x + 2, z), float(top),
          "detailed top refills");
    const TerrainEdits savedEdits(save_catalog::readTerrainEdits(slot).columns);
    SurfaceSampler restoredQuery(*reopened, assets, nullptr,
                                 savedEdits.snapshot());
    EQUAL(restoredQuery.waterAt(dry.x, dry.z).surfaceY, top - 1,
          "saved dry-cell cap survives refill");
    EQUAL(SurfaceSampler(*reopened, assets).waterAt(shoreX, shoreZ).surfaceY,
          top, "unloaded shore refills");
    // Commit a region first. The older water ledger still controls reload.
    chunks.markModified(upper);
    chunks.markModified(lower);
    saveChunks(chunks);
    auto prior = save_catalog::readWorldMap(slot);
    addLedger(*prior);
    save_catalog::readReservoirs(slot, *prior->reservoirs);
    ChunkManager retried(generator, pool, nullptr, *prior, &assets);
    const auto newest = region_file::decode(FileIO::readBytes(regionPath));
    for (auto c : {upper, lower}) {
        registerSlab(retried, *prior, c,
                     newest.chunks.at(RegionData::localIndex(c)));
        ready(retried, c);
    }
    EQUAL(detailedTop(retried, lower, x + 2, z), float(top - 2),
          "restart after saving the region first uses the older ledger");
    CHECK(retried.tileAt(dry.x, dry.y, dry.z)->isAir());
    lodTop(*reopened, top);
}

TEST(hydrology_reservoir_river_budget) {
    auto world = riverLodScene(32);
    // Skipped paths retain their saved IDs, leaving gaps in the reach list.
    world.rivers.insert(world.rivers.begin(), world.rivers.size(),
                        RiverPolyline{});
    const auto generated =
        GeneratedHydrology::build(world, {}, true, true, true);
    const auto& reaches = generated->riverReaches();
    const auto main =
        std::find_if(reaches.begin(), reaches.end(), [](const auto& reach) {
            return reach.end == GeneratedHydrology::RiverEnd::Sea;
        });
    CHECK(main != reaches.end());
    CHECK(main->id > reaches.size());
    const auto sea = GeneratedHydrology::Sea;
    const auto rate = main->spring.unitsPerStep;
    CHECK(rate > 0);
    constexpr int top = WaterSurface::SeaBoundary;
    const auto budgetTest = [&](int area) {
        Reservoirs budget(generated);
        budget.define(sea, StorageCurve::constantArea(area, top - 20, top));
        const auto full = budget.volume(sea);
        CHECK(budget.setReachDelivery(main->id, 0));
        budget.advanceTo(WaterUnitsPerBlock);
        const auto held = budget.held(main->id);
        CHECK(held >= rate * WaterUnitsPerBlock);
        EQUAL(budget.volume(sea), full - held,
              "blocked deliveries stay upstream");
        EQUAL(budget.exchanged(ReservoirExchange::River), 0,
              "no delivery from dam");
        EQUAL(budget.exchanged(ReservoirExchange::Evaporation), -held,
              "background loss continues");

        Reservoirs restored(generated);
        restored.define(sea, StorageCurve::constantArea(area, top - 20, top));
        restored.restore(budget.encode());
        const auto lower = restored.snapshot()->level(sea, top);
        restored.advanceTo(restored.step());
        EQUAL(restored.held(main->id), held, "same step does not repeat input");
        CHECK(restored.setReachDelivery(main->id, 100));
        restored.advanceTo(WaterUnitsPerBlock + 1);
        EQUAL(restored.held(main->id), 0, "previous arrivals spill once");
        EQUAL(restored.volume(sea), full, "returning inflow refills sink");
        constexpr uint32_t alternate = 9000;
        restored.define(alternate, StorageCurve::constantArea(1, 10, 20));
        const auto capacity = restored.volume(alternate);
        EQUAL(restored.transfer(alternate, -capacity,
                                ReservoirExchange::Disposal),
              -capacity, "make room at alternate receiver");
        CHECK(restored.setReachDelivery(main->id, 100, alternate));
        restored.advanceTo(WaterUnitsPerBlock + 2);
        EQUAL(restored.volume(alternate), held / WaterUnitsPerBlock,
              "flow reaches its confirmed alternate receiver");
        EQUAL(restored.volume(sea), full - held / WaterUnitsPerBlock,
              "former sink still loses its background budget");
        return lower;
    };
    CHECK(budgetTest(1) < budgetTest(100));

    const auto lakes = basinWorld();
    const auto lakeHydrology = GeneratedHydrology::build(lakes);
    const auto id = lakeHydrology->candidateBody(First);
    const auto curve = lakeHydrology->storageCurve(id);
    const int level = lakeHydrology->lake(id).surfaceY;
    EQUAL(curve.levelAt(curve.capacity()), level,
          "surveyed storage fills to the spill");
    CHECK(curve.volumeAt(level) - curve.volumeAt(level - 1) >
          curve.volumeAt(level - 1) - curve.volumeAt(level - 2));
}
