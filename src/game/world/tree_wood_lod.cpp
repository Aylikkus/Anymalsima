#include "tree_wood_lod.hpp"

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <mutex>

#include "tree_rasterizer.hpp"

namespace tree_wood_lod {

namespace {

class TreeVoxels final : public structure_voxels::Source {
   public:
    explicit TreeVoxels(tree_wood_lod::Source wood) : wood(std::move(wood)) {
        const auto& tree = *this->wood.tree;
        key = tree.key;
        bounds = tree.wood;
        const uint64_t skeleton =
            uint64_t(reinterpret_cast<uintptr_t>(tree.skeleton.get()));
        revision = (uint64_t(std::bit_cast<uint32_t>(this->wood.age)) << 8 |
                    this->wood.block) ^
                   (skeleton * 0x9e3779b97f4a7c15ULL);
    }

    std::optional<structure_voxels::Slab> rasterize(int level, int sx, int sy,
                                                    int sz) const override {
        const auto& tree = *wood.tree;
        if (!tree.skeleton || wood.age <= 0 || tree.wood.empty()) return {};
        const int s = 1 << level, span = structure_voxels::spanOf(level);
        const TileBox slab{sx * span,       sy * span,       sz * span,
                           (sx + 1) * span, (sy + 1) * span, (sz + 1) * span};
        const auto part = tree.wood.intersect(slab);
        if (part.empty()) return {};
        std::vector<uint8_t> mask;
        markTreeWood(*tree.skeleton, tree.anchor, part, wood.age, mask);
        structure_voxels::Slab result;
        result.block = wood.block;
        // Level zero is also the chunk stamping path. Count samples only
        // when several tiles contribute to a coarse cell.
        std::vector<uint16_t> counts(level ? structure_voxels::Cells : 0);
        for (int y = part.y0; y < part.y1; ++y)
            for (int z = part.z0; z < part.z1; ++z)
                for (int x = part.x0; x < part.x1; ++x) {
                    if (!mask[part.index(x, y, z)]) continue;
                    const size_t i = structure_voxels::cellIndex(
                        (x - slab.x0) / s, (y - slab.y0) / s,
                        (z - slab.z0) / s);
                    if (level)
                        ++counts[i];
                    else
                        result.bits.set(i);
                }
        const int needed = std::max(1, s * s * s / 4);
        for (size_t i = 0; i < counts.size(); ++i)
            if (counts[i] >= needed) result.bits.set(i);
        if (result.bits.none()) return {};
        return result;
    }

    // Projects the skeleton (treeWoodTops) instead of rasterizing slabs.
    void skyTops(const TileBox& columns,
                 std::vector<int>& tops) const override {
        const int width = columns.x1 - columns.x0;
        tops.assign(columns.empty()
                        ? 0
                        : size_t(width) * size_t(columns.z1 - columns.z0),
                    INT_MIN);
        const auto& tree = *wood.tree;
        if (!tree.skeleton || wood.age <= 0) return;
        const auto part = tree.wood.intersect(columns);
        if (part.empty()) return;
        std::vector<int> local;
        treeWoodTops(*tree.skeleton, tree.anchor, part, wood.age, local);
        const int w = part.x1 - part.x0;
        for (int z = part.z0; z < part.z1; ++z)
            for (int x = part.x0; x < part.x1; ++x)
                tops[size_t(z - columns.z0) * width + size_t(x - columns.x0)] =
                    local[size_t(z - part.z0) * w + size_t(x - part.x0)];
    }

