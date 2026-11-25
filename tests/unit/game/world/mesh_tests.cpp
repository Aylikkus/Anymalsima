#include "world/world_editor.hpp"
#include "world_fixture.hpp"

namespace {
ChunkMesh mesh(const Chunk& chunk, ChunkCoord coord = {}) {
    test::Assets assets;
    ChunkMesher mesher(assets);
    const Tile air;
    return mesher.build(chunk, coord, [&](int, int, int) { return &air; });
}
void expectQuads(const ChunkMesh& result, size_t count) {
    EQUAL(result.vertices.size(), count * 4, "quad vertices");
    EQUAL(result.indices.size(), count * 6, "triangle indices");
    for (auto index : result.indices) CHECK(index < result.vertices.size());
}

float triangleArea(const BlockVertex& a, const BlockVertex& b,
                   const BlockVertex& c) {
    const Vec3 u{b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3 v{c.x - a.x, c.y - a.y, c.z - a.z};
    const Vec3 cross{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                     u.x * v.y - u.y * v.x};
    CHECK(cross.x * a.nx + cross.y * a.ny + cross.z * a.nz > 0);
    CHECK(a.nx == b.nx && a.ny == b.ny && a.nz == b.nz);
    CHECK(a.nx == c.nx && a.ny == c.ny && a.nz == c.nz);
    return std::sqrt(cross.x * cross.x + cross.y * cross.y +
                     cross.z * cross.z) *
           .5f;
}

template <typename Include>
float area(const ChunkMesh& result, Include include) {
    float sum = 0;
    for (size_t i = 0; i < result.indices.size(); i += 3) {
        const auto& a = result.vertices.at(result.indices[i]);
        const auto& b = result.vertices.at(result.indices[i + 1]);
        const auto& c = result.vertices.at(result.indices[i + 2]);
        if (include(a) && include(b) && include(c))
            sum += triangleArea(a, b, c);
    }
    return sum;
}
}  // namespace

TEST(mesh_empty) {
    Chunk chunk;
    expectQuads(mesh(chunk), 0);
}

TEST(mesh_isolated_block) {
    Chunk chunk;
    chunk.tiles[8][8][8].block_id = 1;
    expectQuads(mesh(chunk), 6);
}

TEST(mesh_river_step_edges) {
    const test::Assets assets;
    const Tile air;
    Chunk chunk;
    for (int x : {7, 8}) {
        for (int y = 0; y <= 4; ++y) chunk.tiles[8][y][x].block_id = 1;
        auto& water = chunk.tiles[8][5][x];
        water.liquid = LiquidType::Water;
        water.liquidLevel = x == 7 ? 192 : 96;
        water.waterOrigin = WaterOrigin::Reservoir;
    }
    const auto column = [&](int x, int z) -> std::pair<int, WaterSurface> {
        if (z != 8 || (x != 7 && x != 8)) return {-1, {}};
        return {4, {6, 0, 1, {1, 0}, false, chunk.tiles[8][5][x].liquidLevel}};
    };
    const auto result = ChunkMesher(assets).build(
        chunk, {}, [&](int, int, int) { return &air; }, {}, {}, column);
    size_t highEdge = 0, lowEdge = 0, between = 0;
    const float high = 5 + 192 / 255.f, low = 5 + 96 / 255.f;
    for (const auto& v : result.water.vertices) {
        CHECK(v.y >= 5);
        if (v.x != 8) continue;
        if (v.ny == 1 && v.y == high) ++highEdge;
        if (v.ny == 1 && v.y == low) ++lowEdge;
        if (v.nx == 1 && (v.y == high || v.y == low)) ++between;
    }
    EQUAL(highEdge, 2u, "higher top meets the step face");
    EQUAL(lowEdge, 2u, "lower top meets the step face");
    EQUAL(between, 4u, "one face closes the step");

    const auto side = Chunk::cell(9, 5, 8);
    const Tile bank{.block_id = 1, .lightLevel = {}};
    chunk.setCell(side, bank);
    const auto cube = ChunkMesher(assets).build(
        chunk, {}, [&](int, int, int) { return &air; }, {}, {}, column);
    chunk.setCell(side, bank, TileShape::make(TileShapeProfile::Wall10));
    const auto partial = ChunkMesher(assets).build(
        chunk, {}, [&](int, int, int) { return &air; }, {}, {}, column);
    CHECK(partial.water.indices == cube.water.indices);
    CHECK(partial.water.vertices.size() == cube.water.vertices.size());
    for (size_t i = 0; i < cube.water.vertices.size(); ++i) {
        const auto& a = cube.water.vertices[i];
        const auto& b = partial.water.vertices[i];
        CHECK(a.x == b.x && a.y == b.y && a.z == b.z && a.nx == b.nx &&
              a.ny == b.ny && a.nz == b.nz);
    }
}

TEST(mesh_greedy_prism) {
    Chunk chunk;
    chunk.tiles[8][8][8].block_id = 1;
    chunk.tiles[8][8][9].block_id = 1;
    auto result = mesh(chunk);
    expectQuads(result, 6);
    float area = 0;
    for (size_t i = 0; i < result.vertices.size(); i += 4) {
        const auto& a = result.vertices[i];
        const auto& b = result.vertices[i + 1];
        const auto& d = result.vertices[i + 3];
        const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        const float vx = d.x - a.x, vy = d.y - a.y, vz = d.z - a.z;
        const float cx = uy * vz - uz * vy;
        const float cy = uz * vx - ux * vz;
        const float cz = ux * vy - uy * vx;
        area += std::sqrt(cx * cx + cy * cy + cz * cz);
    }
    // 10 exposed unit faces, each grown by the seam overlap on every side.
    CHECK(area > 10.0f && area < 10.1f);
}

TEST(mesh_chunk_seam) {
    test::Assets assets;
    ChunkMesher mesher(assets);
    Chunk chunk;
    chunk.tiles[7][6][15].block_id = 1;
    Tile air, solid;
    solid.block_id = 1;
    bool sawNeighbour = false;
    const auto result =
        mesher.build(chunk, {2, 3, 4}, [&](int x, int y, int z) {
            if (x == 48 && y == 54 && z == 71) {
                sawNeighbour = true;
                return &solid;
            }
            return &air;
        });
    CHECK(sawNeighbour);
    expectQuads(result, 5);
    // Positions are chunk-local (chunk {2,3,4} starts at 32,48,64).
    constexpr float e = ChunkMesher::SeamOverlap * 1.01f;
    for (const auto& vertex : result.vertices) {
        CHECK(vertex.x >= 15 - e && vertex.x <= 16 + e);
        CHECK(vertex.y >= 6 - e && vertex.y <= 7 + e);
        CHECK(vertex.z >= 7 - e && vertex.z <= 8 + e);
    }
}

TEST(mesh_material_light) {
    Chunk chunk;
    chunk.tiles[8][8][8].block_id = 1;
    chunk.tiles[8][8][8].lightLevel.sun_exposure = 3;
    const auto result = mesh(chunk);
    expectQuads(result, 6);
    for (const auto& vertex : result.vertices) {
        EQUAL(vertex.layer, 7.0f, "material array layer");
        // Light comes from the GPU light volume, never from vertex tint.
        EQUAL(vertex.tint, 0xFFFFFFFFu, "neutral tint");
    }
}

TEST(mesh_merges_across_light) {
    Chunk chunk;
    chunk.tiles[8][8][8].block_id = 1;
    chunk.tiles[8][8][9].block_id = 1;
    chunk.tiles[8][8][8].lightLevel.sun_exposure = 15;
    chunk.tiles[8][8][9].lightLevel.sun_exposure = 2;
    // Different light levels no longer split greedy quads.
    expectQuads(mesh(chunk), 6);
}

TEST(mesh_winding) {
    Chunk chunk;
    chunk.tiles[8][8][8].block_id = 1;
    const auto result = mesh(chunk);
    expectQuads(result, 6);
    for (size_t i = 0; i < result.indices.size(); i += 3) {
        const auto& a = result.vertices[result.indices[i]];
        const auto& b = result.vertices[result.indices[i + 1]];
        const auto& c = result.vertices[result.indices[i + 2]];
        const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        const float dot = (uy * vz - uz * vy) * a.nx +
                          (uz * vx - ux * vz) * a.ny +
                          (ux * vy - uy * vx) * a.nz;
        CHECK(dot > 0);
    }
}

TEST(mesh_shape_surfaces) {
    const Tile solid{.block_id = 1, .lightLevel = {}};
    const auto everything = [](const BlockVertex&) { return true; };
    for (uint8_t profile = 1; profile <= 23; ++profile) {
        float referenceArea = 0;
        for (uint8_t rotation = 0; rotation < 4; ++rotation) {
            const auto shape = TileShape::make(TileShapeProfile(profile),
                                               TileFacing(rotation));
            Chunk chunk;
            chunk.setCell(Chunk::cell(8, 8, 8), solid, shape);
            const auto result = mesh(chunk);
            const float total = area(result, everything);
            const auto surface = tile_geometry::surface(shape);
            size_t triangles = 0;
            for (unsigned i = 0; i < surface.count; ++i)
                triangles += surface.faces[i].count - 2;
            EQUAL(result.indices.size(), triangles * 3, "exterior triangles");
            if (rotation == 0) referenceArea = total;
            CHECK(std::abs(total - referenceArea) < 1e-5f);
            if (profile >= 12) {
                std::vector<Vec3> topNormals;
                for (const auto& v : result.vertices)
                    if (v.ny > 0) {
                        const Vec3 n{v.nx, v.ny, v.nz};
                        if (std::find(topNormals.begin(), topNormals.end(),
                                      n) == topNormals.end())
                            topNormals.push_back(n);
                    }
                EQUAL(topNormals.size(), 2u, "junction facet normals");
            }
        }
    }
}

TEST(mesh_shape_chunk_interfaces) {
    test::Assets assets;
    ChunkMesher mesher(assets);
    const Tile air{}, solid{.block_id = 1, .lightLevel = {}};
    const auto axisOf = [](const BlockVertex& v, int axis) {
        return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
    };
    const auto check = [&](int axis, TileShape lowShape, TileShape highShape,
                           float lowArea, float highArea) {
        Chunk low, high;
        int lo[] = {7, 7, 7}, hi[] = {7, 7, 7};
        lo[axis] = 15;
        hi[axis] = 0;
        low.setCell(Chunk::cell(lo[0], lo[1], lo[2]), solid, lowShape);
        high.setCell(Chunk::cell(hi[0], hi[1], hi[2]), solid, highShape);
        const ChunkCoord highCoord{unsigned(axis == 0), unsigned(axis == 1),
                                   unsigned(axis == 2)};
        const auto neighbor = [&](int x, int y,
                                  int z) -> ChunkMesher::Neighbour {
            const int world[] = {x, y, z};
            for (int i = 0; i < 3; ++i)
                if (i != axis && world[i] != lo[i]) return &air;
            if (world[axis] == 15)
                return {&low.tiles[lo[2]][lo[1]][lo[0]], lowShape};
            if (world[axis] == 16)
                return {&high.tiles[hi[2]][hi[1]][hi[0]], highShape};
            return &air;
        };
        const auto left = mesher.build(low, {}, neighbor);
        const auto right = mesher.build(high, highCoord, neighbor);
        const auto atSeam = [&](float boundary) {
            return [&, boundary](const BlockVertex& v) {
                return axisOf(v, axis) == boundary;
            };
        };
        CHECK(std::abs(area(left, atSeam(16)) - lowArea) < 1e-5f);
        CHECK(std::abs(area(right, atSeam(0)) - highArea) < 1e-5f);
    };
    const auto slab = TileShape::make(TileShapeProfile::Slab);
    const auto ramp = TileShape::make(TileShapeProfile::Ramp);
    const auto corner = TileShape::make(TileShapeProfile::OuterCorner);
    const auto wall = TileShape::make(TileShapeProfile::Wall10);
    const auto thick = TileShape::make(TileShapeProfile::Wall75);
    check(0, {}, slab, .5f, 0);
    check(0, slab, {}, 0, .5f);
    check(2, {}, slab, .5f, 0);
    check(2, slab, {}, 0, .5f);
    check(0, ramp, ramp.rotated(2), .25f, .25f);
    check(0, ramp, ramp, 0, 0);
    check(0, corner, ramp, 0, 0);
    check(0, wall, wall.rotated(2), .1f, .1f);
    check(0, thick, thick.rotated(2), .25f, .25f);
    check(1, {}, wall, .9f, 0);
    check(1, wall, {}, 0, .9f);
    check(1, {}, slab, 0, 0);
    // The cube above a slab has no occluder and keeps a greedy quad.
    check(1, slab, {}, 0, std::pow(1 + 2 * ChunkMesher::SeamOverlap, 2));
}

TEST(mesh_skips_hidden_chunks) {
    test::WorldFixture f;
    const ChunkCoord center{4, 4, 4};
    const ChunkCoord around[] = {center,    {5, 4, 4}, {3, 4, 4}, {4, 5, 4},
                                 {4, 3, 4}, {4, 4, 5}, {4, 4, 3}};
    for (const auto& coord : around) test::fill(f.load(coord), 1);
    for (int i = 0; i < 3; ++i) {
        f.chunks.update();
        f.wait();
    }
    // Solid and covered on all six faces: finished without a worker job.
    f.chunks.markDirty(center, true);
    f.chunks.update();
    EQUAL(f.chunks.meshInFlight(), size_t(0), "hidden chunk skips meshing");
    // A partial tile in the neighbour's touching layer also opens the boundary.
    WorldEditor editor(f.chunks, &f.sunlight, f.assets.getStorage<Block>());
    const TilePos neighbor{80, 72, 72};
    const auto wall = TileShape::make(TileShapeProfile::Wall10);
    CHECK(editor.setBlock(neighbor, 1, wall) == EditStatus::Applied);
    f.chunks.update();
    CHECK(f.chunks.meshInFlight(center));

    // Leave that job uncollected, then rotate the same material at the border.
    // The job retains its old shape snapshot and a new topology revision
    // queues.
    CHECK(editor.setBlock(neighbor, 1, wall.rotated(1)) == EditStatus::Applied);
    f.wait();
    f.chunks.update();
    CHECK(f.chunks.meshInFlight(center));
    f.wait();
    f.chunks.update();
    CHECK(!f.chunks.meshInFlight(center));

    const TilePos interior{72, 72, 72};
    CHECK(editor.setBlock(neighbor, 1) == EditStatus::Applied);
    CHECK(editor.setBlock(interior, 1, wall) == EditStatus::Applied);
    f.chunks.update();
    CHECK(f.chunks.meshInFlight(center));
    f.chunks.unregister(center);
    f.wait();
    f.chunks.update();
    CHECK(!f.chunks.isRegistered(center) && !f.chunks.meshInFlight(center));
}

TEST(mesh_local_positions_overlap) {
    // An L of blocks: on the +Z plane a 3-wide quad meets a 1-wide quad in
    // a T-junction at (1, 1).
    Chunk chunk;
    for (int x = 0; x < 3; ++x) chunk.tiles[5][0][x].block_id = 1;
    chunk.tiles[5][1][0].block_id = 1;
    const auto result = mesh(chunk, {40, 30, 20});
    constexpr float e = ChunkMesher::SeamOverlap;
    for (const auto& v : result.vertices) {
        CHECK(v.x >= -e * 1.01f && v.x <= 16 + e * 1.01f);
        CHECK(v.y >= -e * 1.01f && v.y <= 16 + e * 1.01f);
        CHECK(v.z >= -e * 1.01f && v.z <= 16 + e * 1.01f);
    }
    float lowerTop = -1, upperBottom = 99, lowerLeft = 99;
    for (size_t i = 0; i < result.vertices.size(); i += 4) {
        if (result.vertices[i].nz < .5f) continue;  // +Z faces only
        float minY = 99, maxY = -99, minX = 99, maxX = -99;
        for (size_t k = i; k < i + 4; ++k) {
            minY = std::min(minY, result.vertices[k].y);
            maxY = std::max(maxY, result.vertices[k].y);
            minX = std::min(minX, result.vertices[k].x);
            maxX = std::max(maxX, result.vertices[k].x);
        }
        if (maxX > 2) {  // the 3-wide quad
            lowerTop = maxY;
            lowerLeft = minX;
        } else {
            upperBottom = minY;
        }
    }
    // The quads overlap across the shared edge instead of just touching.
    CHECK(lowerTop > 1 && upperBottom < 1 && lowerTop > upperBottom);
    CHECK(lowerLeft < 0);
}
