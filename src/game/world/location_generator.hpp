#pragma once
#include "chunk.hpp"
#include "chunk_coord.hpp"
#include "core/assets_api.hpp"
#include "world/region.hpp"
#include "consts.hpp"

// Pure stateless generator — no ownership, no side effects.
// Takes a region and a chunk coord, fills a Chunk.
class LocationGenerator {
   public:
    explicit LocationGenerator() {}

    // Fill one chunk. coord is in world-chunk space (region-offset included).
    // regionOriginChunk is the coord of chunk (0,0) of the owning region —
    // used to derive local XY position within the region for future noise.
    void generateChunk(Chunk& chunk, const ChunkCoord& coord,
                       const ChunkCoord& regionOriginChunk,
                       const Region& region,
                       const std::vector<RegionLayer>& layers) const;

   private:
    static int surfaceTileY(const Region& region) {
        return region.land_height_m;
    }

    const RegionLayer* layerAtDepth(const std::vector<RegionLayer>& layers,
                                    int depth_m) const;

    void generateColumn(Chunk& chunk, int localX, int localZ, int chunkBaseY,
                        const Region& region,
                        const std::vector<RegionLayer>& layers) const;
};