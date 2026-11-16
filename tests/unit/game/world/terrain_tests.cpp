#include <chrono>
#include <climits>

#include "world/location_manager.hpp"
#include "world/terrain_sampler.hpp"
#include "world_fixture.hpp"

namespace {
World terrainWorld(int width = 5, int height = 5, int ceiling = 420,
                   LandType land = LandType::PLAINS) {
    World world{};
    world.width = width;
    world.height = height;
    world.genSettings.height_seed = 1234;
    world.genSettings.feature_seed = 5678;
    world.regions.resize(width * height);
    for (int z = 0; z < height; ++z)
        for (int x = 0; x < width; ++x) {
            auto& r = world.regions[z * width + x];
            r = test::regionAt(ceiling);
            r.coord = {unsigned(x), unsigned(z)};
            r.land = land;
            r.vegetation = VegetationType::GRASS;
            r.humidity = .5;
            r.temperature_c = 15;
        }
    return world;
}
void close(double a, double b, double tolerance = 1e-6) {
    CHECK(std::abs(a - b) <= tolerance);
}
void addProfiles(TerrainNeighborhood& data, bool restrictSecond = false) {
    for (auto& value : data.regions) {
        const bool second = value.region.coord.x >= 2;
        value.region.addFeature(FeatureMask::DESERT_DUNES);
        value.soil = {uint8_t(second ? 2 : 1),
                      uint8_t(second ? 3 : 2),
                      uint8_t(second ? 2 : 3),
                      {4, 4},
                      {1, 1},
                      42};
        value.soil_rules = {SoilPlacement{}};
        if (second && restrictSecond) {
            value.soil_rules[0].required_features = {FeatureMask::SWAMP};
            value.soil_rules[0].priority = 10;
        }
    }
}
}  // namespace

TEST(terrain_caps_repeatability) {
    test::Assets assets;
    auto world = terrainWorld();
    for (auto& r : world.regions)
        r.land_height_m += int(r.coord.x) * 13 - int(r.coord.y) * 7;
    auto a = TerrainNeighborhood::capture(world, {2, 2}, assets);
    auto b = TerrainNeighborhood::capture(world, {2, 2}, assets);
    TerrainSampler first(a), again(b);
    int changed = 0;
    for (int z = 256; z < 384; z += 3)
        for (int x = 256; x < 384; x += 3) {
            const auto value = first.sample(x, z);
            CHECK(value.surface_y <= world.regions[12].land_height_m);
            CHECK(value.surface_y > SEA_LEVEL_Y);
            EQUAL(value.surface_y, again.sample(x, z).surface_y,
                  "repeatability");
            if (value.surface_y != world.regions[12].land_height_m) ++changed;
        }
    CHECK(changed > 0);
    b.height_seed += 1;
    CHECK(first.sample(300, 300).surface_y !=
              TerrainSampler(b).sample(300, 300).surface_y ||
          first.surface(311, 303) != TerrainSampler(b).surface(311, 303));
}

TEST(terrain_shared_edges_corners) {
    test::Assets assets;
    auto world = terrainWorld();
    for (auto& r : world.regions)
        r.land_height_m += int(r.coord.x) * 12 + int(r.coord.y) * 7;
    for (unsigned z = 1; z <= 2; ++z)
        for (unsigned x = 1; x <= 2; ++x) {
            const auto a = TerrainNeighborhood::capture(world, {x, z}, assets);
            const auto b =
                TerrainNeighborhood::capture(world, {x + 1, z}, assets);
            const auto c =
                TerrainNeighborhood::capture(world, {x, z + 1}, assets);
            const TerrainSampler sa(a), sb(b), sc(c);
            for (int offset = 0; offset <= 128; offset += 8) {
                const double bx = (x + 1) * 128;
                const double bz = z * 128 + offset;
                close(sa.surface(bx, bz), sb.surface(bx, bz));
                close(sa.surface(bx - .0001, bz), sb.surface(bx + .0001, bz),
                      .001);
                const double hx = x * 128 + offset;
                const double hz = (z + 1) * 128;
                close(sa.surface(hx, hz), sc.surface(hx, hz));
                close(sa.surface(hx, hz - .0001), sc.surface(hx, hz + .0001),
                      .001);
            }
            CHECK(sa.surface(x * 128 + 100, z * 128 + 64) >
                  sa.surface(x * 128 + 8, z * 128 + 64));
        }
}

