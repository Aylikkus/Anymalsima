#pragma once

#include <algorithm>
#include <cstddef>

// World tiles [x0, x1) x [y0, y1) x [z0, z1). X may lie outside [0, W) on a
// wrapping world (boxes around an anchor near the seam stay contiguous).
struct TileBox {
    int x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
    int sizeX() const { return x1 - x0; }
    int sizeY() const { return y1 - y0; }
    int sizeZ() const { return z1 - z0; }
    bool empty() const { return x1 <= x0 || y1 <= y0 || z1 <= z0; }
    size_t volume() const {
        return empty() ? 0
                       : size_t(sizeX()) * size_t(sizeY()) * size_t(sizeZ());
    }
    // Index in y-major order (the tree rasterizer's masks).
    size_t index(int x, int y, int z) const {
        return (size_t(y - y0) * size_t(sizeZ()) + size_t(z - z0)) *
                   size_t(sizeX()) +
               size_t(x - x0);
    }
    TileBox intersect(const TileBox& o) const {
        return {std::max(x0, o.x0), std::max(y0, o.y0), std::max(z0, o.z0),
                std::min(x1, o.x1), std::min(y1, o.y1), std::min(z1, o.z1)};
    }
    bool operator==(const TileBox&) const = default;
};
