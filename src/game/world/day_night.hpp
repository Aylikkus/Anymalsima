#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

#include "render/sky_state.hpp"

// 0 is midnight, 0.25 sunrise, 0.5 noon, and 0.75 sunset.
inline SkyState skyAtTime(double timeOfDay, uint64_t worldSeed = 0) {
    const double angle = (timeOfDay - 0.25) * 2.0 * std::numbers::pi;
    // A fixed 20-degree tilt keeps the sun and moon away from the pole.
    constexpr double tilt = 20.0 * std::numbers::pi / 180.0;
    const double height = std::sin(angle);
    const auto elevation = float(height * std::cos(tilt));
    const float t = std::clamp((elevation + 0.18f) / 0.40f, 0.f, 1.f);
    const float daylight = t * t * (3.f - 2.f * t);
    SkyState sky{{float(std::cos(angle)), elevation,
                  float(height * std::sin(tilt)),
                  std::lerp(0.16f, 1.f, daylight)}};
    // SplitMix64 samples preserve variation from every bit of the world seed.
    for (float& value : sky.seed) {
        worldSeed += 0x9e3779b97f4a7c15ull;
        uint64_t bits = worldSeed;
        bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9ull;
        bits = (bits ^ (bits >> 27)) * 0x94d049bb133111ebull;
        bits ^= bits >> 31;
        value = float(bits >> 40) / 16777216.f;
    }
    return sky;
}
