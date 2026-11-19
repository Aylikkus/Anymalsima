#pragma once

#include <unordered_set>

#include "chunk_manager.hpp"
#include "terrain_edits.hpp"
#include "world_editor.hpp"

// Keeps TerrainEdits in sync with the voxel world, one chunk column at a
// time. Sources:
// - WorldEditor::blockChanged: every gameplay edit (player, NPCs, scripts);
// - ChunkManager::becameReady: a chunk read from a save that differs from
//   generated terrain (edits made before tracking existed, or not yet in
//   terrain_edits.dat after a crash), and re-verification of columns that
//   already have an entry.
//
// Main thread only. Call update() each frame before regions can unload, so
// edited chunks are still resident when their column is recomputed.
class TerrainEditTracker {
   public:
    // detectSavedEdits: compare chunks read from saves with generated terrain.
    // Only valid when the save was made by the current generator.
    TerrainEditTracker(ChunkManager& chunks, TerrainEdits& edits,
                       bool detectSavedEdits = true);
    ~TerrainEditTracker();
    TerrainEditTracker(const TerrainEditTracker&) = delete;
    TerrainEditTracker& operator=(const TerrainEditTracker&) = delete;

    void watch(WorldEditor& editor);
    void markDirty(unsigned chunkX, unsigned chunkZ);
    // Recomputes every dirty column from the resident chunks.
    void update();
    size_t pending() const { return dirty.size(); }

   private:
    // Returns false when some slab was not Ready and no earlier data exists
    // for it; the column is retried when more of it becomes ready.
    bool refresh(uint64_t key);

    ChunkManager& chunks;
    TerrainEdits& edits;
    bool detect;
    WorldEditor* editor = nullptr;
    Signal<const BlockChange&>::Connection editConnection;
    Signal<const ChunkCoord&>::Connection readyConnection, rebaseConnection;
    std::unordered_set<uint64_t> dirty;
    // Columns checked this session; a later chunk arrival needs no recheck.
    std::unordered_set<uint64_t> verified;
    // Columns computed while part of them was not loaded.
    std::unordered_set<uint64_t> incomplete;
};
