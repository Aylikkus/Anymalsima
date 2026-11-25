// CPU budget of natural surface shapes: column inputs, chunk generation and
// near meshing, compared against the legacy cube-only generator. Not
// registered in CTest; run from the repository root after building the
// target:
//
//   build/tests/shape_benchmark
//   build/tests/shape_benchmark --legacy
//
// The scene matches tests/support/lod_scene.hpp (seed 1234/5678,
// 32x32 regions, mountains, mixed coastlines and cliffs).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

#include "lod_scene.hpp"
#include "test_assets.hpp"
#include "world/chunk_mesher.hpp"
#include "world/location_generator.hpp"
#include "world/save/chunk_codec.hpp"
#include "world/save/world_meta.hpp"
#include "world/terrain_sampler.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double millis(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
        .count();
}

}  // namespace

int main(int argc, char** argv) {
    const bool natural = !(argc > 1 && std::string_view(argv[1]) == "--legacy");
    test::Assets assets;
    auto world = lodScene(32);
    world.generatorVersion = natural ? WorldMeta::GeneratorVersion : 0;
    const auto terrain = TerrainNeighborhood::capture(world, {16, 16}, assets);
    const std::vector<RegionLayer> layers = {
        {RegionLayerType::Rock, 1, 1024, 0, 0, 1}};
    const auto& region = world.regions[16 * world.width + 16];
    TerrainSampler sampler(terrain);

    if (argc > 1 && std::string_view(argv[1]) == "--find") {
        constexpr int size = 128, window = 24;
        const int x0 = 16 * TerrainTuning::RegionX;
        const int z0 = 16 * TerrainTuning::RegionZ;
        std::vector<int> score(size_t(size) * size);
        for (int z = 0; z < size; ++z)
            for (int x = 0; x < size; ++x) {
                const auto shape = sampler.surfaceShape(x0 + x, z0 + z);
                score[size_t(z) * size + x] =
                    shape.profile() == TileShapeProfile::Ramp
                        ? 1
                        : (shape.profile() == TileShapeProfile::OuterCorner ||
                                   shape.profile() ==
                                       TileShapeProfile::InnerCorner
                               ? 6
                               : 0);
            }
        int best = 0, bx = 0, bz = 0;
        for (int z = 0; z + window <= size; ++z)
            for (int x = 0; x + window <= size; ++x) {
                int sum = 0;
                for (int dz = 0; dz < window; ++dz)
                    for (int dx = 0; dx < window; ++dx)
                        sum += score[size_t(z + dz) * size + x + dx];
                if (sum > best) best = sum, bx = x, bz = z;
            }
        std::printf("best window tile=%d,%d world=%d,%d score=%d\n",
                    bx + window / 2, bz + window / 2, x0 + bx + window / 2,
                    z0 + bz + window / 2, best);
        const auto heights =
            sampler.sample(x0 + bx + window / 2, z0 + bz + window / 2);
        std::printf("center height=%d\n", heights.surface_y);
        return 0;
    }

    if (argc > 1 && std::string_view(argv[1]) == "--stats") {
        constexpr int margin = 2, size = 128;
        const int x0 = 16 * TerrainTuning::RegionX;
        const int z0 = 16 * TerrainTuning::RegionZ;
        auto top = [&](int x, int z) {
            return int(std::floor(sampler.surface(x0 + x, z0 + z))) + 1;
        };
        size_t candidates = 0, accepted = 0, rejected = 0;
        size_t straight = 0, outer = 0, inner = 0, diagonal = 0;
        size_t continuousSide = 0;
        size_t landingHigher = 0, landingLower = 0, twoPerp = 0, twoOpp = 0;
        size_t deepSide = 0, zeroCardinal = 0, innerPattern = 0, innerExact = 0;
        for (int z = -margin; z < size + margin; ++z)
            for (int x = -margin; x < size + margin; ++x) {
                const int h = top(x, z);
                int lows = 0, lowDir = -1;
                for (int d = 0; d < 4; ++d) {
                    static constexpr int dx[4] = {0, 1, 0, -1};
                    static constexpr int dz[4] = {1, 0, -1, 0};
                    if (top(x + dx[d], z + dz[d]) == h - 1) {
                        ++lows;
                        lowDir = d;
                    }
                }
                if (lows == 2) {
                    static constexpr int dx[4] = {0, 1, 0, -1};
                    static constexpr int dz[4] = {1, 0, -1, 0};
                    bool oppositePair = false;
                    for (int d = 0; d < 4; ++d)
                        if (top(x + dx[d], z + dz[d]) == h - 1 &&
                            top(x + dx[(d + 2) & 3], z + dz[(d + 2) & 3]) ==
                                h - 1)
                            oppositePair = true;
                    if (oppositePair)
                        ++twoOpp;
                    else {
                        ++twoPerp;
                    }
                }
                if (lows == 0) {
                    const bool diagonal = top(x + 1, z + 1) == h - 1 ||
                                          top(x - 1, z + 1) == h - 1 ||
                                          top(x + 1, z - 1) == h - 1 ||
                                          top(x - 1, z - 1) == h - 1;
                    if (diagonal) ++zeroCardinal;
                    static constexpr int dx[4] = {0, 1, 0, -1};
                    static constexpr int dz[4] = {1, 0, -1, 0};
                    bool cardinalsHigh = true;
                    for (int d = 0; d < 4; ++d)
                        cardinalsHigh &= top(x + dx[d], z + dz[d]) == h;
                    if (diagonal && cardinalsHigh) {
                        ++innerPattern;
                        int diagonals = 0;
                        diagonals += top(x + 1, z + 1) == h - 1;
                        diagonals += top(x - 1, z + 1) == h - 1;
                        diagonals += top(x + 1, z - 1) == h - 1;
                        diagonals += top(x - 1, z - 1) == h - 1;
                        if (diagonals == 1) ++innerExact;
                    }
                }
                if (lows == 1) {
                    const int facing = (lowDir + 2) & 3;
                    static constexpr int dx[4] = {0, 1, 0, -1};
                    static constexpr int dz[4] = {1, 0, -1, 0};
                    bool sidesOk = true;
                    for (int d = 0; d < 4; ++d) {
                        if (d == lowDir || d == facing) continue;
                        sidesOk &= top(x + dx[d], z + dz[d]) >= h - 1;
                    }
                    if (!sidesOk)
                        ++deepSide;
                    else if (top(x + dx[facing], z + dz[facing]) > h)
                        ++landingHigher;
                    else if (top(x + dx[facing], z + dz[facing]) < h)
                        ++landingLower;
                    if (sidesOk && top(x + dx[facing], z + dz[facing]) >= h &&
                        top(x + dx[lowDir], z + dz[lowDir]) == h - 1) {
                        ++candidates;
                        const auto shape = sampler.surfaceShape(x0 + x, z0 + z);
                        if (shape.code())
                            ++accepted;
                        else
                            ++rejected;
                    }
                }
                if (x < 0 || z < 0 || x >= size || z >= size) continue;
                const auto shape = sampler.surfaceShape(x0 + x, z0 + z);
                switch (shape.profile()) {
                    case TileShapeProfile::Ramp: {
                        ++straight;
                        const unsigned f = unsigned(shape.facing());
                        static constexpr int dx[4] = {0, 1, 0, -1};
                        static constexpr int dz[4] = {1, 0, -1, 0};
                        const unsigned left = (f + 3) & 3;
                        const unsigned right = (f + 1) & 3;
                        const auto a = sampler.surfaceShape(x0 + x + dx[left],
                                                            z0 + z + dz[left]);
                        const auto b = sampler.surfaceShape(x0 + x + dx[right],
                                                            z0 + z + dz[right]);
                        if (a.profile() == TileShapeProfile::Ramp ||
                            b.profile() == TileShapeProfile::Ramp)
                            ++continuousSide;
                        break;
                    }
                    case TileShapeProfile::OuterCorner:
                        ++outer;
                        break;
                    case TileShapeProfile::InnerCorner:
                        ++inner;
                        break;
                    case TileShapeProfile::DiagonalRamp:
                        ++diagonal;
                        break;
                    default:
                        break;
                }
            }
        std::printf(
            "candidates=%zu accepted=%zu rejected=%zu straight=%zu "
            "outer=%zu inner=%zu diagonal=%zu side_continuous=%zu "
            "landing_higher=%zu landing_lower=%zu two_perp=%zu "
            "two_opposite=%zu deep_side=%zu diagonal_low=%zu "
            "inner_pattern=%zu inner_exact=%zu\n",
            candidates, accepted, rejected, straight, outer, inner, diagonal,
            continuousSide, landingHigher, landingLower, twoPerp, twoOpp,
            deepSide, zeroCardinal, innerPattern, innerExact);
        return 0;
    }

    if (argc > 2 && std::string_view(argv[1]) == "--map") {
        // Shape map: region 16,16 at 4x zoom. Bright colours mark converted
        // cells so terrace edges can be inspected without a GPU.
        constexpr int size = 128, scale = 4;
        const int x0 = 16 * TerrainTuning::RegionX;
        const int z0 = 16 * TerrainTuning::RegionZ;
        std::vector<double> heights(size_t(size) * size);
        for (int z = 0; z < size; ++z)
            for (int x = 0; x < size; ++x)
                heights[size_t(z) * size + x] = sampler.surface(x0 + x, z0 + z);
        FILE* file = std::fopen(argv[2], "wb");
        if (!file) return 1;
        std::fprintf(file, "P6\n%d %d\n255\n", size * scale, size * scale);
        for (int z = 0; z < size; ++z)
            for (int y = 0; y < scale; ++y)
                for (int x = 0; x < size; ++x) {
                    const double h = heights[size_t(z) * size + x];
                    const double west =
                        heights[size_t(z) * size + std::max(0, x - 1)];
                    const double north =
                        heights[size_t(std::max(0, z - 1)) * size + x];
                    const double shade = std::clamp(
                        1 + .35 * ((h - west) + (h - north)), .5, 1.4);
                    int r = int(std::clamp(90 + (h - 300) * 2.0, 30.0, 230.0) *
                                shade);
                    int g = int(std::clamp(110 + (h - 300) * 2.0, 40.0, 230.0) *
                                shade);
                    int b = int(std::clamp(80 + (h - 300) * 1.2, 30.0, 220.0) *
                                shade);
                    const auto shape = sampler.surfaceShape(x0 + x, z0 + z);
                    switch (shape.profile()) {
                        case TileShapeProfile::Ramp:
                            r = 235, g = 70, b = 40;
                            break;
                        case TileShapeProfile::OuterCorner:
                            r = 60, g = 120, b = 235;
                            break;
                        case TileShapeProfile::InnerCorner:
                            r = 60, g = 210, b = 110;
                            break;
                        case TileShapeProfile::DiagonalRamp:
                            r = 240, g = 200, b = 40;
                            break;
                        default:
                            break;
                    }
                    const unsigned char pixel[3] = {uint8_t(r), uint8_t(g),
                                                    uint8_t(b)};
                    for (int sx = 0; sx < scale; ++sx)
                        std::fwrite(pixel, 1, 3, file);
                }
        std::fclose(file);
        return 0;
    }

    size_t shapeCells = 0, chunkCells = 0;
    const auto columnStart = Clock::now();
    std::array<ChunkColumnInputs, 8 * 8> columns{};
    for (int cz = 0; cz < 8; ++cz)
        for (int cx = 0; cx < 8; ++cx) {
            columns[cz * 8 + cx] = sampler.columns(128 + cx, 128 + cz);
            for (const auto& column : columns[cz * 8 + cx].columns) {
                ++chunkCells;
                if (column.groundShape.code()) ++shapeCells;
            }
        }
    const double columnMs = millis(columnStart);

    LocationGenerator generator;
    ChunkMesher mesher(assets);
    const Tile air;
    const auto neighbour = [&](int, int, int) {
        return ChunkMesher::Neighbour(&air);
    };
    size_t vertices = 0, triangles = 0, shapeBytes = 0, encodedBytes = 0;
    double generateMs = 0, meshMs = 0;
    const auto chunkStart = Clock::now();
    for (int cz = 0; cz < 8; ++cz)
        for (int cx = 0; cx < 8; ++cx) {
            const auto& inputs = columns[cz * 8 + cx];
            int top = 0;
            for (const auto& column : inputs.columns)
                top = std::max(top, column.surface_y);
            const unsigned slab = unsigned(top / Chunk::SIZE_Y);
            Chunk chunk;
            auto start = Clock::now();
            generator.generateChunk(
                chunk, {unsigned(128 + cx), slab, unsigned(128 + cz)}, {},
                region, layers, {}, &inputs);
            generateMs += millis(start);
            shapeBytes += chunk.shapeStorageBytes();
            encodedBytes += chunk_codec::encode(chunk).size();
            start = Clock::now();
            const auto mesh = mesher.build(
                chunk, {unsigned(128 + cx), slab, unsigned(128 + cz)},
                neighbour);
            meshMs += millis(start);
            vertices += mesh.vertices.size();
            triangles += mesh.indices.size() / 3;
        }
    const double chunkMs = millis(chunkStart);

    // Dense worst case: every cell shaped (alternating ramp/junction), for
    // deciding whether the sparse table still suffices.
    Chunk dense;
    const Tile solid{.block_id = 1};
    for (int y = 0; y < Chunk::SIZE_Y; ++y)
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int x = 0; x < Chunk::SIZE_X; ++x) {
                const auto shape =
                    (x + z) % 3 == 0 ? TileShape::make(TileShapeProfile::Ramp,
                                                       TileFacing((x + y) & 3))
                    : (x + z) % 3 == 1
                        ? TileShape::make(TileShapeProfile::OuterCorner,
                                          TileFacing(y & 3))
                        : TileShape::make(TileShapeProfile::InnerCorner,
                                          TileFacing(z & 3));
                dense.setCell(Chunk::cell(x, y, z), solid, shape);
            }
    const auto denseEncodeStart = Clock::now();
    const auto denseEncoded = chunk_codec::encode(dense);
    const double denseEncodeMs = millis(denseEncodeStart);
    const auto denseMeshStart = Clock::now();
    const auto denseMesh = mesher.build(dense, {}, neighbour);
    const double denseMeshMs = millis(denseMeshStart);

    std::printf(
        "generator=%s cells=%zu shapes=%zu (%.1f%%) columns_ms=%.1f "
        "generate_ms=%.1f mesh_ms=%.1f shape_bytes=%zu encoded_bytes=%zu "
        "vertices=%zu triangles=%zu total_ms=%.1f\n",
        natural ? "natural" : "legacy", chunkCells, shapeCells,
        100.0 * double(shapeCells) / double(chunkCells), columnMs, generateMs,
        meshMs, shapeBytes, encodedBytes, vertices, triangles,
        columnMs + chunkMs);
    std::printf("chunk_tile_bytes=%zu chunk_object_bytes=%zu "
                "mean_encoded_chunk_bytes=%.2f mean_mesh_ms=%.4f\n",
                sizeof(Chunk::tiles), sizeof(Chunk),
                double(encodedBytes) / columns.size(),
                meshMs / columns.size());
    std::printf(
        "dense shapes=%zu shape_bytes=%zu encoded_bytes=%zu "
        "encode_ms=%.1f mesh_ms=%.1f vertices=%zu triangles=%zu\n",
        dense.shapeCount(), dense.shapeStorageBytes(), denseEncoded.size(),
        denseEncodeMs, denseMeshMs, denseMesh.vertices.size(),
        denseMesh.indices.size() / 3);
    return 0;
}
