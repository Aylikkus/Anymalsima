#include <bit>
#include <chrono>
#include <map>
#include <set>

#include "test.hpp"
#include "test_assets.hpp"
#include "world/chunk_mesher.hpp"
#include "world/detail_coverage.hpp"
#include "world/location_generator.hpp"
#include "world/lod_frontier.hpp"

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
            const auto& band = mesh.bands[(z - 256) / 16 * 2 + (x - 256) / 16];
            CHECK(band.minY <= (height - 1) / 16);
            CHECK(band.maxY >= (height - 1) / 16);
        }
    CHECK(mesh.bytes() < TerrainLod::UploadBudget);
}
TEST(lod_smooth_stitched_edges) {
    test::Assets assets;
    auto world = worldForLod();
    SurfaceSampler sampler(world, assets);
    TerrainLod::Patch coarse{256, 256, 4};
    coarse.neighbors.fill(4);
    coarse.neighbors[1] = 2;
    TerrainLod::Patch fine{384, 256, 2};
    fine.neighbors.fill(2);
    fine.neighbors[0] = 4;
    const auto a = TerrainLod::build(coarse, sampler, running);
    const auto b = TerrainLod::build(fine, sampler, running);
    CHECK(!a.vertices.empty());
    CHECK(!b.vertices.empty());
    for (int t = 0; t <= 64; ++t) {
        const float h = TerrainLod::edgeHeight(fine, sampler, 0, t);
        const int start = t / 2 * 2;
        const float h0 = TerrainLod::edgeHeight(coarse, sampler, 1, start);
        const float h1 = TerrainLod::edgeHeight(coarse, sampler, 1, start + 2);
        CHECK(close(h, std::lerp(h0, h1, (t - start) * .5f)));
    }
    const auto coarseEdge = boundary(a, 0, 384), fineEdge = boundary(b, 0, 384);
    for (const auto& [point, unused] : coarseEdge) {
        if (point[2] >= 512 && point[2] <= 640) CHECK(fineEdge.contains(point));
    }
    CHECK(a.bytes() < TerrainLod::UploadBudget);
}
TEST(lod_stepped_smooth_join) {
    test::Assets assets;
    auto world = worldForLod();
    world.regions[2 * 6 + 2].land_height_m = 500;
    world.regions[2 * 6 + 2].addFeature(FeatureMask::CLIFF);
    SurfaceSampler sampler(world, assets);
    TerrainLod::Patch smooth{320, 256, 2};
    smooth.neighbors.fill(2);
    smooth.neighbors[0] = 1;
    const auto mesh = TerrainLod::build(smooth, sampler, running);
    // Every column along the transition has a join endpoint at its voxel top.
    for (int z = 256; z < 320; ++z) {
        const float y = sampler.height(319, z) + 1;
        bool found = false;
        for (const auto& v : mesh.vertices)
            if (v.x == 320 && v.z == z && close(v.y, y)) {
                found = true;
                break;
            }
        CHECK(found);
    }
}
TEST(lod_water_and_bounds) {
    test::Assets assets;
    auto world = worldForLod(2, 2, 350);
    SurfaceSampler sampler(world, assets);
    TerrainLod::Patch patch{0, 0, 8};
    patch.neighbors.fill(8);
    const auto mesh = TerrainLod::build(patch, sampler, running);
    bool water = false, floor = false;
    for (const auto& v : mesh.vertices) {
        CHECK(v.x >= 0 && v.x <= 256 && v.z >= 0 && v.z <= 256);
        if (v.layer == -2) {
            water = true;
            CHECK(v.y == SEA_LEVEL_Y + 1);
            CHECK(v.ny > 0);
        }
        if (v.y == 0) floor = true;
    }
    CHECK(water);
    CHECK(floor);
    CHECK(mesh.bytes() < TerrainLod::UploadBudget);
    EQUAL(sampler.column(20, 20).waterY, SEA_LEVEL_Y + 1, "ocean");
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
        for (int n : p.neighbors)
            CHECK(n <= p.spacing * 2 && n * 2 >= p.spacing);
        if (p.x <= 32000 && p.x + p.size() > 32000 && p.z <= 19200 &&
            p.z + p.size() > 19200)
            EQUAL(p.spacing, 1, "near camera is exact");
    }
    for (int z = 0; z < 129; ++z)
        for (int x = 0; x < 129; ++x)
            if (std::hypot((x - 64) * 64, (z - 64) * 64) < 4096)
                CHECK(seen[z * 129 + x]);
    const auto corner =
        TerrainLod::select(256, 384, {0, 450, 0}, 720, {}, true);
    CHECK(!corner.empty());
    for (const auto& p : corner) CHECK(p.x >= 0 && p.z >= 0);
}
TEST(lod_frontier_delays_and_stale) {
    TerrainLod::Frontier frontier;
    TerrainLod::Patch parent{0, 0, 4}, a{0, 0, 2}, b{64, 0, 2}, c{0, 64, 2},
        d{64, 64, 2};
    auto token = frontier.stage({parent});
    CHECK(!frontier.commit());
    CHECK(frontier.publish(token, parent));
    CHECK(frontier.commit());
    token = frontier.stage({a, b, c, d});
    CHECK(frontier.publish(token, a));
    CHECK(frontier.publish(token, b));
    CHECK(frontier.publish(token, c));
    CHECK(!frontier.commit());
    CHECK(frontier.current() == std::vector{parent});
    CHECK(frontier.publish(token, d));
    CHECK(frontier.commit());
    const auto stale = token;
    token = frontier.stage({TerrainLod::Patch{4096, 4096, 64}});
    CHECK(!frontier.publish(stale, parent));
    CHECK(!frontier.commit());
    CHECK(frontier.current() == std::vector({a, b, c, d}));
    CHECK(frontier.publish(token, frontier.pending()[0]));
    CHECK(frontier.commit());
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
    EQUAL(mesh.indices.size(), size_t(6), "one water surface");
    for (const auto& v : mesh.vertices) {
        CHECK(v.layer == -2);
        CHECK(v.y == 4);
        CHECK(v.ny == 1);
    }
}

