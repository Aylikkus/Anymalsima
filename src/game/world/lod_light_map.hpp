#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "light_sources.hpp"

// Distant glow of small lights (radius <= LightEmission::NearRange) for
// terrain drawn without voxel light: a coarse top-down light map around the
// camera. Towns read as glowing patches at night from far away.
//
// Each texel covers TilesPerTexel^2 tiles: rgb = summed light (color times
// intensity times falloff at the texel centre), a = height of the light that
// contributes most, so shaders can fade light on terrain far above or below.
// Far-reaching lights are not included: they are evaluated per pixel.
class LodLightMap {
   public:
    static constexpr int Size = 1024;
    static constexpr int TilesPerTexel = 4;
    static constexpr int Extent = Size * TilesPerTexel;  // tiles per side
    // The map recentres on the camera in steps of this many tiles.
    static constexpr int Snap = 512;

    // True when the snapped centre moved or the registry changed.
    bool stale(double eyeX, double eyeZ, uint64_t registryVersion) const;
    // eyeX/eyeZ: camera position (canonical X); periodX: world width when X
    // wraps. Light X positions are placed at their copy nearest the centre.
    void build(const LightSourceRegistry& registry, double eyeX, double eyeZ,
               int periodX);

    bool valid() const { return built; }
    double originX() const { return minX; }
    double originZ() const { return minZ; }
    // RGBA floats, row-major (Z rows, X columns), Size x Size.
    const std::vector<float>& texels() const { return data; }
    // Texel (rgb, height) covering world (x, z), or zeros outside.
    std::array<float, 4> at(double x, double z) const;
    size_t lightCount() const { return included; }

   private:
    static double snapped(double v) {
        return std::floor(v / Snap) * Snap + Snap / 2.0;
    }
    std::vector<float> data;
    double minX = 0, minZ = 0;
    double centreX = 0, centreZ = 0;
    uint64_t version = 0;
    bool built = false;
    size_t included = 0;
};
