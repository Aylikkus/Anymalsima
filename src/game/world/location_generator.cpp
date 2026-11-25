#include "location_generator.hpp"

const RegionLayer* LocationGenerator::layerAtDepth(
    const std::vector<RegionLayer>& layers, int depth_m) {
    for (const auto& layer : layers) {
        int top = layer.depth_from_surface_m;
        int bottom = top + layer.thickness_m;
        if (depth_m >= top && depth_m < bottom) return &layer;
    }
    if (layers.empty()) throw std::runtime_error("Empty rock layer stack");
    return &layers.back();
}

uint8_t LocationGenerator::terrainBlock(const Region& region,
                                        const std::vector<RegionLayer>& layers,
                                        const SoilColumn& soil, int surfaceY,
                                        int y) {
    const int depth = surfaceY - y;
    for (int i = 0; i < soil.count; ++i) {
        const auto& horizon = soil.layers[i];
        if (depth >= horizon.depth_from_surface_m &&
            depth < horizon.depth_from_surface_m + horizon.thickness_m)
            return horizon.block_id;
    }
    return layerAtDepth(layers, std::max(0, region.land_height_m - y))
        ->block_id;
}

void LocationGenerator::generateColumn(Chunk& chunk, int localX, int localZ,
                                       int chunkBaseY, const Region& region,
                                       const std::vector<RegionLayer>& layers,
                                       const SoilColumn& soil, int surfaceY,
                                       WaterSurface water) const {
    for (int localY = 0; localY < Chunk::SIZE_Y; ++localY) {
        const int tileY = chunkBaseY + localY;
        Tile& tile = chunk.tiles[localZ][localY][localX];
        tile.clearCover();
        tile.waterOrigin = WaterOrigin::Independent;

        if (tileY <= surfaceY) {
            tile.block_id = terrainBlock(region, layers, soil, surfaceY, tileY);
            tile.liquid = LiquidType::None;
            tile.liquidLevel = 0;
            // Only the surface tile carries cover (soil.cover is set only
            // when that tile is topsoil on dry land).
            if (tileY == surfaceY && soil.cover)
                tile.setCover(soil.cover, soil.coverDensity);

        } else if (water.fills(tileY, surfaceY)) {
            tile.block_id = 0;
            tile.liquid = water.kind();
            tile.liquidLevel = water.fillAt(tileY, surfaceY);

        } else {
            tile.block_id = 0;
            tile.liquid = LiquidType::None;
            tile.liquidLevel = 0;
        }
    }
}

void LocationGenerator::generateChunk(Chunk& chunk, const ChunkCoord& coord,
                                      const ChunkCoord& regionOriginChunk,
                                      const Region& region,
                                      const std::vector<RegionLayer>& layers,
                                      const SoilProfile& soil,
                                      const ChunkColumnInputs* columns) const {
    chunk.clearShapes();
    chunk.rootTerrain.reset();
    chunk.legacyRootTerrain = false;
    chunk.legacyRootShape = false;
    const int chunkBaseY = coord.y * Chunk::SIZE_Y;

    (void)regionOriginChunk;
    for (int localZ = 0; localZ < Chunk::SIZE_Z; ++localZ)
        for (int localX = 0; localX < Chunk::SIZE_X; ++localX) {
            const auto cover =
                columns ? columns->columns[localZ * Chunk::SIZE_X + localX].soil
                        : SoilGenerator::sample(
                              soil, region,
                              int64_t(coord.x) * Chunk::SIZE_X + localX,
                              int64_t(coord.z) * Chunk::SIZE_Z + localZ);
            const auto* column =
                columns ? &columns->columns[localZ * Chunk::SIZE_X + localX]
                        : nullptr;
            const int ground =
                column ? column->surface_y : region.land_height_m;
            generateColumn(chunk, localX, localZ, chunkBaseY, region, layers,
                           cover, ground,
                           column ? column->water : WaterSurface::sea(ground));
        }
}

bool LocationGenerator::isAirChunk(const ChunkCoord& coord,
                                   const ChunkColumnInputs& columns) const {
    int top = -1;
    for (const auto& column : columns.columns)
        top = std::max({top, column.surface_y, column.water.topTile()});
    return int(coord.y) * Chunk::SIZE_Y > top;
}
