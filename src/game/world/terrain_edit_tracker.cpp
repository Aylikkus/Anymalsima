#include "terrain_edit_tracker.hpp"

#include <algorithm>
#include <bitset>

#include "save/chunk_codec.hpp"

namespace {
bool hasTileEdits(const Chunk& chunk) {
    return chunk.legacyRootTerrain ||
           (chunk.rootTerrain && !chunk.rootTerrain->edited.empty());
}

bool sameEdit(const ColumnEdit& a, const ColumnEdit& b) {
    if (a.groundY != b.groundY || a.groundBlock != b.groundBlock ||
        a.waterY != b.waterY || a.reservoirCeiling != b.reservoirCeiling ||
        a.structure.size() != b.structure.size() || a.removed != b.removed)
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
        const auto* chunk = this->chunks.getChunk(c);
        if (incomplete.contains(key) ||
            (detect &&
             (this->chunks.isEdited(c) || (chunk && hasTileEdits(*chunk))))) {
            dirty.insert(key);
            return;
        }
        if (verified.contains(key)) return;
        if (this->edits.contains(key)) dirty.insert(key);
    });
    rebaseConnection = chunks.structureRebased.connect(
        [this](const ChunkCoord& c) { markDirty(c.x, c.z); });
}

TerrainEditTracker::~TerrainEditTracker() {
    chunks.becameReady.disconnect(readyConnection);
    chunks.structureRebased.disconnect(rebaseConnection);
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
    const auto trees = chunks.treeColumn(cx, cz);
    // Tree wood is regenerated from the seed. Record only its wounds.
    const auto& grown = chunks.grownBlocks();
    auto next = std::make_shared<ColumnEdit>();
    bool complete = true;
    bool generated = true;
    int lowestSurface = WORLD_HEIGHT_Y;
    for (int lz = 0; lz < Chunk::SIZE_Z; ++lz)
        for (int lx = 0; lx < Chunk::SIZE_X; ++lx) {
            const int i = ColumnEdit::index(lx, lz);
            const auto& baseline = inputs->columns[i];
            const int surface = baseline.surface_y;
            lowestSurface = std::min(lowestSurface, surface);
            int ground = -1, water = -1, reservoirTop = -1;
            bool projected = false;
            uint8_t block = 0;
            for (int y = std::max({surface, baseline.water.topTile(),
                                   old ? old->waterY[i] - 1 : -1});
                 y >= 0; --y) {
                const auto* chunk =
                    chunks.getChunk({cx, unsigned(y / Chunk::SIZE_Y), cz});
                if (!chunk) {
                    // Unloaded below: keep what was known, or assume the
                    // unloaded slab starts with solid ground.
                    if (old) {
                        ground = old->groundY[i];
                        block = old->groundBlock[i];
                        const int oldCap = old->reservoirCeiling[i];
                        const int oldWater = old->waterY[i];
                        if (water < 0 && oldWater >= 0 && y > ground)
                            water = std::min(y + 1, oldWater);
                        if (reservoirTop < 0 && y > ground &&
                            (oldCap ||
                             oldWater == ColumnEdit::GeneratedWater)) {
                            projected = true;
                            const int ceiling =
                                oldCap ? oldCap : baseline.water.surfaceY;
                            reservoirTop = std::min(y + 1, ceiling);
                        }
                    } else {
                        ground = y;
                        complete = false;
                    }
                    break;
                }
                const Tile& tile = chunk->tiles[lz][y % Chunk::SIZE_Y][lx];
                const auto* root =
                    chunk->rootAt(Chunk::cell(lx, y % Chunk::SIZE_Y, lz));
                if (y <= surface && tile.isSolid() &&
                    (root || !grown.test(tile.block_id))) {
                    ground = y;
                    block = root ? root->block : tile.block_id;
                    break;
                }
                const auto cell = Chunk::cell(lx, y % Chunk::SIZE_Y, lz);
                const bool inReservoir =
                    chunk->projectsWater && baseline.water.body &&
                    !baseline.water.reach && baseline.water.fills(y, surface);
                projected |= inReservoir;
                if (reservoirTop < 0 && inReservoir && !tile.isSolid() &&
                    !chunk->rootEdited(cell) &&
                    (!tile.isLiquid() ||
                     tile.waterOrigin == WaterOrigin::Reservoir))
                    reservoirTop = y + 1;
                if (water < 0 && tile.liquid == LiquidType::Water &&
                    tile.waterOrigin == WaterOrigin::Independent)
                    water = y + 1;
            }
            next->groundY[i] = int16_t(ground);
            next->groundBlock[i] = block;
            const int generatedWater = baseline.water.surfaceY;
            if (projected) {
                const bool unchanged =
                    reservoirTop == generatedWater && water < 0;
                next->waterY[i] =
                    unchanged ? ColumnEdit::GeneratedWater : int16_t(water);
                if (!unchanged)
                    next->reservoirCeiling[i] = int16_t(reservoirTop);
                generated &= ground == surface && unchanged;
            } else {
                next->waterY[i] = chunks.reservoirs() && !baseline.water.body &&
                                          water == generatedWater
                                      ? ColumnEdit::GeneratedWater
                                      : int16_t(water);
                generated &= ground == surface && water == generatedWater;
            }
        }
    // Compare each loaded slab with the source snapshot it was built against.
    int top = chunks.getColumnTopY(cx, cz);
    if (old) {
        top = std::max(top, old->structureTopY());
        if (!old->removed.empty())
            top = std::max(top, int(old->removed.back().first) * Chunk::SIZE_Y);
    }
    top = std::min(top, WORLD_HEIGHT_Y - 1);
    for (int slab = std::max(0, lowestSurface / Chunk::SIZE_Y); slab < Y_CHUNKS;
         ++slab) {
        const ChunkCoord coord{cx, unsigned(slab), cz};
        const auto* chunk = chunks.getChunk(coord);
        if (!chunk) {
            if (slab > top / Chunk::SIZE_Y) continue;
            if (old) {
                if (const auto* blob = old->slab(uint16_t(slab)))
                    next->structure.emplace_back(uint16_t(slab), *blob);
                if (const auto* cells = old->removals(uint16_t(slab)))
                    next->removed.emplace_back(uint16_t(slab), *cells);
            } else {
                complete = false;
            }
            continue;
        }
        Chunk part;
        std::vector<uint16_t> removed;
        const auto* baseline = chunks.structureBaseline(coord);
        const auto* previous = old ? old->removals(uint16_t(slab)) : nullptr;
        std::bitset<structure_voxels::Cells> landmarkWood;
        const auto stamp = chunks.treeStamp(coord);
        if (trees && stamp.placed() && (hasTileEdits(*chunk) || previous)) {
            constexpr int side = Chunk::SIZE_X;
            const TileBox box{int(cx) * side,    slab * side,
                              int(cz) * side,    int(cx + 1) * side,
                              (slab + 1) * side, int(cz + 1) * side};
            std::vector<uint8_t> mask;
            for (const auto& entry : trees->trees) {
                const auto& tree = *entry.tree;
                const auto* rule = chunks.treeSpecies().find(tree.species);
                if (!rule || !rule->landmark) continue;
                auto bounds = tree.wood;
                bounds.x0 += entry.anchor.x - tree.anchor.x;
                bounds.x1 += entry.anchor.x - tree.anchor.x;
                const auto part = box.intersect(bounds);
                if (part.empty()) continue;
                markTreeWood(*tree.skeleton, entry.anchor, part,
                             tree.woodAgeForDay(stamp.day), mask);
                for (int z = part.z0; z < part.z1; ++z)
                    for (int y = part.y0; y < part.y1; ++y)
                        for (int x = part.x0; x < part.x1; ++x)
                            if (mask[part.index(x, y, z)])
                                landmarkWood.set(structure_voxels::cellIndex(
                                    x - box.x0, y - box.y0, z - box.z0));
            }
        }
        bool any = false;
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int y = 0; y < Chunk::SIZE_Y; ++y)
                for (int x = 0; x < Chunk::SIZE_X; ++x) {
                    const auto& tile = chunk->tiles[z][y][x];
                    const int worldY = slab * Chunk::SIZE_Y + y;
                    const auto& column =
                        inputs->columns[ColumnEdit::index(x, z)];
                    if (worldY <= column.surface_y) continue;
                    const bool aboveWater = worldY >= column.water.surfaceY;
                    const auto cell = structure_voxels::cellIndex(x, y, z);
                    auto expected =
                        baseline && aboveWater ? baseline->tileAt(cell) : 0;
                    if (grown.test(expected)) expected = 0;
                    const bool livingWood = grown.test(tile.block_id);
                    const auto actual = livingWood ? 0 : tile.block_id;
                    const bool wound =
                        aboveWater && !livingWood && landmarkWood.test(cell) &&
                        (chunk->legacyRootTerrain ||
                         chunk->rootEdited(uint16_t(cell)) ||
                         (previous &&
                          std::binary_search(previous->begin(), previous->end(),
                                             uint16_t(cell))));
                    if (wound || (expected && actual != expected))
                        removed.push_back(uint16_t(cell));
                    if (actual == expected) continue;
                    if (actual) {
                        part.tiles[z][y][x].block_id = actual;
                        any = true;
                    }
                }
        if (!removed.empty())
            next->removed.emplace_back(uint16_t(slab), std::move(removed));
        if (any)
            next->structure.emplace_back(
                uint16_t(slab),
                std::make_shared<const Bytes>(chunk_codec::encode(part)));
    }
    next->finish();
    if (generated && next->structure.empty() && next->removed.empty())
        edits.erase(key);
    else if (!old || !sameEdit(*old, *next))
        edits.set(key, std::move(next));
    return complete;
}
