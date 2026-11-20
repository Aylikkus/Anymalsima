#pragma once

#include <cmath>

#include "consts.hpp"
#include "tile.hpp"

// Integer boundary above the last occupied tile. Generated water keeps the
// legacy sea fill for this generation version, including inland low ground.
struct WaterSurface {
    static constexpr int SeaBoundary = SEA_LEVEL_Y + 1;
    int surfaceY = -1;

    static constexpr WaterSurface sea(int groundY) {
        return {groundY < SEA_LEVEL_Y ? SeaBoundary : -1};
    }
    constexpr bool wet() const { return surfaceY > 0; }
    constexpr LiquidType kind() const {
        return wet() ? LiquidType::Water : LiquidType::None;
    }
    constexpr int topTile() const { return wet() ? surfaceY - 1 : -1; }
    constexpr bool fills(int y, int groundY) const {
        return y > groundY && y < surfaceY;
    }
    static constexpr float cellHeight(const Tile& tile) {
        // Older chunks can encode water with a zero level. Preserve their
        // full-cell behavior until finite-volume cells have provenance.
        return tile.liquidLevel ? tile.liquidLevel / 255.f : 1.f;
    }
    bool submerged(float y, int groundY, const Tile* cell = nullptr) const {
        if (cell)
            return !cell->isSolid() && cell->liquid == LiquidType::Water &&
                   y - std::floor(y) < cellHeight(*cell);
        return y >= groundY + 1 && y < surfaceY;
    }
};
