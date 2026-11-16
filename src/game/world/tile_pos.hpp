#pragma once

#include <cstddef>
#include <functional>

// World tile coordinates: x, y = height, z.
struct TilePos {
    int x = 0, y = 0, z = 0;
    bool operator==(const TilePos&) const = default;
};

template <>
struct std::hash<TilePos> {
    size_t operator()(const TilePos& p) const noexcept {
        size_t h = std::hash<int>{}(p.x);
        h ^=
            std::hash<int>{}(p.y) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^=
            std::hash<int>{}(p.z) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }
};
