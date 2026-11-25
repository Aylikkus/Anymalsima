#include <chrono>
#include <climits>
#include <map>
#include <vector>

#include "world/location_manager.hpp"
#include "world/natural_shape.hpp"
#include "world/save/chunk_codec.hpp"
#include "world/save/world_meta.hpp"
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

namespace {
using Profile = TileShapeProfile;
using Heights = std::map<std::pair<int, int>, int>;  // top, not surface_y

natural_shape::Field shapeField(const Heights& tops, int fallback, int width,
                                int height, bool edgeCompatibility = true,
                                bool diagonalCorners = true) {
    const auto wrap = [period = width](int x) {
        return period > 0 ? (x % period + period) % period : x;
    };
    return natural_shape::Field(
        [tops, fallback, wrap, height](int64_t x, int64_t z) {
            const int zz = std::clamp(int(z), 0, height - 1);
            const auto it = tops.find({wrap(int(x)), zz});
            const int top = it == tops.end() ? fallback : it->second;
            return top - 1;
        },
        [](int64_t, int64_t) { return true; }, edgeCompatibility,
        diagonalCorners);
}
}  // namespace

TEST(terrain_natural_ramps) {
    constexpr int H = 6;
    Heights tops;
    // A terrace edge: one low west neighbour, level sides and landing.
    tops[{9, 10}] = H - 1;
    auto field = shapeField(tops, H, 32, 32);
    const auto ramp = field.at(10, 10);
    EQUAL(int(ramp.profile()), int(Profile::Ramp), "terrace ramp");
    EQUAL(int(ramp.facing()), int(TileFacing::PosX), "uphill landing");
    CHECK(!field.at(11, 10).code());
}

TEST(terrain_natural_diagonal_corner_ramp) {
    constexpr int H = 6;
    Heights tops;
    // Downhill in both -X and -Z, uphill in both +X and +Z: no level ramp or
    // junction can meet the taller hillside, so use one diagonal ramp plane.
    tops[{9, 10}] = H - 1;
    tops[{10, 9}] = H - 1;
    tops[{11, 10}] = H + 1;
    tops[{10, 11}] = H + 1;
    auto field = shapeField(tops, H, 32, 32);
    const auto corner = field.at(10, 10);
    EQUAL(int(corner.profile()), int(Profile::DiagonalRamp),
          "diagonal corner ramp");
    EQUAL(int(corner.facing()), 0, "diagonal high corner NE");

    // Generator 25 leaves the same pattern as a whole block.
    auto strict = shapeField(tops, H, 32, 32, false, false);
    CHECK(!strict.at(10, 10).code());
    // Generator 26 keeps cubes here too; diagonal corners start at 27.
    auto ridge = shapeField(tops, H, 32, 32, true, false);
    CHECK(!ridge.at(10, 10).code());
}

TEST(terrain_natural_legacy_rule_keeps_blocks) {
    constexpr int H = 6;
    // Generator 25 required cube landings on both ends, so a two-cell ridge
    // and a ramp against a higher wall stayed full blocks.
    Heights ridge;
    ridge[{9, 10}] = H - 1;
    ridge[{12, 10}] = H - 1;
    auto strict = shapeField(ridge, H, 32, 32, false, false);
    CHECK(!strict.at(10, 10).code());
    CHECK(!strict.at(11, 10).code());

    Heights wall;
    wall[{9, 10}] = H - 1;
    wall[{11, 10}] = H + 1;
    auto wallField = shapeField(wall, H, 32, 32, false, false);
    CHECK(!wallField.at(10, 10).code());
}

TEST(terrain_natural_ridge_pairs_meet) {
    constexpr int H = 6;
    Heights tops;
    // A two-cell plateau with a one-metre drop on both outer sides. Each
    // high cell is the other's landing, so both must cut into ramps that
    // meet as a ridge instead of leaving two whole blocks.
    tops[{9, 10}] = H - 1;
    tops[{12, 10}] = H - 1;
    auto field = shapeField(tops, H, 32, 32);
    const auto west = field.at(10, 10);
    const auto east = field.at(11, 10);
    EQUAL(int(west.profile()), int(Profile::Ramp), "west ridge ramp");
    EQUAL(int(east.profile()), int(Profile::Ramp), "east ridge ramp");
    EQUAL(int(west.facing()), int(TileFacing::PosX), "west rises east");
    EQUAL(int(east.facing()), int(TileFacing::NegX), "east rises west");
}

