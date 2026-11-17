#pragma once

#include <array>
#include <cmath>

#include "chunk.hpp"
#include "ground_cover_generator.hpp"
#include "height_profile.hpp"
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
// Coastline: a domain-warped, signed distance to the land/water boundary
// between regions, so shores wander instead of following region edges.
// Total warp must stay well below one region (see coastDistance).
struct WarpOctave {
    double amplitude;
    int scale;  // avoid multiples of the region size: lattices would align
};
inline constexpr std::array<WarpOctave, 3> CoastWarp{
    {{52, 112}, {22, 44}, {8, 16}}};
inline constexpr double CoastWarpTotal = 52 + 22 + 8;
inline constexpr double CoastJitter = 3;
inline constexpr int CoastJitterScale = 8;
inline constexpr double CoastCap = 40;
inline constexpr double BeachWidth = 12;
inline constexpr int BeachRise = 3;
inline constexpr double ShelfWidth = 24;
inline constexpr double ShelfDrop = 8;
inline constexpr double BlendWidth = 4;
// Relief grows with elevation above the plains so mountains get ridges and
// valleys instead of smooth cones.
inline constexpr double ReliefPerMetre = .35;
// Ridged octaves; mutually non-aligned scales hide value-noise lattices.
inline constexpr std::array<WarpOctave, 4> RidgeOctaves{
    {{.45, 173}, {.3, 89}, {.17, 41}, {.08, 19}}};
inline constexpr double RidgeBlend = 64;
inline constexpr double SoilHalfWidth = 32;
inline constexpr double CliffWidth = 8;
inline constexpr int RavineDrop = 16;
// coastDistance inspects only the 3x3 regions around the sample.
static_assert(CoastWarpTotal + CoastCap + CoastJitter <= RegionX);

// Upper bound of any generated surface column inside a region. A warped
// beach may rise above the ceiling of a water region it intrudes into.
inline int surfaceCeiling(int landHeight) {
    return std::clamp(
        landHeight >= SEA_LEVEL_Y ? landHeight : SEA_LEVEL_Y + 1 + BeachRise, 0,
        WORLD_HEIGHT_Y - 1);
}
}  // namespace TerrainTuning

struct TerrainSample {
    int surface_y = 0;
    double slope = 0;
};

struct TerrainRegion {
    Region region{};
    SoilProfile soil;
    std::vector<SoilPlacement> soil_rules;
    // Cover of the region's own soil and climate; columns that take their
    // soil from this region take this cover too.
    GroundCoverProfile cover;
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
    // World width in tiles: the X period of every sampled field.
    int periodX() const {
        return width > 0 ? width * TerrainTuning::RegionX : 0;
    }
    // X wraps around the world; Z clamps to its edge rows.
    const TerrainRegion& at(int x, int z) const {
        z = std::clamp(z, 0, height - 1);
        const int lx = int(WorldWrap::delta(x, int64_t(center.x), width)) + 2;
        const int lz = z - static_cast<int>(center.y) + 2;
        if (lx < 0 || lx >= 5 || lz < 0 || lz >= 5)
            throw std::out_of_range("Terrain sample outside neighborhood halo");
        return regions[lz * 5 + lx];
    }
};

struct ColumnGenerationInput {
    int surface_y = 0;
    SoilColumn soil;
    // Same value as TerrainSampler::sample(x, z).slope.
    double slope = 0;
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
    double coastDistance(int rx, int rz, double x, double z) const;
};
