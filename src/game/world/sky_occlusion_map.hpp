#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

// Coarse sky light from SkyHeightMap (docs/handoff/sky-light.md).
// Stores opaque heights around the camera for surfaces drawn without a
// voxel light volume (distant terrain, joins, structure slabs).
//
// It follows the voxel sunlight (SunLightPropagator): sky light falls
// straight down at full strength until something opaque stops it, then
// spreads sideways losing one level per tile. A point at height y gets
// 15 - d of 15, where d is the 4-neighbour distance in tiles to the nearest
// column whose occluders all lie below y. Each texel covers one footprint
// cell (TilesPerTexel^2 tiles) and stores, for k = 0..3, the lowest
// occluder top within k texels (Thresholds): a point at height y is k
// texels from open sky for the smallest k whose threshold is at most y
// (4 or more: dark). So the flared base of a trunk does not darken the
// trunk above it, while the ground under a wide roof is dark. Pure; built
// on the pool.
class SkyOcclusionMap {
   public:
    static constexpr int TilesPerTexel = 4;
    static constexpr int Size = 1024;
    static constexpr int Extent = Size * TilesPerTexel;  // tiles per side
    // The map recentres on the camera in steps of this many tiles.
    static constexpr int Snap = 512;
    // Sky light levels of the voxel sunlight.
    static constexpr int MaxLight = 15;
    static constexpr int Thresholds = 4;
    // Threshold of open sky (no occluder within k texels); fits half floats.
    static constexpr float Open = -30000.f;

    // Whether a map built for (eyeX, eyeZ) would have another centre.
    bool recentres(double eyeX, double eyeZ) const {
        return !built || snapped(eyeX) != centreX || snapped(eyeZ) != centreZ;
    }
    // Fill the row-major array with coarse heights (exclusive), starting
    // at the supplied world origin. Unset texels retain Open.
    using Raster = std::function<void(int, int, std::vector<float>&)>;
    void build(double eyeX, double eyeZ, const Raster& raster);

    bool valid() const { return built; }
    double originX() const { return minX; }
    double originZ() const { return minZ; }
    // Thresholds floats per texel, row-major (Z rows, X columns).
    const std::vector<float>& texels() const { return data; }
    // Texel covering world (x, z); all Open outside.
    std::array<float, Thresholds> at(double x, double z) const;
    // Sky fraction (0-1) at a point, as the shaders compute it (nearest
    // texel; the shaders blend the four around).
    float sky(double x, double y, double z) const;
    static float sky(const std::array<float, Thresholds>& texel, double y);
    // Texels with an occluder (debug).
    size_t occluded() const { return covered; }

    // Sky fraction k texels (4-neighbour) from open sky, at the texel
    // centre: d = k * TilesPerTexel - (TilesPerTexel - 1) / 2 tiles.
    static float skyAtDistance(int k);
    // Thresholds are tops of coarse cells, so a point counts as above one
    // from Fade[0] below it (fully from Fade[1] below): the occluder's own
    // upper faces stay lit. Keep in sync with shaders/world_surface.sh.
    static constexpr float Fade[2] = {2.f * TilesPerTexel, 1.f * TilesPerTexel};

   private:
    static double snapped(double v) {
        return std::floor(v / Snap) * Snap + Snap / 2.0;
    }
    std::vector<float> data;
    double minX = 0, minZ = 0;
    double centreX = 0, centreZ = 0;
    bool built = false;
    size_t covered = 0;
};
