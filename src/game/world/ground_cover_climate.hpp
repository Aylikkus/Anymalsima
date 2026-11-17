#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "chunk.hpp"
#include "consts.hpp"
#include "items/flora_json.hpp"
#include "world.hpp"

// Climate behind the ground-cover tint, shared by detailed chunks
// (ChunkManager -> ChunkMesher) and distant terrain (SurfaceSampler), so
// grass keeps its colour across the detail/LOD hand-over.
namespace ground_cover_climate {

// Humidity and temperature (C) at a world tile, bilinear between the centres
// of the four nearest regions, so the tint has no seams at region borders.
// X wraps; Z clamps to the map. A world without a map gives a mild default.
inline std::pair<float, float> at(const World& world, double x, double z) {
    if (world.regions.empty() || world.width <= 0 || world.height <= 0)
        return {0.5f, 15.f};
    constexpr double size = CHUNKS_PER_REGION * Chunk::SIZE_X;
    const double fx = x / size - 0.5, fz = z / size - 0.5;
    const auto gx = int64_t(std::floor(fx)), gz = int64_t(std::floor(fz));
    const double tx = fx - double(gx), tz = fz - double(gz);
    double humidity = 0, temperature = 0;
    for (int dz = 0; dz < 2; ++dz)
        for (int dx = 0; dx < 2; ++dx) {
            const auto rx = world.wrap().wrapRegion(gx + dx);
            const auto rz = std::clamp<int64_t>(gz + dz, 0, world.height - 1);
            const auto& r = world.regions[size_t(rz) * world.width +
                                          size_t(std::clamp<int64_t>(
                                              rx, 0, world.width - 1))];
            const double w = (dx ? tx : 1 - tx) * (dz ? tz : 1 - tz);
            humidity += w * r.humidity;
            temperature += w * r.temperature_c;
        }
    return {float(humidity), float(temperature)};
}

// RGB565, red in the high bits; decoded by terrain_lod.frag.
inline uint16_t packRgb565(const Color3& c) {
    const auto q = [](float v, int bits) {
        return uint16_t(
            std::lround(std::clamp(v, 0.f, 1.f) * float((1 << bits) - 1)));
    };
    return uint16_t(q(c.r, 5) << 11 | q(c.g, 6) << 5 | q(c.b, 5));
}

}  // namespace ground_cover_climate
