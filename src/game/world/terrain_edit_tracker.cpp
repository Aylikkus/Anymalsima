#include "terrain_edit_tracker.hpp"

#include <algorithm>

#include "save/chunk_codec.hpp"

namespace {
bool sameEdit(const ColumnEdit& a, const ColumnEdit& b) {
    if (a.groundY != b.groundY || a.groundBlock != b.groundBlock ||
        a.waterY != b.waterY || a.structure.size() != b.structure.size())
        return false;
    for (size_t i = 0; i < a.structure.size(); ++i)
        if (a.structure[i].first != b.structure[i].first ||
            *a.structure[i].second != *b.structure[i].second)
            return false;
    return true;
}
}  // namespace

TerrainEditTracker::TerrainEditTracker(ChunkManager& chunks,
                                       TerrainEdits& edits,
                                       bool detectSavedEdits)
    : chunks(chunks), edits(edits), detect(detectSavedEdits) {
    readyConnection = chunks.becameReady.connect([this](const ChunkCoord& c) {
        const auto key = editKey(c.x, c.z);
        if (incomplete.contains(key)) {
            dirty.insert(key);
            return;
        }
        if (verified.contains(key)) return;
        if ((detect && this->chunks.isEdited(c)) || this->edits.contains(key))
            dirty.insert(key);
    });
}

TerrainEditTracker::~TerrainEditTracker() {
    chunks.becameReady.disconnect(readyConnection);
    if (editor) editor->blockChanged.disconnect(editConnection);
}

void TerrainEditTracker::watch(WorldEditor& source) {
    if (editor) editor->blockChanged.disconnect(editConnection);
    editor = &source;
    editConnection = source.blockChanged.connect([this](const BlockChange& c) {
        markDirty(unsigned(c.pos.x / Chunk::SIZE_X),
                  unsigned(c.pos.z / Chunk::SIZE_Z));
    });
}

void TerrainEditTracker::markDirty(unsigned chunkX, unsigned chunkZ) {
    dirty.insert(editKey(chunkX, chunkZ));
}

void TerrainEditTracker::update() {
    if (dirty.empty()) return;
    // Edits are rare and must be captured before their regions unload, so
    // every dirty column is processed now.
    const auto work = std::move(dirty);
    dirty.clear();
    for (const auto key : work) {
        verified.insert(key);
        if (refresh(key))
            incomplete.erase(key);
        else
            incomplete.insert(key);
    }
}

bool TerrainEditTracker::refresh(uint64_t key) {
    const auto cx = unsigned(key >> 32), cz = unsigned(key);
    const auto inputs = chunks.generatedColumns(cx, cz);
    // Unregistered column (region unloaded meanwhile): keep what is stored.
    if (!inputs) return true;
    const auto old = edits.find(key);
    // Tree wood is regenerated from the seed: neither ground nor structure.
    const auto& grown = chunks.grownBlocks();
    auto next = std::make_shared<ColumnEdit>();
    bool complete = true;
    bool generated = true;
    int lowestSurface = WORLD_HEIGHT_Y;
    for (int lz = 0; lz < Chunk::SIZE_Z; ++lz)
        for (int lx = 0; lx < Chunk::SIZE_X; ++lx) {
            const int i = ColumnEdit::index(lx, lz);
            const int surface = inputs->columns[i].surface_y;
            lowestSurface = std::min(lowestSurface, surface);
            int ground = -1, water = -1;
            uint8_t block = 0;
            for (int y = std::max(surface, SEA_LEVEL_Y); y >= 0; --y) {
                const auto* chunk =
                    chunks.getChunk({cx, unsigned(y / Chunk::SIZE_Y), cz});
                if (!chunk) {
                    // Unloaded below: keep what was known, or assume the
                    // unloaded slab starts with solid ground.
                    if (old) {
                        ground = old->groundY[i];
                        block = old->groundBlock[i];
                        if (water < 0) water = old->waterY[i];
                    } else {
                        ground = y;
                        complete = false;
                    }
                    break;
                }
                const Tile& tile = chunk->tiles[lz][y % Chunk::SIZE_Y][lx];
                if (y <= surface && tile.isSolid() &&
                    !grown.test(tile.block_id)) {
                    ground = y;
                    block = tile.block_id;
                    break;
                }
                if (water < 0 && tile.isLiquid()) water = y + 1;
            }
            next->groundY[i] = int16_t(ground);
            next->groundBlock[i] = block;
            next->waterY[i] = int16_t(water);
            const int generatedWater =
                surface < SEA_LEVEL_Y ? SEA_LEVEL_Y + 1 : -1;
            generated &= ground == surface && water == generatedWater;
        }
    // Structure: every solid tile above its generated surface, except tree
    // wood.
    const int top = std::min(chunks.getColumnTopY(cx, cz), WORLD_HEIGHT_Y - 1);
    for (int slab = std::max(0, lowestSurface / Chunk::SIZE_Y);
         slab <= top / Chunk::SIZE_Y; ++slab) {
        const auto* chunk = chunks.getChunk({cx, unsigned(slab), cz});
        if (!chunk) {
            if (old) {
                if (const auto* blob = old->slab(uint16_t(slab)))
                    next->structure.emplace_back(uint16_t(slab), *blob);
            } else {
                complete = false;
            }
            continue;
        }
        Chunk part;
        bool any = false;
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int y = 0; y < Chunk::SIZE_Y; ++y)
                for (int x = 0; x < Chunk::SIZE_X; ++x) {
                    const auto& tile = chunk->tiles[z][y][x];
                    if (!tile.isSolid() || grown.test(tile.block_id) ||
                        slab * Chunk::SIZE_Y + y <=
                            inputs->columns[ColumnEdit::index(x, z)].surface_y)
                        continue;
                    part.tiles[z][y][x].block_id = tile.block_id;
                    any = true;
                }
        if (any)
            next->structure.emplace_back(
                uint16_t(slab),
                std::make_shared<const Bytes>(chunk_codec::encode(part)));
    }
    next->finish();
    if (generated && next->structure.empty())
        edits.erase(key);
    else if (!old || !sameEdit(*old, *next))
        edits.set(key, std::move(next));
    return complete;
}
