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
    static constexpr uint8_t RestingFill = 223;
    uint8_t topFill = body && !reach ? RestingFill : 255;
    uint8_t riverDepth = 0;  // centerline depth for reach-wide drawdown

    // Water vertices carry normalized depth in their color alpha.
    static constexpr uint32_t depthTint(float depth) {
        return 0x00ffffffu |
               (uint32_t(std::clamp(depth / 6.f, 0.f, 1.f) * 255) << 24);
    }
    constexpr float height() const {
        return wet() ? topTile() + topFill / 255.f : float(surfaceY);
    }
    // Instantaneous waterline at a point in the reach. Keep the displacement
    // in sync with river_wave.sh; storage records the mean stage.
    float surfaceRise(float x, float z, float seconds) const {
        if (!reach) return 0;
        constexpr float phase = 0.3926990817f;
        x = std::fmod(x, 16.f) * phase;
        z = std::fmod(z, 16.f) * phase;
        return .045f * std::sin(x + 2 * z - 2.1f * seconds) +
               .022f * std::sin(2 * x - z + 1.4f * seconds);
    }
    uint8_t surfaceFill(float x, float z, float seconds) const {
        return uint8_t(std::clamp(
            std::lround(topFill + 255 * surfaceRise(x, z, seconds)), 1l, 255l));
    }
    constexpr uint8_t fillAt(int y, int groundY) const {
        return fills(y, groundY) ? (y == topTile() ? topFill : 255) : 0;
    }

    // A vertical face is owned by the higher column. The lower water or
    // neighboring solid terrain covers the portion beneath this boundary.
    // A natural shore returns surfaceY (no face); two standing bodies at
    // different levels, a drained neighbor or a river step need one.
    constexpr int dropBottom(WaterSurface neighbor, int neighborGround) const {
        if (!wet()) return surfaceY;
        return std::min(surfaceY,
                        std::max(neighbor.surfaceY, neighborGround + 1));
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
        return y >= groundY + 1 && y < height();
    }
};

// A mesh covering one continuous lake surface can follow its waterline
// while adjacent chunks and LOD patches upload at different times.
struct LakeMeshSurface {
    uint32_t body = 0;
    Vec2i tile{};
    float height = 0;
    bool mixed = false;

    void include(WaterSurface water, int x, int z) {
        if (mixed) return;
        if (water.body <= 1 || water.body >= 0x80000000u || water.reach ||
            water.edited ||
            (body && (water.body != body || water.height() != height))) {
            mixed = true;
        } else if (!body) {
            body = water.body;
            tile = {x, z};
            height = water.height();
        }
    }
    float offset(WaterSurface current) const {
        return !mixed && body && current.body == body && !current.reach
                   ? current.height() - height
                   : 0.f;
    }
};