TEST(terrain_natural_ramp_meets_higher_wall) {
    constexpr int H = 6;
    Heights tops;
    // The high neighbour is one cell above the ramp top. The ramp is still
    // useful: it reaches the wall's foot instead of leaving a whole block.
    tops[{9, 10}] = H - 1;
    tops[{11, 10}] = H + 1;
    auto field = shapeField(tops, H, 32, 32);
    const auto ramp = field.at(10, 10);
    EQUAL(int(ramp.profile()), int(Profile::Ramp), "ramp to higher wall");
    EQUAL(int(ramp.facing()), int(TileFacing::PosX), "wall uphill");
}

TEST(terrain_natural_shape_queries_are_order_independent) {
    test::Assets assets;
    auto world = terrainWorld(5, 5, 420, LandType::HILLS);
    world.generatorVersion = WorldMeta::GeneratorVersion;
    const auto data = TerrainNeighborhood::capture(world, {2, 2}, assets);
    TerrainSampler forward(data), reverse(data);
    std::vector<std::pair<int, int>> cells;
    for (int z = 0; z < 12; ++z)
        for (int x = 0; x < 12; ++x) cells.emplace_back(16 + x, 16 + z);
    std::vector<uint8_t> first;
    for (const auto& [x, z] : cells)
        first.push_back(forward.surfaceShape(x, z).code());
    size_t index = cells.size();
    for (auto it = cells.rbegin(); it != cells.rend(); ++it, --index)
        EQUAL(int(reverse.surfaceShape(it->first, it->second).code()),
              int(first[index - 1]), "reverse query");
    // A sampler used for one cell only must agree with the warm cache.
    for (size_t i = 0; i < cells.size(); i += 17) {
        TerrainSampler fresh(data);
        EQUAL(int(fresh.surfaceShape(cells[i].first, cells[i].second).code()),
              int(first[i]), "cold query");
    }
}

TEST(terrain_natural_outer_corner) {
    constexpr int H = 6;
    constexpr int dx[4] = {0, 1, 0, -1};
    constexpr int dz[4] = {1, 0, -1, 0};
    for (unsigned turn = 0; turn < 4; ++turn) {
        Heights tops;
        const unsigned west = (3 + turn) & 3, south = (2 + turn) & 3;
        const unsigned east = (1 + turn) & 3, north = (0 + turn) & 3;
        tops[{10 + dx[west], 10 + dz[west]}] = H - 1;
        tops[{10 + dx[south], 10 + dz[south]}] = H - 1;
        tops[{10 + dx[west] + dx[north], 10 + dz[west] + dz[north]}] = H - 1;
        tops[{10 + dx[south] + dx[east], 10 + dz[south] + dz[east]}] = H - 1;
        auto field = shapeField(tops, H, 32, 32);
        const auto corner = field.at(10, 10);
        EQUAL(int(corner.profile()), int(Profile::OuterCorner),
              "outer corner turn " + std::to_string(turn));
        EQUAL(int(corner.facing()), int(turn), "outer facing");
        const auto approachEast = field.at(10 + dx[east], 10 + dz[east]);
        const auto approachNorth = field.at(10 + dx[north], 10 + dz[north]);
        CHECK(approachEast.profile() == Profile::Ramp &&
              unsigned(approachEast.facing()) == ((0 + turn) & 3));
        CHECK(approachNorth.profile() == Profile::Ramp &&
              unsigned(approachNorth.facing()) == ((1 + turn) & 3));
    }
}