    // A cut changes only some segments (pruned ones, the cut one, new
    // sprouts): the voxels differ only around them.
    std::optional<TileBox> changedSince(
        const structure_voxels::Source& olderSource) const override {
        const auto* older = dynamic_cast<const TreeVoxels*>(&olderSource);
        if (!older || older->wood.block != wood.block) return std::nullopt;
        const auto& a = *older->wood.tree;
        const auto& b = *wood.tree;
        if (!a.skeleton || !b.skeleton || !(a.anchor == b.anchor))
            return std::nullopt;
        const auto& ka = *a.skeleton;
        const auto& kb = *b.skeleton;
        const auto sameFlare = [](const TreeFlare& x, const TreeFlare& y) {
            return x.amount == y.amount && x.height == y.height &&
                   x.lobes == y.lobes && x.phase == y.phase;
        };
        if (ka.maturity != kb.maturity || ka.lifespan != kb.lifespan ||
            ka.curve != kb.curve || ka.dead != kb.dead ||
            ka.trunkRadius != kb.trunkRadius || !sameFlare(ka.flare, kb.flare))
            return std::nullopt;
        const auto same = [](const TreeSegment& x, const TreeSegment& y) {
            return x.a == y.a && x.b == y.b && x.r0 == y.r0 && x.r1 == y.r1 &&
                   x.birthA == y.birthA && x.birthB == y.birthB &&
                   x.matureAge == y.matureAge && x.parent == y.parent &&
                   x.attachT == y.attachT && x.order == y.order &&
                   x.flags == y.flags && x.cut == y.cut &&
                   x.appear == y.appear && x.cutNormal == y.cutNormal;
        };
        // Tiles a segment may fill (the anchor point is the centre of the
        // anchor tile's top face), with a tile of slack for flare and
        // rounding.
        TileBox box;
        bool any = false;
        const auto add = [&](const TreeSkeleton& k, const TreeSegment& s) {
            const float age = &k == &ka ? older->wood.age : wood.age;
            // Pruned and unborn segments have no current voxels. In
            // particular, a new sprout's future bounds must not expand a
            // small cut into a rebuild of the whole crown.
            if (!s.live() || k.visible(s, age) <= 0) return;
            const float flare = s.order == 0 ? 1 + k.flare.amount : 1;
            if (std::max(s.r0, s.r1) * flare < TreeVoxelRadius) return;
            const float r =
                std::max(s.r0, s.r1) * (1 + std::max(0.f, k.flare.amount)) + 2;
            const TileBox part{
                int(std::floor(a.anchor.x + 0.5f + std::min(s.a.x, s.b.x) - r)),
                int(std::floor(a.anchor.y + 1 + std::min(s.a.y, s.b.y) - r)),
                int(std::floor(a.anchor.z + 0.5f + std::min(s.a.z, s.b.z) - r)),
                int(std::ceil(a.anchor.x + 0.5f + std::max(s.a.x, s.b.x) + r)),
                int(std::ceil(a.anchor.y + 1 + std::max(s.a.y, s.b.y) + r)),
                int(std::ceil(a.anchor.z + 0.5f + std::max(s.a.z, s.b.z) + r))};
            box = any ? TileBox{std::min(box.x0, part.x0),
                                std::min(box.y0, part.y0),
                                std::min(box.z0, part.z0),
                                std::max(box.x1, part.x1),
                                std::max(box.y1, part.y1),
                                std::max(box.z1, part.z1)}
                      : part;
            any = true;
        };
        const size_t n = std::max(ka.segments.size(), kb.segments.size());
        for (size_t i = 0; i < n; ++i) {
            const bool inA = i < ka.segments.size(),
                       inB = i < kb.segments.size();
            if (inA && inB && same(ka.segments[i], kb.segments[i])) {
                const auto& s = ka.segments[i];
                // Cutting resets the crown's quantized age to its new
                // growth phase. Mature wood is identical at either age;
                // only still-growing segments need invalidation for age.
                const float earliest = std::min(older->wood.age, wood.age);
                if (older->wood.age == wood.age || !s.live() ||
                    (earliest >= s.matureAge && earliest >= s.appear))
                    continue;
            }
            if (inA) add(ka, ka.segments[i]);
            if (inB) add(kb, kb.segments[i]);
        }
        if (!any) return TileBox{};
        // Voxels exist only inside either revision's bounds.
        const TileBox both{std::min(older->bounds.x0, bounds.x0),
                           std::min(older->bounds.y0, bounds.y0),
                           std::min(older->bounds.z0, bounds.z0),
                           std::max(older->bounds.x1, bounds.x1),
                           std::max(older->bounds.y1, bounds.y1),
                           std::max(older->bounds.z1, bounds.z1)};
        return box.intersect(both);
    }

