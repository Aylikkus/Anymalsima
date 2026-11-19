#pragma once

#include <cstdint>
#include <vector>

#include "tile_box.hpp"
#include "tree_skeleton.hpp"

// Turns a tree skeleton into wood tiles (docs/handoff/trees.md). A tile is
// wood when its centre lies within a segment part at least TreeVoxelRadius
// thick. The test only grows with age (fixed sample points along each
// segment, radii that only increase), so raster(a) is a subset of
// raster(b) for a < b and growth only ever adds wood.

// The support tile the tree stands on; the anchor point (skeleton origin)
// is the centre of its top face.
struct TreeAnchor {
    int x = 0, y = 0, z = 0;
    bool operator==(const TreeAnchor&) const = default;
};

// Tile boxes are shared with other structures (tile_box.hpp).
using TreeTileBox = TileBox;

// Tiles that may hold the full-grown tree's wood (live segments).
// includePruned: also the wood cuts removed (where it must be cleared).
TreeTileBox treeWoodBounds(const TreeSkeleton& skeleton, TreeAnchor anchor,
                           bool includePruned = false);
// Only the pieces removed by one cut (1-based), for local cleanup jobs.
TreeTileBox treeCutWoodBounds(const TreeSkeleton& skeleton, TreeAnchor anchor,
                              uint16_t cut);

// Which segments to rasterize: live ones, or the pruned pieces of one cut
// (1-based cut index), for removing the wood a cut took away.
struct TreeWoodSelection {
    uint16_t cut = 0;  // 0: live wood
};

// mask[box.index(x, y, z)] = 1 for wood tiles at `age` (mask is resized
// to the box and cleared first).
void markTreeWood(const TreeSkeleton& skeleton, TreeAnchor anchor,
                  const TreeTileBox& box, float age, std::vector<uint8_t>& mask,
                  TreeWoodSelection selection = {});

// emit(x, y, z) for each wood tile of the box at `age`.
template <typename Emit>
void forEachTreeWood(const TreeSkeleton& skeleton, TreeAnchor anchor,
                     const TreeTileBox& box, float age, Emit&& emit) {
    std::vector<uint8_t> mask;
    markTreeWood(skeleton, anchor, box, age, mask);
    for (int y = box.y0; y < box.y1; ++y)
        for (int z = box.z0; z < box.z1; ++z)
            for (int x = box.x0; x < box.x1; ++x)
                if (mask[box.index(x, y, z)]) emit(x, y, z);
}

// emit(x, y, z) for each tile that is wood at `toAge` but was not at
// `fromAge`: the wood a tree adds between two days.
template <typename Emit>
void forEachTreeGrowth(const TreeSkeleton& skeleton, TreeAnchor anchor,
                       const TreeTileBox& box, float fromAge, float toAge,
                       Emit&& emit) {
    std::vector<uint8_t> before, after;
    markTreeWood(skeleton, anchor, box, fromAge, before);
    markTreeWood(skeleton, anchor, box, toAge, after);
    for (int y = box.y0; y < box.y1; ++y)
        for (int z = box.z0; z < box.z1; ++z)
            for (int x = box.x0; x < box.x1; ++x) {
                const size_t i = box.index(x, y, z);
                if (after[i] && !before[i]) emit(x, y, z);
            }
}
