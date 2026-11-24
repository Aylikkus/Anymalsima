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
    // One opening in a touching neighbour layer exposes a centre face.
    f.chunks.getChunk({5, 4, 4})->tiles[8][8][0].block_id = 0;
    f.chunks.markDirty(center, true);
    f.chunks.update();
    CHECK(f.chunks.meshInFlight(center));
    f.wait();
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
