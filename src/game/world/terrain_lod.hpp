#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <vector>

#include "chunk_coord.hpp"
#include "render/frustrum.hpp"
#include "surface_sampler.hpp"

namespace TerrainLod {
inline constexpr int Cells = 32;
inline constexpr int RootSpacing = 64;
// Player-selectable terrain detail ("terrainDetail" setting).
struct Quality {
    size_t gpuBudget;     // resident LOD meshes and material maps
    size_t uploadBudget;  // LOD bytes uploaded per frame
    float viewDistance;   // LOD range, fog end and far plane
    float exactRadius;    // stepped (spacing 1) patches around the camera
    float pixelError;     // projected error that splits a patch
    int materialMap;      // smooth patch material texels per side
    int chunkRadius;      // detailed voxel streaming and draw radius
};
inline constexpr Quality Qualities[] = {
    {64u << 20, 2u << 20, 4096, 128, 2.0f, 32, 4},   // Low
    {128u << 20, 3u << 20, 6144, 192, 1.4f, 64, 6},  // Medium (2x budget)
    {256u << 20, 4u << 20, 8192, 256, 1.0f, 64, 8},  // High (4x budget)
};
inline constexpr int QualityCount = int(std::size(Qualities));
inline constexpr int MaxChunkRadius = Qualities[QualityCount - 1].chunkRadius;
// Low preset defaults; also the smallest per-frame upload of any preset.
inline constexpr float ViewDistance = Qualities[0].viewDistance;
inline constexpr size_t GpuBudget = Qualities[0].gpuBudget;
inline constexpr size_t UploadBudget = Qualities[0].uploadBudget;
// A mesh depends only on its own square: edges toward finer or coarser
// neighbours are hidden by skirts, so a patch never rebuilds when a
// neighbour changes resolution. Edge order elsewhere: -X, +X, -Z, +Z.
struct Patch {
    int x = 0, z = 0, spacing = 1;
    int size() const { return Cells * spacing; }
    bool operator==(const Patch&) const = default;
    auto operator<=>(const Patch&) const = default;
};
struct SurfaceBand {
    int x, z, minY, maxY;
    bool operator==(const SurfaceBand&) const = default;
};
struct Geometry {
    std::vector<BlockVertex> vertices;
    std::vector<uint32_t> indices;
    AABB bounds{};
    size_t bytes() const {
        return vertices.size() * sizeof(BlockVertex) +
               indices.size() * sizeof(uint32_t);
    }
};
struct WaterBand {
    SurfaceBand slabs;
    std::array<int16_t, Chunk::SIZE_X * Chunk::SIZE_Z> levels;
    bool matches(ChunkCoord coord, std::span<const float> uploaded) const;
};
struct ColumnBands {
    SurfaceBand ground;
    std::optional<WaterBand> water;
};
ColumnBands sampleBands(int cx, int cz, SurfaceSampler& sampler);

struct Mesh : Geometry {
    Geometry water;
    // RG: surface material layer, BA: bare rock layer, little endian.
    std::vector<uint8_t> materials;
    int mapSize = 1;
    // Ground cover (CoverTexel per texel): R overlay layer + 1, G density,
    // BA tint RGB565. coverSize texels per side over the whole patch.
    std::vector<uint8_t> covers;
    int coverSize = 1;
    std::vector<ColumnBands> bands;
    // RG: water boundary Y, little endian, zero for dry ground.
    std::vector<uint8_t> waterLevels;
    int waterMapSize = 1;
    float error = 0;
    size_t bytes() const {
        return Geometry::bytes() + water.bytes() + materials.size() +
               covers.size() + waterLevels.size();
    }
};
using ErrorFn = std::function<float(const Patch&)>;
// periodX > 0: the world wraps on X with this period (tiles). Selection then
// runs around the eye across the seam and returns canonical patches
// (x in [0, periodX)). Requires wrapsExactly(periodX, viewDistance).
std::vector<Patch> select(int width, int depth, Vec3 eye, float viewportHeight,
                          const ErrorFn& error, bool coarse = false,
                          float pixelError = 2, float exactRadius = 128,
                          std::optional<Vec3> detailFocus = std::nullopt,
                          float viewDistance = ViewDistance, int periodX = 0);
// The root lattice tiles the period and no patch is selected twice.
inline bool wrapsExactly(int periodX, float viewDistance) {
    constexpr int root = Cells * RootSpacing;
    return periodX > 0 && periodX % root == 0 &&
           2 * viewDistance + 2 * root <= periodX;
}
// The four quadrants of a patch, and the patch that contains it.
std::array<Patch, 4> children(const Patch& patch);
Patch parent(const Patch& patch);
bool shareEdge(const Patch& a, const Patch& b, int periodX);
// Smooth patches hang a skirt of this depth below every inner edge; stepped
// patches extend their boundary side walls this far below the lower column.
float skirtDepth(const Patch& patch, float error);
inline constexpr int SteppedSkirt = 8;
// materialMap: texels per side of a smooth patch's material map. Sampling it
// is the largest part of a smooth patch's build cost.
// wrapX: the world wraps on X, so X edges get skirts toward the patch
// across the seam instead of walls down to the floor.
Mesh build(const Patch& patch, SurfaceSampler& sampler,
           const std::atomic_bool& cancelled, int materialMap = Cells * 2,
           bool wrapX = false);
// Height of the actual triangulated LOD surface, for joining it to loaded
// column boundaries independently of the current LOD resolution.
float meshHeight(const Patch& patch, SurfaceSampler& sampler, float x, float z);
using DetailedFn = std::function<bool(int, int)>;
// periodX > 0: layout is canonical while columns may lie in the camera's
// frame (outside [0, periodX)); joins are emitted in the columns' frame.
Mesh joinDetails(std::span<const Patch> layout,
                 std::span<const SurfaceBand> columns, SurfaceSampler& sampler,
                 const DetailedFn& detailed, int periodX = 0);
}  // namespace TerrainLod
