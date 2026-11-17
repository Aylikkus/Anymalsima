#pragma once

#include <bitset>
#include <memory>
#include <unordered_map>

#include "chunk.hpp"
#include "chunk_coord.hpp"
#include "tree_generator.hpp"

// Voxel wood of massive trees at distance (docs/handoff/trees.md, "Distant
// trees"): their living wood is rasterized per 16^3 slab, the real
// rasterizer at full detail (level 0, so the voxels match the chunks) or in
// coarser cells of 2^level tiles for far trees. StructureLod meshes the
// slabs like built structures. Pure; runs on the pool.
namespace tree_wood_lod {

// A massive tree to show: its wood at `age` (quantized by the caller, so
// it is rebuilt only when growth changes it noticeably), as `block` tiles.
struct Source {
    TreeInstancePtr tree;
    float age = 0;
    uint8_t block = 0;
    bool operator==(const Source&) const = default;
};

// Wood cells of one slab: bit (z * 16 + y) * 16 + x.
struct Slab {
    std::bitset<Chunk::SIZE_X * Chunk::SIZE_Y * Chunk::SIZE_Z> bits;
    uint8_t block = 0;
    // Full, with full slabs on all six sides: no visible face, so no
    // proxy (kept so the neighbours' border faces stay hidden).
    bool enclosed = false;
};
using Slabs = std::unordered_map<ChunkCoord, Slab>;

// Slab keys: level 0 slabs are ordinary chunk coordinates; coarser ones
// count slabs of 16 cells and carry the level in the high bits of y.
inline constexpr unsigned LevelShift = 12;
inline constexpr int MaxLevel = 2;
inline int levelOf(const ChunkCoord& c) { return int(c.y >> LevelShift); }
inline ChunkCoord baseOf(ChunkCoord c) {
    c.y &= (1u << LevelShift) - 1;
    return c;
}
inline ChunkCoord withLevel(ChunkCoord c, int level) {
    c.y |= unsigned(level) << LevelShift;
    return c;
}

// The tree's wood at `level`: a cell is wood when at least a quarter of its
// tiles are. periodTiles: world width when X wraps (keys canonical), else 0.
std::shared_ptr<const Slabs> rasterize(const Source& source, int level,
                                       int periodTiles);

}  // namespace tree_wood_lod

using TreeWoodSource = tree_wood_lod::Source;
