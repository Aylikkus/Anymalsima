#pragma once

#include <algorithm>
#include <cmath>

#include "consts.hpp"

// Maps the normalized world-map height (0..1, sea at 0.5) to a world Y.
// Plains stay close to the sea; above MountainStart the profile rises
// exponentially so mountains tower over the lowlands.
namespace HeightProfile {
inline constexpr float SeaHeight = .5f;
inline constexpr float MountainStart = .6f;
// The world-map noise practically never exceeds ~0.85 (measured p99.9 is
// ~0.76-0.78 across seeds), so the curve reaches PeakY there and clamps.
inline constexpr float MountainTop = .85f;
inline constexpr int OceanFloorY = 256;
inline constexpr int PlainsTopY = 460;
inline constexpr int SkySpace = 128;
inline constexpr int PeakY = WORLD_HEIGHT_Y - SkySpace;
inline constexpr float MountainCurve = 1.8f;

inline int heightToY(float h) {
    h = std::clamp(h, 0.f, 1.f);
    float y;
    if (h < SeaHeight) {
        y = OceanFloorY + (SEA_LEVEL_Y - OceanFloorY) * (h / SeaHeight);
    } else if (h < MountainStart) {
        y = SEA_LEVEL_Y + (PlainsTopY - SEA_LEVEL_Y) * (h - SeaHeight) /
                              (MountainStart - SeaHeight);
    } else {
        const float t =
            std::min(1.f, (h - MountainStart) / (MountainTop - MountainStart));
        y = PlainsTopY + (PeakY - PlainsTopY) *
                             (std::exp(MountainCurve * t) - 1) /
                             (std::exp(MountainCurve) - 1);
    }
    return std::clamp(int(y), 0, PeakY);
}
}  // namespace HeightProfile
