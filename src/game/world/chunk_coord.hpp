#pragma once
#include <functional>
#include <cstdint>
#include <cstddef>

struct ChunkCoord {
    unsigned int x, y, z;

    bool operator==(const ChunkCoord& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

template<>
struct std::hash<ChunkCoord> {
    size_t operator()(const ChunkCoord& c) const {
        size_t hx = (size_t)c.x & 0xFFFFF;
        size_t hy = (size_t)c.y & 0xFFFFF;
        size_t hz = (size_t)c.z & 0xFF;
        return hx | (hy << 20) | (hz << 40);
    }
};