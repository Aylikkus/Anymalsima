#pragma once

#include <vector>

#include "chunk.hpp"

namespace voxel_light {

inline constexpr uint8_t Opaque = 15;
inline constexpr uint8_t Unknown = 255;
inline constexpr int Steps[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                    {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};

struct PieceCoverage {
    struct FacePolygon {
        uint8_t opacity;
        std::vector<Vec3> vertices;
    };
    std::vector<tile_geometry::TilePiece> pieces;  // blockId holds opacity
    std::array<std::vector<FacePolygon>, 6> faces;
    uint8_t vertical = 0;
    uint8_t maximum = 0;
};

std::shared_ptr<const PieceCoverage> coverPieces(
    std::span<const tile_geometry::TilePiece> pieces);
using OpacityTable = std::array<uint8_t, 256>;
std::shared_ptr<const PieceCoverage> coverPieces(const SolidCellSnapshot& cell,
                                                 const OpacityTable& opacity);

struct Cell {
    uint8_t opacity = 0;
    TileShape shape{};

    std::shared_ptr<const PieceCoverage> compound;

    Cell() = default;
    Cell(uint8_t opacity, TileShape shape = {})
        : opacity(opacity), shape(shape) {}
    explicit Cell(std::shared_ptr<const PieceCoverage> compound)
        : opacity(compound ? compound->maximum : 0),
          compound(std::move(compound)) {}
    bool solidOpaque() const {
        return opacity >= Opaque && !shape.code() && !compound;
    }
    bool operator==(const Cell& other) const {
        return opacity == other.opacity && shape == other.shape &&
               (compound == other.compound ||
                (compound && other.compound &&
                 compound->pieces == other.compound->pieces));
    }
};

// Snapshot shapes only in chunks that contain them, indexed like Chunk::cell.
class Shapes {
   public:
    void capture(const Chunk& chunk, const OpacityTable& opacity) {
        compounds.clear();
        chunk.forEachPieceCell([&](uint16_t i, const SolidCellSnapshot& cell) {
            if (cell.compound) compounds.emplace(i, coverPieces(cell, opacity));
        });
        cells.clear();
        if (!chunk.hasShapes()) return;
        cells.resize(Chunk::SIZE_X * Chunk::SIZE_Y * Chunk::SIZE_Z);
        chunk.forEachShape(
            [&](uint16_t i, TileShape shape) { cells[i] = shape; });
    }
    TileShape at(int i) const { return cells.empty() ? TileShape{} : cells[i]; }
    Cell cell(int i, uint8_t opacity) const {
        if (!compounds.empty())
            if (const auto it = compounds.find(uint16_t(i));
                it != compounds.end())
                return Cell(it->second);
        return {opacity, at(i)};
    }
    bool empty() const { return cells.empty() && compounds.empty(); }

   private:
    std::vector<TileShape> cells;
    std::map<uint16_t, std::shared_ptr<const PieceCoverage>> compounds;
};

uint8_t shapedVerticalLoss(const Cell& cell);
uint8_t rayOpacity(const Cell& cell, Vec3 origin, Vec3 direction, float enter,
                   float leave);

// Integrate the stronger material opacity where the two boundary polygons
// overlap. An emissive block can pass an empty source to ignore its own body.
uint8_t faceLoss(const Cell& from, const Cell& to, Face face);

// Preserve the existing departure penalty and terminal surface lighting for
// ordinary cubes. Crossings involving profiles use their shared boundary.
inline uint8_t spreadLoss(const Cell& from, const Cell& to, Face face) {
    if (to.opacity == Unknown) return Opaque;
    if (!from.compound && !to.compound && !from.shape.code() &&
        !to.shape.code())
        return from.opacity;
    return faceLoss(from, to, face);
}

// A slab covers the whole overhead footprint. A thin wall covers its strip.
inline uint8_t verticalLoss(const Cell& cell) {
    if (cell.compound) return cell.compound->vertical;
    return cell.shape.code() ? shapedVerticalLoss(cell) : cell.opacity;
}

}  // namespace voxel_light
