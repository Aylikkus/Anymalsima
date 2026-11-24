#include <bit>
#include <chrono>
#include <limits>
#include <map>
#include <set>

#include "test.hpp"
#include "test_assets.hpp"
#include "world/chunk_manager.hpp"
#include "world/chunk_mesher.hpp"
#include "world/detail_coverage.hpp"
#include "world/location_generator.hpp"
#include "world/lod_cover.hpp"

namespace {
World worldForLod(int width = 6, int depth = 6, int height = 430) {
    World world{};
    world.width = width;
    world.height = depth;
    world.genSettings.width = width;
    world.genSettings.height = depth;
    world.genSettings.height_seed = 1234;
    world.genSettings.feature_seed = 5678;
    world.regions.resize(width * depth);
    for (int z = 0; z < depth; ++z)
        for (int x = 0; x < width; ++x) {
            auto& r = world.regions[z * width + x];
            r = test::regionAt(height);
            r.coord = {unsigned(x), unsigned(z)};
            r.land = LandType::HILLS;
        }
    return world;
}
std::atomic_bool running{false};
bool close(float a, float b, float tolerance = .0001f) {
    return std::abs(a - b) <= tolerance;
}
using Edge = std::array<int, 3>;
std::map<Edge, int> boundary(const TerrainLod::Mesh& mesh, int axis,
                             float value) {
    std::map<Edge, int> result;
    for (const auto& v : mesh.vertices) {
        if (v.layer != -1 || !close(axis == 0 ? v.x : v.z, value)) continue;
        result[{int(v.x * 2), int(v.y * 2), int(v.z * 2)}] = 1;
    }
    return result;
}
}  // namespace
TEST(lod_surface_matches_chunks) {
    test::Assets assets;
    auto world = worldForLod();
    world.regions[2 * 6 + 2].addFeature(FeatureMask::CLIFF);
    for (auto& r : world.regions) r.vegetation = VegetationType::GRASS;
    Soil soil{};
    soil.id = 1;
    soil.name = "lod_soil";
    soil.block_name = "block2";
    soil.topsoil_block_name = "block3";
    soil.depth_m = {4, 4};
    soil.placement = {SoilPlacement{}};
    assets.setData<Soil>({soil});
    SurfaceSampler sampler(world, assets);
    LocationGenerator generator;
    LayersGenerator geology(world.genSettings, &assets);
    for (unsigned rz : {1u, 2u})
        for (unsigned rx : {1u, 2u}) {
            const auto& r = world.regions[rz * 6 + rx];
            const auto neighborhood =
                TerrainNeighborhood::capture(world, {rx, rz}, assets);
            TerrainSampler terrain(neighborhood);
            const auto layers = geology.get(r);
            const unsigned cx = rx * 8 + 7, cz = rz * 8 + 7;
            const auto columns = terrain.columns(cx, cz);
            for (unsigned cy : {23u, 24u, 25u, 26u}) {
                Chunk chunk{};
                generator.generateChunk(chunk, {cx, cy, cz},
                                        {rx * 8, 0, rz * 8}, r, layers, {},
                                        &columns);
                for (int z = 0; z < 16; ++z)
                    for (int x = 0; x < 16; ++x) {
                        const auto c = sampler.column(cx * 16 + x, cz * 16 + z);
                        EQUAL(c.surfaceY, columns.columns[z * 16 + x].surface_y,
                              "surface");
                        for (int y = 0; y < 16; ++y) {
                            const auto& t = chunk.tiles[z][y][x];
                            const int wy = cy * 16 + y;
                            const bool wet = c.water.fills(wy, c.surfaceY);
                            CHECK((t.liquid == LiquidType::Water) == wet);
                            if (wet)
                                EQUAL(int(t.liquidLevel), 255, "full cell");
                            if (wy <= c.surfaceY)
                                EQUAL(sampler.materialAt(cx * 16 + x, wy,
                                                         cz * 16 + z),
                                      assets.getMaterialAlbedoLayer(
                                          assets.getStorage<Block>()
                                              .get(t.block_id)
                                              .material),
                                      "material at depth");
                            if (wy == c.surfaceY)
                                EQUAL(c.material,
                                      sampler.materialAt(cx * 16 + x, wy,
                                                         cz * 16 + z),
                                      "exposed material");
                        }
                    }
            }
        }
    // A boundary-height surface occupies the slab below the boundary.
    // Air classification and the unloaded query use the same interval.
    ChunkColumnInputs raised;
    for (auto& c : raised.columns) {
        c.surface_y = 380;
        c.water = {400};
    }
    CHECK(!generator.isAirChunk({0, 24, 0}, raised));
    CHECK(generator.isAirChunk({0, 25, 0}, raised));
    Chunk water{};
    const auto& region = world.regions.front();
    generator.generateChunk(water, {0, 24, 0}, {0, 0, 0}, region,
                            geology.get(region), {}, &raised);
    const auto& cell = water.tiles[0][15][0];
    CHECK(cell.liquid == LiquidType::Water);
    for (float eye : {399.99f, 400.f, 400.01f}) {
        const Tile air{};
        const bool loaded =
            WaterSurface{}.submerged(eye, -1, eye < 400.f ? &cell : &air);
        const bool unloaded = raised.columns.front().water.submerged(eye, 380);
        CHECK(loaded == unloaded);
        CHECK(loaded == (eye < 400.f));
    }
}
TEST(lod_geology_order_independent) {
    test::Assets assets;
    auto world = worldForLod();
    LayersGenerator first(world.genSettings, &assets),
        second(world.genSettings, &assets);
    auto rock = assets.getStorage<Rock>().items[0];
    rock.id = 2;
    rock.name = "second_rock";
    rock.block_name = "block2";
    rock.body_size_m = {5, 25};
    auto original = assets.getStorage<Rock>().items[0];
    original.body_size_m = {5, 25};
    assets.setData<Rock>({original, rock});
    const auto expected = first.get(world.regions[14]);
    for (auto it = world.regions.rbegin(); it != world.regions.rend(); ++it)
        second.get(*it);
    const auto actual = second.get(world.regions[14]);
    EQUAL(expected.size(), actual.size(), "layer count");
    for (size_t i = 0; i < actual.size(); ++i) {
        EQUAL(expected[i].block_id, actual[i].block_id, "rock");
        EQUAL(expected[i].thickness_m, actual[i].thickness_m, "thickness");
        EQUAL(expected[i].depth_from_surface_m, actual[i].depth_from_surface_m,
              "depth");
    }
}
TEST(lod_stepped_surface_and_bands) {
    test::Assets assets;
    auto world = worldForLod();
    SurfaceSampler sampler(world, assets);
    const TerrainLod::Patch patch{256, 256, 1};
    const auto mesh = TerrainLod::build(patch, sampler, running);
    CHECK(!mesh.indices.empty());
    EQUAL(mesh.bands.size(), size_t(4), "chunk bands");
    for (int z = 256; z < 288; ++z)
        for (int x = 256; x < 288; ++x) {
            const int height = sampler.height(x, z) + 1;
            bool found = false;
            for (const auto& v : mesh.vertices)
                if (v.x == x && v.z == z && v.y == height && v.ny == 1 &&
                    v.layer >= 0) {
                    found = true;
                    break;
                }
            CHECK(found);
            const auto& band =
                mesh.bands[(z - 256) / 16 * 2 + (x - 256) / 16].ground;
            CHECK(band.minY <= (height - 1) / 16);
            CHECK(band.maxY >= (height - 1) / 16);
        }
    CHECK(mesh.bytes() < TerrainLod::UploadBudget);
}
TEST(lod_smooth_skirts_cover_edges) {
    test::Assets assets;
    auto world = worldForLod();
    SurfaceSampler sampler(world, assets);
    const TerrainLod::Patch coarse{256, 256, 4}, fine{384, 256, 2};
    const auto a = TerrainLod::build(coarse, sampler, running);
    const auto b = TerrainLod::build(fine, sampler, running);
    CHECK(!a.vertices.empty());
    CHECK(!b.vertices.empty());
    // Each patch's skirt along the shared edge reaches below both surfaces,
    // so any T-junction gap between them is closed.
    const auto lowest = [](const TerrainLod::Mesh& mesh, float normalX,
                           float z0, float z1) {
        float result = std::numeric_limits<float>::infinity();
        for (const auto& v : mesh.vertices)
            if (v.v == 1 && v.layer == -1 && v.nx == normalX &&
                close(v.x, 384) && v.z >= z0 && v.z <= z1)
                result = std::min(result, v.y);
        return result;
    };
    // The fine patch spans z 256..320 of the coarse edge.
    for (int z = 256; z < 320; ++z) {
        const float surface =
            std::min(TerrainLod::meshHeight(coarse, sampler, 384, z + .5f),
                     TerrainLod::meshHeight(fine, sampler, 384, z + .5f));
        const float coarseStart = z / 4 * 4, fineStart = z / 2 * 2;
        CHECK(lowest(a, 1, coarseStart, coarseStart + 4) < surface);
        CHECK(lowest(b, -1, fineStart, fineStart + 2) < surface);
    }
    CHECK(a.bytes() < TerrainLod::UploadBudget);
}
TEST(lod_stepped_boundary_walls) {
    test::Assets assets;
    auto world = worldForLod();
    world.regions[2 * 6 + 2].land_height_m = 500;
    world.regions[2 * 6 + 2].addFeature(FeatureMask::CLIFF);
    SurfaceSampler generated(world, assets);
    auto edits = std::make_shared<EditMap>();
    for (int cz : {16, 17}) {
        auto edit = std::make_shared<ColumnEdit>();
        edit->waterY.fill(-1);
        for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x) {
                const int i = ColumnEdit::index(x, z);
                edit->groundY[i] = generated.height(320 + x, cz * 16 + z);
                if (x == 0) {
                    --edit->groundY[i];
                    edit->groundBlock[i] = 2;  // distinct from generated rock
                }
            }
        edits->emplace(editKey(20, cz), std::move(edit));
    }
    SurfaceSampler sampler(world, assets, nullptr, edits);
    const TerrainLod::Patch stepped{320, 256, 1};
    const auto mesh = TerrainLod::build(stepped, sampler, running);
    // Every boundary column has a side wall reaching below the lower of its
    // own top and the outside column, whatever the neighbour's resolution.
    for (int z = 256; z < 288; ++z) {
        const auto column = sampler.column(320, z);
        const int top = column.surfaceY + 1;
        const int outside = sampler.height(319, z) + 1;
        const float bottom =
            std::max(0, std::min(top, outside) - TerrainLod::SteppedSkirt);
        bool foundBottom = false, foundSurface = false;
        CHECK(column.material != sampler.materialAt(320, top - 2, z));
        for (const auto& v : mesh.vertices)
            if (v.v == 2 && v.nx == -1 && v.x == 320 && v.z >= z &&
                v.z <= z + 1 && v.u == top) {
                if (close(v.y, bottom) &&
                    v.layer == sampler.materialAt(320, int(bottom), z))
                    foundBottom = true;
                if (close(v.y, top - 2) && v.layer == column.material)
                    foundSurface = true;
            }
        CHECK(foundBottom);
        CHECK(foundSurface);
    }
}
TEST(lod_water_and_bounds) {
    test::Assets assets;
    auto world = worldForLod(2, 2, 350);
    SurfaceSampler sampler(world, assets);
    TerrainLod::Patch patch{0, 0, 8};
    const auto mesh = TerrainLod::build(patch, sampler, running);
    bool water = false, floor = false;
    for (const auto& v : mesh.vertices) {
        CHECK(v.x >= 0 && v.x <= 256 && v.z >= 0 && v.z <= 256);
        CHECK(v.layer != -2);
        if (v.y == 0) floor = true;
    }
    for (const auto& v : mesh.water.vertices) {
        water = true;
        CHECK(v.layer == -2);
        CHECK(v.y == SEA_LEVEL_Y + 1);
        CHECK(v.ny > 0);
    }
    CHECK(water);
    CHECK(floor);
    CHECK(mesh.bytes() < TerrainLod::UploadBudget);
    EQUAL(sampler.column(20, 20).water.surfaceY, SEA_LEVEL_Y + 1, "ocean");
    CHECK(mesh.water.bounds.minY == SEA_LEVEL_Y + 1);
    CHECK(mesh.water.bounds.maxY == SEA_LEVEL_Y + 1);

    // A dry strip below sea level and a rising bank straddle a chunk
    // boundary. Coarse water must honor both the footprint and its terrain.
    auto edits = std::make_shared<EditMap>();
    for (int cx : {0, 1}) {
        auto edit = std::make_shared<ColumnEdit>();
        for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x) {
                const int tx = cx * 16 + x;
                const int i = ColumnEdit::index(x, z);
                edit->groundY[i] = tx >= 22 ? 402 : 350;
                edit->waterY[i] = (tx >= 14 && tx < 18) || tx >= 22 ? -1 : 385;
                edit->groundBlock[i] = 1;
            }
        (*edits)[editKey(cx, 0)] = edit;
    }
    sampler.setEdits(edits);
    for (int spacing : {1, 2, 4, 8, 16, 64}) {
        const TerrainLod::Patch shore{0, 0, spacing};
        const auto shoreMesh = TerrainLod::build(shore, sampler, running, 4);
        CHECK(!shoreMesh.water.vertices.empty());
        for (size_t i = 0; i < shoreMesh.water.indices.size(); i += 3) {
            const auto& a =
                shoreMesh.water.vertices[shoreMesh.water.indices[i]];
            const auto& b =
                shoreMesh.water.vertices[shoreMesh.water.indices[i + 1]];
            const auto& c =
                shoreMesh.water.vertices[shoreMesh.water.indices[i + 2]];
            const float x = (a.x + b.x + c.x) / 3.f,
                        z = (a.z + b.z + c.z) / 3.f;
            if (a.ny == 0) {
                const int wetX = int(x - .25f * a.nx);
                const int wetZ = int(z - .25f * a.nz);
                const int dryX = int(x + .25f * a.nx);
                const int dryZ = int(z + .25f * a.nz);
                EQUAL(std::max({a.y, b.y, c.y}),
                      sampler.waterAt(wetX, wetZ).height(),
                      "wall starts at the higher water surface");
                EQUAL(std::min({a.y, b.y, c.y}),
                      float(sampler.height(dryX, dryZ) + 1),
                      "wall closes the exposed drop to dry ground");
                continue;
            }
            EQUAL(a.y, float(sampler.waterAt(int(x), int(z)).surfaceY),
                  "water triangle lies in the sampled footprint");
            CHECK(TerrainLod::meshHeight(shore, sampler, x, z) <= a.y + .001f);
        }
    }
    std::atomic_bool cancel{true};
    CHECK(TerrainLod::build(patch, sampler, cancel).vertices.empty());
}
TEST(lod_selection_coverage_balance) {
    const auto layout =
        TerrainLod::select(64000, 38400, {32000, 450, 19200}, 1080,
                           [](const auto& p) { return p.spacing * .1f; });
    CHECK(!layout.empty());
    std::array<bool, 129 * 129> seen{};
    for (const auto& p : layout) {
        for (int z = 0; z < 129; ++z)
            for (int x = 0; x < 129; ++x) {
                const int wx = 32000 + (x - 64) * 64,
                          wz = 19200 + (z - 64) * 64;
                if (wx >= p.x && wx < p.x + p.size() && wz >= p.z &&
                    wz < p.z + p.size()) {
                    CHECK(!seen[z * 129 + x]);
                    seen[z * 129 + x] = true;
                }
            }
        if (p.x <= 32000 && p.x + p.size() > 32000 && p.z <= 19200 &&
            p.z + p.size() > 19200)
            EQUAL(p.spacing, 1, "near camera is exact");
    }
    for (int z = 0; z < 129; ++z)
        for (int x = 0; x < 129; ++x)
            if (std::hypot((x - 64) * 64, (z - 64) * 64) < 4096)
                CHECK(seen[z * 129 + x]);
    // 2:1 balance: edge-adjacent patches differ by at most one level.
    for (const auto& a : layout)
        for (const auto& b : layout) {
            const bool alongX = a.x + a.size() == b.x && a.z < b.z + b.size() &&
                                b.z < a.z + a.size();
            const bool alongZ = a.z + a.size() == b.z && a.x < b.x + b.size() &&
                                b.x < a.x + a.size();
            if (alongX || alongZ)
                CHECK(a.spacing <= b.spacing * 2 && b.spacing <= a.spacing * 2);
        }
    const auto corner =
        TerrainLod::select(256, 384, {0, 450, 0}, 720, {}, true);
    CHECK(!corner.empty());
    for (const auto& p : corner) CHECK(p.x >= 0 && p.z >= 0);
}
TEST(lod_cover_fallback) {
    using TerrainLod::Patch;
    const Patch root{0, 0, 4};
    const auto parts = TerrainLod::children(root);
    std::set<Patch> drawable;
    int width = 128;
    const auto choose = [&](const std::set<Patch>& split) {
        const auto inView = [&](const Patch& p) { return p.x < width; };
        const auto isDrawable = [&](const Patch& p) {
            return drawable.contains(p);
        };
        const auto below = [&](const Patch& p) {
            return std::any_of(drawable.begin(), drawable.end(), [&](auto& q) {
                return q.spacing < p.spacing && q.x >= p.x && q.z >= p.z &&
                       q.x < p.x + p.size() && q.z < p.z + p.size();
            });
        };
        const TerrainLod::Cover cover(split, inView, isDrawable, below);
        std::vector<Patch> out;
        cover.select(root, out);
        // Every in-view tile of the root is covered exactly once.
        for (int z = 0; z < root.size(); z += 8)
            for (int x = 0; x < std::min(width, root.size()); x += 8) {
                int owners = 0;
                for (const auto& p : out)
                    owners += x >= p.x && x < p.x + p.size() && z >= p.z &&
                              z < p.z + p.size();
                if (!out.empty()) EQUAL(owners, 1, "single coverage");
            }
        return out;
    };
    const std::set<Patch> splitRoot{root};
    // Only the coarse root has arrived: it stays until all children exist.
    drawable = {root, parts[0], parts[1], parts[2]};
    CHECK(choose(splitRoot) == std::vector{root});
    const TerrainLod::Cover prospective(
        splitRoot, [](const Patch&) { return true; },
        [&](const Patch& p) { return drawable.contains(p) || p == parts[3]; },
        [](const Patch&) { return false; });
    CHECK(prospective.coveredByDescendants(root));
    // A transaction cannot also evict a sibling supporting the replacement.
    drawable.erase(parts[2]);
    CHECK(!prospective.coveredByDescendants(root));
    drawable.insert(parts[2]);
    drawable.insert(parts[3]);
    EQUAL(choose(splitRoot).size(), size_t(4), "children replace root");
    // Zooming out: the desired root is missing, finer leftovers stay.
    drawable = {parts[0], parts[1], parts[2], parts[3]};
    EQUAL(choose({}).size(), size_t(4), "leftovers cover a loading root");
    drawable.insert(root);
    CHECK(choose({}) == std::vector{root});
    // Nothing cached yet: nothing drawn (startup).
    drawable.clear();
    CHECK(choose(splitRoot).empty());
    // Children outside the world or view never block a replacement.
    width = 64;
    drawable = {root, parts[0], parts[2]};
    EQUAL(choose(splitRoot).size(), size_t(2), "out-of-view child skipped");
}
TEST(lod_loaded_water_mesh) {
    test::Assets assets;
    Chunk chunk{};
    test::fill(chunk);
    chunk.tiles[2][3][1].liquid = LiquidType::Water;
    chunk.tiles[2][3][1].liquidLevel = 255;
    chunk.tiles[2][3][1].lightLevel.sun_exposure = 15;
    ChunkMesher mesher(assets);
    Tile air{};
    const auto mesh =
        mesher.build(chunk, {0, 0, 0}, [&](int, int, int) { return &air; });
    EQUAL(mesh.water.indices.size(), size_t(6), "one water surface");
    for (const auto& v : mesh.water.vertices) {
        CHECK(v.layer == -2);
        CHECK(v.y == 4);
        CHECK(v.ny == 1);
    }

    // Two finite cells with different fills meet at one continuous edge.
    for (int x : {1, 2}) {
        auto& cell = chunk.tiles[2][3][x];
        cell.liquid = LiquidType::Water;
        cell.waterOrigin = WaterOrigin::Local;
        cell.liquidLevel = x == 1 ? 20 : 80;
    }
    const auto film = mesher.build(
        chunk, {}, [&](int, int, int) { return &air; }, {}, {},
        [&](int x, int z) -> std::pair<int, WaterSurface> {
            if (z == 2 && (x == 1 || x == 2))
                return {2,
                        {4, 0, 0, {}, true, chunk.tiles[2][3][x].liquidLevel}};
            return {2, {}};
        });
    static_assert(Vec2{}.x == 0 && Vec2{}.y == 0);
    int shared = 0;
    for (const auto& v : film.water.vertices) {
        if (v.ny == 1) CHECK(v.layer == -2);
        if (v.x != 2 || v.ny != 1) continue;
        CHECK(std::abs(v.y - (3 + 50.f / 255)) < .00001f);
        ++shared;
    }
    EQUAL(shared, 4, "both tops use the same two corner heights");
    for (const auto& v : film.water.vertices) CHECK(v.x != 2 || v.nx == 0);
    chunk.tiles[2][3][2] = {};
    chunk.tiles[2][3][1] = Tile{};
    chunk.tiles[2][15][1] =
        Tile{.liquid = LiquidType::Water, .liquidLevel = 255, .lightLevel = {}};
    const auto unknown = [](int, int, int) -> const Tile* { return nullptr; };
    CHECK(mesher.build(chunk, {0, 24, 0}, unknown).water.indices.empty());
    const auto boundary =
        mesher.build(chunk, {0, 24, 0}, unknown, {}, [&](int x, int y, int z) {
            EQUAL(x, 1, "halo x");
            EQUAL(y, 400, "halo y is in the slab above");
            EQUAL(z, 2, "halo z");
            return &air;
        });
    EQUAL(boundary.water.indices.size(), size_t(6), "water top from halo");
    for (const auto& v : boundary.water.vertices) {
        EQUAL(v.y, 16.f, "boundary height");
        CHECK(v.x >= 1.f && v.x <= 2.f && v.z >= 2.f && v.z <= 3.f);
    }
    const Tile solid{.block_id = 1, .lightLevel = {}};
    const auto occluded = mesher.build(
        chunk, {0, 24, 0}, [&](int, int, int) { return &solid; }, {},
        [&](int, int, int) { return &air; });
    CHECK(occluded.water.indices.empty());
    const Tile partial{
        .liquid = LiquidType::Water, .liquidLevel = 64, .lightLevel = {}};
    CHECK(WaterSurface{}.submerged(399.2f, -1, &partial));
    CHECK(!WaterSurface{}.submerged(399.3f, -1, &partial));
    // Inside a generated lake the higher cell owns the exposed strip, even
    // when it is full and both columns have the same integer surface level.
    Chunk lake{};
    lake.tiles[2][3][1] = Tile{.liquid = LiquidType::Water, .liquidLevel = 255};
    lake.tiles[2][3][2] = Tile{.liquid = LiquidType::Water, .liquidLevel = 220};
    const auto stepped = mesher.build(
        lake, {}, [&](int, int, int) { return &air; }, {}, {},
        [](int, int) { return std::pair{2, WaterSurface{4, 1}}; });
    std::vector<float> edge;
    for (const auto& v : stepped.water.vertices)
        if (v.nx == 1 && v.x == 2) edge.push_back(v.y);
    EQUAL(edge.size(), 4u, "one face between the two levels");
    EQUAL(*std::min_element(edge.begin(), edge.end()), 3 + 220 / 255.f,
          "strip starts at the lower water level");
    EQUAL(*std::max_element(edge.begin(), edge.end()), 4.f,
          "strip ends at the upper water level");
}