TEST(terrain_relief_strength) {
    test::Assets assets;
    const auto amplitude = [&](LandType land, int maximum) {
        auto world = terrainWorld(5, 5, 450, land);
        const auto data = TerrainNeighborhood::capture(world, {2, 2}, assets);
        TerrainSampler sampler(data);
        double lo = 450, hi = 0;
        for (int z = 256; z < 384; z += 4)
            for (int x = 256; x < 384; x += 4) {
                const double y = sampler.surface(x, z);
                CHECK(y >= 450 - maximum && y <= 450);
                lo = std::min(lo, y);
                hi = std::max(hi, y);
            }
        CHECK(hi - lo > .5);
        return hi - lo;
    };
    const double plain = amplitude(LandType::PLAINS, 3);
    const double hill = amplitude(LandType::HILLS, 12);
    const double mountain = amplitude(LandType::MOUNTAINS, 32);
    CHECK(hill > plain && mountain > hill);
}

TEST(terrain_cliff_segments) {
    test::Assets assets;
    auto world = terrainWorld();
    for (auto& r : world.regions) r.land_height_m = r.coord.x < 2 ? 400 : 448;
    auto smoothData = TerrainNeighborhood::capture(world, {2, 2}, assets);
    world.regions[12].addFeature(FeatureMask::CLIFF);
    auto cliffData = TerrainNeighborhood::capture(world, {2, 2}, assets);
    auto neighbor = TerrainNeighborhood::capture(world, {1, 2}, assets);
    TerrainSampler smoothSurface(smoothData), cliffs(cliffData),
        other(neighbor);
    int drops = 0;
    for (int z = 272; z < 368; ++z) {
        CHECK(std::abs(smoothSurface.surface(256, z) -
                       smoothSurface.surface(255, z)) < 1);
        if (cliffs.surface(256, z) - cliffs.surface(255, z) > 5) ++drops;
        close(cliffs.surface(256, z), other.surface(256, z));
        CHECK(cliffs.sample(256, z).surface_y <= 448);
    }
    CHECK(drops > 0);
    close(cliffs.surface(256, 256), smoothSurface.surface(256, 256));
    world.regions[12].features = FeatureMask::RAVINE;
    auto ravine = TerrainNeighborhood::capture(world, {2, 2}, assets);
    CHECK(TerrainSampler(ravine).surface(256, 320) >=
          smoothSurface.surface(256, 320));
}

TEST(terrain_coast_dry_inland) {
    test::Assets assets;
    auto world = terrainWorld(5, 5, SEA_LEVEL_Y + 1, LandType::MOUNTAINS);
    auto inland = TerrainNeighborhood::capture(world, {2, 2}, assets);
    TerrainSampler dry(inland);
    for (int z = 256; z < 384; z += 7)
        for (int x = 256; x < 384; x += 7)
            CHECK(dry.sample(x, z).surface_y == SEA_LEVEL_Y + 1);
    for (auto& r : world.regions)
        if (r.coord.x < 2) {
            r.land_height_m = SEA_LEVEL_Y - 12;
            r.land = LandType::WATER;
        }
    auto coast = TerrainNeighborhood::capture(world, {2, 2}, assets);
    TerrainSampler wet(coast);
    CHECK(wet.sample(256, 320).surface_y < SEA_LEVEL_Y);
    CHECK(wet.sample(288, 320).surface_y > SEA_LEVEL_Y);
    CHECK(wet.sample(310, 320).surface_y > SEA_LEVEL_Y);
    auto water = TerrainNeighborhood::capture(world, {1, 2}, assets);
    close(wet.surface(256, 320), TerrainSampler(water).surface(256, 320));
    const auto columns = wet.columns(16, 20);
    const std::vector<RegionLayer> rocks = {
        {RegionLayerType::Rock, 1, 1024, 0, 0, 1}};
    Chunk slab;
    LocationGenerator{}.generateChunk(slab, {16, 24, 20}, {}, world.regions[12],
                                      rocks, {}, &columns);
    CHECK(slab.tiles[0][0][0].liquid == LiquidType::Water);
    CHECK(slab.tiles[0][1][0].isAir());
}

