#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "render/frustrum.hpp"
#include "surface_sampler.hpp"

namespace TerrainLod {
inline constexpr int Cells = 32;
inline constexpr int RootSpacing = 64;
inline constexpr float ViewDistance = 4096;
inline constexpr size_t GpuBudget = 64 * 1024 * 1024;
inline constexpr size_t UploadBudget = 2 * 1024 * 1024;
// Edge order: -X, +X, -Z, +Z. Neighbors are part of the mesh identity.
struct Patch {
    int x = 0, z = 0, spacing = 1;
    std::array<int, 4> neighbors{1, 1, 1, 1};
    int size() const { return Cells * spacing; }
    bool operator==(const Patch&) const = default;
    auto operator<=>(const Patch&) const = default;
};
struct SurfaceBand {
    int x, z, minY, maxY;
    bool operator==(const SurfaceBand&) const = default;
};
struct Mesh {
    std::vector<BlockVertex> vertices;
    std::vector<uint32_t> indices;
    // RG: surface material layer, BA: bare rock layer, little endian.
    std::vector<uint8_t> materials;
    int mapSize = 1;
    std::vector<SurfaceBand> bands;
    AABB bounds{};
    float error = 0;
    size_t bytes() const {
        return vertices.size() * sizeof(BlockVertex) +
               indices.size() * sizeof(uint32_t) + materials.size();
    }
};
using ErrorFn = std::function<float(const Patch&)>;
std::vector<Patch> select(int width, int depth, Vec3 eye, float viewportHeight,
                          const ErrorFn& error, bool coarse = false,
                          float pixelError = 2, float exactRadius = 128,
                          std::optional<Vec3> detailFocus = std::nullopt);
Mesh build(const Patch& patch, SurfaceSampler& sampler,
           const std::atomic_bool& cancelled);
// Height of the actual triangulated LOD surface, for joining it to loaded
// column boundaries independently of the current LOD resolution.
float meshHeight(const Patch& patch, SurfaceSampler& sampler, float x, float z);
using DetailedFn = std::function<bool(int, int)>;
Mesh joinDetails(std::span<const Patch> layout,
                 std::span<const SurfaceBand> columns, SurfaceSampler& sampler,
                 const DetailedFn& detailed);

// Shared boundary interpolation used by both tessellation and transition joins.
float edgeHeight(const Patch& patch, SurfaceSampler& sampler, int edge,
                 float along);
}  // namespace TerrainLod
