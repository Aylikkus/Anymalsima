#pragma once
#include <climits>

#include "chunk.hpp"
#include "chunk_coord.hpp"
#include "consts.hpp"
#include "core/assets_api.hpp"
#include "soil_generator.hpp"
#include "terrain_sampler.hpp"
#include "world/region.hpp"

// Pure stateless generator — no ownership, no side effects.
// Takes a region and a chunk coord, fills a Chunk.
class LocationGenerator {
   public:
    explicit LocationGenerator() {}

    // Fill one chunk. coord is in world-chunk space (region-offset included).
    // Optional immutable columns enable relief; omitted inputs keep flat
    // fixtures.
    void generateChunk(Chunk& chunk, const ChunkCoord& coord,
                       const ChunkCoord& regionOriginChunk,
                       const Region& region,
                       const std::vector<RegionLayer>& layers,
                       const SoilProfile& soil = {},
                       const ChunkColumnInputs* columns = nullptr) const;

    // True when generateChunk would leave every tile of the chunk as air, so
    // callers may skip the worker job. Keep every "is this chunk air?" rule
    // here (future sky islands and underworld layers must extend it).
    // woodTop: the highest tile tree wood may occupy over the column
    // (TreeColumn::woodTop), INT_MIN without trees.
    bool isAirChunk(const ChunkCoord& coord, const ChunkColumnInputs& columns,
                    int woodTop = INT_MIN) const;

   private:
    const RegionLayer* layerAtDepth(const std::vector<RegionLayer>& layers,
                                    int depth_m) const;

    void generateColumn(Chunk& chunk, int localX, int localZ, int chunkBaseY,
                        const Region& region,
                        const std::vector<RegionLayer>& layers,
                        const SoilColumn& soil, int surfaceY) const;
};