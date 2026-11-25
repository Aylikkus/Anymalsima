#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

#include "chunk_manager.hpp"

struct TileRaycastHit {
    int tx, ty, tz;  // canonical tile coordinates, Y up
    Tile tile;
    TileShape shape;
    float distance;
    Vec3 position;  // world-space position in the ray's copy of the world
    Vec3 normal;    // geometric outward normal, zero for an inside hit
    TilePos placementOffset;
    tile_geometry::PieceId pieceId;
    SolidCellRevision revision;
};

// The direction is unit length. Liquid-only cells retain whole-cell picking.
// Grid ties visit Z, Y, then X, as in the original traversal. Within a shaped
// cell the geometry's canonical plane order breaks ties.
inline std::optional<TileRaycastHit> castTileRay(ChunkManager& chunks,
                                                 float originX, float originY,
                                                 float originZ, float dirX,
                                                 float dirY, float dirZ,
                                                 float maxDistance = 64.0f,
                                                 bool hitLiquids = true) {
    const std::array<double, 3> origin{originX, originY, originZ};
    const std::array<double, 3> dir{dirX, dirY, dirZ};
    const Vec3 direction{dirX, dirY, dirZ};
    std::array<int, 3> tile{};
    std::array<int, 3> step{};
    std::array<double, 3> next{}, stride{};
    for (size_t axis = 0; axis < tile.size(); ++axis) {
        tile[axis] = static_cast<int>(std::floor(origin[axis]));
        step[axis] = dir[axis] >= 0 ? 1 : -1;
        const int boundary = tile[axis] + (step[axis] > 0);
        const double infinity = std::numeric_limits<double>::infinity();
        next[axis] =
            dir[axis] == 0 ? infinity : (boundary - origin[axis]) / dir[axis];
        stride[axis] = dir[axis] == 0 ? infinity : std::abs(1 / dir[axis]);
    }

    double entry = 0;
    std::array<float, 3> enteredFace{};
    bool first = true;
    while (entry < maxDistance) {
        size_t axis = 2;
        if (next[1] < next[axis]) axis = 1;
        if (next[0] < next[axis]) axis = 0;
        const auto selected = chunks.solidCellAt({tile[0], tile[1], tile[2]});
        const auto* cell = selected ? &selected->cell.base : nullptr;
        if (cell &&
            (cell->tile.isSolid() || (hitLiquids && !cell->tile.isAir()))) {
            const Vec3 gridNormal{enteredFace[0], enteredFace[1],
                                  enteredFace[2]};
            std::optional<tile_geometry::RayHit> hit{
                tile_geometry::RayHit{0, gridNormal}};
            auto hitTile = cell->tile;
            auto hitShape = cell->shape;
            tile_geometry::PieceId pieceId =
                selected->cell.simple ? selected->cell.simple->geometry.id : 0;
            if (selected->cell.compound || cell->shape != TileShape{}) {
                auto local = [&](size_t i) {
                    return static_cast<float>(std::clamp(
                        origin[i] - tile[i] + dir[i] * entry, 0.0, 1.0));
                };
                const Vec3 start{local(0), local(1), local(2)};
                const float limit =
                    float(std::min(next[axis], double(maxDistance)) - entry);
                if (selected->cell.compound) {
                    const auto piece = selected->cell.geometry().intersectRay(
                        start, direction, 0, limit);
                    if (piece) {
                        hit = piece->hit;
                        if (!first && hit->normal == Vec3{})
                            hit->normal = gridNormal;
                        pieceId = piece->id;
                        hitTile.block_id = piece->blockId;
                        for (const auto& part : selected->cell.pieces())
                            if (part.geometry.id == pieceId)
                                hitShape = part.geometry.shape;
                    } else {
                        hit.reset();
                    }
                } else if (!first ||
                           !tile_geometry::contains(cell->shape, start)) {
                    hit = tile_geometry::intersectRay(cell->shape, start,
                                                      direction, 0, limit);
                    if (hit && hit->normal == Vec3{}) hit->normal = gridNormal;
                }
            }
            if (hit && entry + hit->t < maxDistance) {
                const double t = entry + hit->t;
                const auto n = hit->normal;
                const TilePos offset = n.y != 0
                                           ? TilePos{0, n.y > 0 ? 1 : -1, 0}
                                           : TilePos{int(n.x), 0, int(n.z)};
                return TileRaycastHit{int(chunks.wrap().wrapTile(tile[0])),
                                      tile[1],
                                      tile[2],
                                      hitTile,
                                      hitShape,
                                      static_cast<float>(t),
                                      {float(origin[0] + dir[0] * t),
                                       float(origin[1] + dir[1] * t),
                                       float(origin[2] + dir[2] * t)},
                                      n,
                                      offset,
                                      pieceId,
                                      selected->revision};
            }
        }

        first = false;
        entry = next[axis];
        next[axis] += stride[axis];
        tile[axis] += step[axis];
        enteredFace = {};
        enteredFace[axis] = static_cast<float>(-step[axis]);
    }
    return std::nullopt;
}
