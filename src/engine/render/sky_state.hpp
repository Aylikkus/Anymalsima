#pragma once

#include <array>

struct SkyState {
    // xyz: unit direction toward the sun, w: strength of outdoor sky light.
    std::array<float, 4> sun{-0.349563f, 0.848939f, 0.399501f, 1.f};
    std::array<float, 4> seed{};
};