TEST(terrain_natural_inner_corner) {
    constexpr int H = 6;
    constexpr int dx[4] = {0, 1, 0, -1};
    constexpr int dz[4] = {1, 0, -1, 0};
    for (unsigned turn = 0; turn < 4; ++turn) {
        Heights tops;
        const unsigned west = (3 + turn) & 3, south = (2 + turn) & 3;
        tops[{10 + dx[west] + dx[south], 10 + dz[west] + dz[south]}] = H - 1;
        auto field = shapeField(tops, H, 32, 32);
        const auto corner = field.at(10, 10);
        EQUAL(int(corner.profile()), int(Profile::InnerCorner),
              "inner corner turn " + std::to_string(turn));
        EQUAL(int(corner.facing()), int(turn), "inner facing");
        const auto approachWest = field.at(10 + dx[west], 10 + dz[west]);
        const auto approachSouth = field.at(10 + dx[south], 10 + dz[south]);
        CHECK(approachWest.profile() == Profile::Ramp &&
              unsigned(approachWest.facing()) == ((0 + turn) & 3));
        CHECK(approachSouth.profile() == Profile::Ramp &&
              unsigned(approachSouth.facing()) == ((1 + turn) & 3));
    }
}

TEST(terrain_natural_landing_is_not_cut) {
    constexpr int H = 6;
    Heights tops;
    tops[{9, 10}] = H - 1;   // low west of the candidate
    tops[{11, 11}] = H - 1;  // low north of its eastern landing
    auto field = shapeField(tops, H, 32, 32);
    CHECK(!field.at(10, 10).code());
    CHECK(field.at(11, 10).profile() == Profile::Ramp);
}

TEST(terrain_natural_ramp_wraps_x) {
    constexpr int H = 6, W = 32;
    Heights tops;
    tops[{W - 1, 10}] = H - 1;  // low west of the seam cell
    auto field = shapeField(tops, H, W, 32);
    const auto atZero = field.at(0, 10);
    const auto atPeriod = field.at(W, 10);
    CHECK(atZero.code());
    EQUAL(int(atZero.code()), int(atPeriod.code()), "seam repeat");
    EQUAL(int(atZero.facing()), int(TileFacing::PosX), "seam uphill");
}

TEST(terrain_generation_and_lod_apply_natural_shapes) {
    test::Assets assets;
    auto world = terrainWorld(5, 5, 420, LandType::HILLS);
    world.generatorVersion = WorldMeta::GeneratorVersion;
    const std::vector<RegionLayer> layers = {
        {RegionLayerType::Rock, 1, 1024, 0, 0, 1}};
    auto data = TerrainNeighborhood::capture(world, {2, 2}, assets);
    TerrainSampler sampler(data);
    bool found = false;
    for (int cz = 16; cz < 24 && !found; ++cz)
        for (int cx = 16; cx < 24 && !found; ++cx) {
            const auto columns = sampler.columns(cx, cz);
            for (int i = 0; i < Chunk::SIZE_X * Chunk::SIZE_Z; ++i) {
                const auto shape = columns.columns[i].groundShape;
                if (!shape.code()) continue;
                found = true;
                const int lx = i % Chunk::SIZE_X, lz = i / Chunk::SIZE_X;
                const int surface = columns.columns[i].surface_y;
                Chunk chunk;
                LocationGenerator{}.generateChunk(
                    chunk, {unsigned(cx), unsigned(surface / 16), unsigned(cz)},
                    {}, world.regions[12], layers, {}, &columns);
                const auto cell = chunk.cellAt(lx, surface % 16, lz);
                CHECK(cell.tile.isSolid());
                EQUAL(int(cell.shape.code()), int(shape.code()),
                      "generated surface shape");
                break;
            }
        }
    CHECK(found);

    // Generator 25 still selects natural shapes, but keeps its narrow
    // cube-landing compatibility rule; 26 matches shared edges.
    world.generatorVersion = WorldMeta::NaturalShapeVersion;
    auto strict = TerrainNeighborhood::capture(world, {2, 2}, assets);
    CHECK(strict.naturalShapes() && !strict.ridgeShapes());
    world.generatorVersion = WorldMeta::NaturalShapeRidgeVersion;
    auto ridge = TerrainNeighborhood::capture(world, {2, 2}, assets);
    CHECK(ridge.naturalShapes() && ridge.ridgeShapes());
    CHECK(!ridge.diagonalShapes());
    world.generatorVersion = WorldMeta::NaturalShapeDiagonalVersion;
    auto diagonal = TerrainNeighborhood::capture(world, {2, 2}, assets);
    CHECK(diagonal.naturalShapes() && diagonal.ridgeShapes() &&
          diagonal.diagonalShapes());

    // The same map under the previous generator stays cube-only.
    world.generatorVersion = WorldMeta::NaturalShapeVersion - 1;
    auto legacy = TerrainNeighborhood::capture(world, {2, 2}, assets);
    CHECK(!legacy.naturalShapes());
    const auto columns = TerrainSampler(legacy).columns(16, 16);
    for (const auto& column : columns.columns)
        CHECK(!column.groundShape.code());
}

