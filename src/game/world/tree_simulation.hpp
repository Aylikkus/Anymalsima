#pragma once

#include <deque>
#include <string>

#include "chunk_manager.hpp"
#include "world_editor.hpp"

// Trees during play (docs/handoff/trees.md, steps 5 and 6). Main thread.
//
// Growth: loaded chunks whose trees grew since their TreeStamp day get the
// new wood (into air only) through the editor, a few chunks per update;
// chunks loading from saves catch up in their generation job instead.
//
// Cutting: removing a tree's living wood through the editor cuts the
// branch or trunk once no wood is left in its cross-section at that height
// (thin parts, radius <= 2, at once). The cut is recorded in the tree
// overrides (trees.dat), the tree is rebuilt with it, and the wood above
// the cut disappears (no physics yet); it regrows from the cut later, or,
// cut at the root, from the stump when the species resprouts (otherwise
// the tree is dead and the stump stays).
//
// Death: a tree that loses its base dies and leaves the world at once. The
// base is the ground under its trunk (the anchor layer within the flared
// trunk radius) and, for trees with wood blocks, the first row of trunk
// wood. When every tile of either is gone, the tree's remaining wood is
// cleared, its crown disappears, and it is recorded as removed (planted
// trees are forgotten) so generation never brings it back.
//
// Planting (debug tool): a sapling on a solid tile, with the free height
// above it as its headroom, so trees grow indoors under ceilings.
class TreeSimulation {
   public:
    static constexpr int ChunksPerUpdate = 4;

    TreeSimulation(ChunkManager& chunks, WorldEditor& editor);
    ~TreeSimulation();
    TreeSimulation(const TreeSimulation&) = delete;
    TreeSimulation& operator=(const TreeSimulation&) = delete;

    // today: game day (WorldClock::days).
    void update(double today);
    // Plants a sapling of `species` on the solid tile `ground`. Returns
    // false when the tile is not solid ground with air above, or a tree
    // already stands there.
    bool plant(TilePos ground, uint8_t species, double today);
    // A line about the tree a tile belongs to (debug panel), or empty.
    std::string describe(TilePos tile, double today) const;

    // trees.dat must be written (a cut or a planted tree).
    bool dirty() const { return changed; }
    void clearDirty() { changed = false; }
    // Debug counters.
    size_t cutCount() const { return cuts; }
    size_t removedCount() const { return deaths; }

   private:
    struct Owner {
        TreeColumn::Entry entry;
        int32_t segment = -1;
        float t = 0;
    };
    void blockChanged(const BlockChange& change);
    void woodRemoved(const BlockChange& change);
    // Trees whose base the change may have taken die.
    void checkBases(TilePos pos);
    bool baseLost(const TreeColumn::Entry& entry);
    void kill(const TreeColumn::Entry& entry);
    std::optional<Owner> owner(TilePos pos, uint8_t block) const;
    bool severed(const Owner& owner, TilePos removed) const;
    void cut(const Owner& owner);
    // Applies tile changes through the editor without reacting to them.
    void applyChanges(const ChunkCoord& coord,
                      const std::vector<TreeWoodChange>& changes);
    void commitTree(TreeRecord record, const TreeInstancePtr& tree,
                    const TreeTileBox& oldBounds);

    ChunkManager& chunks;
    WorldEditor& editor;
    Signal<const BlockChange&>::Connection connection;
    bool applying = false;
    bool changed = false;
    double today = 0;
    size_t cuts = 0;
    size_t deaths = 0;
    std::deque<ChunkCoord> sweep;
    uint64_t sweepRevision = ~0ULL;
    size_t updates = 0;
};
