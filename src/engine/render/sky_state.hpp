#pragma once

#include <array>

struct SkyState {
    // xyz: unit direction toward the sun, w: strength of outdoor sky light.
    std::array<float, 4> sun{-0.349563f, 0.848939f, 0.399501f, 1.f};
    std::array<float, 4> seed{};
    // Regional weather presentation (docs/handoff/weather.md, step 09):
    // x cloud cover 0..1, y visual precipitation 0..1, z snow fraction 0..1,
    // w weather time in game hours for movement. All zero is clear.
    std::array<float, 4> weather{};
    // Accumulated ground-parallel cloud drift in cloud-shell units. The CPU
    // integrates wind over time so moving the camera or a changing wind
    // direction never multiplies a fresh direction by the whole elapsed time.
    std::array<float, 4> cloudOffset{};
};
