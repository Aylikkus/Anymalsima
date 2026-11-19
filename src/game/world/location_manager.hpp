#pragma once

#include <chrono>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chunk_manager.hpp"
#include "consts.hpp"
#include "save/region_store.hpp"
#include "world.hpp"

// Decides which regions are resident and where their chunks come from.
// With a RegionStore, regions saved on disk are decoded from their files and
// regions not generated yet fall back to the generator; callers only see
// chunks becoming registered/ready in ChunkManager. Without a store
// (headless fixtures) every region is generated and nothing is saved.
class LocationManager {
   public:
    LocationManager(ChunkManager& chunks, const World& world,
                    RegionStore* store = nullptr);

    // Load the ring of regions centred on regionCoord.
    // radius=0 → just that region, radius=1 → 3x3 regions, etc.
    // Regions read from disk register their chunks in a later update().
    void loadRegion(const Vec2u& regionCoord, int radius = 1);

    // Unload (and save) regions outside the keep radius around centre.
    // gradual: over the following updates, a few chunk columns at a time
    // within UnloadBudget (a region is 4096 chunks: unloading three at once
    // stalled a frame by 50-100 ms at every region border). Each column is
    // snapshotted as it goes, so later edits to the rest are kept.
    void unloadDistant(const Vec2u& centre, int keepRadius = 2,
                       bool gradual = false);

    // Call when the player crosses into a new region
    void onPlayerRegionChanged(const Vec2u& newRegion) {
        loadRegion(newRegion, 1);
        unloadDistant(newRegion, 2, true);
    }

    // Registers regions whose stored data finished loading, and continues
    // gradual unloading.
    void update();
    // Finishes every gradual unload now.
    void finishUnloading();
    bool isUnloading(const Vec2u& rc) const { return unloading.contains(rc); }
    static constexpr auto UnloadBudget = std::chrono::microseconds(2000);

    // Writes changed chunks of one loaded region; the region stays resident.
    void saveRegion(const Vec2u& rc);
    // Writes every loaded region. With unload, regions are also unloaded.
    void saveAll(bool unload);

    bool isLoaded(const Vec2u& rc) const { return loadedRegions.contains(rc); }
    bool isLoading(const Vec2u& rc) const { return opening.contains(rc); }
    bool hasPendingLoads() const { return !opening.empty(); }
    std::vector<Vec2u> loaded() const {
        return {loadedRegions.begin(), loadedRegions.end()};
    }

   private:
    ChunkManager& chunks;
    const World& world;
    RegionStore* store;

    std::unordered_set<Vec2u> loadedRegions;
    std::unordered_set<Vec2u> opening;
    // Regions being unloaded gradually: the next column (z * 8 + x) and the
    // snapshot of the columns already gone.
    struct Unloading {
        int column = 0;
        RegionSnapshot snapshot;
    };
    std::unordered_map<Vec2u, Unloading> unloading;
    // Unloads one region up to the deadline (none: all of it) and stores it
    // once its last column is gone; true when done.
    bool advanceUnload(
        const Vec2u& rc, Unloading& state,
        std::optional<std::chrono::steady_clock::time_point> deadline);
    // Unloads up to the deadline (none: everything); true when done.
    bool continueUnloading(
        std::optional<std::chrono::steady_clock::time_point> deadline);

    // World-chunk origin for a given region
    static ChunkCoord regionOriginChunk(const Vec2u& rc) {
        return {rc.x * CHUNKS_PER_REGION, 0, rc.y * CHUNKS_PER_REGION};
    }

    void unloadRegion(const Vec2u& rc);
    void registerRegionChunks(const Vec2u& rc, const RegionData* data);
    void unregisterRegionChunks(const Vec2u& rc);
};
