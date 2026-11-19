#pragma once

#include <deque>
#include <future>
#include <string>
#include <unordered_map>

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
    // Wood a cut or a fallen tree takes away (or a settling trunk moves)
    // goes chunk by chunk, nearest to the cut first: each chunk's change is
    // rasterized on the pool (MaxClearJobs at once) and applied on the main
    // thread within ClearBudgetMs per update. The broken block shows at
    // once, the branch follows.
    static constexpr double ClearBudgetMs = 2;
    static constexpr size_t MaxClearJobs = 8;

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

    // Loaded chunks still waiting for wood to be cleared.
    size_t pendingClears() const { return clears.size() + clearJobs.size(); }
    // Clears all of them now (tests, tools).
    void finishClears();

    // trees.dat must be written (a cut or a planted tree).
    bool dirty() const { return changed; }
    void clearDirty() { changed = false; }
    // Debug counters.
    size_t cutCount() const { return cuts; }
    size_t removedCount() const { return deaths; }
    // Main-thread time of the last removal of living wood, by stage (ms):
    // finding the segment, the cross-section test, and for a cut the
    // skeleton rebuild, the commit (overrides, tree sets, columns) and
    // queuing the chunks whose wood goes (cleared over the next updates).
    struct BreakTimes {
        double owner = 0, severed = 0, rebuild = 0, commit = 0, clear = 0;
        double total() const {
            return owner + severed + rebuild + commit + clear;
        }
    };
    const BreakTimes& lastBreak() const { return times; }

   private:
    struct Owner {
        TreeColumn::Entry entry;
        int32_t segment = -1;
        float t = 0;
        TreeVec normal{};  // TreeCut::normal: the sawn face found severed
    };
    void blockChanged(const BlockChange& change);
    void woodRemoved(const BlockChange& change);
    // Trees whose base the change may have taken die.
    void checkBases(TilePos pos);
    bool baseLost(const TreeColumn::Entry& entry);
    void kill(const TreeColumn::Entry& entry);
    // The segments whose wood holds the tile, best match first (one inside
    // a segment's length before an end cap, then the nearest surface). A
    // flared trunk and its buttresses share tiles, so a removal may sever
    // any of them.
    std::vector<Owner> owners(TilePos pos, uint8_t block) const;
    // thin: parts up to ThinCut are cut by any removal (the best match
    // only; other candidates need their whole cross-section broken).
    // Branch cuts update owner.t to the retained face of the cleared row.
    bool severed(Owner& owner, TilePos removed, bool thin) const;
    // Slits that are not tile-grid slices (slanted, turned, stepped): the
    // limb is severed when its wood beyond the removal no longer reaches
    // its wood before it (face-connected, within a window along the limb's
    // axis; unloaded tiles count as wood). Then owner gets the plane fitted
    // to the slit, offset so every tile of the far piece lies beyond it.
    bool severedByConnectivity(Owner& owner, TilePos removed) const;
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
    BreakTimes times;
    // A loaded chunk losing wood from one cut, or all of a removed tree's.
    struct Clear {
        ChunkCoord coord;
        double cutDay = 0;
        TreeInstancePtr removed;  // set: remove this tree's wood
        TreeAnchor anchor;        // its column copy
        // The exact cut snapshot, independent of column rebuilds and later
        // cuts. Its last cut is the only one this job processes.
        TreeInstancePtr tree;
        uint64_t settling = 0;  // tree key of a settling trunk cut, or 0
    };
    struct ClearJob {
        Clear clear;
        std::future<TreeWoodDelta> delta;
    };
    std::deque<Clear> clears;
    std::vector<ClearJob> clearJobs;
    // Trees whose trunk is settling (clears still pending): their trunk is
    // not cut again meanwhile (its sawn ring is still air).
    std::unordered_map<uint64_t, size_t> settling;
    void finishClear(const Clear& clear);
    void startClear(const Clear& clear);
    // Queues the loaded chunks of `box` (column copy), nearest to `from`.
    void queueClears(const TreeTileBox& box, TilePos from, Clear clear);
    void clearSome(double budgetMs);

    std::deque<ChunkCoord> sweep;
    uint64_t sweepRevision = ~0ULL;
    size_t updates = 0;
};
