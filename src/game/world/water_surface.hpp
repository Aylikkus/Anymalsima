#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "consts.hpp"
#include "core/geometry.hpp"
#include "tile.hpp"

// Integer boundary above the last occupied tile. Body IDs identify generated
// water and are absent for legacy water and local edit summaries.
struct WaterSurface {
    static constexpr int SeaBoundary = SEA_LEVEL_Y + 1;
    int surfaceY = -1;
    uint32_t body = 0;
    uint32_t reach = 0;
    Vec2 flow{0, 0};
    bool edited = false;

    // A vertical face is owned by the higher column. The lower water or
    // neighboring solid terrain covers the portion beneath this boundary.
    constexpr int dropBottom(WaterSurface neighbor, int neighborGround) const {
        if (!wet() ||
            !(edited || (neighbor.wet() && (reach || neighbor.reach))))
            return surfaceY;
        return std::max(neighbor.surfaceY, neighborGround + 1);
    }

    constexpr int depth(int groundY) const {
        return wet() ? surfaceY - groundY - 1 : 0;
    }

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
