#include "tree_wood_lod.hpp"

#include <algorithm>

#include "tree_rasterizer.hpp"
#include "world_wrap.hpp"

namespace tree_wood_lod {

std::shared_ptr<const Slabs> rasterize(const TreeWoodSource& source, int level,
                                       int periodTiles) {
    auto out = std::make_shared<Slabs>();
    const auto& tree = *source.tree;
    if (!tree.skeleton || source.age <= 0 || tree.wood.empty()) return out;
    const int s = 1 << level, span = Chunk::SIZE_X * s;
    const auto floorDiv = [](int v, int d) {
        return v >= 0 ? v / d : -((-v + d - 1) / d);
    };
    const int sx0 = floorDiv(tree.wood.x0, span),
              sx1 = floorDiv(tree.wood.x1 - 1, span);
    const int sy0 = std::max(0, floorDiv(tree.wood.y0, span)),
              sy1 = std::min((WORLD_HEIGHT_Y + span - 1) / span - 1,
                             floorDiv(tree.wood.y1 - 1, span));
    const int sz0 = std::max(0, floorDiv(tree.wood.z0, span)),
              sz1 = floorDiv(tree.wood.z1 - 1, span);
    // A cell is wood when at least a quarter of its tiles are.
    const int needed = std::max(1, s * s * s / 4);
    const int chunksX = periodTiles / span;
    std::vector<uint8_t> mask;
    std::vector<uint16_t> counts;
    for (int sz = sz0; sz <= sz1; ++sz)
        for (int sy = sy0; sy <= sy1; ++sy)
            for (int sx = sx0; sx <= sx1; ++sx) {
                const TreeTileBox slab{sx * span,       sy * span,
                                       sz * span,       (sx + 1) * span,
                                       (sy + 1) * span, (sz + 1) * span};
                const auto part = tree.wood.intersect(slab);
                if (part.empty()) continue;
                markTreeWood(*tree.skeleton, tree.anchor, part, source.age,
                             mask);
                if (std::none_of(mask.begin(), mask.end(),
                                 [](uint8_t m) { return m != 0; }))
                    continue;
                counts.assign(
                    size_t(Chunk::SIZE_X) * Chunk::SIZE_Y * Chunk::SIZE_Z, 0);
                const auto cell = [&](int x, int y, int z) {
                    return size_t((((z - slab.z0) / s) * Chunk::SIZE_Y +
                                   (y - slab.y0) / s) *
                                      Chunk::SIZE_X +
                                  (x - slab.x0) / s);
                };
                for (int y = part.y0; y < part.y1; ++y)
                    for (int z = part.z0; z < part.z1; ++z)
                        for (int x = part.x0; x < part.x1; ++x)
                            if (mask[part.index(x, y, z)])
                                ++counts[cell(x, y, z)];
                Slab result;
                result.block = source.block;
                for (size_t i = 0; i < counts.size(); ++i)
                    if (counts[i] >= needed) result.bits.set(i);
                if (result.bits.none()) continue;
                int cx = sx;
                if (chunksX > 0)
                    cx = int(WorldWrap::mod(sx, chunksX));
                else if (sx < 0)
                    continue;
                (*out)[withLevel(
                    ChunkCoord{unsigned(cx), unsigned(sy), unsigned(sz)},
                    level)] = result;
            }
    // Slabs inside the bole of a massive trunk show nothing.
    const auto full = [&](ChunkCoord c, int dx, int dy, int dz) {
        const auto base = baseOf(c);
        int64_t x = int64_t(base.x) + dx;
        if (chunksX > 0) x = WorldWrap::mod(x, chunksX);
        const auto it = out->find(
            withLevel(ChunkCoord{unsigned(x), unsigned(int(base.y) + dy),
                                 unsigned(int(base.z) + dz)},
                      level));
        return it != out->end() && it->second.bits.all();
    };
    for (auto& [coord, slab] : *out)
        slab.enclosed = slab.bits.all() && full(coord, 1, 0, 0) &&
                        full(coord, -1, 0, 0) && full(coord, 0, 1, 0) &&
                        full(coord, 0, -1, 0) && full(coord, 0, 0, 1) &&
                        full(coord, 0, 0, -1);
    return out;
}

}  // namespace tree_wood_lod
