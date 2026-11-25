#include "chunk.hpp"

const Chunk::PieceEntry* Chunk::pieceEntry(uint16_t index) const {
    if (!pieces) return nullptr;
    const auto it =
        std::ranges::lower_bound(*pieces, index, {}, &PieceEntry::cell);
    return it != pieces->end() && it->cell == index ? &*it : nullptr;
}

size_t Chunk::compoundCount() const {
    return pieces ? std::ranges::count_if(*pieces,
                                          [](const PieceEntry& entry) {
                                              return bool(entry.compound);
                                          })
                  : 0;
}

size_t Chunk::pieceStorageBytes() const {
    if (!pieces) return 0;
    size_t bytes = sizeof(PieceTable) + pieces->capacity() * sizeof(PieceEntry);
    for (const auto& entry : *pieces)
        if (entry.compound)
            bytes += sizeof(PieceList) +
                     entry.compound->capacity() * sizeof(CellPiece);
    return bytes;
}

SolidCellSnapshot Chunk::solidCellAt(uint16_t index,
                                     PieceProvenance fallback) const {
    const auto tile = tileAt(index);
    const auto shape = shapeAt(index);
    if (!tile.isSolid()) return {{tile, shape}};
    if (const auto* entry = pieceEntry(index)) {
        if (entry->compound) return {{tile, shape}, {}, entry->compound};
        return {{tile, shape},
                CellPiece{{entry->id, tile.block_id, shape}, entry->origin}};
    }
    return {{tile, shape},
            CellPiece{{0, tile.block_id, shape},
                      implicitPieceOrigin(index, fallback)}};
}

PieceProvenance Chunk::implicitPieceOrigin(uint16_t index,
                                           PieceProvenance fallback) const {
    if (const auto* root = rootAt(index))
        return {PieceOrigin::Structure, root->owner};
    if (rootEdited(index)) return {PieceOrigin::Player};
    return fallback;
}

CellPieceEdit Chunk::setPieces(uint16_t index,
                               std::span<const CellPiece> replacement,
                               PieceProvenance fallback) {
    if (index >= CellCount) return CellPieceEdit::Invalid;
    if (replacement.size() > MaxPiecesPerCell) return CellPieceEdit::TooMany;
    if (!replacement.empty() && tileAt(index).isLiquid() &&
        tileAt(index).waterOrigin == WaterOrigin::Local)
        return CellPieceEdit::Invalid;
    tile_geometry::CellGeometry geometry;
    for (const auto& piece : replacement) {
        if (piece.origin.kind > PieceOrigin::Environment)
            return CellPieceEdit::Invalid;
        switch (geometry.tryAdd(piece.geometry)) {
            case tile_geometry::PiecePlacement::Added:
                break;
            case tile_geometry::PiecePlacement::Overlap:
                return CellPieceEdit::Overlap;
            default:
                return CellPieceEdit::Invalid;
        }
    }

    const auto before = solidCellAt(index);
    const auto previous = before.pieces();
    Tile tile = before.base.tile;
    TileShape shape;
    PieceEntry entry{.cell = index};
    if (replacement.empty()) {
        tile.block_id = 0;
        tile.clearCover();
    } else {
        const auto& first = *std::ranges::min_element(
            replacement, {},
            [](const auto& piece) { return piece.geometry.id; });
        const auto legacy = first.geometry.legacyShape();
        if (replacement.size() == 1 && legacy) {
            shape = *legacy;
            entry.id = first.geometry.id;
            entry.origin = first.origin;
        } else {
            auto sorted = std::make_shared<PieceList>(replacement.begin(),
                                                      replacement.end());
            std::ranges::sort(*sorted, {}, [](const auto& piece) {
                return piece.geometry.id;
            });
            entry.compound = std::move(sorted);
            shape = legacy.value_or(TileShape{});
        }
        if (previous.empty() ||
            previous.front().geometry.id != first.geometry.id ||
            tile.block_id != first.geometry.blockId)
            tile.clearCover();
        tile.block_id = first.geometry.blockId;
        tile.liquid = LiquidType::None;
        tile.liquidLevel = 0;
        tile.waterOrigin = WaterOrigin::Independent;
    }
    const auto implicit = implicitPieceOrigin(index, fallback);
    const bool store =
        entry.compound || entry.id != 0 || entry.origin != implicit;
    auto update = preparePieceUpdate(index, !replacement.empty() && store);
    updateShape(index, shape);
    tileAt(index) = tile;
    commitPieceUpdate(std::move(update), index,
                      !replacement.empty() && store ? &entry : nullptr);
    return CellPieceEdit::Applied;
}

// Prepare the copy and capacity before changing either side of the cell.
std::shared_ptr<Chunk::PieceTable> Chunk::preparePieceUpdate(uint16_t index,
                                                             bool store) const {
    const bool existed = pieceEntry(index) != nullptr;
    if (!store && !existed) return {};
    auto updated = !pieces ? std::make_shared<PieceTable>()
                   : pieces.unique()
                       ? std::const_pointer_cast<PieceTable>(pieces)
                       : std::make_shared<PieceTable>(*pieces);
    if (store && !existed && updated->size() == updated->capacity())
        updated->reserve(
            std::clamp(updated->size() * 2, size_t{1}, size_t{CellCount}));
    return updated;
}

void Chunk::commitPieceUpdate(std::shared_ptr<PieceTable> updated,
                              uint16_t index, PieceEntry* replacement) {
    if (!updated) return;
    const auto it =
        std::ranges::lower_bound(*updated, index, {}, &PieceEntry::cell);
    const bool found = it != updated->end() && it->cell == index;
    if (replacement) {
        if (found)
            *it = std::move(*replacement);
        else
            updated->insert(it, std::move(*replacement));
    } else if (found) {
        updated->erase(it);
    }
    pieces = updated->empty() ? nullptr : std::move(updated);
}
