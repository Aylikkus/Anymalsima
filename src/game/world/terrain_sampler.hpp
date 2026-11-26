#pragma once

#include <array>
#include <cmath>
#include <memory>
#include <unordered_map>

#include "chunk.hpp"
#include "ground_cover_generator.hpp"
#include "height_profile.hpp"
#include "natural_shape.hpp"
#include "soil_generator.hpp"
#include "water_surface.hpp"
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
    double originalSlope = 0;
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
    int generatorVersion = 0;
    uint32_t height_seed = 0;
    uint32_t feature_seed = 0;
    std::shared_ptr<const GeneratedHydrology> hydrology;

    static TerrainNeighborhood capture(const World& world, Vec2u center,
                                       const IAssets& assets);
    // Natural ramps were introduced with WorldMeta::NaturalShapeVersion.
    // Older saved generators keep the cube-only surface rule.
    bool naturalShapes() const;
    // Generator 26 matches shared ramp edges; 25 kept cube landings.
    bool ridgeShapes() const;
    // Generator 27 uses the diagonal corner ramp for two-axis steps.
    bool diagonalShapes() const;
    // World width in tiles: the X period of every sampled field.
    int periodX() const {
        return width > 0 ? width * TerrainTuning::RegionX : 0;
    }
    // X wraps around the world; Z clamps to its edge rows.
    const TerrainRegion& at(int x, int z) const {
        z = std::clamp(z, 0, height - 1);
        int64_t dx = int64_t(x) - center.x;
        if (width > 0 && (dx < -width / 2 || dx >= width - width / 2))
            dx = WorldWrap::delta(x, center.x, width);
        const auto lx = dx + 2;
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
    WaterSurface water;
    // Generated surface profile for the tile at surface_y. Cube for legacy
    // worlds and for columns the ramp rule leaves uncut.
    TileShape groundShape{};
    // Preserve the original hydrology when a reservoir snapshot updates water.
    bool generatedWet = false;
    // Largest local grade in the one-tile neighbourhood of a polar footprint.
    double snowSlope = 0;
};

struct ChunkColumnInputs {
    std::array<ColumnGenerationInput, Chunk::SIZE_X * Chunk::SIZE_Z> columns;
};

class TerrainSampler {
   public:
    explicit TerrainSampler(const TerrainNeighborhood& data) : data(data) {}
    // The pure-shape cache is owned per sampler; the field's callbacks point
    // back at it, so a sampler must stay in place.
    TerrainSampler(const TerrainSampler&) = delete;
    TerrainSampler& operator=(const TerrainSampler&) = delete;
    TerrainSampler(TerrainSampler&&) = delete;
    TerrainSampler& operator=(TerrainSampler&&) = delete;
    double surface(double x, double z) const;
    // Floor of the generated surface at a whole tile, memoized with the
    // natural-shape rule's column heights.
    int tileSurface(int64_t x, int64_t z) const;
    bool belowSea(int x, int z) const;
    TerrainSample sample(int64_t x, int64_t z) const;
    double snowSlope(int64_t x, int64_t z) const;
    // Generated surface profile of one tile, using the same deterministic
    // rule as columns(). Empty when the world's generator predates natural
    // shapes or the column is not eligible.
    TileShape surfaceShape(int64_t x, int64_t z) const;
    WaterSurface water(int64_t x, int64_t z, int groundY) const;
    SoilColumn soil(int64_t x, int64_t z, const TerrainSample& terrain) const;
    SoilColumn soil(int64_t x, int64_t z, const TerrainSample& terrain,
                    WaterSurface water) const;
    ChunkColumnInputs columns(unsigned chunkX, unsigned chunkZ) const;

   private:
    struct Heights {
        double ground;
        double original;
    };
    Heights heightsAt(double x, double z) const;
    bool shapeEligible(int64_t x, int64_t z) const;
    // Memoized column heights for the pure shape rule. A region-wide sampler
    // reuses them across every query, including the terrain halo primed by
    // columns(); a bounded cache keeps a long-lived sampler cheap.
    Heights shapeHeights(int64_t x, int64_t z) const;
    void rememberShapeHeight(int64_t x, int64_t z, Heights value) const;

    const TerrainNeighborhood& data;
    double control(int gx, int gz, bool amplitude) const;
    double cliffLift(int rx, int rz, double x, double z) const;
    double coastDistance(int rx, int rz, double x, double z) const;

    mutable std::unordered_map<uint64_t, Heights> shapeHeightCache;
    mutable std::unordered_map<uint64_t, bool> shapeEligibleCache;
    mutable std::unique_ptr<natural_shape::Field> shapeField;
};
