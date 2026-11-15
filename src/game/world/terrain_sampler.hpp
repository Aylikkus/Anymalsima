#pragma once

#include <array>
#include <cmath>

#include "chunk.hpp"
#include "soil_generator.hpp"
#include "world.hpp"

namespace TerrainTuning {
inline constexpr int RegionX = CHUNKS_PER_REGION * Chunk::SIZE_X;
inline constexpr int RegionZ = CHUNKS_PER_REGION * Chunk::SIZE_Z;
inline constexpr double PlainRelief = 3;
inline constexpr double HillRelief = 12;
inline constexpr double MountainRelief = 32;
inline constexpr double WaterRelief = 4;
inline constexpr int BroadScale = 64;
inline constexpr int FineScale = 16;
inline constexpr double SoilSlopeStart = .25;
inline constexpr double SoilSlopeEnd = 1;
inline constexpr double CoastWidth = 32;
inline constexpr double SoilHalfWidth = 32;
inline constexpr double CliffWidth = 8;
inline constexpr int RavineDrop = 16;
}  // namespace TerrainTuning

struct TerrainSample {
    int surface_y = 0;
    double slope = 0;
};

struct TerrainRegion {
    Region region{};
    SoilProfile soil;
    std::vector<SoilPlacement> soil_rules;
};

// A two-region halo is sufficient for sampling the adjacent column's surface,
// its controls and water distances. It owns no references into the live world.
struct TerrainNeighborhood {
    std::array<TerrainRegion, 25> regions;
    Vec2u center{};
    int width = 0;
    int height = 0;
    uint32_t height_seed = 0;
    uint32_t feature_seed = 0;

    static TerrainNeighborhood capture(const World& world, Vec2u center,
                                       const IAssets& assets);
    const TerrainRegion& at(int x, int z) const {
        x = std::clamp(x, 0, width - 1);
        z = std::clamp(z, 0, height - 1);
        const int lx = x - static_cast<int>(center.x) + 2;
        const int lz = z - static_cast<int>(center.y) + 2;
        if (lx < 0 || lx >= 5 || lz < 0 || lz >= 5)
            throw std::out_of_range("Terrain sample outside neighborhood halo");
        return regions[lz * 5 + lx];
    }
};

struct ColumnGenerationInput {
    int surface_y = 0;
    SoilColumn soil;
};

struct ChunkColumnInputs {
    std::array<ColumnGenerationInput, Chunk::SIZE_X * Chunk::SIZE_Z> columns;
};

class TerrainSampler {
   public:
    explicit TerrainSampler(const TerrainNeighborhood& data) : data(data) {}
    double surface(double x, double z) const;
    TerrainSample sample(int64_t x, int64_t z) const;
    SoilColumn soil(int64_t x, int64_t z, const TerrainSample& terrain) const;
    ChunkColumnInputs columns(unsigned chunkX, unsigned chunkZ) const;

   private:
    const TerrainNeighborhood& data;
    double control(int gx, int gz, bool amplitude) const;
    double cliffLift(int rx, int rz, double x, double z) const;
};
