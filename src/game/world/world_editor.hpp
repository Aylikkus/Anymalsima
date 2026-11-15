#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chunk_manager.hpp"
#include "sunlight_propagator.hpp"

// World tile coordinates: x, y = height, z.
struct TilePos {
    int x = 0, y = 0, z = 0;
    bool operator==(const TilePos&) const = default;
};

enum class EditStatus : uint8_t {
    Applied,       // the tile changed
    Unchanged,     // the request matched the current tile
    NotLoaded,     // the chunk is not resident (or outside the world)
    Occupied,      // Place: a solid block is already there
    Empty,         // Remove: there is no solid block to remove
    InvalidBlock,  // the block id is not defined
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
};

struct BlockChange {
    TilePos pos;
    Tile before;
    Tile after;
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
// Main thread only, like ChunkManager's mutating API.
class WorldEditor {
   public:
    WorldEditor(ChunkManager& chunks, SunLightPropagator* sunlight,
                const DataStorage<Block>& blocks);

    EditStatus placeBlock(TilePos pos, uint8_t block);
    EditStatus removeBlock(TilePos pos);
    EditStatus setBlock(TilePos pos, uint8_t block);
    EditStatus apply(const EditRequest& request);
    // Applies requests in order. Later requests see earlier results (placing
    // twice into one tile reports Occupied). Returns one status per request.
    std::vector<EditStatus> apply(std::span<const EditRequest> requests);

    // Emitted after a batch is committed, once per changed tile.
    Signal<const BlockChange&> blockChanged;

   private:
    struct Pending;
    EditStatus applyOne(const EditRequest& request, Pending& pending);
    void commit(Pending& pending);

    ChunkManager& chunks;
    SunLightPropagator* sunlight;
    const DataStorage<Block>& blocks;
};
