#pragma once

#include "tile.hpp"

struct Chunk {
    static constexpr int SIZE_X = 16;
    static constexpr int SIZE_Y = 16;
    static constexpr int SIZE_Z = 16;
    Tile tiles[SIZE_Z][SIZE_Y][SIZE_X];  // [z][y][x]
};
