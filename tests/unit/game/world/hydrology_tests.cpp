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
#include "world/world_clock.hpp"
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
    const auto coverage = hydrology->refinedLakeCoverage();
    CHECK(coverage.at(First) > 0);
    EQUAL(coverage.at(Dry), 0, "a dry inland bowl has no preview lake");
    const auto& beds = hydrology->riverBeds();
    EQUAL(beds.size(), size_t(1), "generated river baseline");
    EQUAL(beds.front().size(), size_t(2), "bed samples");
    EQUAL(beds.front().back().catchment, id, "river receiving basin");
}

TEST(hydrology_channel_fast_forward_and_plug) {
    Reservoirs water(nullptr), restored(nullptr);
    constexpr uint32_t lake = 42, receiver = 43;
    for (auto* ledger : {&water, &restored}) {
        ledger->define(lake, StorageCurve::constantArea(100, 401, 421));
        ledger->define(receiver, StorageCurve::constantArea(500, 300, 310));
    }
    const auto capacity = water.volume(lake);
    water.transfer(receiver, -water.volume(receiver),
                   ReservoirExchange::Disposal);
    const TilePos mouth{1, 401, 5}, plug{2, 400, 5};
    CHECK(water.setChannelMouth(lake, {receiver, 0, 310}, mouth, mouth.y,
                                {mouth, plug}));
    water.advanceTo(60);
    EQUAL(water.volume(receiver), WaterUnitsPerBlock * 60, "receiver credited");
    EQUAL(water.volume(lake) + water.volume(receiver), capacity,
          "the lake pays for every unit delivered");
    restored.restore(water.encode());
    restored.blockChannelCell(plug);
    restored.advanceTo(7200);
    EQUAL(restored.volume(lake), water.volume(lake), "plug holds on reload");
    water.advanceTo(7200);
    EQUAL(water.volume(lake), 0, "a bottom outlet empties a finite lake");
    EQUAL(water.volume(receiver), capacity, "bulk drainage conserves water");
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
    CHECK(initial.waterAt(shoreX, shoreZ).topFill < 255);
    EQUAL(initial.waterAt(x, z).topFill, 255,
          "the deep sea retains a level surface away from the shoreline");

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
        const auto fractional = SurfaceSampler(world, assets).waterAt(x + 2, z);
        CHECK(fractional.height() < top && fractional.height() > top - 1);
        EQUAL(detailedTop(chunks, upper, x + 2, z), fractional.height(),
              "fractional top agrees in detail and the unloaded sampler");
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

TEST(hydrology_dam_pool_spills_at_crest) {
    auto world = riverLodScene(32);
    const auto generated =
        GeneratedHydrology::build(world, {}, true, true, true);
    const auto& reaches = generated->riverReaches();
    const auto main =
        std::find_if(reaches.begin(), reaches.end(), [](const auto& reach) {
            return reach.end == GeneratedHydrology::RiverEnd::Sea;
        });
    CHECK(main != reaches.end());
    const auto sea = GeneratedHydrology::Sea;
    constexpr int top = WaterSurface::SeaBoundary;
    constexpr uint32_t pool = 9001;  // upstream pool behind the dam

    Reservoirs dam(generated);
    // The pool: one column at the crest height. Constant area is enough to
    // make the crest crossing exact. The sea gets the same constant-area
    // treatment as the river budget fixture so its accounting is exact.
    const auto curve = StorageCurve::constantArea(1, top - 20, top - 4);
    std::vector<Vec2i> footprint{{main->spring.tile}};
    CHECK(dam.definePool(pool, curve, footprint));
    dam.define(sea, StorageCurve::constantArea(100, top - 20, top));
    // The dam closes the generated route; the spill crosses the crest into
    // the sea once the pool tops out.
    CHECK(dam.setReachDelivery(main->id, 0));
    // The crest spills into the sea; the pool behind the dam fills first.
    CHECK(dam.setSpillRoute(main->id, pool, sea, top - 4, 100));
    const auto crest = dam.spillHeight(main->id);
    CHECK(crest.has_value());
    EQUAL(*crest, top - 4, "spill stands at the dam crest");

    dam.advanceTo(WaterUnitsPerBlock);
    // One step's delivery fills the pool toward the crest; the sea is
    // untouched until the pool tops out.
    EQUAL(dam.exchanged(ReservoirExchange::River), 0,
          "nothing crosses while the pool fills");
    CHECK(dam.snapshot()->pools.at(pool).level >= 0);

    dam.advanceTo(2 * WaterUnitsPerBlock);
    // The pool tops the crest and the surplus crosses at the spill rate.
    const auto after = dam.snapshot()->pools.at(pool).level;
    CHECK(after >= 0);
    CHECK(dam.exchanged(ReservoirExchange::River) > 0);
    // The pool level never exceeds the crest while the spill is closed.
    CHECK(dam.snapshot()->pools.at(pool).level <= top - 4);

    // Reopening the route drains the held volume downstream once.
    CHECK(dam.setReachDelivery(main->id, 100));
    dam.advanceTo(3 * WaterUnitsPerBlock);
    EQUAL(dam.held(main->id), 0, "reopening drains the held delivery");
}

TEST(hydrology_portal_drains_pocket_and_refills) {
    const auto world = basinWorld();
    const auto hydrology = GeneratedHydrology::build(world);
    Reservoirs water(hydrology);
    const auto id = hydrology->candidateBody(First);
    // A low receiving body standing below the pocket's column, opened by
    // draining it so the pocket has somewhere to go (a body without a
    // store entry stands at its generated baseline: full).
    water.define(id, StorageCurve::constantArea(2, 400, 406));
    water.transfer(id, -water.volume(id), ReservoirExchange::Disposal);
    EQUAL(water.volume(id), 0, "the receiver starts empty");

    // An enclosed pocket cell one tile above the receiver's floor, so a
    // single band of stored water can top its column for the refill case.
    const TilePos pocket{center(First).x + 2, 401, center(First).y};
    water.creditLocal(pocket, 255, ReservoirExchange::Input);
    CHECK(water.setPortal(pocket, id));
    EQUAL(int(water.snapshot()->cells.at(pocket)), 255,
          "the pocket starts full");

    // The pocket drains through the ledger: each reconcile moves one unit,
    // so repeated small transfers retain their volume exactly. The pocket's
    // volume lands in the reservoir in the same operation.
    for (int i = 0; i < 255; ++i) water.reconcile();
    CHECK(!water.snapshot()->cells.contains(pocket));
    CHECK(water.snapshot()->portals.contains(pocket));
    EQUAL(water.volume(id), WaterUnitsPerBlock,
          "drained volume arrives in the reservoir");

    // Topping the body past the pocket's column reverses the exchange: the
    // pocket refills from the reservoir. One band here holds 510 units, so
    // after a full band the surface stands above the pocket's tile at 401.
    water.transfer(id, 2 * WaterUnitsPerBlock, ReservoirExchange::Input);
    water.reconcile();
    CHECK(water.snapshot()->cells.contains(pocket));
    // Conservation: the ledger's volume moved between cell and body. The
    // refill took 255 from the body into the cell, leaving the input that
    // topped the body (765 total: 255 drained + 510 added).
    EQUAL(water.volume(id) + water.snapshot()->cells.at(pocket),
          3 * WaterUnitsPerBlock, "portal exchange conserves volume");
}

TEST(hydrology_runtime_state_round_trips) {
    const auto world = basinWorld();
    const auto hydrology = GeneratedHydrology::build(world);
    const auto id = hydrology->candidateBody(First);
    Reservoirs water(hydrology);
    water.define(id, StorageCurve::constantArea(2, 400, 406));

    // A portal survives the save: the pocket connection is runtime state.
    const TilePos pocket{center(First).x + 2, 401, center(First).y};
    CHECK(water.setPortal(pocket, id));
    Reservoirs reloaded(hydrology);
    reloaded.restore(water.encode());
    CHECK(reloaded.snapshot()->portals.contains(pocket));

    // An empty ledger keeps restoring under v1 semantics (no new blocks).
    Reservoirs plain(hydrology);
    plain.define(id, StorageCurve::constantArea(2, 400, 406));
    const auto bare = plain.encode();
    Reservoirs bareRestored(hydrology);
    bareRestored.restore(bare);
    CHECK(!bareRestored.snapshot()->portals.contains(pocket));
}

TEST(hydrology_outlet_keys_to_rock_site) {
    auto world = riverLodScene(32);
    const auto generated =
        GeneratedHydrology::build(world, {}, true, true, true);
    Reservoirs water(generated);
    const auto& reach = generated->riverReaches().front();
    const auto tile = reach.spring.tile;
    const int rockY = reach.spring.rockY;

    const TilePos face{tile.x, rockY + 1, tile.y};
    const auto first =
        water.registerOutlet(reach.id, tile, rockY, face, WaterUnitsPerBlock);
    CHECK(first);
    // The same rock through another face or after a reload adopts the key:
    // the flow follows the newest exposed face, never a second source.
    const TilePos otherFace{tile.x + 1, rockY + 1, tile.y};
    EQUAL(water.registerOutlet(reach.id, tile, rockY, otherFace,
                               WaterUnitsPerBlock),
          first, "one rock, one outlet");
    EQUAL(water.outletRate(first), WaterUnitsPerBlock, "open outlet feeds");
    CHECK(water.closeAtRock(tile.x, tile.y, rockY));
    EQUAL(water.outletRate(first), 0, "a sealed rock stops the flow");
    // Reopening cannot duplicate: the registry returns the same outlet.
    EQUAL(water.registerOutlet(reach.id, tile, rockY, face, WaterUnitsPerBlock),
          first, "reopening reuses the key");
    // Round trip: outlets survive the save.
    Reservoirs restored(generated);
    restored.restore(water.encode());
    EQUAL(restored.outletRate(first), WaterUnitsPerBlock,
          "the outlet survives the save");
}

TEST(hydrology_local_flow_conserves_and_freezes_unknown_boundary) {
    Reservoirs water(nullptr);
    const TilePos source{15, 11, 0};
    EQUAL(water.creditLocal(source, 255, ReservoirExchange::Input), 255,
          "one full cell is credited");
    const auto access = [](TilePos p) {
        if (p.y == 10 && p.x == 15) return Reservoirs::CellAccess::Unknown;
        if (p.y == 11 && p.z == 0 && (p.x == 15 || p.x == 16))
            return Reservoirs::CellAccess::Open;
        return Reservoirs::CellAccess::Blocked;
    };
    water.flow(1, access);
    auto state = water.snapshot();
    EQUAL(int(state->cells.at(source)), 223, "sideways transfer is capped");
    EQUAL(int(state->cells.at(TilePos{16, 11, 0})), 32,
          "neighbor receives exactly the debit");
    EQUAL(state->localTop(15, 0), 12, "source column remains wet");
    EQUAL(state->localTop(16, 0), 12, "recipient column becomes wet");
    const auto saved = water.encode();
    Reservoirs reloaded(nullptr);
    reloaded.restore(saved);
    state = reloaded.snapshot();
    EQUAL(state->cells.at(source) + state->cells.at(TilePos{16, 11, 0}), 255,
          "reload preserves exact local volume");
    EQUAL(reloaded.exchanged(ReservoirExchange::Input), 255,
          "flow does not create a second input");
    const auto blocked = [](TilePos p) {
        return p.y == 11 && p.x == 15 && p.z == 0
                   ? Reservoirs::CellAccess::Open
                   : Reservoirs::CellAccess::Unknown;
    };
    reloaded.flow(2, blocked);
    EQUAL(int(reloaded.snapshot()->cells.at(source)), 223,
          "unknown boundaries hold their volume");
}

TEST(hydrology_local_flow_falls_across_slab_and_wrap) {
    Reservoirs water(nullptr);
    EQUAL(water.creditLocal({31, 16, 2}, 255, ReservoirExchange::Input), 255,
          "source is full");
    const auto access = [](TilePos p) {
        if (p.z != 2) return Reservoirs::CellAccess::Blocked;
        if ((p.x == 31 && p.y == 16) || (p.x == 31 && p.y == 15) ||
            (p.x == 0 && p.y == 15))
            return Reservoirs::CellAccess::Open;
        return Reservoirs::CellAccess::Unknown;
    };
    water.flow(1, access, 32);
    const auto state = water.snapshot();
    CHECK(!state->cells.contains({31, 16, 2}));
    EQUAL(int(state->cells.at({31, 15, 2})), 255,
          "downward transfer crosses a slab exactly");
    EQUAL(state->localTop(31, 2), 16, "drained surface follows the lower slab");
    water.flow(1, access, 32);
    const auto after = water.snapshot();
    EQUAL(int(after->cells.at({31, 15, 2})) + int(after->cells.at({0, 15, 2})),
          255, "wrapped transfer conserves the cell");
}

TEST(hydrology_local_flow_prefers_nearest_drop) {
    Reservoirs water(nullptr);
    using Access = Reservoirs::CellAccess;
    const TilePos source{8, 10, 8};
    const TilePos towardDrop{7, 10, 8};
    EQUAL(water.creditLocal(source, 255, ReservoirExchange::Input), 255,
          "start with one cell of water");
    const auto terrain = [](TilePos p) {
        if (p.y == 10 || p == TilePos{6, 9, 8} || p == TilePos{11, 9, 8})
            return Access::Open;
        return Access::Blocked;
    };
    water.flow(1, terrain);
    const auto result = water.snapshot();
    CHECK(result->cells.contains(towardDrop));
    EQUAL(result->cells.size(), 2u, "only one outgoing face is fed");
    EQUAL(int(result->cells.at(source)) + result->cells.at(towardDrop), 255,
          "preferring a drop preserves volume");
}

namespace {
class WaterEditFixture {
   public:
    explicit WaterEditFixture(World& world)
        : world(world),
          chunks(generator, pool, nullptr, world, &assets),
          editor(chunks, nullptr, assets.getStorage<Block>()) {}

    void load(TilePos p, ChunkBlob blob = {}) {
        const ChunkCoord c{unsigned(p.x / 16), unsigned(p.y / 16),
                           unsigned(p.z / 16)};
        if (!chunks.isRegistered(c)) {
            const auto rx = c.x / CHUNKS_PER_REGION;
            const auto rz = c.z / CHUNKS_PER_REGION;
            chunks.registerChunk(c, {rx * 8, 0, rz * 8},
                                 &world.regions[rz * world.width + rx],
                                 std::move(blob));
        }
        for (int i = 0; i < 12 && !chunks.request(c); ++i) {
            chunks.update();
            CHECK(pool.wait_for(std::chrono::seconds(5)));
        }
        CHECK(chunks.isReady(c));
    }

    const Tile& tile(TilePos p) {
        const auto* result = chunks.tileAt(p.x, p.y, p.z);
        CHECK(result);
        return *result;
    }

    World& world;
    test::Assets assets;
    LocationGenerator generator;
    ThreadPool pool{2};
    ChunkManager chunks;
    WorldEditor editor;
};
}  // namespace

TEST(hydrology_editor_certifies_dry_downspout) {
    auto world = riverLodScene(32);
    world.hydrology = GeneratedHydrology::build(world, {}, true, true, true);
    world.reservoirs = std::make_shared<Reservoirs>(world.hydrology);
    auto& water = *world.reservoirs;
    WaterEditFixture f(world);
    const Vec2i site{30 * Side + Side / 2, Side / 2};
    const auto [ground, lake] = world.hydrology->sampleColumn(site.x, site.y);
    CHECK(lake.body == GeneratedHydrology::Sea);
    water.define(lake.body,
                 StorageCurve::constantArea(500, ground, lake.surfaceY));
    const TilePos mouth{site.x, ground, site.y};
    const TilePos bottom{site.x, ground - 5, site.y};
    const TilePos receiver{site.x + 1, bottom.y, site.y};
    const auto dig = [&](TilePos p) {
        f.load(p);
        CHECK(f.editor.removeBlock(p) == EditStatus::Applied);
    };
    f.load({mouth.x, mouth.y + 1, mouth.z});
    dig(receiver);
    for (int y = bottom.y; y <= mouth.y; ++y) dig({mouth.x, y, mouth.z});
    // Seed a lower standing pocket adjoining the tunnel, not the shaft.
    CHECK(water.openBreach(
        world.hydrology->riverReaches().front().id, receiver,
        {receiver.y + 1,
         {{{receiver.x, receiver.z}, receiver.y, receiver.y}}}));
    water.transfer(water.snapshot()->pools.begin()->first, WaterUnitsPerBlock,
                   ReservoirExchange::Input);
    CHECK(f.tile(receiver).waterOrigin == WaterOrigin::Reservoir);
    const auto before = water.volume(lake.body);
    f.chunks.advanceWater(7200);
    CHECK(water.channelCount() > 0);
    CHECK(before - water.volume(lake.body) > 1000 * WaterUnitsPerBlock);
}

TEST(hydrology_editor_connects_generated_water) {
    for (int kind = 0; kind < 3; ++kind) {
        auto world = kind == 2 ? riverLodScene(32) : basinWorld();
        world.hydrology = GeneratedHydrology::build(world, {}, kind == 2);
        world.reservoirs = std::make_shared<Reservoirs>(world.hydrology);
        auto site = center(kind == 0 ? WrappedCoast : First);
        if (kind == 2)
            site = world.hydrology->riverReaches().front().spring.tile;
        WaterEditFixture f(world);
        const auto [ground, baseline] =
            world.hydrology->sampleColumn(site.x, site.y);
        CHECK(baseline.wet());
        const auto before = world.reservoirs->volume(baseline.body);
        const TilePos cut{site.x, ground, site.y};
        const TilePos source{cut.x, cut.y + 1, cut.z};
        f.load(cut);
        f.load(source);
        CHECK(f.tile(source).liquid == LiquidType::Water);
        CHECK(f.editor.removeBlock(cut) == EditStatus::Applied);
        uint64_t tick = 0;
        f.chunks.advanceWater(++tick);
        CHECK(f.tile(cut).waterOrigin == WaterOrigin::Local);
        CHECK(f.tile(cut).liquidLevel > 0);
        const auto after = world.reservoirs->snapshot();
        if (kind != 2) {
            EQUAL(after->cells.at(cut), WaterUnitsPerBlock,
                  "standing water feeds the opening from above");
            EQUAL(world.reservoirs->volume(baseline.body),
                  before - WaterUnitsPerBlock, "the body pays for the fill");
        }
        CHECK(!world.reservoirs->exchanged(ReservoirExchange::Input));
    }
}

TEST(hydrology_editor_water_crosses_ready_boundary) {
    auto world = basinWorld();
    world.hydrology = GeneratedHydrology::build(world, {}, false);
    world.reservoirs = std::make_shared<Reservoirs>(world.hydrology);
    WaterEditFixture f(world);
    // At the sea surface, clear a previously placed block on one side of a
    // chunk seam. Without the source slab, the opening must stay dry.
    const TilePos cut{0, SEA_LEVEL_Y, center(WrappedCoast).y};
    const TilePos west{int(world.wrap().tiles) - 1, cut.y, cut.z};
    f.load(cut);
    CHECK(f.tile(cut).liquid == LiquidType::Water);
    for (auto p : {TilePos{1, cut.y, cut.z}, TilePos{0, cut.y, cut.z - 1},
                   TilePos{0, cut.y, cut.z + 1}}) {
        f.load(p);
        CHECK(f.editor.placeBlock(p, 1) == EditStatus::Applied);
    }
    CHECK(f.editor.placeBlock(cut, 1) == EditStatus::Applied);
    CHECK(f.editor.removeBlock(cut) == EditStatus::Applied);
    f.chunks.advanceWater(1);
    CHECK(f.tile(cut).isAir());
    f.load(west);
    CHECK(f.tile(west).liquid == LiquidType::Water);
    f.chunks.advanceWater(2);
    EQUAL(f.tile(cut).liquidLevel, 255, "wet face activates across X wrap");
    const ChunkCoord c{0, unsigned(cut.y / 16), unsigned(cut.z / 16)};
    const auto mesh = ChunkMesher(f.assets).build(
        *f.chunks.getChunk(c), c,
        [&](int x, int y, int z) { return f.chunks.tileAt(x, y, z); });
    CHECK(!mesh.waterTops.empty());
    EQUAL(mesh.waterTops[(cut.z % 16) * 16], float(cut.y % 16 + 1),
          "detailed mesh shows water in the cut");

    // Drain a separately credited full cell down into a sealed hole. A
    // vacated local column/slab must not retain a stale water tile.
    const TilePos air{cut.x + 4, (cut.y / 16 + 1) * 16, cut.z};
    const TilePos below{air.x, air.y - 1, air.z};
    f.load(air);
    f.load(below);
    EQUAL(world.reservoirs->creditLocal(air, 255, ReservoirExchange::Input),
          255, "credit air above the sea");
    CHECK(f.tile(air).isLiquid());
    f.chunks.flowWater(512);
    CHECK(f.tile(air).isAir());
    CHECK(f.tile(below).isLiquid());
}

TEST(hydrology_editor_dam_backs_up_river) {
    auto world = riverLodScene(32);
    world.hydrology = GeneratedHydrology::build(world, {}, true, false, true);
    world.reservoirs = std::make_shared<Reservoirs>(world.hydrology);
    WaterEditFixture f(world);
    SurfaceSampler sampler(world, f.assets);
    const auto& main = world.hydrology->riverReaches().front();
    const int x = main.spring.tile.x - 24;
    const int z = main.spring.tile.y;
    const auto middle = sampler.waterAt(x, z);
    CHECK(middle.reach == main.id && middle.flow.x < 0);
    std::vector<TilePos> wall;
    for (int dz = -48; dz <= 48; ++dz) {
        const auto c = sampler.column(x, z + dz);
        if (c.water.reach != main.id) continue;
        for (int y = c.surfaceY + 1; y < c.water.surfaceY; ++y)
            wall.push_back({x, y, z + dz});
    }
    CHECK(wall.size() > 1);
    const auto open = wall.back();
    wall.pop_back();
    for (const auto p : wall) {
        f.load(p);
        CHECK(f.editor.placeBlock(p, 1) == EditStatus::Applied);
    }
    f.load(open);
    const auto upstream = sampler.waterAt(x + 1, z);
    const TilePos backedUp{x + 1, upstream.surfaceY, z};
    f.load(backedUp);
    f.load({backedUp.x, backedUp.y - 1, backedUp.z});
    f.chunks.advanceWater(1);
    CHECK(world.reservoirs->snapshot()->cells.empty());
    CHECK(f.editor.placeBlock(open, 1) == EditStatus::Applied);
    f.chunks.advanceWater(2);
    const auto pond = world.reservoirs->snapshot();
    CHECK(world.reservoirs->damAt(main.id));
    EQUAL(pond->pools.size(), 1u, "one certified upstream pool");
    const auto pool = pond->pools.begin()->first;
    CHECK(world.reservoirs->volume(pool) > 0);
    const auto receiver = sampler.waterAt(x - 1, z);
    sampler.setReservoirs(pond);
    CHECK(sampler.waterAt(x - 1, z).height() < receiver.height());
    CHECK(std::any_of(
        pond->pools.at(pool).footprint->columns.begin(),
        pond->pools.at(pool).footprint->columns.end(), [&](const auto& column) {
            const int px = int(column.first >> 32);
            const int pz = int(uint32_t(column.first));
            return sampler.waterAt(px, pz).height() > column.second.first;
        }));
    std::set<ChunkCoord, decltype([](ChunkCoord a, ChunkCoord b) {
                 return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
             })>
        slabs;
    wall.push_back(open);
    for (const auto p : wall)
        slabs.insert(
            {unsigned(p.x / 16), unsigned(p.y / 16), unsigned(p.z / 16)});
    const ChunkCoord wallSlab{unsigned(open.x / 16), unsigned(open.y / 16),
                              unsigned(open.z / 16)};
    const auto wallBlob = std::make_shared<const Bytes>(
        chunk_codec::encode(*f.chunks.getChunk(wallSlab)));
    for (const auto c : slabs) f.chunks.unregister(c);
    Reservoirs restored(world.hydrology);
    restored.restore(world.reservoirs->encode());
    CHECK(restored.damAt(main.id));
    f.chunks.advanceWater(86400, std::chrono::seconds::zero());
    EQUAL(world.reservoirs->step(), 86400u, "unloaded dam still advances");
    sampler.setReservoirs(world.reservoirs->snapshot());
    EQUAL(sampler.waterAt(x - 1, z).height(), receiver.height(),
          "delivery resumes over the crest");
    CHECK(world.reservoirs->volume(pool) > 0);
    f.load(open, wallBlob);
    CHECK(f.editor.removeBlock(open) == EditStatus::Applied);
    f.chunks.advanceWater(86400 + 120);
    CHECK(!world.reservoirs->damAt(main.id));
    EQUAL(world.reservoirs->volume(pool), 0,
          "opening a wall drains previously stored head");
    CHECK(!world.reservoirs->exchanged(ReservoirExchange::Input));
}

TEST(hydrology_broken_bank_fills_unloaded_valley) {
    auto world = riverLodScene(32);
    world.hydrology = GeneratedHydrology::build(world, {}, true, false, true);
    world.reservoirs = std::make_shared<Reservoirs>(world.hydrology);
    WaterEditFixture f(world);
    SurfaceSampler terrain(world, f.assets);
    const auto& reach = world.hydrology->riverReaches().front();
    TilePos gate{};
    TilePos upstream{};
    int direction = 0;
    constexpr int Length = 40;
    for (int k = 8; !direction && k < 160; ++k) {
        const int x = terrain.wrapX(reach.spring.tile.x - k);
        for (int side : {-1, 1}) {
            for (int offset = 1; !direction && offset <= 32; ++offset) {
                const int z = reach.spring.tile.y + side * offset;
                const auto river = terrain.waterAt(x, z - side);
                if (river.reach != reach.id || terrain.waterAt(x, z).wet() ||
                    terrain.height(x, z) != river.topTile())
                    continue;
                bool dry = true;
                for (int d = 1; d <= Length; ++d)
                    for (int across = -2; across <= 2; ++across)
                        dry &= !terrain.waterAt(x + across, z + side * d).wet();
                if (!dry) continue;
                gate = {x, river.topTile(), z};
                upstream = {x, river.topTile(), z - side};
                direction = side;
            }
        }
    }
    CHECK(direction);
    const int head = gate.y + 1;
    // Supply the saved height summaries of a formerly dug valley. Only the
    // last bank block is edited in this session. The far valley is never
    // registered with ChunkManager, including during the two-day skip.
    std::map<uint64_t, std::shared_ptr<ColumnEdit>> edits;
    for (int d = 0; d <= Length + 1; ++d)
        for (int across = -3; across <= 3; ++across) {
            const int x = gate.x + across, z = gate.z + d * direction;
            const auto key = editKey(unsigned(x / 16), unsigned(z / 16));
            auto& edit = edits[key];
            if (!edit) {
                edit = std::make_shared<ColumnEdit>();
                edit->waterY.fill(ColumnEdit::GeneratedWater);
                for (int dz = 0; dz < 16; ++dz)
                    for (int dx = 0; dx < 16; ++dx)
                        edit->groundY[ColumnEdit::index(dx, dz)] =
                            terrain.height(int(key >> 32) * 16 + dx,
                                           int(uint32_t(key)) * 16 + dz);
            }
            if (!terrain.waterAt(x, z).wet()) {
                const bool bowl = d > 0 && d <= Length && std::abs(across) <= 2;
                edit->groundY[ColumnEdit::index(x % 16, z % 16)] =
                    bowl ? head - 8 : head;
            }
        }
    EditMap summaries;
    for (const auto& [key, edit] : edits) summaries.emplace(key, edit);
    TerrainEdits saved(std::move(summaries));
    f.chunks.setWaterTerrainEdits(&saved);
    const TilePos valley{gate.x, head - 7, gate.z + Length * direction};
    const Vec2i downstream{upstream.x - 8, upstream.z};
    const auto river = terrain.waterAt(downstream.x, downstream.y);
    CHECK(river.reach == reach.id);
    f.load(gate);
    f.load(upstream);
    const TilePos dryEdit{gate.x, head + 32, gate.z};
    f.load(dryEdit);
    CHECK(f.editor.placeBlock(dryEdit, 1) == EditStatus::Applied);
    CHECK(f.editor.removeBlock(dryEdit) == EditStatus::Applied);
    CHECK(!f.chunks.tileAt(valley.x, valley.y, valley.z));
    CHECK(f.editor.removeBlock(gate) == EditStatus::Applied);
    f.chunks.advanceWater(120);
    CHECK(world.reservoirs->hasBreach(gate));
    const auto snapshot = world.reservoirs->snapshot();
    EQUAL(snapshot->pools.size(), 1u, "one valley lake");
    const auto pool = snapshot->pools.begin()->first;
    const auto volume = world.reservoirs->volume(pool);
    CHECK(volume > 0);
    CHECK(!f.chunks.tileAt(valley.x, valley.y, valley.z));
    SurfaceSampler wet(world, f.assets, nullptr, saved.snapshot(),
                       SurfaceSampler::WaterDetail::Exact, snapshot);
    CHECK(wet.waterAt(valley.x, valley.z).body == pool);
    CHECK(wet.waterAt(downstream.x, downstream.y).height() < river.height());
    Reservoirs uninterrupted(world.hydrology);
    uninterrupted.advanceTo(120);
    EQUAL(uninterrupted.exchanged(ReservoirExchange::River) -
              world.reservoirs->exchanged(ReservoirExchange::River),
          volume, "the new lake receives exactly what the old receiver lost");

    Reservoirs stepped(world.hydrology);
    stepped.restore(world.reservoirs->encode());
    EQUAL(stepped.volume(pool), volume, "saved lake retains its volume");
    for (uint64_t step = 121; step <= 720; ++step) stepped.advanceTo(step);
    f.chunks.advanceWater(720, std::chrono::seconds::zero());
    EQUAL(world.reservoirs->step(), 720u,
          "valley advances without cell replay");
    EQUAL(world.reservoirs->volume(pool), stepped.volume(pool),
          "bulk and one-second splits deliver the same lake volume");
    constexpr uint64_t TwoDays = 2 * 86400;
    f.chunks.advanceWater(TwoDays, std::chrono::seconds::zero());
    EQUAL(world.reservoirs->step(), TwoDays,
          "two-day skip needs no chunk work");
    CHECK(!f.chunks.tileAt(valley.x, valley.y, valley.z));
    wet.setReservoirs(world.reservoirs->snapshot());
    EQUAL(wet.waterAt(valley.x, valley.z).surfaceY, head,
          "valley retains a lake up to the inlet head");
    const TerrainLod::Patch patch{valley.x / 16 * 16, valley.z / 16 * 16, 1};
    const auto mesh = TerrainLod::build(patch, wet, std::atomic_bool{false}, 4);
    CHECK(
        std::any_of(mesh.water.vertices.begin(), mesh.water.vertices.end(),
                    [&](const auto& v) { return v.ny > .9f && v.y == head; }));
    EQUAL(wet.waterAt(downstream.x, downstream.y).height(), river.height(),
          "a full terminal valley stops taking net flow from the river");
    const auto full = world.reservoirs->volume(pool);
    CHECK(f.editor.placeBlock(gate, 1) == EditStatus::Applied);
    f.chunks.advanceWater(TwoDays + 60);
    CHECK(!world.reservoirs->hasBreach(gate));
    EQUAL(world.reservoirs->volume(pool), full,
          "plugging the inlet does not delete the lake it created");
}

TEST(hydrology_editor_seals_spring_and_restores_river) {
    auto world = riverLodScene(32);
    world.hydrology = GeneratedHydrology::build(world, {}, true, false, true);
    world.reservoirs = std::make_shared<Reservoirs>(world.hydrology);
    WaterEditFixture f(world);
    const auto& reach = world.hydrology->riverReaches().front();
    const TilePos mouth{reach.spring.tile.x, reach.spring.rockY + 1,
                        reach.spring.tile.y};
    const TilePos downstream{mouth.x - 24, mouth.y, mouth.z};
    SurfaceSampler s(world, f.assets);
    const auto original = s.waterAt(downstream.x, downstream.z);
    CHECK(original.reach == reach.id);
    f.load(mouth);
    CHECK(f.editor.placeBlock(mouth, 1) == EditStatus::Applied);
    f.chunks.advanceWater(1);
    s.setReservoirs(world.reservoirs->snapshot());
    CHECK(!s.waterAt(downstream.x, downstream.z).wet());
    Reservoirs restored(world.hydrology);
    restored.restore(world.reservoirs->encode());
    CHECK(restored.snapshot()->riverStages.at(reach.id).percent == 0);
    CHECK(f.editor.removeBlock(mouth) == EditStatus::Applied);
    f.chunks.advanceWater(2);
    s.setReservoirs(world.reservoirs->snapshot());
    EQUAL(s.waterAt(downstream.x, downstream.z).height(), original.height(),
          "removing the plug restores the original river");
    CHECK(world.reservoirs->snapshot()->cells.empty());

    const TilePos ordinary{downstream.x, original.topTile(), downstream.z};
    f.load(ordinary);
    CHECK(f.editor.placeBlock(ordinary, 1) == EditStatus::Applied);
    f.chunks.advanceWater(3);
    CHECK(!world.reservoirs->damAt(reach.id));
    CHECK(f.editor.removeBlock(ordinary) == EditStatus::Applied);
    f.chunks.advanceWater(4);
    EQUAL(f.tile(ordinary).liquidLevel, 255, "the ordinary river tile refills");
    CHECK(world.reservoirs->snapshot()->cells.empty());
    CHECK(world.reservoirs->snapshot()->pools.empty());
}

TEST(hydrology_editor_reopens_saved_cut) {
    auto world = basinWorld();
    world.hydrology = GeneratedHydrology::build(world, {}, false);
    world.reservoirs = std::make_shared<Reservoirs>(world.hydrology);
    WaterEditFixture f(world);
    const auto site = center(First);
    const auto [ground, baseline] =
        world.hydrology->sampleColumn(site.x, site.y);
    CHECK(baseline.body > GeneratedHydrology::Sea);
    const TilePos cut{site.x, ground, site.y};
    const TilePos source{site.x, ground + 1, site.y};
    const ChunkCoord c{unsigned(cut.x / 16), unsigned(cut.y / 16),
                       unsigned(cut.z / 16)};
    f.load(cut);
    f.load(source);
    CHECK(f.editor.removeBlock(cut) == EditStatus::Applied);
    auto saved = *f.chunks.getChunk(c);
    // Simulate a slab saved before reservoir eligibility was recorded.
    saved.projectsWater = false;
    for (auto& plane : saved.tiles)
        for (auto& row : plane)
            for (auto& tile : row) tile.waterOrigin = WaterOrigin::Independent;
    f.chunks.unregister(c);
    f.load(cut, std::make_shared<const Bytes>(chunk_codec::encode(saved)));
    CHECK(!f.chunks.getChunk(c)->projectsWater);
    CHECK(f.tile(cut).isAir());
    f.chunks.advanceWater(1);
    CHECK(f.tile(cut).waterOrigin == WaterOrigin::Local);
    CHECK(f.tile(cut).liquidLevel > 0);
}

TEST(hydrology_time_speed_consumes_the_same_water_steps) {
    constexpr uint64_t End = 120;
    constexpr auto Unlimited = std::chrono::steady_clock::duration::max();
    const auto run = [&](double speed) {
        auto world = riverLodScene(32);
        world.hydrology = GeneratedHydrology::build(world);
        world.reservoirs = std::make_shared<Reservoirs>(world.hydrology);
        WaterEditFixture f(world);
        const auto site = world.hydrology->riverReaches().front().spring.tile;
        const auto [ground, baseline] =
            world.hydrology->sampleColumn(site.x, site.y);
        CHECK(baseline.reach);
        f.load({site.x, ground + 1, site.y});
        for (int y = ground; y >= ground - 20; --y) {
            const TilePos cut{site.x, y, site.y};
            f.load(cut);
            CHECK(f.editor.removeBlock(cut) == EditStatus::Applied);
        }

        WorldClock clock;
        clock.setSpeed(speed);
        uint64_t target = 0;
        for (int frame = 0; frame < 1000 && target < End; ++frame) {
            clock.advance(1.0 / 60);
            target = std::min(End, uint64_t(clock.days() * 86400));
            const auto before = world.reservoirs->step();
            const auto budget =
                speed == 720 ? std::chrono::seconds::zero() : Unlimited;
            f.chunks.advanceWater(target, budget);
            CHECK(world.reservoirs->step() - before <=
                  ChunkManager::MaxWaterStepsPerUpdate);
        }
        EQUAL(target, End, "every clock reaches the same target");
        if (speed == 720)
            EQUAL(
                world.reservoirs->step(), 1u,
                "high speed leaves pending water time instead of skipping it");
        // Catch up at a fixed clock reading, without accruing any more time.
        for (int frame = 0; frame < 8 && world.reservoirs->step() < End;
             ++frame)
            f.chunks.advanceWater(target, Unlimited);
        EQUAL(world.reservoirs->step(), End, "pending time is processed");
        CHECK(f.tile({site.x, ground - 20, site.y}).isLiquid());
        const auto unchanged = world.reservoirs->revision();
        f.chunks.advanceWater(target, Unlimited);
        EQUAL(world.reservoirs->revision(), unchanged,
              "no water steps without elapsed game time or a backlog");
        // Includes per-cell fills, basin storage, and the river volume ledger.
        return world.reservoirs->encode();
    };
    const auto normal = run(1);
    CHECK(run(60) == normal);
    CHECK(run(720) == normal);
}

TEST(hydrology_water_time_budget_and_coarse_fast_forward) {
    auto world = basinWorld();
    world.hydrology = GeneratedHydrology::build(world, {}, false);
    world.reservoirs = std::make_shared<Reservoirs>(world.hydrology);
    WaterEditFixture f(world);
    f.chunks.advanceWater(1000, std::chrono::seconds::zero());
    EQUAL(world.reservoirs->step(), 1000u,
          "a coarse-only world does not accrue cell simulation backlog");
    const auto site = center(First);
    const TilePos cut{
        site.x, world.hydrology->sampleColumn(site.x, site.y).first, site.y};
    f.load(cut);
    CHECK(f.editor.removeBlock(cut) == EditStatus::Applied);
    f.chunks.advanceWater(1100, std::chrono::seconds::zero());
    EQUAL(world.reservoirs->step(), 1001u,
          "a spent real-time budget still makes one whole step of progress");
    Reservoirs reloaded(world.hydrology);
    reloaded.restore(world.reservoirs->encode());
    EQUAL(reloaded.step(), 1001u, "save retains only the time simulated");
    f.chunks.advanceWater(2000, std::chrono::steady_clock::duration::max());
    EQUAL(world.reservoirs->step(), 2000u,
          "a large jump uses a bounded bulk update, not a step backlog");
}

TEST(hydrology_split_lake_drains_channel_at_7200) {
    auto world = basinWorld();
    world.hydrology = GeneratedHydrology::build(world, {}, false);
    world.reservoirs = std::make_shared<Reservoirs>(world.hydrology);
    auto& water = *world.reservoirs;
    WaterEditFixture f(world);
    const Vec2i site = center(First);
    const auto [ground, lake] = world.hydrology->sampleColumn(site.x, site.y);
    CHECK(lake.body > GeneratedHydrology::Sea && lake.depth(ground) > 0);
    // Open a receiving tunnel below the lake, then wall off the 5x5 section
    // once the original lake has already started feeding the outlet.
    const TilePos mouth{site.x, ground, site.y};
    std::vector<TilePos> wall;
    int floor = ground;
    for (int dz = -3; dz <= 3; ++dz)
        for (int dx = -3; dx <= 3; ++dx) {
            const int x = site.x + dx, z = site.y + dz;
            const auto [bed, s] = world.hydrology->sampleColumn(x, z);
            EQUAL(s.body, lake.body, "wall is inside the original lake");
            floor = std::min(floor, bed);
            for (int y = bed; y < lake.surfaceY; ++y) {
                const TilePos p{x, y, z};
                f.load(p);
                if (y > bed && (std::abs(dx) == 3 || std::abs(dz) == 3))
                    wall.push_back(p);
            }
        }
    const int tunnelY = floor - 8;
    const auto dig = [&](TilePos p) {
        f.load(p);
        CHECK(f.editor.removeBlock(p) == EditStatus::Applied);
    };
    for (int y = ground - 1; y > tunnelY; --y) dig({site.x, y, site.y});
    for (int i = 0; i < 160; ++i)
        for (int dz = 0; dz < 5; ++dz) dig({site.x + i, tunnelY, site.y + dz});
    dig(mouth);
    const auto original = water.volume(lake.body);
    f.chunks.advanceWater(1);
    CHECK(water.snapshot()->pools.empty());
    const auto before = water.volume(lake.body);
    CHECK(before < original);
    CHECK(!f.tile({site.x + 8, tunnelY, site.y}).isLiquid());
    for (const auto p : wall)
        CHECK(f.editor.placeBlock(p, 1) == EditStatus::Applied);
    f.chunks.commitWaterEdits();
    const auto split = water.snapshot();
    EQUAL(split->pools.size(), 1u, "the wall creates a separate reservoir");
    const auto id = split->pools.begin()->first;
    EQUAL(split->pools.at(id).footprint->columns.size(), 25u,
          "only the enclosed 5 by 5 is detached");
    const auto initial = water.volume(id);
    EQUAL(water.volume(lake.body) + initial, before,
          "splitting moves existing water rather than creating it");

    WorldClock clock;
    clock.setSpeed(7200);
    clock.advance(1.0 / 60);
    const uint64_t target = uint64_t(clock.days() * 86400);
    CHECK(target > ChunkManager::MaxWaterStepsPerUpdate);
    f.chunks.advanceWater(target, std::chrono::seconds::zero());
    EQUAL(water.step(), target, "7200x advances the water clock in one call");
    CHECK(water.volume(id) < initial);
    CHECK(water.snapshot()->pools.at(id).level <= mouth.y);
    CHECK(f.tile({site.x + 8, tunnelY, site.y}).isLiquid());
    EQUAL(water.volume(lake.body), before - initial,
          "the cut cannot draw from the other side of the wall");
    WaterVolume delivered = 0;
    for (const auto& [p, fill] : water.snapshot()->cells) delivered += fill;
    EQUAL(delivered + water.volume(id), initial + original - before,
          "the fast-forward keeps all the drained water in the channel");
    Reservoirs restored(world.hydrology);
    restored.restore(water.encode());
    EQUAL(restored.volume(id), water.volume(id),
          "the isolated volume survives reload");
    CHECK(!restored.snapshot()
               ->projectStanding(world.hydrology.get(), site.x, site.y, ground,
                                 lake)
               .wet());
}

TEST(hydrology_river_fractional_top_matches_detail) {
    auto world = riverLodScene(32);
    world.hydrology = GeneratedHydrology::build(world, {}, true, true, true);
    world.reservoirs = std::make_shared<Reservoirs>(world.hydrology);
    WaterEditFixture f(world);
    const auto head = world.hydrology->riverReaches().front().spring.tile;
    SurfaceSampler surface(world, f.assets);
    std::set<int> topFills;
    for (int i = 4; i < 18; ++i) {
        const auto s = surface.waterAt(head.x - i, head.y);
        if (!s.reach) continue;
        CHECK(s.topFill > 0 && s.topFill < 255);
        CHECK(s.flow.x != 0 || s.flow.y != 0);
        const TilePos p{head.x - i, s.topTile(), head.y};
        f.load(p);
        EQUAL(f.tile(p).liquidLevel, s.topFill,
              "the loaded top matches the terrain water sampler");
        topFills.insert(s.topFill);
    }
    CHECK(topFills.size() > 1);
}