TEST(lod_detail_survives_refresh) {
    TerrainLod::DetailCoverage coverage(TerrainLod::Qualities[0].chunkRadius);
    const std::array bands{TerrainLod::SurfaceBand{7, 7, 26, 28}};
    bool current = true, drawable = true;
    auto latest = [&](ChunkCoord) { return current; };
    auto uploaded = [&](ChunkCoord) { return drawable; };
    coverage.update({7, 28, 7}, bands, latest, uploaded);
    CHECK(coverage.contains(7, 7));
    const auto version = coverage.version();
    // Cross x=128 while the distant frontier has no exact patches and a
    // sunlight remesh is pending. The same uploaded chunks stay visible.
    current = false;
    coverage.update({8, 28, 7}, {}, latest, uploaded);
    CHECK(coverage.contains(7, 7));
    EQUAL(coverage.version(), version, "LOD refresh does not reset detail");
    coverage.update({8, 28, 7}, bands, latest, uploaded);
    CHECK(coverage.contains(7, 7));
    drawable = false;
    coverage.update({8, 28, 7}, bands, latest, uploaded);
    CHECK(!coverage.contains(7, 7));
    // A replacement at the same coordinate has no uploaded mesh yet.
    coverage.update({8, 28, 7}, bands, latest, uploaded);
    CHECK(!coverage.contains(7, 7));
    current = drawable = true;
    coverage.update({8, 28, 7}, bands, latest, uploaded);
    CHECK(coverage.contains(7, 7));
}
TEST(lod_detail_hysteresis) {
    constexpr std::array radii{4, 6, 8};
    for (size_t i = 0; i < radii.size(); ++i) {
        const int radius = TerrainLod::Qualities[i].chunkRadius;
        EQUAL(radius, radii[i], "voxel radius for terrain preset");
        TerrainLod::DetailCoverage coverage(radius);
        const std::array bands{TerrainLod::SurfaceBand{20, 7, 26, 28}};
        const auto ready = [](ChunkCoord) { return true; };
        const unsigned enter = unsigned(20 + radius - 2);
        coverage.update({enter, 28, 7}, bands, ready, ready);
        CHECK(coverage.contains(20, 7));
        coverage.update({enter + 1, 28, 7}, bands, ready, ready);
        CHECK(coverage.contains(20, 7));
        // Walk beyond the detailed chunk box while the old mesh still
        // exists. Distant terrain must be visible when looking back.
        coverage.update({enter + 2, 28, 7}, bands, ready, ready);
        CHECK(!coverage.contains(20, 7));
        coverage.update({enter + 3, 28, 7}, bands, ready, ready);
        CHECK(!coverage.contains(20, 7));
        coverage.update({enter + 1, 28, 7}, bands, ready, ready);
        CHECK(!coverage.contains(20, 7));
        coverage.update({enter, 28, 7}, bands, ready, ready);
        CHECK(coverage.contains(20, 7));

        // Moving above the old surface band has the same handoff.
        coverage.update({enter, unsigned(26 + radius), 7}, bands, ready, ready);
        CHECK(!coverage.contains(20, 7));
    }
}

