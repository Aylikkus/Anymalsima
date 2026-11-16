#pragma once

#include <cstdint>

enum class RockType {
    SEDIMENTARY,
    METAMORPHIC,
    IGNEOUS_INTRUSIVE,
    IGNEOUS_EXTRUSIVE
};

enum class BlockRenderCategory : uint8_t {
    TERRAIN_ROCK  = 0,
    TERRAIN_SOIL  = 1,
    STRUCTURE_WALL = 2,
    STRUCTURE_FLOOR = 3,
    VEGETATION   = 4,
    FURNITURE    = 5,
};