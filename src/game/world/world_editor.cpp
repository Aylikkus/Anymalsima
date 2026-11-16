#include "world_editor.hpp"

#include <unordered_set>

struct WorldEditor::Pending {
    std::unordered_set<ChunkCoord> modified;
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

EditStatus WorldEditor::applyOne(const EditRequest& request, Pending& pending) {
    // X wraps: a position just past the seam edits the canonical tile.
    auto pos = request.pos;
    pos.x = decltype(pos.x)(chunks.wrap().wrapTile(pos.x));
    const auto [x, y, z] = pos;
    if (x < 0 || y < 0 || z < 0) return EditStatus::NotLoaded;
    const bool placing = request.mode != EditMode::Remove && request.block;
    if (placing && request.block > blocks.items.size())
        return EditStatus::InvalidBlock;

    const ChunkCoord coord{unsigned(x / Chunk::SIZE_X),
                           unsigned(y / Chunk::SIZE_Y),
                           unsigned(z / Chunk::SIZE_Z)};
    const int lx = x % Chunk::SIZE_X, ly = y % Chunk::SIZE_Y,
              lz = z % Chunk::SIZE_Z;
    auto status = EditStatus::NotLoaded;
    BlockChange change{pos, {}, {}};
    chunks.withChunkLocked(coord, [&](Chunk& chunk) {
        Tile& tile = chunk.tiles[lz][ly][lx];
        change.before = tile;
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
        if (tile.block_id == block &&
            (block == 0 || tile.liquid == LiquidType::None)) {
            status = EditStatus::Unchanged;
            return;
        }
        tile.block_id = block;
        if (block) {
            // A block displaces any liquid in its cell.
            tile.liquid = LiquidType::None;
            tile.liquidLevel = 0;
        }
        // Light stays until sunlight recomputes, so edits never flash dark.
        change.after = tile;
        status = EditStatus::Applied;
    });
    if (status != EditStatus::Applied) return status;

    pending.modified.insert(coord);
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
    if (change.after.block_id) chunks.raiseColumnTopY(coord.x, coord.z, y);
    pending.columns.insert((uint64_t{coord.x} << 32) | coord.z);
    pending.changes.push_back(change);
    return status;
}

void WorldEditor::commit(Pending& pending) {
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
