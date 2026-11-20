#pragma once

#include <climits>
#include <memory>
#include <vector>

#include "chunk.hpp"
#include "chunk_coord.hpp"
#include "flora_generator.hpp"
#include "items/block.hpp"
#include "items/tree.hpp"
#include "terrain_sampler.hpp"
#include "tree_overrides.hpp"
#include "tree_rasterizer.hpp"
#include "tree_skeleton.hpp"

// Where trees grow (docs/handoff/trees.md, "Placement and generation").
// Every tree is placed and built by the job of the region holding its
// anchor, from that region's terrain neighbourhood, so each skeleton has a
// single source; chunks read the trees of the regions around them.

struct TreeSpeciesRule {
    uint8_t id = 0;
    uint32_t seed = 0;  // from the species name, stable across file order
    Tree species;
    // Soils, covers, climate, vegetation, slope, density and clustering.
    FloraSpeciesRule site;
    int cell = 1;  // candidate grid cell, tiles (one candidate per cell)
    uint8_t woodBlock = 0;
    uint8_t innerWoodBlock = 0;
    int woodShell() const {
        return innerWoodBlock ? species.wood.shell_thickness : 0;
    }
    bool ownsWood(uint8_t block) const {
        return block && (block == woodBlock || block == innerWoodBlock);
    }
    bool landmark = false;
};

// Resolved, immutable tree rules shared by jobs.
class TreeSpeciesTable {
   public:
    TreeSpeciesTable() = default;
    // Empty without tree data (headless fixtures).
    explicit TreeSpeciesTable(const IAssets& assets);

    const std::vector<TreeSpeciesRule>& species() const { return rules; }
    const TreeSpeciesRule* find(uint8_t id) const {
        return id && id <= byId.size() && byId[id - 1] >= 0
                   ? &rules[size_t(byId[id - 1])]
                   : nullptr;
    }
    bool empty() const { return rules.empty(); }
    // Soil properties for site rules (shared with flora).
    const FloraSpeciesTable& sites() const { return siteTable; }
    // Living wood block ids (natural_growth).
    const GrownBlockSet& grown() const { return grownBlocks; }
    bool rootSubstrate(uint8_t block) const {
        return rootSubstrates.test(block) && !grownBlocks.test(block);
    }
    // Ordinary (non-landmark) trees: the furthest a tree reaches from its
    // anchor, and the widest spacing between two trees.
    double maxExtent() const { return extent; }
    double maxSpacing() const { return spacing; }
    // Landmarks: the furthest one reaches from its anchor (0 without any).
    double maxLandmarkExtent() const { return landmarkExtent; }
    // Ordinary trees share a few shapes per species (TreeVariants, each in
    // four quarter turns), so their crown meshes are shared and drawn
    // instanced. Cached; safe from any thread.
    static constexpr int TreeVariants = 8;
    TreeSkeletonPtr variant(uint8_t species, int variant, int rotation) const;

   private:
    std::vector<TreeSpeciesRule> rules;
    std::vector<int> byId;
    FloraSpeciesTable siteTable;
    GrownBlockSet grownBlocks;
    std::bitset<256> rootSubstrates;
    double extent = 0, spacing = 0, landmarkExtent = 0;
    std::shared_ptr<TreeSkeletonCache> variants =
        std::make_shared<TreeSkeletonCache>(1024);
};

