#include <cmath>
#include <map>

#include "test.hpp"
#include "world/structure_voxels.hpp"

// Distant structure voxels (docs/handoff/structures.md): per-slab level
// planning and multi-block slabs.
namespace {
using namespace structure_voxels;

// Every tile of `bounds` lies in exactly one planned slab.
void expectExactCover(const TileBox& bounds, const std::vector<Planned>& plan) {
    size_t covered = 0;
    for (const auto& p : plan) covered += bounds.intersect(boxOf(p)).volume();
    EQUAL(covered, bounds.volume(), "planned slabs cover the box once");
    for (size_t i = 0; i < plan.size(); ++i)
        for (size_t j = i + 1; j < plan.size(); ++j)
            CHECK(boxOf(plan[i]).intersect(boxOf(plan[j])).empty());
}

std::unordered_set<ChunkCoord> keys(const std::vector<Planned>& plan) {
    std::unordered_set<ChunkCoord> out;
    for (const auto& p : plan) out.insert(p.key);
    return out;
}
}  // namespace

TEST(structure_voxels_plan_levels) {
    // A structure 2 km long on a world that does not wrap; the eye at its
    // west end.
    const TileBox bounds{0, 64, 0, 2048, 192, 64};
    const WorldWrap none{};
    const auto plan = structure_voxels::plan(bounds, 10, 100, 30, none, {});
    expectExactCover(bounds, plan);
    std::map<int, int> perLevel;
    for (const auto& p : plan) {
        ++perLevel[levelOf(p.key)];
        // Each slab is at the level its distance allows (no hysteresis on
        // a first plan): a slab of level L > 0 lies beyond Limits[L - 1],
        // and its parent's distance is below Limits[L] (else it would be
        // coarser).
        const int level = levelOf(p.key);
        if (level > 0) CHECK(p.distance >= Limits[level - 1] - 1);
    }
    CHECK(perLevel[0] > 0);
    CHECK(perLevel[1] > 0);
    CHECK(perLevel[2] > 0);
    // Nearest first, and full tiles at the eye.
    for (size_t i = 1; i < plan.size(); ++i)
        CHECK(plan[i - 1].distance <= plan[i].distance);
    EQUAL(levelOf(plan.front().key), 0, "full tiles at the eye");
    // Far slabs are coarse: a level 0 slab lies in a level 1 cell nearer
    // than the first limit.
    for (const auto& p : plan)
        if (levelOf(p.key) == 0) CHECK(p.distance < Limits[0] + spanOf(1));
}

TEST(structure_voxels_plan_hysteresis) {
    const TileBox bounds{0, 0, 0, 1024, 64, 64};
    const WorldWrap none{};
    // A level-1 cell starting at x = 576 (32-tile cells): its distance from
    // an eye at x = 64 - d is 512 + d tiles.
    const auto at = [&](double eyeX,
                        const std::unordered_set<ChunkCoord>& previous) {
        return structure_voxels::plan(bounds, eyeX, 32, 32, none, previous);
    };
    const auto levelAt = [](const std::vector<Planned>& plan, int tileX) {
        for (const auto& p : plan) {
            const auto box = boxOf(p);
            if (tileX >= box.x0 && tileX < box.x1 && box.y0 == 0 && box.z0 == 0)
                return levelOf(p.key);
        }
        return -1;
    };
    // Coarse while clearly beyond the limit.
    const auto far = at(64 - 100, {});
    EQUAL(levelAt(far, 576), 1, "coarse beyond the limit");
    // Approaching to just inside the limit: still coarse (margin).
    const auto closer = at(64 + 20, keys(far));
    EQUAL(levelAt(closer, 576), 1, "kept coarse within the margin");
    // Well inside: full tiles.
    const auto near = at(64 + 100, keys(closer));
    EQUAL(levelAt(near, 576), 0, "full tiles inside the band");
    // Backing off to just beyond the limit: stays fine (margin).
    const auto back = at(64 - 20, keys(near));
    EQUAL(levelAt(back, 576), 0, "kept fine within the margin");
    expectExactCover(bounds, back);
}

TEST(structure_voxels_plan_wraps) {
    // A structure across the X seam of a 2048-tile world: keys canonical,
    // distances measured to the copy nearest the eye.
    const WorldWrap wrap{2048};
    const TileBox bounds{-100, 0, 0, 100, 32, 32};
    const auto plan = structure_voxels::plan(bounds, 2040, 16, 16, wrap, {});
    expectExactCover(bounds, plan);
    for (const auto& p : plan) {
        const auto base = baseOf(p.key);
        CHECK(base.x < unsigned(2048 / spanOf(levelOf(p.key))));
        EQUAL(levelOf(p.key), 0, "the eye is beside it across the seam");
    }
}

TEST(structure_voxels_slab_merge) {
    // Two structures overlapping in one slab: the first keeps its cells,
    // the second fills the rest with its own block.
    Slab wood;
    wood.block = 9;
    wood.bits.set(cellIndex(1, 1, 1));
    wood.bits.set(cellIndex(2, 1, 1));
    Slab stone;
    stone.block = 4;
    stone.bits.set(cellIndex(2, 1, 1));
    stone.bits.set(cellIndex(3, 1, 1));
    wood.merge(stone);
    EQUAL(int(wood.bits.count()), 3, "union of cells");
    EQUAL(int(wood.blockAt(cellIndex(1, 1, 1))), 9, "own cell");
    EQUAL(int(wood.blockAt(cellIndex(2, 1, 1))), 9, "first keeps overlap");
    EQUAL(int(wood.blockAt(cellIndex(3, 1, 1))), 4, "other block");
    // Same single block: no per-cell ids needed.
    Slab a, b;
    a.block = b.block = 9;
    b.bits.set(5);
    a.merge(b);
    CHECK(a.blocks.empty());
    CHECK(a.bits.test(5));
}
