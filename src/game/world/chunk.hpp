#pragma once

#include <algorithm>
#include <cassert>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "chunk_coord.hpp"
#include "tile.hpp"
#include "tile_piece.hpp"

struct RootVoxel {
    uint64_t owner;
    uint8_t block, cover, coverState;  // displaced natural substrate
    TileShape shape{};
};

struct RootTerrain {
    std::map<uint16_t, RootVoxel> roots;
    std::set<uint16_t> edited;
};

enum class PieceOrigin : uint8_t { Terrain, Structure, Player, Environment };

struct PieceProvenance {
    PieceOrigin kind = PieceOrigin::Terrain;
    uint64_t owner = 0;  // source identifier when known
    bool operator==(const PieceProvenance&) const = default;
};

struct CellPiece {
    tile_geometry::TilePiece geometry;
    PieceProvenance origin;
    bool operator==(const CellPiece&) const = default;
};

// Environmental solids projected over a cell (deposited snow and, later,
// ice). They are owned by their simulation, never by the chunk save, and are
// combined with the chunk's own pieces for queries and rendering.
using EnvironmentCell = std::shared_ptr<const tile_geometry::CellGeometry>;
using ChunkEnvironment = std::map<uint16_t, EnvironmentCell>;
using EnvironmentMap =
    std::unordered_map<ChunkCoord, std::shared_ptr<const ChunkEnvironment>>;

// Owns the solid revision even after the live chunk changes or unloads.
struct SolidCellSnapshot {
    ShapedTile base;
    std::optional<CellPiece> simple{};
    std::shared_ptr<const std::vector<CellPiece>> compound{};
    EnvironmentCell environment{};

    std::span<const CellPiece> pieces() const& {
        if (compound) return *compound;
        if (simple) return {&*simple, 1};
        return {};
    }
    std::span<const CellPiece> pieces() const&& = delete;

    bool hasEnvironment() const { return bool(environment); }
    bool hasSolid() const { return base.tile.isSolid() || environment; }
    bool usesGeometry() const {
        return bool(compound) || bool(environment) || base.shape != TileShape{};
    }

    tile_geometry::CellGeometry geometry() const {
        tile_geometry::CellGeometry result;
        for (const auto& piece : pieces()) result.tryAdd(piece.geometry);
        if (environment) result.append(*environment);
        return result;
    }
};

enum class CellPieceEdit {
    Applied,
    Invalid,
    TooMany,
    Overlap,
    Stale,
    NotLoaded,
    Blocked
};

struct Chunk {
    static constexpr int SIZE_X = 16;
    static constexpr int SIZE_Y = 16;
    static constexpr int SIZE_Z = 16;
    static constexpr int CellCount = SIZE_X * SIZE_Y * SIZE_Z;
    static constexpr size_t MaxPiecesPerCell = 32;
    static constexpr size_t MaxPieceRemovals = CellCount * MaxPiecesPerCell;
    using PieceRemoval = std::tuple<uint16_t, uint64_t, tile_geometry::PieceId>;
    using PieceRemovals = std::set<PieceRemoval>;

    Tile tiles[SIZE_Z][SIZE_Y][SIZE_X];  // [z][y][x]
    // v1–v4 slabs keep ambiguous water and dry edits as saved.
    bool projectsWater = false;
    // Legacy slabs also preserve uncertainty about terrain replacements.
    bool legacyRootTerrain = false;
    bool legacyRootShape = false;
    std::shared_ptr<const RootTerrain> rootTerrain;

