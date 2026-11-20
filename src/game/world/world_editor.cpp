#include "world_editor.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

struct WorldEditor::Pending {
    std::unordered_set<ChunkCoord> modified;
    // Plants to remove, by chunk: indices into that chunk's layer.
    std::unordered_map<ChunkCoord, std::vector<size_t>> plants;
    std::unordered_set<ChunkCoord> remesh;
    std::unordered_set<uint64_t> columns;
    std::vector<BlockChange> changes;
};

WorldEditor::WorldEditor(ChunkManager& chunks, SunLightPropagator* sunlight,
                         const DataStorage<Block>& blocks)
    : chunks(chunks), sunlight(sunlight), blocks(blocks) {}

EditStatus WorldEditor::placeBlock(TilePos pos, uint8_t block) {
    return apply(EditRequest{pos, EditMode::Place, block});
}
EditStatus WorldEditor::removeBlock(TilePos pos) {
    return apply(EditRequest{pos, EditMode::Remove, 0});
}
EditStatus WorldEditor::setBlock(TilePos pos, uint8_t block) {
    return apply(EditRequest{pos, EditMode::Replace, block});
}

EditStatus WorldEditor::apply(const EditRequest& request) {
    Pending pending;
    const auto status = applyOne(request, pending);
    commit(pending);
    return status;
}

std::vector<EditStatus> WorldEditor::apply(
    std::span<const EditRequest> requests) {
    Pending pending;
    std::vector<EditStatus> result;
    result.reserve(requests.size());
    for (const auto& request : requests)
        result.push_back(applyOne(request, pending));
    commit(pending);
    return result;
}

template <typename F>
void WorldEditor::forPlantsNear(int64_t x, int y, int64_t z, F&& fn) {
    const auto& table = chunks.floraSpecies();
    if (table.empty()) return;
    const auto wrap = chunks.wrap();
    // Footprints reach one chunk sideways at most; plants stand on anchors
    // up to the tallest species below the tile.
    const int64_t cx = x / Chunk::SIZE_X, cz = z / Chunk::SIZE_Z;
    const int lowY = std::max(0, (y - table.maxHeight() - 1) / Chunk::SIZE_Y);
    for (int64_t dz = -1; dz <= 1; ++dz)
        for (int64_t dx = -1; dx <= 1; ++dx) {
            const int64_t nx = wrap.wrapChunk(cx + dx), nz = cz + dz;
            if (nx < 0 || nz < 0) continue;
            for (int ny = lowY; ny <= y / Chunk::SIZE_Y; ++ny) {
                const ChunkCoord coord{unsigned(nx), unsigned(ny),
                                       unsigned(nz)};
                const auto layer = chunks.flora(coord);
                if (!layer) continue;
                for (size_t i = 0; i < layer->instances.size(); ++i) {
                    const auto& p = layer->instances[i];
                    const auto* rule = table.find(p.species);
                    if (!rule) continue;
                    auto volume = floraVolume(p, coord, *rule);
                    // Compare in the edited tile's frame across the X seam.
                    volume.minX = x + wrap.deltaTile(volume.minX, x);
                    fn(coord, i, volume, *rule);
                }
            }
        }
}