TEST(lod_detail_low_handoff_wraps) {
    TerrainLod::DetailCoverage coverage(TerrainLod::Qualities[0].chunkRadius);
    const std::array bands{TerrainLod::SurfaceBand{63, 7, 26, 28}};
    const auto ready = [](ChunkCoord) { return true; };
    coverage.update({1, 28, 7}, bands, ready, ready, 64);
    CHECK(coverage.contains(63, 7));
    coverage.update({2, 28, 7}, bands, ready, ready, 64);
    CHECK(coverage.contains(63, 7));
    // Beyond the retained ring across the wrapping seam.
    coverage.update({3, 28, 7}, bands, ready, ready, 64);
    CHECK(!coverage.contains(63, 7));

    // The water slab can be ready with the ocean floor outside the draw band.
    // Pending uploads, light failure, and eviction leave the proxy visible.
    test::Assets assets;
    auto world = worldForLod(8, 1, 350);
    SurfaceSampler sampler(world, assets);
    const auto bandsAtSeam = TerrainLod::sampleBands(63, 7, sampler);
    CHECK(bandsAtSeam.water);
    const std::array waterBands{*bandsAtSeam.water};
    const ChunkCoord slab{63, unsigned(waterBands[0].minY), 7};
    TerrainLod::DetailCoverage water(4), ground(4);
    bool current = false, mesh = false, light = false;
    auto drawable = [&](ChunkCoord) { return mesh && light; };
    const auto frame = [&](unsigned x, bool detail) {
        const ChunkCoord camera{x, slab.y, 7};
        ground.update(
            camera, std::span(&bandsAtSeam.ground, 1),
            [](ChunkCoord) { return false; }, drawable, 64);
        water.update(
            camera, waterBands,
            [&](ChunkCoord c) { return current && drawable(c); }, drawable, 64);
        CHECK(!ground.contains(63, 7));
        const bool detailed = water.contains(63, 7);
        const bool proxy = !detailed;
        EQUAL(int(proxy) + int(detailed && drawable(slab)), 1,
              "one drawable owner");
        CHECK(detailed == detail);
    };
    frame(1, false);
    mesh = current = true;
    frame(1, false);
    light = true;
    frame(1, true);
    current = false;
    frame(2, true);
    light = false;
    frame(2, false);
    light = true;
    current = true;
    frame(1, true);
    mesh = false;
    frame(1, false);
    CHECK(TerrainLod::shareEdge({992, 96, 1}, {0, 96, 1}, 1024));
    CHECK(!TerrainLod::shareEdge({992, 96, 1}, {0, 96, 1}, 0));
    const auto west =
        TerrainLod::build({960, 64, 2}, sampler, running, 4, true);
    const auto east = TerrainLod::build({0, 64, 2}, sampler, running, 4, true);
    const auto edgeHeights = [](const auto& mesh, float x) {
        std::map<float, float> result;
        for (const auto& v : mesh.water.vertices)
            if (v.x == x) result[v.z] = v.y;
        return result;
    };
    const auto westEdge = edgeHeights(west, 1024.f);
    CHECK(!westEdge.empty());
    CHECK(westEdge == edgeHeights(east, 0.f));
}

