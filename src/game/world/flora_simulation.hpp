#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chunk_manager.hpp"
#include "flora_generator.hpp"
#include "tile_pos.hpp"
#include "world_editor.hpp"

// Items a harvest yields (Flora "forage"). Names refer to the future items
// data type and are not resolved yet.
struct ForageYield {
    std::string item;
    int count = 0;
};

// Advances one chunk's plants to `today` (pure, docs/handoff/flora.md
// step 8): plants past their death age are removed (populations continue
// only through seeds), and harvested plants regrow once
// regrow_after_harvest_days have passed. Returns whether any plant changed.
bool advanceFlora(FloraLayer& layer, const DataStorage<Flora>& species,
                  double today);

// Main-thread plant life over game time (steps 8 and 9). Growth stages are
// analytic (FloraRenderer evaluates them from WorldClock); this resolves
// what analytic stages cannot express, lazily and within a frame budget,
// when a chunk becomes ready and in a slow sweep over loaded chunks:
//   - dead plants are removed and harvests regrow (advanceFlora);
//   - seeding: once per seeding cycle passed since the chunk was last
//     resolved, at the end of its seed stage, a plant drops seeds within
//     its spread radius. A seed germinates (a few days later) on suitable,
//     unoccupied ground with probability germination * (1 - local / target),
//     where `local` counts the species within its crowding radius and
//     `target` is its generated density there (density and cluster
//     patches). This logistic rule refills gaps left by dead plants and
//     stops at the species' density, so populations neither thin into
//     hollows nor overgrow. Failed seeds cost nothing (never stored). Seeds
//     are deterministic (parent seed and cycle), so resolving again never
//     plants twice;
//   - a chunk unloaded for CatchUpDays or longer is not replayed generation
//     by generation: its plants are re-sampled at equilibrium, as
//     generation would place them in the current epoch (CatchUpDays long),
//     with ages spread over their lives. Bounded cost after any absence;
//   - ground cover regrows on exposed topsoil (after edits) while loaded:
//     it appears after about a day and thickens to the generated density.
// Harvesting is the hook for the future items system.
class FloraSimulation {
   public:
    static constexpr int LoadsPerUpdate = 16;
    // Re-sampled chunks per update (each runs plant generation, ~ms).
    static constexpr int RegenerationsPerUpdate = 2;
    static constexpr double CatchUpDays = ChunkManager::FloraEpochDays;
    // Seeding cycles replayed per plant and resolve (shorter absences).
    static constexpr int MaxCyclesPerResolve = 16;
    // Neighbourhood counted for crowding: at least CrowdRadius metres, and
    // wide enough to hold CrowdPlants of the species at full density.
    static constexpr double CrowdRadius = 2.5;
    static constexpr double CrowdPlants = 3;
    static constexpr int SweepsPerUpdate = 4;
    static constexpr int CoverChunksPerUpdate = 4;
    // Cover regrowth: days until it appears, then density levels per day.
    static constexpr double CoverAppearDays = 1;
    static constexpr double CoverLevelsPerDay = 2;

    FloraSimulation(ChunkManager& chunks, const IAssets& assets,
                    const World& world);
    ~FloraSimulation();
    FloraSimulation(const FloraSimulation&) = delete;
    FloraSimulation& operator=(const FloraSimulation&) = delete;

    // Tiles changed by the editor may expose topsoil for cover regrowth.
    void watch(WorldEditor& editor);
    void update(double today);

    // Harvests the plants on (standing on, or growing into) a tile:
    // plants in their forage stage yield items; plants that regrow after
    // harvest are marked Harvested, others are removed.
    std::vector<ForageYield> harvest(TilePos tile, double today);

    struct PlantInfo {
        const Flora* species;
        FloraInstance plant;
        FloraStageState state;
    };
    // Plants occupying or standing on a tile (debug UI).
    std::vector<PlantInfo> plantsAt(TilePos tile, double today) const;

    size_t pendingLoads() const { return loaded.size(); }
    size_t coverChunks() const { return coverPending.size(); }

   private:
    // Resolves a chunk: deaths, regrowth and seeding (or re-samples it after
    // a long absence).
    void resolve(const ChunkCoord& coord, double today);
    void spread(const ChunkCoord& coord, const FloraLayer& layer, double since,
                double today,
                std::unordered_map<ChunkCoord, FloraLayer>& changed);
    // Adds an offspring if the ground at world (x, z) near anchor height
    // suits it. Returns whether it was added.
    // germination: the seed's chance on empty ground, before crowding;
    // roll: its deterministic draw in [0, 1).
    bool plant(const FloraSpeciesRule& rule, double x, double z, int nearY,
               int32_t birthDay, uint16_t seed, double roll, double germination,
               std::unordered_map<ChunkCoord, FloraLayer>& changed);
    // Same-species plants (and dormant seeds) within radius of (x, z).
    int crowd(const FloraSpeciesRule& rule, double x, double z, int y,
              double radius,
              const std::unordered_map<ChunkCoord, FloraLayer>& changed) const;
    // Re-samples a long-unloaded chunk at equilibrium.
    void regenerate(const ChunkCoord& coord, double today);
    bool regrowCover(const ChunkCoord& coord, double today);
    // Large-plant footprints over a tile (from loaded layers).
    bool inFootprint(
        int64_t x, int y, int64_t z,
        const std::unordered_map<ChunkCoord, FloraLayer>& changed) const;
    const FloraLayer* layerOf(
        const ChunkCoord& coord,
        const std::unordered_map<ChunkCoord, FloraLayer>& changed) const;

    ChunkManager& chunks;
    const IAssets& assets;
    const World& world;
    WorldEditor* editor = nullptr;
    std::vector<ChunkCoord> loaded;  // became ready, not yet resolved
    int regenerations = 0;           // this update
    std::vector<ChunkCoord> sweep;   // round robin over chunks with plants
    size_t sweepAt = 0;
    std::unordered_set<ChunkCoord> coverPending;
    std::unordered_map<ChunkCoord, double> coverSince;
    Signal<const ChunkCoord&>::Connection readyConnection, removedConnection;
    Signal<const BlockChange&>::Connection editConnection;
    double lastToday = 0;
};