TEST(terrain_world_bounds) {
    test::Assets assets;
    auto world = terrainWorld(1, 1);
    const auto data = TerrainNeighborhood::capture(world, {0, 0}, assets);
    TerrainSampler sampler(data);
    for (const auto [x, z] : std::array<std::pair<int, int>, 4>{
             {{0, 0}, {127, 0}, {0, 127}, {127, 127}}}) {
        CHECK(sampler.sample(x, z).surface_y <= 420);
        CHECK(sampler.sample(x, z).surface_y > SEA_LEVEL_Y);
    }
    close(sampler.surface(-1, 64), sampler.surface(0, 64));
    close(sampler.surface(128, 64), sampler.surface(127, 64));
    LocationGenerator generator;
    ThreadPool pool(1);
    ChunkManager chunks(generator, pool, nullptr, world, &assets);
    LocationManager locations(chunks, world);
    locations.loadRegion({0, 0}, 2);
    CHECK(chunks.isRegistered({0, 0, 0}));
    CHECK(chunks.isRegistered({7, 47, 7}));
    CHECK(!chunks.isRegistered({8, 0, 0}));
    CHECK(!chunks.isRegistered({0, 0, 8}));
    locations.unloadDistant({4, 4}, 0);
    CHECK(chunks.cachedColumnInputs() == 0);
}

TEST(terrain_geology_erosion) {
    auto r = test::regionAt(420);
    ChunkColumnInputs inputs;
    for (auto& c : inputs.columns) {
        c.surface_y = 410;
        c.soil = {{{{RegionLayerType::Soil, 3, 1, 0, 0, 1, true},
                    {RegionLayerType::Soil, 2, 2, 1, 0, 1, false}}},
                  2,
                  3};
    }
    const std::vector<RegionLayer> layers = {
        {RegionLayerType::Rock, 3, 5, 0, .7f, 1},
        {RegionLayerType::Rock, 1, 20, 5, .2f, 1},
        {RegionLayerType::Rock, 2, 1000, 25, 0, 1}};
    Chunk upper, lower;
    LocationGenerator gen;
    gen.generateChunk(upper, {0, 25, 0}, {}, r, layers, {}, &inputs);
    gen.generateChunk(lower, {0, 24, 0}, {}, r, layers, {}, &inputs);
    CHECK(upper.tiles[0][11][0].isAir());
    EQUAL(int(upper.tiles[0][10][0].block_id), 3, "topsoil follows surface");
    EQUAL(int(upper.tiles[0][9][0].block_id), 2, "subsoil");
    EQUAL(int(upper.tiles[0][7][0].block_id), 1, "eroded shallow rock");
    EQUAL(int(lower.tiles[0][12][0].block_id), 1,
          "original deep rock elevation");
    EQUAL(int(lower.tiles[0][11][0].block_id), 2, "deep boundary preserved");
    CHECK(layers[0].aquifer_saturation == .7f);
}