EditStatus WorldEditor::applyOne(const EditRequest& request, Pending& pending) {
    // X wraps: a position just past the seam edits the canonical tile.
    auto pos = request.pos;
    pos.x = decltype(pos.x)(chunks.wrap().wrapTile(pos.x));
    const auto [x, y, z] = pos;
    if (x < 0 || y < 0 || z < 0) return EditStatus::NotLoaded;
    const bool placing = request.mode != EditMode::Remove && request.block;
    if (placing && request.block > blocks.items.size())
        return EditStatus::InvalidBlock;
    if (placing && request.mode == EditMode::Place) {
        bool blocked = false;
        forPlantsNear(x, y, z,
                      [&](const ChunkCoord&, size_t, const FloraVolume& v,
                          const FloraSpeciesRule& r) {
                          blocked = blocked ||
                                    (r.blocksPlacement && v.contains(x, y, z));
                      });
        if (blocked) return EditStatus::Blocked;
    }

    const ChunkCoord coord{unsigned(x / Chunk::SIZE_X),
                           unsigned(y / Chunk::SIZE_Y),
                           unsigned(z / Chunk::SIZE_Z)};
    const int lx = x % Chunk::SIZE_X, ly = y % Chunk::SIZE_Y,
              lz = z % Chunk::SIZE_Z;
    auto status = EditStatus::NotLoaded;
    BlockChange change{pos, {}, {}};
    chunks.withChunkLocked(coord, [&](Chunk& chunk) {
        Tile& tile = chunk.tiles[lz][ly][lx];
        const auto cell = Chunk::cell(lx, ly, lz);
        change.before = tile;
        change.wasRoot = chunk.rootAt(cell) != nullptr;
        switch (request.mode) {
            case EditMode::Place:
                if (tile.isSolid()) {
                    status = EditStatus::Occupied;
                    return;
                }
                break;
            case EditMode::Remove:
                if (!tile.isSolid()) {
                    status = EditStatus::Empty;
                    return;
                }
                break;
            case EditMode::Replace:
                break;
        }
        const uint8_t block =
            request.mode == EditMode::Remove ? 0 : request.block;
        if (request.previousWood &&
            (!request.treeGrowth || tile.block_id != request.previousWood ||
             !chunks.grownBlocks().test(tile.block_id) ||
             !chunks.grownBlocks().test(block) || chunk.rootEdited(cell))) {
            status = EditStatus::Blocked;
            return;
        }
        if (request.previousWood && request.rootOwner) {
            const auto* root = chunk.rootAt(cell);
            if (!root || root->owner != request.rootOwner) {
                status = EditStatus::Blocked;
                return;
            }
        }
        if (request.rootOwner && !request.previousWood &&
            (chunk.legacyRootTerrain || chunk.rootEdited(cell) ||
             !tile.isSolid() || tile.isLiquid() ||
             chunks.grownBlocks().test(tile.block_id))) {
            status = EditStatus::Blocked;
            return;
        }
        if (request.treeGrowth && change.wasRoot &&
            !(request.rootOwner && request.previousWood)) {
            status = EditStatus::Blocked;
            return;
        }
        if (tile.block_id == block &&
            (block == 0 || tile.liquid == LiquidType::None)) {
            if (!request.treeGrowth && tile.isSolid() && !change.wasRoot &&
                chunk.markRootEdited(cell))
                pending.modified.insert(coord);
            status = EditStatus::Unchanged;
            return;
        }
        if (block && tile.waterOrigin == WaterOrigin::Local) {
            status = EditStatus::Blocked;
            return;
        }
        if (request.rootOwner && !request.previousWood)
            chunk.recordRoot(cell, request.rootOwner, tile);
        else if (!request.treeGrowth)
            chunk.markRootEdited(cell);
        tile.block_id = block;
        // A new or removed block never keeps the old block's ground cover.
        tile.clearCover();
        if (block) {
            // A block displaces any liquid in its cell.
            tile.liquid = LiquidType::None;
            tile.liquidLevel = 0;
            tile.waterOrigin = WaterOrigin::Independent;
        }
        // Light stays until sunlight recomputes, so edits never flash dark.
        change.after = tile;
        status = EditStatus::Applied;
    });
    if (status != EditStatus::Applied) return status;

    pending.modified.insert(coord);
    // Plants in the way of a new block, or standing on a removed one.
    forPlantsNear(x, y, z,
                  [&](const ChunkCoord& c, size_t i, const FloraVolume& v,
                      const FloraSpeciesRule&) {
                      if (change.after.block_id ? v.contains(x, y, z)
                                                : v.supportedBy(x, y, z))
                          pending.plants[c].push_back(i);
                  });
    // Face visibility of the six neighbors depends on this tile.
    auto neighbor = [&](int local, int size, int axis) {
        int delta = local == 0 ? -1 : (local == size - 1 ? 1 : 0);
        if (!delta) return;
        int64_t c[3] = {coord.x, coord.y, coord.z};
        c[axis] += delta;
        if (axis == 0) c[0] = chunks.wrap().wrapChunk(c[0]);
        if (c[axis] < 0) return;
        pending.remesh.insert({unsigned(c[0]), unsigned(c[1]), unsigned(c[2])});
    };
    neighbor(lx, Chunk::SIZE_X, 0);
    neighbor(ly, Chunk::SIZE_Y, 1);
    neighbor(lz, Chunk::SIZE_Z, 2);
    if (change.after.block_id) {
        chunks.raiseColumnTopY(coord.x, coord.z, y);
        // Cover needs open sky above it: a block placed on a covered tile
        // smothers that cover (regrowth comes with flora growth).
        if (y > 0) {
            const ChunkCoord below{coord.x, unsigned((y - 1) / Chunk::SIZE_Y),
                                   coord.z};
            bool cleared = false;
            chunks.withChunkLocked(below, [&](Chunk& chunk) {
                Tile& tile = chunk.tiles[lz][(y - 1) % Chunk::SIZE_Y][lx];
                if (!tile.cover) return;
                tile.clearCover();
                cleared = true;
            });
            if (cleared) pending.modified.insert(below);
        }
    }
    pending.columns.insert((uint64_t{coord.x} << 32) | coord.z);
    pending.changes.push_back(change);
    return status;
}

void WorldEditor::commit(Pending& pending) {
    for (auto& [coord, indices] : pending.plants) {
        const auto layer = chunks.flora(coord);
        if (!layer) continue;
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()),
                      indices.end());
        // Copy the layer so its header (resolvedDay) survives: a reset day
        // would make FloraSimulation re-sample or re-seed the whole chunk.
        auto kept = std::make_shared<FloraLayer>(*layer);
        kept->instances.clear();
        for (size_t i = 0; i < layer->instances.size(); ++i)
            if (!std::binary_search(indices.begin(), indices.end(), i))
                kept->instances.push_back(layer->instances[i]);
        chunks.setFlora(coord, std::move(kept));
    }
    // Saved, remeshed urgently, and light volumes refreshed.
    for (const auto& coord : pending.modified) chunks.markModified(coord);
    for (const auto& coord : pending.remesh)
        if (!pending.modified.contains(coord)) chunks.markDirty(coord, true);
    // Opacity changed: recompute direct sunlight down each column; indirect
    // light around its chunks follows from the propagator's invalidation.
    if (sunlight)
        for (const auto column : pending.columns)
            sunlight->markColumnDirty(unsigned(column >> 32), unsigned(column));
    for (const auto& change : pending.changes) blockChanged.emit(change);
}
