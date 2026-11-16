#pragma once

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
    static constexpr int EnterRadius = 6;
    static constexpr int KeepRadius = 7;
    using Key = std::pair<int, int>;
    using Columns = std::map<Key, SurfaceBand>;

    // periodChunks > 0: X wraps with this many chunk columns.
    template <typename Current, typename Drawable>
    void update(ChunkCoord focus, std::span<const SurfaceBand> candidates,
                Current&& current, Drawable&& drawable, int periodChunks = 0) {
        auto next = visible;
        const auto inRange = [&](const SurfaceBand& band, int radius) {
            return std::abs(WorldWrap::delta(band.x, int(focus.x),
                                             periodChunks)) <= radius &&
                   std::abs(band.z - int(focus.z)) <= radius &&
                   band.minY >= int(focus.y) - KeepRadius &&
                   band.maxY <= int(focus.y) + KeepRadius;
        };
        const auto ready = [](const SurfaceBand& band, auto&& predicate) {
            for (int y = band.minY; y <= band.maxY; ++y)
                if (!predicate(ChunkCoord{unsigned(band.x), unsigned(y),
                                          unsigned(band.z)}))
                    return false;
            return true;
        };
        for (auto it = next.begin(); it != next.end();)
            if (!inRange(it->second, KeepRadius) ||
                !ready(it->second, drawable))
                it = next.erase(it);
            else
                ++it;
        for (const auto& band : candidates)
            if (!next.contains({band.x, band.z}) &&
                inRange(band, EnterRadius) && ready(band, current))
                next[{band.x, band.z}] = band;
        if (next != visible) {
            visible = std::move(next);
            ++revision;
        }
    }
    bool contains(int x, int z) const { return visible.contains({x, z}); }
    const Columns& columns() const { return visible; }
    uint64_t version() const { return revision; }

   private:
    Columns visible;
    uint64_t revision = 0;
};
}  // namespace TerrainLod
