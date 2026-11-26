#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "chunk_manager.hpp"
#include "sunlight_propagator.hpp"
#include "tile_pos.hpp"

enum class EditStatus : uint8_t {
    Applied,       // the tile changed
    Unchanged,     // the request matched the current tile
    NotLoaded,     // the chunk is not resident (or outside the world)
    Occupied,      // Place: a solid block is already there
    Empty,         // Remove: there is no solid block to remove
    InvalidBlock,  // the block id is not defined
    Blocked,       // Place: a plant footprint that blocks placement is there
};

enum class EditMode : uint8_t {
    Place,    // only into air or liquid; displaces the liquid
    Remove,   // only a solid block; leaves air
    Replace,  // unconditional set (block 0 = air)
};

struct EditRequest {
    TilePos pos;
    EditMode mode = EditMode::Place;
    uint8_t block = 0;  // ignored by Remove
    bool treeGrowth = false;
    uint64_t rootOwner = 0;    // root growth claims natural substrate
    uint8_t previousWood = 0;  // material change requires this living block
    TileShape shape{};
};

struct BlockChange {
    TilePos pos;
    Tile before;
    Tile after;
    bool wasRoot = false;
    TileShape beforeShape{}, afterShape{};
    bool piecesChanged = false;
};

// The single entry point for gameplay changes to world tiles: the player,
// mobs, explosions and scripted structures all edit through it.
//
// Each command is validated against the live tile, written under the chunk
// lock, and then propagated: the chunk is marked for saving and remeshing,
// neighbor chunks whose faces depend on a border tile are remeshed, column
// tops rise for new blocks, sunlight recomputes the affected columns, and
// light volumes refresh. Bulk edits coalesce that bookkeeping, so removing
// thousands of tiles costs one notification per touched chunk and column.
//
// Ground cover is cleared on material or shape changes,
// and on the tile below a placed block. Cover-only changes are saved and
// remeshed but not reported through blockChanged.
//
// Plants (ChunkManager::flora): Place refuses tiles inside the footprint of
// a plant that blocks placement (Blocked); any block placed into another
// plant's volume removes that plant, and a removed block takes the plants
// it supported with it. Replace never refuses; it removes plants in the
// way.
//
// Main thread only, like ChunkManager's mutating API.
class WorldEditor {
   public:
    WorldEditor(ChunkManager& chunks, SunLightPropagator* sunlight,
                const DataStorage<Block>& blocks);

    EditStatus placeBlock(TilePos pos, uint8_t block, TileShape shape = {});
    EditStatus removeBlock(TilePos pos);
    EditStatus setBlock(TilePos pos, uint8_t block, TileShape shape = {});
    EditStatus apply(const EditRequest& request);
    CellPieceEdit checkPiecePlacement(TilePos pos,
                                      const SolidCellSnapshot& before,
                                      tile_geometry::TilePiece piece);
    CellPieceEdit placePiece(TilePos pos, SolidCellRevision expected,
                             tile_geometry::TilePiece piece);
    CellPieceEdit removePiece(TilePos pos, SolidCellRevision expected,
                              tile_geometry::PieceId id);
    // Environmental pieces retain their own save and publication owner.
    void setEnvironmentRemoval(
        std::function<bool(TilePos, tile_geometry::PieceId)> remove) {
        removeEnvironment = std::move(remove);
    }
    // Applies requests in order. Later requests see earlier results (placing
    // twice into one tile reports Occupied). Returns one status per request.
    std::vector<EditStatus> apply(std::span<const EditRequest> requests);

    // Emitted after commit for each applied request, in request order.
    Signal<const BlockChange&> blockChanged;

   private:
    friend class ChunkManager;
    struct Pending;
    CellPieceEdit editPiece(TilePos pos, SolidCellRevision expected,
                            tile_geometry::PieceId id,
                            const tile_geometry::TilePiece* placement);
    void recordChange(const BlockChange& change, Pending& pending);
    void publishStructureChanges(std::span<const BlockChange> changes);
    EditStatus applyOne(const EditRequest& request, Pending& pending);
    // Calls fn(coord, index, plant, volume, rule) for plants near a tile.
    template <typename F>
    void forPlantsNear(int64_t x, int y, int64_t z, F&& fn);
    void commit(Pending& pending);

    ChunkManager& chunks;
    SunLightPropagator* sunlight;
    const DataStorage<Block>& blocks;
    std::function<bool(TilePos, tile_geometry::PieceId)> removeEnvironment;
};
