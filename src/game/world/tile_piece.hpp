#pragma once

#include <vector>

#include "tile_shape.hpp"

namespace tile_geometry {

using PieceId = uint32_t;

// BelowProfile uses the original flat bottom and profile top. AboveProfile
// uses that same profile surface as a bottom and the ceiling as a flat top.
enum class PieceFill { BelowProfile, AboveProfile };
enum class LayerThickness : uint8_t {
    Cm5 = 5,
    Cm10 = 10,
    Cm25 = 25,
    Cm50 = 50,
    Cm75 = 75,
    Cm100 = 100,
};

struct TilePiece {
    PieceId id;       // unique within the cell
    uint8_t blockId;  // selects the block definition and its materials
    TileShape shape;
    // Facing rotates the profile around its original cell centre. Offset
    // translates that rotated solid. The result is clipped to the target cell.
    Vec3 offset{};
    PieceFill fill = PieceFill::BelowProfile;
    // Horizontal clip in the profile's coordinates, before translation.
    float ceiling = 1;

    static TilePiece layer(PieceId id, uint8_t blockId,
                           LayerThickness thickness, Vec3 offset = {});
};

struct PieceFace {
    PieceId id;
    uint8_t blockId;
    Plane plane;
    std::vector<Vec3> vertices;  // convex, CCW from outside
};

struct PieceRayHit {
    PieceId id;
    uint8_t blockId;
    RayHit hit;
};

struct VerticalInterval {
    float low;
    float high;
};

enum class PiecePlacement { Added, Invalid, Empty, DuplicateId, Overlap };

// Geometry for one cell. Queries use metres, in [0, 1] on each axis.
class CellGeometry {
   public:
    // Face, edge and point contact are allowed. A rejected candidate leaves
    // the cell unchanged.
    PiecePlacement tryAdd(TilePiece piece);
    bool contains(Vec3 point) const;
    std::vector<PieceFace> exteriorFaces() const;
    // Convex polygons covering the unoccupied area of a cell face. Vertices
    // lie in cell coordinates and wind CCW when viewed from outside.
    std::vector<std::vector<Vec3>> boundaryOpenings(Face face) const;
    // Positive-length gaps in ascending Y order, with contact endpoints.
    // X/Z outside the cell return no intervals.
    std::vector<VerticalInterval> freeIntervals(Vec2 xz) const;
    // Highest exposed upward surface at or below maxHeight, over a footprint
    // of positive area. Vec2 represents X/Z.
    std::optional<float> supportHeight(Vec2 low, Vec2 high,
                                       float maxHeight = 1) const;
    // The first exterior entry, or a zero normal when starting in the solid.
    // Ties between pieces go to the lower ID.
    std::optional<PieceRayHit> intersectRay(
        Vec3 origin, Vec3 direction, float tMin = 0,
        float tMax = std::numeric_limits<float>::infinity()) const;

   private:
    struct Solid {
        PieceId id;
        uint8_t blockId;
        std::vector<std::vector<Plane>> parts;
    };
    std::vector<Solid> solids_;
};

}  // namespace tile_geometry
