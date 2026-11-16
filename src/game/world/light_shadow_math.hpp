#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "items/light_emission.hpp"

// Cube shadow map conventions shared by LightShadows (rendering) and
// shaders/shadow_sampling.sh (sampling). Keep both in sync.
//
// Faces: 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z. A face looks along `forward`
// with `up`; right = cross(up, forward), as bx::mtxLookAt (left-handed)
// builds it. A direction v lands on the face of its major axis at
//   ndc = (dot(v, right), dot(v, up)) / dot(v, forward).
// Each shadowed light owns tiles slot * 6 + face of an atlas Columns tiles
// wide, rows top to bottom.
namespace LightShadowMath {
inline constexpr int Columns = 8;
inline constexpr int FaceCount = 6;
// Each face is rendered slightly wider than 90 degrees: a guard band of about
// this many texels on every side. Face borders (the exact 45 degree directions,
// where walls around a block-aligned lamp meet along an edge) then fall
// inside the tile instead of on its clamped last texel, so they cannot
// leak.
inline constexpr double GuardTexels = 2;
// ndc range of a tile, [-scale, scale], for tileSize texels.
inline double guardScale(int tileSize) {
    return 1 + 2 * GuardTexels / tileSize;
}
// Vertical field of view (degrees) of a face with its guard band.
inline double guardFov(int tileSize) {
    return 2 * std::atan(guardScale(tileSize)) * 180 / 3.14159265358979;
}

struct Basis {
    std::array<float, 3> forward, up, right;
};
inline constexpr std::array<Basis, FaceCount> Faces{{
    {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
    {{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
    {{0, 1, 0}, {0, 0, -1}, {1, 0, 0}},
    {{0, -1, 0}, {0, 0, 1}, {1, 0, 0}},
    {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}},
    {{0, 0, -1}, {0, 1, 0}, {-1, 0, 0}},
}};

inline double dot(const std::array<float, 3>& a, double x, double y, double z) {
    return a[0] * x + a[1] * y + a[2] * z;
}

// Face of direction (x, y, z) and its ndc on that face.
inline int face(double x, double y, double z, double& ndcX, double& ndcY) {
    const double ax = std::abs(x), ay = std::abs(y), az = std::abs(z);
    int f;
    if (ax >= ay && ax >= az)
        f = x > 0 ? 0 : 1;
    else if (ay >= az)
        f = y > 0 ? 2 : 3;
    else
        f = z > 0 ? 4 : 5;
    const auto& b = Faces[f];
    const double depth = dot(b.forward, x, y, z);
    ndcX = dot(b.right, x, y, z) / depth;
    ndcY = dot(b.up, x, y, z) / depth;
    return f;
}

// View matrix of a face for a light at the origin (bgfx column-major).
inline std::array<float, 16> viewMatrix(int f) {
    const auto& b = Faces[f];
    return {b.right[0],
            b.up[0],
            b.forward[0],
            0,
            b.right[1],
            b.up[1],
            b.forward[1],
            0,
            b.right[2],
            b.up[2],
            b.forward[2],
            0,
            0,
            0,
            0,
            1};
}

// Top-left texel of a tile in the atlas.
inline void tileOrigin(int slot, int f, int tileSize, int& x, int& y) {
    const int tile = slot * FaceCount + f;
    x = (tile % Columns) * tileSize;
    y = (tile / Columns) * tileSize;
}

// Model matrix placing a mesh whose vertex (0,0,0) sits at `offset`
// relative to the light (bgfx column-major).
inline std::array<float, 16> translation(double x, double y, double z) {
    return {1, 0, 0, 0, 0,        1,        0,        0,
            0, 0, 1, 0, float(x), float(y), float(z), 1};
}

// Whether a box given relative to the light touches the light's sphere.
inline bool reaches(double minX, double minY, double minZ, double maxX,
                    double maxY, double maxZ, double radius) {
    const double gx = std::max({minX, 0.0, -maxX});
    const double gy = std::max({minY, 0.0, -maxY});
    const double gz = std::max({minZ, 0.0, -maxZ});
    return gx * gx + gy * gy + gz * gz <= radius * radius;
}

// Faces a light can reach: all six, or those within the cone's outer angle
// plus the face's half-diagonal (54.74 degrees).
inline uint8_t reachableFaces(const LightEmission& emission,
                              const std::array<float, 3>& direction) {
    if (!emission.cone) return 0x3f;
    constexpr double HalfDiagonal = 54.7357;
    const double limit = emission.cone->outerAngle + HalfDiagonal;
    if (limit >= 180) return 0x3f;
    const double minDot = std::cos(limit * 3.14159265358979 / 180);
    uint8_t mask = 0;
    for (int f = 0; f < FaceCount; ++f)
        if (dot(Faces[f].forward, direction[0], direction[1], direction[2]) >
            minDot)
            mask |= uint8_t(1u << f);
    return mask;
}
}  // namespace LightShadowMath