TEST(terrain_soil_transition) {
    test::Assets assets;
    auto world = terrainWorld();
    auto left = TerrainNeighborhood::capture(world, {1, 2}, assets);
    auto right = TerrainNeighborhood::capture(world, {2, 2}, assets);
    addProfiles(left);
    addProfiles(right);
    TerrainSampler a(left), b(right);
    int first = 0, second = 0, offGrid = 0;
    const TerrainSample flat{420, 0};
    for (int z = 256; z < 384; z += 2)
        for (int x = 240; x < 272; ++x) {
            const auto cover = a.soil(x, z, flat);
            // Switches between soils must not follow the 16-tile chunk grid.
            if (x > 240 && x % 16 &&
                a.soil(x - 1, z, flat).layers[0].data_id !=
                    cover.layers[0].data_id)
                ++offGrid;
            const auto again = b.soil(x, z, flat);
            EQUAL(cover.count, 2, "whole profile");
            EQUAL(cover.layers[0].data_id, again.layers[0].data_id,
                  "snapshot independence");
            CHECK(cover.layers[0].data_id == cover.layers[1].data_id);
            CHECK(cover.layers[0].topsoil && !cover.layers[1].topsoil);
            if (cover.layers[0].data_id == 1)
                ++first;
            else
                ++second;
        }
    CHECK(first > 0 && second > 0);
    CHECK(offGrid > 0);
    EQUAL(int(a.soil(200, 300, flat).layers[0].data_id), 1, "interior profile");
    EQUAL(int(b.soil(310, 300, flat).layers[0].data_id), 2, "interior profile");
    CHECK(a.soil(256, 300, {420, 1}).count == 0);
    CHECK(a.soil(256, 300, {420, .5}).depth_m < 4);
    addProfiles(left, true);
    for (int z = 256; z < 384; z += 3)
        EQUAL(int(TerrainSampler(left).soil(255, z, flat).layers[0].data_id), 1,
              "wetland soil needs its own region to qualify");
    // A qualifying wetland soil (its region has the feature, and a higher
    // priority) still blends across the border instead of stopping at it.
    for (auto& value : left.regions)
        if (value.region.coord.x >= 2)
            value.region.addFeature(FeatureMask::SWAMP);
    const TerrainSampler wetland(left);
    int dry = 0, wetBand = 0;
    for (int z = 256; z < 384; z += 2) {
        for (int x = 256 - int(TerrainTuning::SoilHalfWidth); x < 256; ++x)
            (wetland.soil(x, z, flat).layers[0].data_id == 2 ? wetBand : dry)++;
        for (int x = 130; x < 256 - int(TerrainTuning::SoilHalfWidth); x += 3)
            EQUAL(int(wetland.soil(x, z, flat).layers[0].data_id), 1,
                  "beyond the blend band");
    }
    CHECK(wetBand > 0 && dry > 0);
    // Same soil on both sides, but the second regions are bare (no
    // vegetation, so no topsoil): topsoil fades out across the border band
    // instead of stopping at it.
    addProfiles(left);
    for (auto& value : left.regions)
        if (value.region.coord.x >= 2) {
            value.soil = left.regions[0].soil;
            value.region.vegetation = VegetationType::NONE;
        }
    const TerrainSampler bare(left);
    int covered = 0, uncovered = 0;
    for (int z = 256; z < 384; z += 2) {
        for (int x = 256 - int(TerrainTuning::SoilHalfWidth);
             x < 256 + int(TerrainTuning::SoilHalfWidth); ++x)
            (bare.soil(x, z, flat).layers[0].topsoil ? covered : uncovered)++;
        CHECK(bare.soil(200, z, flat).layers[0].topsoil);
        CHECK(!bare.soil(320, z, flat).layers[0].topsoil);
    }
    CHECK(covered > 0 && uncovered > 0);
    // Both sides of the border line itself mix as well.
    int leftBare = 0, rightCovered = 0;
    for (int z = 256; z < 384; ++z) {
        leftBare += !bare.soil(250, z, flat).layers[0].topsoil;
        rightCovered += bare.soil(261, z, flat).layers[0].topsoil;
    }
    CHECK(leftBare > 0 && rightCovered > 0);
    auto wet = TerrainSampler(right).soil(256, 300, {SEA_LEVEL_Y - 1, 0});
    CHECK(wet.count ==
          0);  // Both profiles initially forbid underwater placement.
    for (auto& r : right.regions) r.soil_rules[0].allow_underwater = true;
    wet = TerrainSampler(right).soil(256, 300, {SEA_LEVEL_Y - 1, 0});
    EQUAL(wet.count, 1, "subsoil only underwater");
    CHECK(!wet.layers[0].topsoil);
}

TEST(terrain_streaming_cache) {
    test::Assets assets;
    auto world = terrainWorld(1, 1);
    auto& region = world.regions[0];
    LocationGenerator generator;
    ThreadPool pool(1);
    ChunkManager chunks(generator, pool, nullptr, world, &assets);
    chunks.registerChunk({0, 25, 0}, {}, &region);
    chunks.registerChunk({0, 26, 0}, {}, &region);
    EQUAL(chunks.cachedColumnInputs(), 1u, "horizontal cache shared by slabs");
    EQUAL(chunks.getColumnTopY(0, 0), 420, "conservative ceiling");
    chunks.raiseColumnTopY(0, 0, 440);
    EQUAL(chunks.getColumnTopY(0, 0), 440, "edit raises lighting ceiling");
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto blocker = pool.submit_task([gate] { gate.wait(); });
    // Always release the worker if any subsequent check throws.
    struct Release {
        std::promise<void>& promise;
        ~Release() { promise.set_value(); }
    };
    {
        Release guard{release};
        chunks.request({0, 26, 0});
        chunks.update();
        chunks.unregister({0, 26, 0});
        chunks.unregister({0, 25, 0});
        EQUAL(chunks.cachedColumnInputs(), 0u, "unloaded cache released");
        chunks.registerChunk({0, 26, 0}, {}, &region);
    }
    CHECK(pool.wait_for(std::chrono::seconds(3)));
    chunks.update();
    CHECK(!chunks.isReady(
        {0, 26, 0}));  // Old job must not publish into replacement.
    chunks.request({0, 26, 0});
    chunks.update();
    CHECK(pool.wait_for(std::chrono::seconds(3)));
    chunks.update();
    const auto* generated = chunks.getChunk({0, 26, 0});
    CHECK(generated != nullptr);
    const int surface = chunks.generatedSurfaceY(0, 0);
    for (int y = 0; y < Chunk::SIZE_Y; ++y)
        CHECK(generated->tiles[0][y][0].isSolid() == (416 + y <= surface));
}

