#pragma once

#include <vector>

#include "chunk.hpp"

namespace voxel_light {

inline constexpr uint8_t Opaque = 15;
inline constexpr uint8_t Unknown = 255;
inline constexpr int Steps[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                    {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};

struct Cell {
    uint8_t opacity = 0;
    TileShape shape{};

    constexpr Cell() = default;
    constexpr Cell(uint8_t opacity, TileShape shape = {})
        : opacity(opacity), shape(shape) {}
    constexpr bool solidOpaque() const {
        return opacity >= Opaque && !shape.code();
    }
    bool operator==(const Cell&) const = default;
};

// Snapshot shapes only in chunks that contain them, indexed like Chunk::cell.
class Shapes {
   public:
    void capture(const Chunk& chunk) {
        cells.clear();
        if (!chunk.hasShapes()) return;
        cells.resize(Chunk::SIZE_X * Chunk::SIZE_Y * Chunk::SIZE_Z);
        chunk.forEachShape(
            [&](uint16_t i, TileShape shape) { cells[i] = shape; });
    }
    TileShape at(int i) const { return cells.empty() ? TileShape{} : cells[i]; }
    bool empty() const { return cells.empty(); }

   private:
    std::vector<TileShape> cells;
};

uint8_t shapedVerticalLoss(Cell cell);

// Integrate the stronger material opacity where the two boundary polygons
// overlap. An emissive block can pass an empty source to ignore its own body.
uint8_t faceLoss(Cell from, Cell to, Face face);

// Preserve the existing departure penalty and terminal surface lighting for
// ordinary cubes. Crossings involving profiles use their shared boundary.
inline uint8_t spreadLoss(Cell from, Cell to, Face face) {
    if (to.opacity == Unknown) return Opaque;
    if (!from.shape.code() && !to.shape.code()) return from.opacity;
    return faceLoss(from, to, face);
}

// A slab covers the whole overhead footprint. A thin wall covers its strip.
inline uint8_t verticalLoss(Cell cell) {
    return cell.shape.code() ? shapedVerticalLoss(cell) : cell.opacity;
}

}  // namespace voxel_light
