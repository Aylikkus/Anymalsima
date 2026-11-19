#include "tree_wood_lod.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

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
        if (std::none_of(mask.begin(), mask.end(),
                         [](uint8_t m) { return m != 0; }))
            return {};
        // A cell is wood when at least a quarter of its tiles are.
        const int needed = std::max(1, s * s * s / 4);
        std::vector<uint16_t> counts(structure_voxels::Cells, 0);
        for (int y = part.y0; y < part.y1; ++y)
            for (int z = part.z0; z < part.z1; ++z)
                for (int x = part.x0; x < part.x1; ++x)
                    if (mask[part.index(x, y, z)])
                        ++counts[structure_voxels::cellIndex(
                            (x - slab.x0) / s, (y - slab.y0) / s,
                            (z - slab.z0) / s)];
        structure_voxels::Slab result;
        result.block = wood.block;
        for (size_t i = 0; i < counts.size(); ++i)
            if (counts[i] >= needed) result.bits.set(i);
        if (result.bits.none()) return {};
        return result;
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
                   x.flags == y.flags && x.cut == y.cut && x.appear == y.appear;
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

   private:
    tree_wood_lod::Source wood;
};

}  // namespace

structure_voxels::SourcePtr voxels(const Source& source) {
    if (!source.tree) return nullptr;
    return std::make_shared<TreeVoxels>(source);
}

std::shared_ptr<const Slabs> rasterize(const Source& source, int level,
                                       int periodTiles) {
    const auto v = voxels(source);
    return std::make_shared<const Slabs>(
        v ? structure_voxels::rasterizeAll(*v, level, WorldWrap{periodTiles})
          : Slabs{});
}

}  // namespace tree_wood_lod
