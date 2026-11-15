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
    EQUAL(area, 10.0f, "2x1x1 prism exposed area");
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
    for (const auto& vertex : result.vertices) {
        CHECK(vertex.x >= 47 && vertex.x <= 48);
        CHECK(vertex.y >= 54 && vertex.y <= 55);
        CHECK(vertex.z >= 71 && vertex.z <= 72);
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
        EQUAL(vertex.tint, 0xFF333333u, "sunlight 3/15 brightness");
    }
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
