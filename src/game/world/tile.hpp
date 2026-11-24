#pragma once
#include <cstdint>

#include "items/block.hpp"

enum class LiquidType : uint8_t {
    None = 0,
    Water = 1,
    Lava = 2,
};

enum class WaterOrigin : uint8_t { Independent, Reservoir, Local };

constexpr int waterOpacity(uint8_t fill) {
    // Legacy water used a zero level for a full cell.
    return fill ? (8 * fill + 254) / 255 : 8;
}

struct TileLightning {
    uint8_t sun_exposure : 4 = 0;
    uint8_t artificial_light : 4 = 0;
};

struct Tile {
    // Low nibble of coverState: cover density 0-15. The high nibble is
    // reserved (trampled, withered, snow-covered).
    static constexpr uint8_t CoverDensityMask = 0x0F;

    uint8_t block_id = 0;  // 0 = air
    LiquidType liquid : 6 = LiquidType::None;
    WaterOrigin waterOrigin : 2 = WaterOrigin::Independent;
    uint8_t liquidLevel = 0;  // 0-255
    TileLightning lightLevel;
    // Ground cover (GroundCover id, 0 = none) painted on this solid tile's
    // faces. It never changes solidity, opacity, sunlight or edit detection;
    // see docs/handoff/flora.md.
    uint8_t cover = 0;
    uint8_t coverState = 0;

    uint8_t coverDensity() const { return coverState & CoverDensityMask; }
    void setCover(uint8_t id, uint8_t density) {
        cover = id;
        coverState = id ? uint8_t((coverState & ~CoverDensityMask) |
                                  (density & CoverDensityMask))
                        : 0;
    }
    void clearCover() { cover = coverState = 0; }

    bool isAir() const { return block_id == 0 && liquid == LiquidType::None; }
    bool isVisible() const { return block_id != 0; }
    bool isSolid() const { return block_id != 0; }
    bool isLiquid() const { return liquid != LiquidType::None; }
};
static_assert(sizeof(Tile) == 6, "Tile layout is saved and cached per chunk");
