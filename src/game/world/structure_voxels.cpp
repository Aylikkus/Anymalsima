#include "structure_voxels.hpp"

#include <algorithm>
#include <climits>
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

void Source::skyTops(const TileBox& columns, std::vector<int>& tops) const {
    const int width = columns.x1 - columns.x0;
    const int depth = columns.z1 - columns.z0;
    tops.assign(columns.empty() ? 0 : size_t(width) * size_t(depth), INT_MIN);
    const TileBox box = columns.intersect(bounds);
    if (box.empty()) return;
    const auto floorDiv = [](int a) {
        return a >= 0 ? a / Side : -((-a + Side - 1) / Side);
    };
    for (int sz = floorDiv(box.z0); sz <= floorDiv(box.z1 - 1); ++sz)
        for (int sx = floorDiv(box.x0); sx <= floorDiv(box.x1 - 1); ++sx) {
            // Tiles of the box in this slab column still without a top.
            const int tx0 = std::max(box.x0, sx * Side);
            const int tx1 = std::min(box.x1, (sx + 1) * Side);
            const int tz0 = std::max(box.z0, sz * Side);
            const int tz1 = std::min(box.z1, (sz + 1) * Side);
            int open = (tx1 - tx0) * (tz1 - tz0);
            for (int sy = floorDiv(box.y1 - 1); sy >= floorDiv(box.y0) && open;
                 --sy) {
                const auto slab = rasterize(0, sx, sy, sz);
                if (!slab) continue;
                for (int z = tz0; z < tz1; ++z)
                    for (int x = tx0; x < tx1; ++x) {
                        int& top = tops[size_t(z - columns.z0) * width +
                                        size_t(x - columns.x0)];
                        if (top != INT_MIN) continue;
                        for (int ly = Side - 1; ly >= 0; --ly) {
                            const int y = sy * Side + ly;
                            if (y < box.y0 || y >= box.y1) continue;
                            if (slab->bits.test(cellIndex(x - sx * Side, ly,
                                                          z - sz * Side))) {
                                top = y;
                                --open;
                                break;
                            }
                        }
                    }
            }
        }
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

FootprintPtr footprint(const Source& source, const Occupancy* occupancy,
                       const Footprint* previous,
                       const std::optional<TileBox>& changed) {
    auto out = std::make_shared<Footprint>();
    const auto& b = source.bounds;
    if (b.empty()) return out;
    constexpr int level = FootprintLevel;
    const int s = spanOf(level), cell = Footprint::Cell;
    const int sx0 = floorDiv(b.x0, s), sx1 = floorDiv(b.x1 - 1, s);
    const int sz0 = floorDiv(b.z0, s), sz1 = floorDiv(b.z1 - 1, s);
    const int sy0 = std::max(0, floorDiv(b.y0, s)),
              sy1 = std::min((WORLD_HEIGHT_Y + s - 1) / s - 1,
                             floorDiv(b.y1 - 1, s));
    // Slab columns to rasterize: all of them, or those under the change.
    int rx0 = sx0, rx1 = sx1, rz0 = sz0, rz1 = sz1;
    if (previous && changed) {
        rx0 = std::max(sx0, floorDiv(changed->x0, s));
        rx1 = std::min(sx1, floorDiv(changed->x1 - 1, s));
        rz0 = std::max(sz0, floorDiv(changed->z0, s));
        rz1 = std::min(sz1, floorDiv(changed->z1 - 1, s));
        const int perSlab = s / cell;
        for (const auto& [key, top] : previous->tops) {
            const auto [cx, cz] = Footprint::unpackColumn(key);
            const int sx = floorDiv(cx, perSlab), sz = floorDiv(cz, perSlab);
            if (sx >= rx0 && sx <= rx1 && sz >= rz0 && sz <= rz1) continue;
            out->tops.emplace(key, top);
        }
    }
    for (int sz = rz0; sz <= rz1; ++sz)
        for (int sx = rx0; sx <= rx1; ++sx) {
            // Top down: the first solid cell of a column is its top, and
            // lower slabs cannot raise it.
            std::bitset<size_t(Side) * Side> topped;
            for (int sy = sy1; sy >= sy0 && !topped.all(); --sy) {
                if (occupancy && !occupancy->contains(level, sx, sy, sz))
                    continue;
                const auto slab = source.rasterize(level, sx, sy, sz);
                if (!slab || slab->bits.none()) continue;
                for (int z = 0; z < Side; ++z)
                    for (int x = 0; x < Side; ++x) {
                        if (topped.test(size_t(z * Side + x))) continue;
                        for (int y = Side - 1; y >= 0; --y) {
                            if (!slab->bits.test(cellIndex(x, y, z))) continue;
                            out->tops.emplace(Footprint::packColumn(
                                                  sx * Side + x, sz * Side + z),
                                              (sy * Side + y + 1) * cell);
                            topped.set(size_t(z * Side + x));
                            break;
                        }
                    }
            }
        }
    return out;
}

}  // namespace structure_voxels
