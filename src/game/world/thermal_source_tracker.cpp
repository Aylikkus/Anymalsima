#include "thermal_source_tracker.hpp"

#include <algorithm>

#include "world_editor.hpp"

ThermalSourceTracker::ThermalSourceTracker(ChunkManager& chunks,
                                           const DataStorage<Block>& blocks,
                                           ThermalSourceRegistry& registry)
    : chunks(chunks), registry(registry) {
    for (const auto& block : blocks.items)
        if (block.thermal && block.thermal->emits())
            emissions[block.id] = &*block.thermal;
    readyConnection = chunks.becameReady.connect(
        [this](const ChunkCoord& coord) { ready(coord); });
    removedConnection = chunks.removed.connect(
        [this](const ChunkCoord& coord) { removed(coord); });
    chunks.forEachReady(
        [this](const ChunkCoord& coord, Chunk&) { scanChunk(coord); });
}

ThermalSourceTracker::~ThermalSourceTracker() {
    chunks.becameReady.disconnect(readyConnection);
    chunks.removed.disconnect(removedConnection);
    if (editor) editor->blockChanged.disconnect(editorConnection);
}

void ThermalSourceTracker::watch(WorldEditor& value) {
    if (editor) editor->blockChanged.disconnect(editorConnection);
    editor = &value;
    editorConnection = editor->blockChanged.connect(
        [this](const BlockChange& change) { changed(change); });
}

void ThermalSourceTracker::ready(const ChunkCoord& coord) { scanChunk(coord); }

void ThermalSourceTracker::removed(const ChunkCoord& coord) {
    const TilePos origin{int(coord.x) * Chunk::SIZE_X,
                         int(coord.y) * Chunk::SIZE_Y,
                         int(coord.z) * Chunk::SIZE_Z};
    const TilePos end{origin.x + Chunk::SIZE_X, origin.y + Chunk::SIZE_Y,
                      origin.z + Chunk::SIZE_Z};
    for (const auto tile : registry.blockThermalsIn(origin, end))
        registry.removeBlockThermal(tile);
}

void ThermalSourceTracker::changed(const BlockChange& change) {
    const auto x = chunks.wrap().wrapTile(change.pos.x);
    if (x < 0 || change.pos.y < 0 || change.pos.z < 0) return;
    const TilePos pos{int(x), change.pos.y, change.pos.z};
    const uint8_t block = change.after.block_id;
    const auto* emission =
        block < emissions.size() ? emissions[block] : nullptr;
    if (emission)
        registry.setBlockThermal(pos, block, *emission);
    else
        registry.removeBlockThermal(pos);
}

void ThermalSourceTracker::scanChunk(const ChunkCoord& coord) {
    const TilePos origin{int(coord.x) * Chunk::SIZE_X,
                         int(coord.y) * Chunk::SIZE_Y,
                         int(coord.z) * Chunk::SIZE_Z};
    std::unordered_set<TilePos> present;
    chunks.withChunkLocked(coord, [&](const Chunk& chunk) {
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int y = 0; y < Chunk::SIZE_Y; ++y)
                for (int x = 0; x < Chunk::SIZE_X; ++x) {
                    const uint8_t block = chunk.tiles[z][y][x].block_id;
                    const auto* emission =
                        block < emissions.size() ? emissions[block] : nullptr;
                    if (!emission) continue;
                    const TilePos tile{origin.x + x, origin.y + y,
                                       origin.z + z};
                    present.insert(tile);
                    registry.setBlockThermal(tile, block, *emission);
                }
    });
    const TilePos end{origin.x + Chunk::SIZE_X, origin.y + Chunk::SIZE_Y,
                      origin.z + Chunk::SIZE_Z};
    for (const auto tile : registry.blockThermalsIn(origin, end))
        if (!present.contains(tile)) registry.removeBlockThermal(tile);
}
