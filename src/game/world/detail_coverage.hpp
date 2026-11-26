#pragma once

#include <algorithm>
#include <map>
#include <span>

#include "chunk_coord.hpp"
#include "terrain_lod.hpp"
#include "world_wrap.hpp"

namespace TerrainLod {
// Detailed chunks own their visibility after handover. Neither a new LOD
// layout nor a pending lighting remesh may demote an already drawable column.
class DetailCoverage {
   public:
    explicit DetailCoverage(int chunkRadius)
        : keep(std::max(0, chunkRadius - 1)) {}
    // Leave a ring for mesh neighbours, plus one chunk of horizontal
    // hysteresis so crossing a boundary does not immediately hide a column.
    int keepRadius() const { return keep; }
    int enterRadius() const { return std::max(0, keep - 1); }
    using Key = std::pair<int, int>;
    using Columns = std::map<Key, SurfaceBand>;

    static void includeSlab(Columns& columns, ChunkCoord coord) {
        const Key key{int(coord.x), int(coord.z)};
        const int y = int(coord.y);
        auto& band =
            columns.try_emplace(key, SurfaceBand{key.first, key.second, y, y})
                .first->second;
        band.minY = std::min(band.minY, y);
        band.maxY = std::max(band.maxY, y);
    }

    // periodChunks > 0: X wraps with this many chunk columns.
    template <typename Current, typename Drawable>
    void update(ChunkCoord focus, std::span<const SurfaceBand> candidates,
                Current&& current, Drawable&& drawable, int periodChunks = 0,
                bool refreshBands = false,
                const snow::ChunkColumns* pendingColumns = nullptr) {
        auto next = visible;
        const auto inRange = [&](const SurfaceBand& band, int radius) {
            return std::abs(WorldWrap::delta(band.x, int(focus.x),
                                             periodChunks)) <= radius &&
                   std::abs(band.z - int(focus.z)) <= radius &&
                   band.minY >= int(focus.y) - keep &&
                   band.maxY <= int(focus.y) + keep;
        };
        const auto ready = [](const SurfaceBand& band, auto&& predicate) {
            for (int y = band.minY; y <= band.maxY; ++y)
                if (!predicate(ChunkCoord{unsigned(band.x), unsigned(y),
                                          unsigned(band.z)}))
                    return false;
            return true;
        };
        for (auto it = next.begin(); it != next.end();)
            if (!inRange(it->second, keep) || !ready(it->second, drawable))
                it = next.erase(it);
            else
                ++it;
        for (const auto& band : candidates) {
            const Key key{band.x, band.z};
            const auto old = next.find(key);
            if (old != next.end() && (!refreshBands || old->second == band))
                continue;
            // Keep the exterior until a new streaming projection can be
            // included in the current chunk meshes. A drawable column can
            // retain its last mesh while a replacement is in flight.
            if (pendingColumns &&
                pendingColumns->contains({unsigned(band.x), unsigned(band.z)}))
                continue;
            if (inRange(band, old == next.end() ? enterRadius() : keep) &&
                ready(band, current))
                next[key] = band;
        }
        if (next != visible) {
            visible = std::move(next);
            ++revision;
        }
    }
    void remove(const Key& key) {
        if (visible.erase(key)) ++revision;
    }
    bool contains(int x, int z) const { return visible.contains({x, z}); }
    const Columns& columns() const { return visible; }
    uint64_t version() const { return revision; }

   private:
    const int keep;
    Columns visible;
    uint64_t revision = 0;
};
}  // namespace TerrainLod