TEST(terrain_natural_shapes_survive_chunk_codec) {
    test::Assets assets;
    auto world = terrainWorld(5, 5, 420, LandType::HILLS);
    world.generatorVersion = WorldMeta::GeneratorVersion;
    const auto data = TerrainNeighborhood::capture(world, {2, 2}, assets);
    TerrainSampler sampler(data);
    const std::vector<RegionLayer> layers = {
        {RegionLayerType::Rock, 1, 1024, 0, 0, 1}};
    bool found = false;
    for (int cz = 16; cz < 24 && !found; ++cz)
        for (int cx = 16; cx < 24 && !found; ++cx) {
            const auto columns = sampler.columns(cx, cz);
            int top = 0;
            for (const auto& column : columns.columns)
                top = std::max(top, column.surface_y);
            Chunk chunk;
            LocationGenerator{}.generateChunk(
                chunk,
                {unsigned(cx), unsigned(top / Chunk::SIZE_Y), unsigned(cz)}, {},
                world.regions[12], layers, {}, &columns);
            if (!chunk.hasShapes()) continue;
            found = true;
            Chunk decoded;
            chunk_codec::decode(chunk_codec::encode(chunk), decoded);
            EQUAL(decoded.shapeCount(), chunk.shapeCount(), "shape count");
            for (int z = 0; z < Chunk::SIZE_Z; ++z)
                for (int y = 0; y < Chunk::SIZE_Y; ++y)
                    for (int x = 0; x < Chunk::SIZE_X; ++x) {
                        const auto index = Chunk::cell(x, y, z);
                        CHECK(decoded.shapeAt(index) == chunk.shapeAt(index));
                        EQUAL(int(decoded.tiles[z][y][x].block_id),
                              int(chunk.tiles[z][y][x].block_id),
                              "generated block");
                    }
        }
    CHECK(found);
}

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

TEST(terrain_surface_continuous_at_region_borders) {
    // The surface height must not step along region borders: a step on a
    // beach strips the soil from its edge and shows a straight line of
    // rock (low land beside the sea used to clamp the beach berm to its
    // own, lower ceiling while the sea region next to it did not).
    test::Assets assets;
    for (const int land : {SEA_LEVEL_Y + 1, SEA_LEVEL_Y + 2, 420}) {
        auto world = terrainWorld(5, 5, land, LandType::PLAINS);
        for (auto& r : world.regions)
            if (r.coord.x < 2 || (r.coord.x == 3 && r.coord.y == 1)) {
                r.land_height_m = SEA_LEVEL_Y - 12;
                r.land = LandType::WATER;
            }
        const auto data = TerrainNeighborhood::capture(world, {2, 2}, assets);
        const TerrainSampler sampler(data);
        double worst = 0;
        for (const double border : {256.0, 384.0})
            for (double along = 200; along < 440; along += 1) {
                worst = std::max(
                    worst, std::abs(sampler.surface(border - 1e-4, along) -
                                    sampler.surface(border + 1e-4, along)));
                worst = std::max(
                    worst, std::abs(sampler.surface(along, border - 1e-4) -
                                    sampler.surface(along, border + 1e-4)));
            }
        CHECK(worst < 0.05);
    }
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
    // X wraps around the world: the seam joins column 127 to column 0.
    CHECK(sampler.surface(-1, 64) == sampler.surface(127, 64));
    CHECK(sampler.surface(128, 64) == sampler.surface(0, 64));
    close(sampler.surface(127, 64), sampler.surface(0, 64), 1);
    // Z stays bounded and clamps to the edge row.
    close(sampler.surface(64, -1), sampler.surface(64, 0));
    close(sampler.surface(64, 128), sampler.surface(64, 127));
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