TEST(terrain_column_inputs_seams) {
    test::Assets assets;
    auto world = terrainWorld(5, 5, 418);
    LocationGenerator generator;
    const std::vector<RegionLayer> rocks = {
        {RegionLayerType::Rock, 3, 5, 0, 0, 1},
        {RegionLayerType::Rock, 1, 1000, 5, 0, 1}};
    for (const unsigned cx : {23u, 24u}) {
        auto data = TerrainNeighborhood::capture(world, {cx / 8, 2}, assets);
        addProfiles(data);
        TerrainSampler sampler(data);
        const auto columns = sampler.columns(cx, 20);
        const auto& region = world.regions[2 * 5 + cx / 8];
        Chunk upper, lower;
        generator.generateChunk(upper, {cx, 26, 20}, {}, region, rocks, {},
                                &columns);
        generator.generateChunk(lower, {cx, 25, 20}, {}, region, rocks, {},
                                &columns);
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int x = 0; x < Chunk::SIZE_X; ++x) {
                const auto terrain = sampler.sample(cx * 16 + x, 320 + z);
                const auto& input = columns.columns[z * Chunk::SIZE_X + x];
                EQUAL(input.surface_y, terrain.surface_y,
                      "cached halo surface");
                const auto expectedCover =
                    sampler.soil(cx * 16 + x, 320 + z, terrain);
                EQUAL(input.soil.depth_m, expectedCover.depth_m,
                      "cached halo slope");
                for (int y = 400; y < 432; ++y) {
                    const auto& tile = y >= 416 ? upper.tiles[z][y - 416][x]
                                                : lower.tiles[z][y - 400][x];
                    if (y > terrain.surface_y) {
                        CHECK(tile.isAir());
                        continue;
                    }
                    const int rockDepth = region.land_height_m - y;
                    int expected = rockDepth < 5 ? 3 : 1;
                    const int soilDepth = terrain.surface_y - y;
                    if (input.soil.count && soilDepth < input.soil.depth_m) {
                        const int horizon =
                            soilDepth < input.soil.layers[0].thickness_m ? 0
                                                                         : 1;
                        expected = input.soil.layers[horizon].block_id;
                    }
                    EQUAL(int(tile.block_id), expected,
                          "terrain at slab/region seam");
                }
            }
    }
    // Record representative CPU preparation cost without a machine-dependent
    // pass/fail threshold. No graphics or audio devices participate.
    auto data = TerrainNeighborhood::capture(world, {2, 2}, assets);
    TerrainSampler sampler(data);
    const auto start = std::chrono::steady_clock::now();
    int checksum = 0;
    for (unsigned z = 16; z < 24; ++z)
        for (unsigned x = 16; x < 24; ++x)
            checksum += sampler.columns(x, z).columns.front().surface_y;
    CHECK(checksum > 0);
    std::cout << "Prepared 64 horizontal chunk grids in "
              << std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start)
                     .count()
              << " ms\n";
}