TEST(lod_local_water_keeps_drawable_detail_during_remesh) {
    constexpr ChunkCoord River{7, 25, 7}, Canal{7, 24, 7};
    test::Assets assets;
    Chunk chunk{};
    auto& local = chunk.tiles[2][3][1];
    local.liquid = LiquidType::Water;
    local.liquidLevel = 64;
    local.waterOrigin = WaterOrigin::Local;
    const Tile air{};
    const auto columns = [](int, int) {
        return std::pair{int(Canal.y) * 16 + 2,
                         WaterSurface{int(Canal.y) * 16 + 4}};
    };
    ChunkMesher mesher(assets);
    const auto build = [&] {
        return mesher.build(
            chunk, Canal, [&](int, int, int) { return &air; }, {}, {}, columns);
    };
    const auto partial = build();
    EQUAL(partial.waterTops[2 * 16 + 1], 3.f + 64.f / 255.f,
          "voxel surface keeps its fractional height");
    local.liquidLevel = 255;
    const auto full = build();
    CHECK(std::any_of(full.water.vertices.begin(), full.water.vertices.end(),
                      [](const auto& v) { return v.ny == 0; }));

    TerrainLod::DetailCoverage coverage(4);
    auto drawable = [](ChunkCoord) { return true; };
    const std::array riverBands{TerrainLod::SurfaceBand{7, 7, 25, 25}};
    coverage.update(River, riverBands, drawable, drawable, 0, true);
    TerrainLod::DetailCoverage::Columns bands{{{7, 7}, riverBands[0]}};
    TerrainLod::DetailCoverage::includeSlab(bands, Canal);
    const std::array withCanal{bands.begin()->second};
    coverage.update(
        River, withCanal, [&](ChunkCoord c) { return c == River; }, drawable, 0,
        true);
    EQUAL(coverage.columns().at({7, 7}).minY, 25,
          "keep river detail while canal upload is pending");
    coverage.update(River, {}, drawable, drawable, 0, true);
    CHECK(coverage.contains(7, 7));
    coverage.update(River, withCanal, drawable, drawable, 0, true);
    EQUAL(coverage.columns().at({7, 7}).minY, 24,
          "the fractional canal acquires detail below the proxy band");

    bands.clear();
    TerrainLod::DetailCoverage::includeSlab(bands, Canal);
    TerrainLod::DetailCoverage noProxy(4);
    const std::array localOnly{bands.begin()->second};
    noProxy.update(River, localOnly, drawable, drawable, 0, true);
    CHECK(noProxy.contains(7, 7));

    ChunkHandle handle;
    handle.meshTopologyRevision = 3;
    handle.meshRevision = 7;
    CHECK(handle.canPublishMesh(6));
    CHECK(!handle.canPublishMesh(2));
    handle.uploaded = true;
    handle.uploadedRevision = 6;
    CHECK(!handle.canPublishMesh(6));
    CHECK(handle.canPublishMesh(7));
    handle.meshTopologyRevision = handle.meshRevision = 8;
    CHECK(!handle.canPublishMesh(7));
}

