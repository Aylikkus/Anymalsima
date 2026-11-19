#include <cmath>
#include <map>

#include "test.hpp"
#include "world/sky_occlusion_map.hpp"
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

// Footprints and sky occlusion (docs/handoff/structures.md, step 2).
namespace {
// A structure made of boxes of one block (a castle in miniature): cells of
// coarse levels are solid when at least a quarter of their tiles are.
class BoxSource : public Source {
   public:
    BoxSource(std::vector<TileBox> parts, uint64_t rev = 1,
              std::optional<TileBox> change = std::nullopt)
        : parts(std::move(parts)), change(change) {
        key = 7;
        revision = rev;
        bounds = this->parts.front();
        for (const auto& p : this->parts)
            bounds = {std::min(bounds.x0, p.x0), std::min(bounds.y0, p.y0),
                      std::min(bounds.z0, p.z0), std::max(bounds.x1, p.x1),
                      std::max(bounds.y1, p.y1), std::max(bounds.z1, p.z1)};
    }
    std::optional<Slab> rasterize(int level, int sx, int sy,
                                  int sz) const override {
        const int c = 1 << level, s = spanOf(level);
        Slab slab;
        slab.block = 3;
        for (int z = 0; z < Side; ++z)
            for (int y = 0; y < Side; ++y)
                for (int x = 0; x < Side; ++x) {
                    const TileBox cell{
                        sx * s + x * c,       sy * s + y * c,
                        sz * s + z * c,       sx * s + (x + 1) * c,
                        sy * s + (y + 1) * c, sz * s + (z + 1) * c};
                    size_t solid = 0;
                    for (const auto& p : parts)
                        solid += p.intersect(cell).volume();
                    if (solid * 4 >= cell.volume())
                        slab.bits.set(cellIndex(x, y, z));
                }
        if (slab.bits.none()) return std::nullopt;
        return slab;
    }
    std::optional<TileBox> changedSince(const Source&) const override {
        return change;
    }

   private:
    std::vector<TileBox> parts;
    std::optional<TileBox> change;
};
}  // namespace

TEST(structure_footprint_tops) {
    // A roof one tile thick on a pillar: the roof casts, at its top cell.
    const BoxSource castle({{0, 100, 0, 64, 101, 64}, {0, 0, 0, 4, 100, 4}});
    const auto fp = footprint(castle, nullptr);
    EQUAL(fp->tops.size(), size_t(16 * 16), "one column per 4x4 cell");
    for (const auto& [key, top] : fp->tops) EQUAL(top, 104, "roof cell top");
}

TEST(structure_footprint_incremental) {
    // A new tower on one corner of a wide roof: only its slab columns are
    // rasterized again, and the result equals a full rebuild.
    const std::vector<TileBox> roof{{0, 100, 0, 256, 104, 256}};
    auto grown = roof;
    grown.push_back({200, 104, 200, 208, 300, 208});
    const BoxSource before(roof, 1);
    const BoxSource after(grown, 2, TileBox{200, 104, 200, 208, 300, 208});
    const auto old = footprint(before, nullptr);
    const auto full = footprint(after, nullptr);
    const auto partial =
        footprint(after, nullptr, old.get(), after.changedSince(before));
    CHECK(*partial == *full);
    EQUAL(full->tops.at(Footprint::packColumn(50, 50)), 300, "tower top");
    EQUAL(full->tops.at(Footprint::packColumn(10, 10)), 104, "roof top");
}

TEST(sky_occlusion_matches_sunlight) {
    // Under a 256x256 roof the voxel sunlight is 15 - d, d the 4-neighbour
    // distance to the nearest open column; the map gives the same within a
    // light level at texel centres, independent of the roof's height.
    const BoxSource roof({{0, 400, 0, 256, 404, 256}});
    SkyOcclusionMap map;
    map.build({footprint(roof, nullptr)}, 128, 128, 0);
    EQUAL(map.occluded(), size_t(64 * 64), "roof texels");
    for (int k = 0; k < 8; ++k)
        for (int z : {130, 2 + 4 * k}) {
            const int x = 2 + 4 * k;  // texel centre, tile column x
            const int d = std::min({x + 1, 256 - x, z + 1, 256 - z});
            const float expected = std::max(0, 15 - d) / 15.f;
            const float got = map.sky(x + 0.5, 64, z + 0.5);
            CHECK(std::abs(got - expected) <= 1.f / 15 + 1e-4f);
        }
    CHECK(map.sky(128, 64, 128) < 1.f / 15);  // dark well under the roof
    EQUAL(map.sky(128, 420, 128), 1.f, "full sky above it");
    EQUAL(map.sky(-10, 64, 128), 1.f, "full sky beside it");
    EQUAL(map.sky(128, 64, 300), 1.f, "full sky beside it");
}

TEST(sky_occlusion_by_height) {
    // Light reaches a point from columns whose occluders all lie below it:
    // a low roof (top 100) beside a high one (top 400). Between the two
    // heights, the edge of the high roof is lit from the low side; below
    // both, the whole covered area is one roof.
    const BoxSource roofs(
        {{0, 96, 0, 128, 100, 256}, {128, 396, 0, 256, 400, 256}});
    SkyOcclusionMap map;
    map.build({footprint(roofs, nullptr)}, 128, 128, 0);
    const int x = 130, z = 128;  // first texel under the high roof
    const float lit = map.sky(x + 0.5, 200, z + 0.5);
    CHECK(std::abs(lit - (15 - 3) / 15.f) <= 1.f / 15 + 1e-4f);
    CHECK(map.sky(x + 0.5, 50, z + 0.5) < 1.f / 15);
    // Deep under the high roof at that height: dark.
    CHECK(map.sky(200, 200, z) < 1.f / 15);
    // A tower on a wide plinth (a trunk on its flared base): beside the
    // tower above the plinth there is full sky.
    const BoxSource tower({{0, 0, 0, 64, 8, 64}, {24, 0, 24, 40, 300, 40}});
    SkyOcclusionMap towerMap;
    towerMap.build({footprint(tower, nullptr)}, 32, 32, 0);
    EQUAL(towerMap.sky(20, 150, 32), 1.f, "beside the tower");
    // Inside it, two texels from the open side.
    CHECK(std::abs(towerMap.sky(32, 150, 32) -
                   SkyOcclusionMap::skyAtDistance(2)) < 1e-4f);
}

TEST(sky_occlusion_wraps) {
    // An overhang across the X seam of a 2048-tile world shades both sides
    // of the seam as seen from the eye.
    const BoxSource bridge({{-64, 200, 0, 64, 204, 128}});
    SkyOcclusionMap map;
    map.build({footprint(bridge, nullptr)}, 2040, 64, 2048);
    CHECK(map.sky(2040, 50, 64) < 0.5f);
    CHECK(map.sky(2050, 50, 64) < 0.5f);  // x = 2 past the seam
    EQUAL(map.sky(1900, 50, 64), 1.f, "open beyond it");
}
