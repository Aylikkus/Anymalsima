#pragma once
#include <cstdint>

#include "items/block.hpp"

enum class LiquidType : uint8_t {
    None = 0,
    Water = 1,
    Lava = 2,
};

struct TileLightning {
    uint8_t sun_exposure : 4 = 0;
    uint8_t artificial_light : 4 = 0;
};

struct Tile {
    uint8_t block_id = 0;  // 0 = air
    LiquidType liquid = LiquidType::None;
    uint8_t liquidLevel = 0;  // 0-255
    TileLightning lightLevel;

    bool isAir() const { return block_id == 0 && liquid == LiquidType::None; }
    bool isVisible() const { return block_id != 0; }
    bool isSolid() const { return block_id != 0; }
    bool isLiquid() const { return liquid != LiquidType::None; }
};