// One generated tree. Immutable; shared by every chunk it touches.
struct TreeInstance {
    uint64_t key = 0;  // stable identity (species and grid cell)
    uint8_t species = 0;
    uint32_t seed = 0;
    TreeAnchor anchor;     // canonical X
    int32_t birthDay = 0;  // game day it germinated (negative: before day 0)
    TreeSkeletonPtr skeleton;  // turned by `rotation`
    TreeSkeletonPtr uncut;     // exterior reference after the first cut
    TreeSkeletonPtr roots;     // independent of the crown and its cut indices
    // Shared shape (TreeSpeciesTable::variant) or -1 for a tree of its own
    // (landmarks; trees whose branches the ground prunes).
    int8_t variant = -1;
    uint8_t rotation = 0;  // quarter turns of the shared shape
    // Cuts applied to the skeleton (TreeRecord::cuts): chunks loaded later
    // clear the wood of cuts made after their stamp.
    std::vector<TreeCut> cuts;
    TreeTileBox wood;  // tiles that can hold its wood, full grown
    TreeTileBox rootWood;
    float rootReach = 0;
    TreeTileBox bounds;     // wood and foliage
    double discRadius = 0;  // trunk with root flare, on the ground
    float age(double day) const { return float(day - double(birthDay)); }
    // The age its wood and crown have: growth stops at death (the end of
    // its lifespan, or a root cut of a species that does not resprout).
    float growthAge(double day) const {
        float a = age(day);
        if (skeleton) {
            a = std::min(a, skeleton->lifespan);
            if (skeleton->dead && !cuts.empty())
                a = std::min(a, cuts.back().age);
        }
        return a;
    }
    // Chunk stamps use whole days. Include a cut already recorded during
    // this day, especially the settled wood that appears at its exact age.
    float woodAgeForDay(double day) const {
        if (!cuts.empty()) {
            const double cutDay = double(birthDay) + cuts.back().age;
            if (cutDay > day && std::floor(cutDay) == std::floor(day))
                day = cutDay;
        }
        return growthAge(day);
    }
};
using TreeInstancePtr = std::shared_ptr<const TreeInstance>;
using TreeSet = std::vector<TreeInstancePtr>;

// A massive tree's site, chosen once per world from region data.
struct TreeLandmark {
    uint8_t species = 0;
    int64_t x = 0, z = 0;  // canonical tile
    uint32_t hash = 0;
    double spacing = 0;
};
// Landmarks of the whole map: a pure function of the world and the seed.
inline uint32_t treeGenerationSeed(const World& world) {
    return world.genSettings.feature_seed ^ 0x7ee5u;
}
std::vector<TreeLandmark> placeTreeLandmarks(const TreeSpeciesTable& table,
                                             const World& world, uint32_t seed);

struct RegionTreesInput {
    const TreeSpeciesTable* table = nullptr;
    // Captured around `region` (TerrainNeighborhood::capture).
    const TerrainNeighborhood* terrain = nullptr;
    Vec2u region{};
    uint32_t seed = 0;
    // Landmarks within two regions of `region` (others cannot matter).
    const std::vector<TreeLandmark>* landmarks = nullptr;
    // Cut and planted trees (trees.dat); null: none.
    const TreeOverrideMap* overrides = nullptr;
    // Placement only (far billboards): every tree takes its shared shape
    // (no ground fit, no own skeleton, no wood bounds, no cuts); landmarks
    // only exclude trees around their trunks, and removed trees are left
    // out. Positions, species and ages equal the full
    // generation's.
    bool lightweight = false;
};

// A distant tree drawn as a billboard (TreeRenderer): where it stands and
// how big its full-grown shape is.
struct TreeBillboard {
    float x = 0, y = 0, z = 0;    // anchor point (canonical X)
    float height = 1, crown = 1;  // full-grown shape (metres)
    float maturity = 1, lifespan = 1, curve = 2;
    int32_t birthDay = 0;
    uint32_t hash = 0;  // thinning order and yaw
    uint8_t species = 0;
};
// The billboards of one region, sorted by hash (a prefix is an even
// thinning of the forest).
std::vector<TreeBillboard> regionBillboards(RegionTreesInput input);

// Fills a tree's wood box, bounds and trunk disc from its skeleton and
// anchor. Wood and bounds include the wood its cuts removed, so chunks
// loaded after a cut still find the tree and clear that wood.
void finishTreeInstance(TreeInstance& tree);
// A planted tree from its record (skeleton without terrain pruning, so the
// game and a later rebuild agree), with its cuts. Null for an unknown
// species.
TreeInstancePtr buildPlantedTree(const TreeSpeciesTable& table,
                                 const TreeRecord& record);
// The age-independent identity of a planted tree at `anchor`.
uint64_t plantedTreeKey(TreeAnchor anchor);
// Trees anchored in one region, with their skeletons, sorted by key.
TreeSet generateRegionTrees(const RegionTreesInput& input);