TEST(terrain_height_profile) {
    using namespace HeightProfile;
    EQUAL(heightToY(0), OceanFloorY, "ocean floor");
    EQUAL(heightToY(SeaHeight), SEA_LEVEL_Y, "sea level");
    EQUAL(heightToY(MountainStart), PlainsTopY, "mountain foot");
    EQUAL(heightToY(1), PeakY, "highest peak");
    CHECK(PeakY <= WORLD_HEIGHT_Y - SkySpace);
    int previous = heightToY(0);
    for (int i = 1; i <= 1000; ++i) {
        const int y = heightToY(i / 1000.f);
        CHECK(y >= previous);
        previous = y;
    }
    EQUAL(heightToY(MountainTop), PeakY, "noise maximum reaches the peak");
    // Exponential: the last mountain step rises more than the plains band.
    CHECK(heightToY(MountainTop) - heightToY(MountainTop - .05f) >
          PlainsTopY - SEA_LEVEL_Y);
}

TEST(terrain_coast_organic) {
    test::Assets assets;
    auto world = terrainWorld(5, 5, 420);
    for (auto& r : world.regions)
        if (r.coord.x < 2) {
            r.land_height_m = SEA_LEVEL_Y - 12;
            r.land = LandType::WATER;
        }
    auto data = TerrainNeighborhood::capture(world, {2, 2}, assets);
    TerrainSampler sampler(data);
    int lowest = INT_MAX, highest = INT_MIN;
    for (int z = 256; z < 384; ++z) {
        int crossing = -1;
        for (int x = 160; x < 352; ++x)
            if (sampler.sample(x, z).surface_y >= SEA_LEVEL_Y) {
                crossing = x;
                break;
            }
        CHECK(crossing >= 0);
        lowest = std::min(lowest, crossing);
        highest = std::max(highest, crossing);
        // Far from the region boundary the coast field is saturated.
        CHECK(sampler.sample(352, z).surface_y > SEA_LEVEL_Y);
        CHECK(sampler.sample(160, z).surface_y < SEA_LEVEL_Y);
    }
    // The shoreline wanders instead of following the region edge at x=256.
    CHECK(highest - lowest >= 16);
    CHECK(lowest != 256 || highest != 256);
}

TEST(terrain_surface_ceiling) {
    test::Assets assets;
    auto world = terrainWorld(5, 5, 700, LandType::MOUNTAINS);
    for (auto& r : world.regions) {
        r.land_height_m += int(r.coord.x) * 37 - int(r.coord.y) * 23;
        if ((r.coord.x + r.coord.y) % 3 == 0) {
            r.land_height_m = SEA_LEVEL_Y - 20;
            r.land = LandType::WATER;
        }
    }
    auto data = TerrainNeighborhood::capture(world, {2, 2}, assets);
    TerrainSampler sampler(data);
    for (int z = 256; z < 384; z += 3)
        for (int x = 256; x < 384; x += 3) {
            const auto& owner = world.regions[(z / 128) * 5 + x / 128];
            CHECK(sampler.sample(x, z).surface_y <=
                  TerrainTuning::surfaceCeiling(owner.land_height_m));
        }
}

TEST(terrain_mountain_relief) {
    test::Assets assets;
    auto world = terrainWorld(5, 5, 820, LandType::MOUNTAINS);
    auto data = TerrainNeighborhood::capture(world, {2, 2}, assets);
    TerrainSampler sampler(data);
    int lowest = INT_MAX, highest = INT_MIN;
    for (int z = 256; z < 384; z += 2)
        for (int x = 256; x < 384; x += 2) {
            const int y = sampler.sample(x, z).surface_y;
            lowest = std::min(lowest, y);
            highest = std::max(highest, y);
        }
    CHECK(highest <= 820);
    // Elevation-scaled ridges carve valleys of tens of metres into peaks.
    CHECK(highest - lowest >= 60);
}

TEST(terrain_air_chunks_skip_jobs) {
    test::Assets assets;
    auto world = terrainWorld(1, 1);
    LocationGenerator generator;
    ThreadPool pool(1);
    ChunkManager chunks(generator, pool, nullptr, world, &assets);
    LocationManager locations(chunks, world);
    locations.loadRegion({0, 0}, 0);
    // Far above every generated surface: published in the same update.
    const ChunkCoord sky{3, Y_CHUNKS - 1, 3}, ground{3, SEA_LEVEL_Y / 16, 3};
    chunks.request(sky);
    chunks.request(ground);
    chunks.update();
    CHECK(chunks.isReady(sky));
    CHECK(!chunks.isReady(ground));
    CHECK(chunks.getChunk(sky)->tiles[15][15][15].isAir());
    pool.wait();
}
