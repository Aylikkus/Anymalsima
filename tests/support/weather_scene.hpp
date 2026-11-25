#pragma once

#include <algorithm>
#include <iostream>
#include <vector>

#include "serialization/file_io.hpp"
#include "world/save/chunk_codec.hpp"
#include "world/save/flora_codec.hpp"
#include "world/save/save_catalog.hpp"
#include "world/tree_simulation.hpp"

struct WeatherScene {
    struct Cell {
        TilePos pos;
        uint8_t block;
        TileShape shape;
    };

    int x, y, z;
    std::vector<Cell> cells;

    // Both sites use the same local coordinates. The rim at dx=-1,0 crosses
    // either a regular chunk face or the world's wrapped X face.
    WeatherScene(int x, int y, int z, uint8_t stone, uint8_t clearRoof,
                 uint8_t soil)
        : x(x), y(y), z(z) {
        using enum TileShapeProfile;
        const auto put = [&](int dx, int dy, int dz, uint8_t block,
                             TileShape shape = {}) {
            cells.push_back({{x + dx, y + dy, z + dz}, block, shape});
        };
        for (int dz = -5; dz <= 5; ++dz)
            for (int dx = -6; dx <= 5; ++dx) put(dx, 0, dz, stone);
        for (unsigned turn = 0; turn < 4; ++turn)
            put(-5 + int(turn) * 2, 1, -4, stone,
                TileShape::make(Ramp).rotated(turn));
        put(-5, 1, -2, stone, TileShape::make(DiagonalRamp));
        put(-3, 1, -2, stone, TileShape::make(InnerCorner));
        put(-1, 1, -2, stone, TileShape::make(OuterCorner));

        // Descend toward -Z and off the toe into the lower landing.
        put(3, 1, 4, stone);
        put(3, 2, 4, stone);
        put(3, 3, 4, stone, TileShape::make(Ramp));
        put(3, 1, 3, stone);
        put(3, 2, 3, stone, TileShape::make(Ramp));

        for (int dx = -2; dx <= 1; ++dx) put(dx, 1, 5, stone);
        for (int dx = -5; dx <= -2; ++dx)
            for (int dz = 1; dz <= 2; ++dz)
                put(dx, 3, dz, dx <= -4 ? stone : clearRoof);
        // A two-tile pool, 96/255 m deep, bounded by a one-block rim.
        for (int dx = 1; dx <= 4; ++dx)
            for (int dz = -2; dz <= 0; ++dz)
                if (dx == 1 || dx == 4 || dz != -1) put(dx, 1, dz, stone);
        put(5, 0, 3, soil);
        put(5, 0, 5, soil);
    }

    bool ready(ChunkManager& chunks) const {
        return chunks.tileAt(tree().x, y + 1, tree().z) &&
               std::all_of(cells.begin(), cells.end(), [&](const Cell& cell) {
                   const auto [x, y, z] = cell.pos;
                   return chunks.tileAt(x, y, z);
               });
    }

    bool place(ChunkManager& chunks, WorldEditor& editor,
               SunLightPropagator& sunlight, uint8_t floraSpecies,
               uint8_t treeSpecies, double day) const {
        std::vector<EditRequest> requests;
        for (const auto& cell : cells)
            requests.push_back({.pos = cell.pos,
                                .mode = EditMode::Replace,
                                .block = cell.block,
                                .shape = cell.shape});
        for (const auto status : editor.apply(requests))
            if (status != EditStatus::Applied &&
                status != EditStatus::Unchanged)
                return false;

        const auto water = poolStart();
        const auto waterChunk = owner(water, chunks.wrap());
        if (!chunks.withChunkLocked(waterChunk, [&](Chunk& chunk) {
                for (int dx = 0; dx < 2; ++dx) {
                    auto& tile = chunk.tiles[water.z % 16][water.y % 16]
                                            [(water.x + dx) % 16];
                    tile.liquid = LiquidType::Water;
                    tile.liquidLevel = 96;
                    tile.waterOrigin = WaterOrigin::Independent;
                }
            }))
            return false;
        chunks.markModified(waterChunk);
        sunlight.markColumnDirty(waterChunk.x, waterChunk.z);

        const auto flower = plant();
        auto layer = std::make_shared<FloraLayer>();
        layer->instances.push_back({.species = floraSpecies,
                                    .seed = 2024,
                                    .x = uint8_t((flower.x % 16) * 16 + 8),
                                    .z = uint8_t((flower.z % 16) * 16 + 8),
                                    .y = uint8_t(flower.y % 16),
                                    .birthDay = -30});
        chunks.setFlora(owner(flower, chunks.wrap()), layer);
        TreeSimulation planter(chunks, editor);
        return planter.plant(tree(), treeSpecies, day);
    }

    bool verifySave(const SaveSlot& slot, const WorldWrap& wrap) const {
        const auto saved = [&](ChunkCoord coord) {
            return region_file::decode(FileIO::readBytes(region_file::path(
                slot.directory,
                {coord.x / CHUNKS_PER_REGION, coord.z / CHUNKS_PER_REGION})));
        };
        const auto cellAt = [&](TilePos pos) {
            const auto coord = owner(pos, wrap);
            const auto region = saved(coord);
            Chunk chunk;
            chunk_codec::decode(
                *region.chunks.at(RegionData::localIndex(coord)), chunk);
            return chunk.cellAt(wrap.wrapTile(pos.x) % 16, pos.y % 16,
                                pos.z % 16);
        };
        // Check the ramp west of X=0 as well, through the canonical save key.
        const bool shape = cellAt({x - 5, y + 1, z - 4}).shape ==
                           TileShape::make(TileShapeProfile::Ramp);
        const auto water = cellAt(poolStart()).tile;
        const bool pool =
            water.liquid == LiquidType::Water && water.liquidLevel == 96;
        const auto plantCoord = owner(plant(), wrap);
        const auto region = saved(plantCoord);
        const bool flora =
            !flora_codec::decode(
                 *region.flora.at(RegionData::localIndex(plantCoord)))
                 .empty();
        const auto overrides = save_catalog::readStructures(slot);
        const bool tree =
            std::any_of(overrides.trees.begin(), overrides.trees.end(),
                        [](const auto& pair) { return pair.second.planted(); });
        std::cout << "weather_saved_shape=" << shape << " flora=" << flora
                  << " structure_override=" << tree << " water=" << pool
                  << '\n';
        return shape && pool && flora && tree;
    }

   private:
    static ChunkCoord owner(TilePos pos, const WorldWrap& wrap) {
        return {unsigned(wrap.wrapTile(pos.x) / Chunk::SIZE_X),
                unsigned(pos.y / Chunk::SIZE_Y),
                unsigned(pos.z / Chunk::SIZE_Z)};
    }

    TilePos poolStart() const { return {x + 2, y + 1, z - 1}; }
    TilePos plant() const { return {x + 5, y, z + 3}; }
    TilePos tree() const { return {x + 5, y, z + 5}; }
};
