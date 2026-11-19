#include "structure_voxels.hpp"

#include <algorithm>
#include <cmath>

namespace structure_voxels {

void Slab::merge(const Slab& other) {
    enclosed |= other.enclosed;
    if (blocks.empty() && other.blocks.empty() && block == other.block) {
        bits |= other.bits;
        return;
    }
    if (blocks.empty()) blocks.assign(Cells, block);
    const auto added = other.bits & ~bits;
    if (added.none()) return;
    for (size_t i = 0; i < Cells; ++i)
        if (added.test(i)) blocks[i] = other.blockAt(i);
    bits |= added;
}

void Occupancy::add(int sx, int sy, int sz) {
    for (int level = 0; level <= MaxLevel; ++level) {
        // Floor division by 2^level of possibly negative coordinates.
        const auto down = [&](int v) { return v >> level; };
        if (!slabs[size_t(level)]
                 .insert(packSlab(down(sx), down(sy), down(sz)))
                 .second &&
            level > 0)
            break;  // its parents are listed already
    }
}

std::optional<ChunkCoord> keyOf(int level, int sx, int sy, int sz,
                                const WorldWrap& wrap) {
    if (sy < 0 || sz < 0 || sy * spanOf(level) >= WORLD_HEIGHT_Y)
        return std::nullopt;
    int64_t x = sx;
    if (wrap.enabled()) {
        const int chunksX = wrap.tiles / spanOf(level);
        if (chunksX <= 0) return std::nullopt;
        x = WorldWrap::mod(x, chunksX);
    } else if (x < 0) {
        return std::nullopt;
    }
    return withLevel(ChunkCoord{unsigned(x), unsigned(sy), unsigned(sz)},
                     level);
}

namespace {

int floorDiv(int v, int d) { return v >= 0 ? v / d : -((-v + d - 1) / d); }

TileBox slabBox(int level, int sx, int sy, int sz) {
    const int s = spanOf(level);
    return {sx * s, sy * s, sz * s, (sx + 1) * s, (sy + 1) * s, (sz + 1) * s};
}

}  // namespace

TileBox boxOf(const Planned& p) {
    return slabBox(levelOf(p.key), p.sx, p.sy, p.sz);
}

std::vector<Planned> plan(const TileBox& bounds, double eyeX, double eyeY,
                          double eyeZ, const WorldWrap& wrap,
                          const std::unordered_set<ChunkCoord>& previous,
                          const Occupancy* occupancy) {
    std::vector<Planned> out;
    if (bounds.empty()) return out;
    // Distance from the eye to the part of a cell the structure fills.
    const auto distance = [&](const TileBox& box) {
        const double hx = box.sizeX() * 0.5;
        const double cx = box.x0 + hx;
        const double dx = std::max(
            0.0,
            std::abs(wrap.enabled() ? wrap.deltaX(cx, eyeX) : cx - eyeX) - hx);
        const double dy =
            std::max({0.0, double(box.y0) - eyeY, eyeY - double(box.y1)});
        const double dz =
            std::max({0.0, double(box.z0) - eyeZ, eyeZ - double(box.z1)});
        return float(std::sqrt(dx * dx + dy * dy + dz * dz));
    };
    const auto had = [&](int level, int sx, int sy, int sz) {
        const auto key = keyOf(level, sx, sy, sz, wrap);
        return key && previous.contains(*key);
    };
    const auto split = [&](auto&& self, int level, int sx, int sy,
                           int sz) -> void {
        if (occupancy && !occupancy->contains(level, sx, sy, sz)) return;
        const auto part = bounds.intersect(slabBox(level, sx, sy, sz));
        if (part.empty()) return;
        const float d = distance(part);
        bool finer = false;
        if (level > 0) {
            // Was this cell drawn at this level or coarser last time? Then
            // it splits only once clearly inside the finer band.
            bool coarse = false;
            for (int l = level; l <= MaxLevel && !coarse; ++l) {
                const int shift = l - level;
                coarse = had(l, sx >> shift, sy >> shift, sz >> shift);
            }
            const float limit = Limits[level - 1];
            const float margin =
                previous.empty() ? 0.f : (coarse ? -Margin : Margin);
            finer = d < limit + margin;
        }
        if (!finer) {
            if (const auto key = keyOf(level, sx, sy, sz, wrap))
                out.push_back({*key, sx, sy, sz, d});
            return;
        }
        for (int dz = 0; dz < 2; ++dz)
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                    self(self, level - 1, sx * 2 + dx, sy * 2 + dy,
                         sz * 2 + dz);
    };
    const int s = spanOf(MaxLevel);
    const int sy0 = std::max(0, floorDiv(bounds.y0, s)),
              sy1 = std::min((WORLD_HEIGHT_Y + s - 1) / s - 1,
                             floorDiv(bounds.y1 - 1, s));
    const int sz0 = std::max(0, floorDiv(bounds.z0, s)),
              sz1 = floorDiv(bounds.z1 - 1, s);
    for (int sz = sz0; sz <= sz1; ++sz)
        for (int sy = sy0; sy <= sy1; ++sy)
            for (int sx = floorDiv(bounds.x0, s);
                 sx <= floorDiv(bounds.x1 - 1, s); ++sx)
                split(split, MaxLevel, sx, sy, sz);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.distance < b.distance;
    });
    return out;
}

Slabs rasterizeAll(const Source& source, int level, const WorldWrap& wrap) {
    Slabs out;
    const auto& b = source.bounds;
    if (b.empty()) return out;
    const int s = spanOf(level);
    for (int sz = std::max(0, floorDiv(b.z0, s)); sz <= floorDiv(b.z1 - 1, s);
         ++sz)
        for (int sy = std::max(0, floorDiv(b.y0, s));
             sy <= floorDiv(b.y1 - 1, s); ++sy)
            for (int sx = floorDiv(b.x0, s); sx <= floorDiv(b.x1 - 1, s);
                 ++sx) {
                const auto key = keyOf(level, sx, sy, sz, wrap);
                if (!key) continue;
                if (auto slab = source.rasterize(level, sx, sy, sz))
                    out[*key] = std::move(*slab);
            }
    markEnclosed(out, wrap);
    return out;
}

void markEnclosed(Slabs& slabs, const WorldWrap& wrap) {
    const auto full = [&](ChunkCoord c, int dx, int dy, int dz) {
        const int level = levelOf(c);
        const auto base = baseOf(c);
        const auto key = keyOf(level, int(base.x) + dx, int(base.y) + dy,
                               int(base.z) + dz, wrap);
        if (!key) return false;
        const auto it = slabs.find(*key);
        return it != slabs.end() && it->second.bits.all();
    };
    for (auto& [coord, slab] : slabs)
        slab.enclosed = slab.bits.all() && full(coord, 1, 0, 0) &&
                        full(coord, -1, 0, 0) && full(coord, 0, 1, 0) &&
                        full(coord, 0, -1, 0) && full(coord, 0, 0, 1) &&
                        full(coord, 0, 0, -1);
}

}  // namespace structure_voxels
