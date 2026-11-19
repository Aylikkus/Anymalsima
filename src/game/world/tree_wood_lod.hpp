#pragma once

#include <functional>
#include <memory>

#include "structure_voxels.hpp"
#include "tree_generator.hpp"

// Voxel wood of massive trees at distance (docs/handoff/trees.md, "Distant
// trees"): a tree's living wood as a structure_voxels::Source, so
// StructureLod draws it like any structure. Level 0 is the real rasterizer
// (the voxels match the chunks exactly); coarser levels count the wood
// tiles of each 2^level cell. Pure; runs on the pool.
namespace tree_wood_lod {

// A massive tree to show: its wood at `age` (quantized by the caller, so
// it is rebuilt only when growth changes it noticeably), as `block` tiles.
struct Source {
    TreeInstancePtr tree;
    float age = 0;
    uint8_t block = 0;
    bool operator==(const Source&) const = default;
};

// The tree's wood as structure voxels: key = TreeInstance::key, revision
// from the age, the skeleton (cuts rebuild it) and the block.
structure_voxels::SourcePtr voxels(const Source& source);

// Combine ordinary tree and landmark wood with the other kinds that have
// voxels in the column. A factory can reuse a tree source across columns.
using VoxelFactory = std::function<structure_voxels::SourcePtr(const Source&)>;
structure_voxels::ColumnPtr columnVoxels(
    unsigned cx, unsigned cz, const TreeColumn& trees,
    const TreeSpeciesTable& table, double day, const WorldWrap& wrap,
    const std::vector<structure_voxels::Kind>& kinds = {},
    const VoxelFactory& make = voxels);

// Every slab of the tree's wood at one level (tests). periodTiles: world
// width when X wraps (keys canonical), else 0.
std::shared_ptr<const structure_voxels::Slabs> rasterize(const Source& source,
                                                         int level,
                                                         int periodTiles);

using structure_voxels::baseOf;
using structure_voxels::levelOf;
using structure_voxels::MaxLevel;
using structure_voxels::Slab;
using structure_voxels::Slabs;
using structure_voxels::withLevel;

}  // namespace tree_wood_lod

using TreeWoodSource = tree_wood_lod::Source;