TEST(lod_detail_survives_refresh) {
    TerrainLod::DetailCoverage coverage;
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
    TerrainLod::DetailCoverage coverage;
    const std::array bands{TerrainLod::SurfaceBand{13, 7, 26, 28}};
    auto ready = [](ChunkCoord) { return true; };
    coverage.update({7, 28, 7}, bands, ready, ready);
    CHECK(coverage.contains(13, 7));
    coverage.update({6, 28, 7}, bands, ready, ready);
    CHECK(coverage.contains(13, 7));
    coverage.update({5, 28, 7}, bands, ready, ready);
    CHECK(!coverage.contains(13, 7));
    coverage.update({6, 28, 7}, bands, ready, ready);
    CHECK(!coverage.contains(13, 7));
    coverage.update({7, 28, 7}, bands, ready, ready);
    CHECK(coverage.contains(13, 7));
}
TEST(lod_mesh_height_matches_triangles) {
    test::Assets assets;
    auto world = worldForLod();
    world.regions[2 * 6 + 2].land_height_m = 500;
    world.regions[2 * 6 + 2].addFeature(FeatureMask::CLIFF);
    SurfaceSampler sampler(world, assets);
    for (int spacing : {2, 4, 8}) {
        TerrainLod::Patch patch{256, 256, spacing};
        patch.neighbors.fill(spacing);
        patch.neighbors[0] = spacing * 2;
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
    TerrainLod::Patch patch{256, 256, 8};
    patch.neighbors.fill(8);
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
        CHECK(close(d.y,
                    std::max(TerrainLod::meshHeight(patch, sampler, d.x, d.z),
                             float(SEA_LEVEL_Y + 1))));
        CHECK(close(c.y,
                    std::max(TerrainLod::meshHeight(patch, sampler, c.x, c.z),
                             float(SEA_LEVEL_Y + 1))));
        CHECK(a.y == b.y);
    }
}