TEST(lod_mesh_height_matches_triangles) {
    test::Assets assets;
    auto world = worldForLod();
    world.regions[2 * 6 + 2].land_height_m = 500;
    world.regions[2 * 6 + 2].addFeature(FeatureMask::CLIFF);
    SurfaceSampler sampler(world, assets);
    for (int spacing : {2, 4, 8}) {
        const TerrainLod::Patch patch{256, 256, spacing};
        const auto mesh = TerrainLod::build(patch, sampler, running);
        int checked = 0;
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            const auto a = mesh.vertices[mesh.indices[i]],
                       b = mesh.vertices[mesh.indices[i + 1]],
                       c = mesh.vertices[mesh.indices[i + 2]];
            if (a.layer != -1 || b.layer != -1 || c.layer != -1 ||
                a.ny <= .01f || b.ny <= .01f || c.ny <= .01f)
                continue;
            const float x = (a.x + b.x + c.x) / 3, z = (a.z + b.z + c.z) / 3;
            CHECK(close(TerrainLod::meshHeight(patch, sampler, x, z),
                        (a.y + b.y + c.y) / 3, .002f));
            ++checked;
        }
        CHECK(checked > 1000);
    }
}
TEST(lod_detail_coarse_joins) {
    test::Assets assets;
    auto world = worldForLod();
    world.regions[2 * 6 + 2].land_height_m = 500;
    world.regions[2 * 6 + 2].addFeature(FeatureMask::CLIFF);
    SurfaceSampler sampler(world, assets);
    const TerrainLod::Patch patch{256, 256, 8};
    const std::array layout{patch};
    const std::array columns{TerrainLod::SurfaceBand{17, 17, 26, 31},
                             TerrainLod::SurfaceBand{18, 17, 26, 31}};
    const auto detailed = [](int x, int z) {
        return (x == 17 || x == 18) && z == 17;
    };
    const auto mesh =
        TerrainLod::joinDetails(layout, columns, sampler, detailed);
    CHECK(!mesh.vertices.empty());
    CHECK(mesh.bytes() <= TerrainLod::UploadBudget);
    for (size_t i = 0; i < mesh.vertices.size(); i += 4) {
        const auto& a = mesh.vertices[i];
        const auto& b = mesh.vertices[i + 1];
        const auto& c = mesh.vertices[i + 2];
        const auto& d = mesh.vertices[i + 3];
        // No internal wall between adjacent detailed columns.
        CHECK(!(a.x == 288 && b.x == 288));
        CHECK(close(d.y, TerrainLod::meshHeight(patch, sampler, d.x, d.z)));
        CHECK(close(c.y, TerrainLod::meshHeight(patch, sampler, c.x, c.z)));
        CHECK(a.y == b.y);
    }
}
namespace {
// Lowers every tile of chunk columns [x0, x1] x [z0, z1] by `depth`.
std::shared_ptr<const EditMap> loweredEdits(SurfaceSampler& generated, int x0,
                                            int x1, int z0, int z1, int depth) {
    auto map = std::make_shared<EditMap>();
    for (int cz = z0; cz <= z1; ++cz)
        for (int cx = x0; cx <= x1; ++cx) {
            auto edit = std::make_shared<ColumnEdit>();
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x) {
                    const int i = ColumnEdit::index(x, z);
                    edit->groundY[i] = int16_t(
                        generated.height(cx * 16 + x, cz * 16 + z) - depth);
                    edit->groundBlock[i] = 1;
                    edit->waterY[i] = -1;
                }
            edit->finish();
            (*map)[editKey(cx, cz)] = std::move(edit);
        }
    return map;
}
}  // namespace
TEST(lod_edits_lower_ground) {
    test::Assets assets;
    auto world = worldForLod();
    SurfaceSampler plain(world, assets);
    const auto edits = loweredEdits(plain, 17, 17, 17, 17, 10);
    SurfaceSampler sampler(world, assets, nullptr, edits);
    for (int z = 272; z < 288; ++z)
        for (int x = 272; x < 288; ++x) {
            EQUAL(sampler.height(x, z), plain.height(x, z) - 10, "height");
            EQUAL(sampler.column(x, z).surfaceY,
                  plain.column(x, z).surfaceY - 10, "column");
            // Pit walls keep the generated strata.
            EQUAL(sampler.materialAt(x, 400, z), plain.materialAt(x, 400, z),
                  "strata");
        }
    EQUAL(sampler.height(271, 280), plain.height(271, 280), "outside");
    // Stepped patch: tops follow the pit and a side wall reaches its floor.
    const auto mesh =
        TerrainLod::build(TerrainLod::Patch{256, 256, 1}, sampler, running);
    for (int z = 272; z < 288; ++z)
        for (int x = 272; x < 288; ++x) {
            const float top = sampler.height(x, z) + 1;
            CHECK(std::any_of(
                mesh.vertices.begin(), mesh.vertices.end(), [&](auto& v) {
                    return v.x == x && v.z == z && v.y == top && v.ny == 1;
                }));
        }
    const float floor = sampler.height(272, 280) + 1;
    CHECK(std::any_of(mesh.vertices.begin(), mesh.vertices.end(), [&](auto& v) {
        return v.x == 272 && v.y == floor && v.nx != 0;
    }));
    // Smooth patch over a large lowered area sits lower.
    const auto wide = loweredEdits(plain, 16, 31, 16, 31, 20);
    SurfaceSampler lowered(world, assets, nullptr, wide);
    const TerrainLod::Patch smooth{256, 256, 8};
    const auto average = [&](SurfaceSampler& s) {
        const auto m = TerrainLod::build(smooth, s, running);
        double sum = 0;
        int count = 0;
        for (const auto& v : m.vertices)
            if (v.layer == -1 && v.ny > .01f) {
                sum += v.y;
                ++count;
            }
        CHECK(count > 0);
        return sum / count;
    };
    CHECK(average(lowered) < average(plain) - 15);
    EQUAL(lowered.structureTop(300, 300), -1, "no structure");
}
