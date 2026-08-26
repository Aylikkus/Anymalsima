#pragma once
#include <cmath>
#include <optional>

#include "chunk_manager.hpp"

// ---------------------------------------------------------------------------
// Result of a tile raycast hit.
// ---------------------------------------------------------------------------
struct TileRaycastHit {
    // World-tile coordinates of the hit tile.
    int tx, ty, tz;  // tz = height axis (matches tileAt's second param)

    // A copy of the tile data at the hit position.
    Tile tile;
};

// ---------------------------------------------------------------------------
// Cast a ray from (origin) in direction (dir) through the chunk world,
// stepping tile-by-tile (DDA) up to maxDistance tiles.
//
// Coordinate convention matches ChunkManager::tileAt(tx, ty, tz):
//   tx = world X tile,  ty = world height tile,  tz = world Y tile
//
// Returns the first non-air tile hit, or std::nullopt if nothing was found.
// ---------------------------------------------------------------------------
inline std::optional<TileRaycastHit> castTileRay(
    ChunkManager& chunks, float originX, float originY,
    float originZ,                       // world-space position
    float dirX, float dirY, float dirZ,  // unit direction vector
    float maxDistance = 64.0f) {
    // Current tile position (integer).
    int tx = (int)std::floor(originX);
    int ty = (int)std::floor(originY);  // height
    int tz = (int)std::floor(originZ);

    // Direction signs.
    int stepX = dirX >= 0 ? 1 : -1;
    int stepY = dirY >= 0 ? 1 : -1;
    int stepZ = dirZ >= 0 ? 1 : -1;

    // tMax: distance along the ray to the first tile boundary in each axis.
    // tDelta: distance to cross one full tile in each axis.
    auto calcTMax = [](float origin, float dir, int step) -> float {
        if (std::abs(dir) < 1e-9f) return 1e30f;
        float boundary = (float)(step > 0 ? (int)std::floor(origin) + 1
                                          : (int)std::floor(origin));
        return (boundary - origin) / dir;
    };
    auto calcTDelta = [](float dir) -> float {
        return std::abs(dir) < 1e-9f ? 1e30f : std::abs(1.0f / dir);
    };

    float tMaxX = calcTMax(originX, dirX, stepX);
    float tMaxY = calcTMax(originY, dirY, stepY);
    float tMaxZ = calcTMax(originZ, dirZ, stepZ);
    float tDeltaX = calcTDelta(dirX);
    float tDeltaY = calcTDelta(dirY);
    float tDeltaZ = calcTDelta(dirZ);

    float tCurrent = 0.0f;

    while (tCurrent < maxDistance) {
        // Check current tile.
        const Tile* tile = chunks.tileAt(tx, ty, tz);
        if (tile && !tile->isAir()) {
            return TileRaycastHit{tx, ty, tz, *tile};
        }

        // Advance to the nearest tile boundary.
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            tCurrent = tMaxX;
            tMaxX += tDeltaX;
            tx += stepX;
        } else if (tMaxY < tMaxZ) {
            tCurrent = tMaxY;
            tMaxY += tDeltaY;
            ty += stepY;
        } else {
            tCurrent = tMaxZ;
            tMaxZ += tDeltaZ;
            tz += stepZ;
        }
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Build a direction vector from camera yaw and pitch.
// Matches the movement math already in gameplay_state.cpp:
//   flatFwdX = sin(yaw),  flatFwdZ = cos(yaw),  vertical = pitch
// ---------------------------------------------------------------------------
inline void cameraDirection(float yaw, float pitch, float& outX, float& outY,
                            float& outZ) {
    float cosPitch = std::cos(pitch);
    outX = std::sin(yaw) * cosPitch;
    outY = std::sin(pitch);
    outZ = std::cos(yaw) * cosPitch;
}