    // Level-0 slabs within reach of a live, born wood segment: spheres
    // along each segment (at most 8 tiles apart, grown by half the step)
    // marked against slab boxes. Cut planes and the radius at this age are
    // ignored, so it only over-approximates the rasterizer.
    structure_voxels::OccupancyPtr occupancy() const override {
        std::call_once(occupancyOnce, [&] {
            auto out = std::make_shared<structure_voxels::Occupancy>();
            const auto& tree = *wood.tree;
            if (!tree.skeleton || wood.age <= 0 || tree.wood.empty()) {
                occupied = std::move(out);
                return;
            }
            const auto& k = *tree.skeleton;
            const auto& a = tree.anchor;
            constexpr int Side = structure_voxels::Side;
            const auto floorDiv = [](int v) {
                return v >= 0 ? v / Side : -((-v + Side - 1) / Side);
            };
            for (const auto& s : k.segments) {
                if (!s.live() || k.visible(s, wood.age) < 0) continue;
                const float flare = s.order == 0 ? 1 + k.flare.amount : 1;
                const float rMax = std::max(s.r0, s.r1) * flare;
                if (rMax < TreeVoxelRadius) continue;
                const TreeVec axis = s.b - s.a;
                const float length = axis.length();
                const int steps = std::max(1, int(std::ceil(length / 8)));
                const float step = length / float(steps);
                // Tile reach of the rasterizer plus the sample spacing.
                const float reach = rMax + 1.5f + step * 0.5f;
                for (int i = 0; i <= steps; ++i) {
                    const TreeVec p = lerp(s.a, s.b, float(i) / float(steps));
                    // Tile centre (x, y, z) is at (x - a.x, y - a.y - 0.5,
                    // z - a.z) in skeleton space.
                    const float px = p.x + float(a.x),
                                py = p.y + float(a.y) + .5f,
                                pz = p.z + float(a.z);
                    const int x0 = floorDiv(int(std::floor(px - reach))),
                              x1 = floorDiv(int(std::ceil(px + reach))),
                              y0 = floorDiv(int(std::floor(py - reach))),
                              y1 = floorDiv(int(std::ceil(py + reach))),
                              z0 = floorDiv(int(std::floor(pz - reach))),
                              z1 = floorDiv(int(std::ceil(pz + reach)));
                    for (int sz = z0; sz <= z1; ++sz)
                        for (int sy = std::max(0, y0); sy <= y1; ++sy)
                            for (int sx = x0; sx <= x1; ++sx) {
                                const auto gap = [](float v, int lo) {
                                    return std::max({0.f, float(lo) - v,
                                                     v - float(lo + Side - 1)});
                                };
                                const float dx = gap(px, sx * Side),
                                            dy = gap(py, sy * Side),
                                            dz = gap(pz, sz * Side);
                                if (dx * dx + dy * dy + dz * dz <=
                                    reach * reach)
                                    out->add(sx, sy, sz);
                            }
                }
            }
            occupied = std::move(out);
        });
        return occupied;
    }

   private:
    tree_wood_lod::Source wood;
    mutable std::once_flag occupancyOnce;
    mutable structure_voxels::OccupancyPtr occupied;
};

}  // namespace

structure_voxels::SourcePtr voxels(const Source& source) {
    if (!source.tree) return nullptr;
    return std::make_shared<TreeVoxels>(source);
}

structure_voxels::ColumnPtr columnVoxels(
    unsigned cx, unsigned cz, const TreeColumn& trees,
    const TreeSpeciesTable& table, double day, const WorldWrap& wrap,
    const std::vector<structure_voxels::Kind>& kinds,
    const VoxelFactory& make) {
    auto column = std::make_shared<structure_voxels::Column>(cx, cz);
    for (const auto& kind : kinds) kind.addTo(*column, wrap);
    for (const auto& entry : trees.trees) {
        const auto& tree = *entry.tree;
        const auto* rule = table.find(tree.species);
        if (!rule || !rule->woodBlock || tree.wood.empty()) continue;
        const float age = tree.growthAge(day);
        if (age <= 0) continue;
        const Source input{entry.tree, age, rule->woodBlock};
        column->add(make(input), entry.anchor.x - tree.anchor.x);
    }
    column->orderByKey();
    return column;
}

std::shared_ptr<const Slabs> rasterize(const Source& source, int level,
                                       int periodTiles) {
    const auto v = voxels(source);
    return std::make_shared<const Slabs>(
        v ? structure_voxels::rasterizeAll(*v, level, WorldWrap{periodTiles})
          : Slabs{});
}

}  // namespace tree_wood_lod