    static constexpr uint16_t cell(int x, int y, int z) {
        return uint16_t((z * SIZE_Y + y) * SIZE_X + x);
    }
    bool hasShapes() const { return bool(shapes); }
    size_t shapeCount() const { return shapes ? shapes->cells.size() : 0; }
    // Resident sparse-table bytes including spare capacity, for profiling.
    size_t shapeStorageBytes() const {
        return shapes ? sizeof(ShapeTable) +
                            shapes->cells.capacity() * sizeof(uint16_t) +
                            shapes->codes.capacity() * sizeof(TileShape)
                      : 0;
    }
    TileShape shapeAt(uint16_t index) const {
        if (!shapes) return {};
        const auto it =
            std::lower_bound(shapes->cells.begin(), shapes->cells.end(), index);
        return it != shapes->cells.end() && *it == index
                   ? shapes->codes[it - shapes->cells.begin()]
                   : TileShape{};
    }
    template <typename F>
    void forEachShape(F&& visit) const {
        if (!shapes) return;
        for (size_t i = 0; i < shapes->cells.size(); ++i)
            visit(shapes->cells[i], shapes->codes[i]);
    }
    ShapedTile cellAt(int x, int y, int z) const {
        return {tiles[z][y][x], shapeAt(cell(x, y, z))};
    }
    bool hasCompoundAt(uint16_t index) const {
        if (!pieces) return false;
        const auto* entry = pieceEntry(index);
        return entry && entry->compound;
    }
    bool hasCompounds() const { return pieces && compoundCount() != 0; }
    const PieceRemovals& removedStructurePieces() const {
        static const PieceRemovals empty;
        return pieceRemovals ? *pieceRemovals : empty;
    }
    void addPieceRemovals(std::span<const PieceRemoval> removals);
    std::vector<PieceRemoval> structurePieceRemovals(
        uint16_t cell, const SolidCellSnapshot& generated,
        std::span<const CellPiece> retained) const;
    size_t pieceCellCount() const { return pieces ? pieces->size() : 0; }
    bool hasPieceEntry(uint16_t index) const { return pieceEntry(index); }
    template <typename F>
    void forEachPieceCell(F&& visit) const {
        if (pieces)
            for (const auto& entry : *pieces)
                visit(entry.cell, solidCellAt(entry.cell));
    }
    size_t compoundCount() const;
    size_t pieceStorageBytes() const;
    SolidCellSnapshot solidCellAt(uint16_t index,
                                  PieceProvenance fallback = {}) const;
    // Replaces the cell atomically. Pass the retained pieces to remove one
    // piece without changing the others' IDs or provenance.
    CellPieceEdit setPieces(uint16_t index,
                            std::span<const CellPiece> replacement,
                            PieceProvenance fallback = {},
                            bool preserveEntry = false);
    // For live chunks, call under the same lock used for tile edits.
    // A whole-cell replacement discards any explicit pieces.
    void setCell(uint16_t index, Tile tile, TileShape shape = {}) {
        assert(index < CellCount);
        auto update = pieces ? preparePieceUpdate(index, false) : nullptr;
        updateShape(index, tile.isSolid() ? shape : TileShape{});
        tileAt(index) = tile;
        if (update) commitPieceUpdate(std::move(update), index, nullptr);
    }
    // Bulk generation replaces the entire slab with cube-only terrain.
    void clearSolidStorage() {
        shapes.reset();
        pieces.reset();
        pieceRemovals.reset();
    }
    const RootVoxel* rootAt(uint16_t cell) const {
        if (!rootTerrain) return nullptr;
        const auto it = rootTerrain->roots.find(cell);
        return it == rootTerrain->roots.end() ? nullptr : &it->second;
    }
    bool rootEdited(uint16_t cell) const {
        return rootTerrain && rootTerrain->edited.contains(cell);
    }
    void recordRoot(uint16_t cell, uint64_t owner, const Tile& tile) {
        changeRootTerrain().roots.emplace(
            cell, RootVoxel{owner, tile.block_id, tile.cover, tile.coverState,
                            shapeAt(cell)});
    }
    bool markRootEdited(uint16_t cell) {
        if (legacyRootTerrain || rootEdited(cell)) return false;
        auto& data = changeRootTerrain();
        data.roots.erase(cell);
        data.edited.insert(cell);
        return true;
    }
    void restoreRootSubstrate(const GrownBlockSet& grown) {
        if (rootTerrain && !rootTerrain->roots.empty()) {
            auto& data = changeRootTerrain();
            for (const auto& [cell, root] : data.roots) {
                auto tile = tiles[cell / (SIZE_X * SIZE_Y)]
                                 [(cell / SIZE_X) % SIZE_Y][cell % SIZE_X];
                if (!grown.test(tile.block_id)) continue;
                tile.block_id = root.block;
                tile.cover = root.cover;
                tile.coverState = root.coverState;
                setCell(cell, tile, root.shape);
            }
            data.roots.clear();
        }
        legacyRootShape = false;
    }

