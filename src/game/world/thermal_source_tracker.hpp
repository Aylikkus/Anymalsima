#pragma once

#include <array>
#include <unordered_set>

#include "chunk_manager.hpp"
#include "items/block.hpp"
#include "thermal_sources.hpp"

struct BlockChange;

// Keeps the registry's block-owned thermal sources in step with loaded
// chunks. It scans a chunk when it becomes ready and reconciles one tile on a
// normal gameplay edit, so a heater survives streaming and a removed heater
// leaves no residual source. Item sources are owned by their own lifecycle.
class ThermalSourceTracker {
   public:
    ThermalSourceTracker(ChunkManager& chunks, const DataStorage<Block>& blocks,
                         ThermalSourceRegistry& registry);
    ~ThermalSourceTracker();

    void watch(WorldEditor& editor);

   private:
    void ready(const ChunkCoord& coord);
    void removed(const ChunkCoord& coord);
    void changed(const BlockChange& change);
    void scanChunk(const ChunkCoord& coord);

    ChunkManager& chunks;
    ThermalSourceRegistry& registry;
    std::array<const ThermalEmission*, 256> emissions{};
    Signal<const ChunkCoord&>::Connection readyConnection;
    Signal<const ChunkCoord&>::Connection removedConnection;
    Signal<const BlockChange&>::Connection editorConnection;
    WorldEditor* editor = nullptr;
};
