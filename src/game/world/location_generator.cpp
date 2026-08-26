#include "location_generator.hpp"

const RegionLayer* LocationGenerator::layerAtDepth(
    const std::vector<RegionLayer>& layers, int depth_m) const {
    for (const auto& layer : layers) {
        int top = layer.depth_from_surface_m;
        int bottom = top + layer.thickness_m;
        if (depth_m >= top && depth_m < bottom) return &layer;
    }
    return &layers[layers.size() - 1];
}

void LocationGenerator::generateColumn(
    Chunk& chunk, int localX, int localZ, int chunkBaseY, const Region& region,
    const std::vector<RegionLayer>& layers) const {
    const int surfaceY = surfaceTileY(region);

    for (int localY = 0; localY < Chunk::SIZE_Y; ++localY) {
        const int tileY = chunkBaseY + localY;
        Tile& tile = chunk.tiles[localZ][localY][localX];

        if (tileY <= surfaceY) {
            int depth_m = surfaceY - tileY;
            const RegionLayer* layer = layerAtDepth(layers, depth_m);
            tile.block_id = layer->block_id;
            tile.liquid = LiquidType::None;
            tile.liquidLevel = 0;

        } else if (tileY <= SEA_LEVEL_Y) {
            // surface is below sea level — fill with water
            tile.block_id = 0;
            tile.liquid = LiquidType::Water;
            tile.liquidLevel = 255;

        } else {
            tile.block_id = 0;
            tile.liquid = LiquidType::None;
            tile.liquidLevel = 0;
        }
    }
}

void LocationGenerator::generateChunk(
    Chunk& chunk, const ChunkCoord& coord, const ChunkCoord& regionOriginChunk,
    const Region& region, const std::vector<RegionLayer>& layers) const {
    const int chunkBaseY = coord.y * Chunk::SIZE_Y;

    // Local XY within region — useful later for per-column noise offset
    // (not used yet, but passed through so the signature is stable)
    const int regionLocalChunkX = coord.x - regionOriginChunk.x;
    const int regionLocalChunkY = coord.y - regionOriginChunk.y;
    (void)regionLocalChunkX;
    (void)regionLocalChunkY;

    for (int localZ = 0; localZ < Chunk::SIZE_Z; ++localZ)
        for (int localX = 0; localX < Chunk::SIZE_X; ++localX)
            generateColumn(chunk, localX, localZ, chunkBaseY, region, layers);
}