    Tile& tileAt(uint16_t index) {
        assert(index < CellCount);
        return tiles[index / (SIZE_X * SIZE_Y)][(index / SIZE_X) % SIZE_Y]
                    [index % SIZE_X];
    }
    const Tile& tileAt(uint16_t index) const {
        assert(index < CellCount);
        return tiles[index / (SIZE_X * SIZE_Y)][(index / SIZE_X) % SIZE_Y]
                    [index % SIZE_X];
    }

   private:
    using PieceList = std::vector<CellPiece>;
    struct PieceEntry {
        uint16_t cell;
        tile_geometry::PieceId id = 0;
        PieceProvenance origin{};
        std::shared_ptr<const PieceList> compound{};
    };
    using PieceTable = std::vector<PieceEntry>;
    std::shared_ptr<const PieceTable> pieces;
    std::shared_ptr<const PieceRemovals> pieceRemovals;

    const PieceEntry* pieceEntry(uint16_t index) const;
    PieceProvenance implicitPieceOrigin(uint16_t index,
                                        PieceProvenance fallback) const;
    std::shared_ptr<PieceTable> preparePieceUpdate(uint16_t index,
                                                   bool store) const;
    void commitPieceUpdate(std::shared_ptr<PieceTable> updated, uint16_t index,
                           PieceEntry* replacement);

    struct ShapeTable {
        std::vector<uint16_t> cells;
        std::vector<TileShape> codes;
    };
    std::shared_ptr<const ShapeTable> shapes;

    void updateShape(uint16_t index, TileShape shape) {
        const bool cube = shape.code() == 0;
        if (!shapes && cube) return;

        size_t offset = 0;
        bool found = false;
        if (shapes) {
            const auto it = std::lower_bound(shapes->cells.begin(),
                                             shapes->cells.end(), index);
            offset = it - shapes->cells.begin();
            found = it != shapes->cells.end() && *it == index;
            if ((found && shapes->codes[offset] == shape) || (!found && cube))
                return;
            if (cube && shapes->cells.size() == 1) {
                shapes.reset();
                return;
            }
        }

        auto updated = !shapes ? std::make_shared<ShapeTable>()
                       : shapes.unique()
                           ? std::const_pointer_cast<ShapeTable>(shapes)
                           : std::make_shared<ShapeTable>(*shapes);
        if (cube) {
            updated->cells.erase(updated->cells.begin() + offset);
            updated->codes.erase(updated->codes.begin() + offset);
        } else if (found) {
            updated->codes[offset] = shape;
        } else {
            // Reserve both buffers before inserting to keep them in sync if
            // an allocation fails. Grow geometrically for bulk insertion.
            const size_t size = updated->cells.size();
            if (updated->cells.capacity() == size)
                updated->cells.reserve(std::max(size_t{1}, size * 2));
            if (updated->codes.capacity() == size)
                updated->codes.reserve(std::max(size_t{1}, size * 2));
            updated->cells.insert(updated->cells.begin() + offset, index);
            updated->codes.insert(updated->codes.begin() + offset, shape);
        }
        shapes = std::move(updated);
    }

    RootTerrain& changeRootTerrain() {
        if (!rootTerrain) rootTerrain = std::make_shared<RootTerrain>();
        if (!rootTerrain.unique())
            rootTerrain = std::make_shared<RootTerrain>(*rootTerrain);
        return *std::const_pointer_cast<RootTerrain>(rootTerrain);
    }
};