// The trees that matter to one chunk column, in the column's X frame.
struct TreeColumn {
    struct Entry {
        TreeInstancePtr tree;
        TreeAnchor anchor;  // X: the copy nearest the column
    };
    std::vector<Entry> trees;     // bounds (wood or foliage) meet the column
    std::vector<TreeDisc> discs;  // trunks near the column (plants avoid)
};
// Plant generation looks this far around a chunk for trunks.
inline constexpr int TreeDiscMargin = 32;
TreeColumn makeTreeColumn(unsigned chunkX, unsigned chunkZ,
                          const std::vector<const TreeSet*>& sets, int periodX);

// What a chunk whose wood dates from `since` (its TreeStamp day) needs to
// reach `today`: wood its trees grew since then (into air only), and the
// natural wood of cuts made since then that the tree no longer has. Player
// holes in older wood stay (growth only adds what the older tree lacked).
// Tile index is z * 256 + y * 16 + x.
struct TreeWoodChange {
    int index;
    uint8_t block;
    uint64_t rootOwner = 0;
    uint8_t previousBlock = 0;
    bool preserveEdit = false;
};
std::vector<TreeWoodChange> treeWoodChanges(const Chunk& chunk,
                                            const ChunkCoord& coord,
                                            const TreeColumn& column,
                                            const TreeSpeciesTable& table,
                                            double since, double today,
                                            bool legacyBark = false);
// The same change before the tiles are consulted: pure rasterization, safe
// on any thread (a massive tree costs milliseconds per chunk). `remove`
// lists tiles to clear if they still hold either wood ID (block or
// previousBlock). `add` fills air; `recolor` replaces existing bark with
// core. filterTreeWood checks the current tiles and preserves excavations
// on additions that request it.
struct TreeWoodDelta {
    std::vector<TreeWoodChange> remove, add, recolor;
};
TreeWoodDelta treeWoodDelta(const ChunkCoord& coord, const TreeColumn& column,
                            const TreeSpeciesTable& table, double since,
                            double today, bool legacyBark = false);
// One cut of one immutable tree snapshot, without unrelated growth or cuts.
// Settling also fills the new wood which appears at this cut's age.
TreeWoodDelta treeCutWoodDelta(const ChunkCoord& coord,
                               const TreeInstance& tree, TreeAnchor anchor,
                               uint8_t woodBlock, uint16_t cut, double today,
                               bool settles, int periodTiles,
                               uint8_t innerBlock = 0, int shell = 0);
// All of one tree's wood in a chunk at `age` (a removed tree), as removals.
// anchor: the column copy the chunk sees.
TreeWoodDelta treeWoodRemoval(const ChunkCoord& coord, const TreeInstance& tree,
                              TreeAnchor anchor, uint8_t woodBlock, float age,
                              int periodTiles, uint8_t innerBlock = 0);
std::vector<TreeWoodChange> filterTreeWood(const Chunk& chunk,
                                           const TreeWoodDelta& delta);
inline void applyTreeWoodChanges(Chunk& chunk,
                                 const std::vector<TreeWoodChange>& changes) {
    for (const auto& c : changes) {
        Tile& tile = (&chunk.tiles[0][0][0])[c.index];
        if (c.rootOwner && !c.previousBlock)
            chunk.recordRoot(uint16_t(c.index), c.rootOwner, tile);
        tile.block_id = c.block;
        if (c.block) tile.clearCover();
    }
}

// Roots replace only eligible natural terrain. A separate path keeps buried
// wood out of the air-only structure proxy and skylight projections.
std::vector<TreeWoodChange> treeRootChanges(
    const Chunk& chunk, const ChunkCoord& coord, const TreeColumn& column,
    const TreeSpeciesTable& table, const ChunkColumnInputs& terrain,
    double since, double today, bool legacyBark = false);

// Writes the column's wood at `day` into the air tiles of one chunk.
// Returns the number of tiles written.
int placeTreeWood(Chunk& chunk, const ChunkCoord& coord,
                  const TreeColumn& column, const TreeSpeciesTable& table,
                  double day);
