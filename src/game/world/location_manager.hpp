#pragma once

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
    void unloadDistant(const Vec2u& centre, int keepRadius = 2);

    // Call when the player crosses into a new region
    void onPlayerRegionChanged(const Vec2u& newRegion) {
        loadRegion(newRegion, 1);
        unloadDistant(newRegion, 2);
    }

    // Registers regions whose stored data finished loading.
    void update();

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

    // World-chunk origin for a given region
    static ChunkCoord regionOriginChunk(const Vec2u& rc) {
        return {rc.x * CHUNKS_PER_REGION, 0, rc.y * CHUNKS_PER_REGION};
    }

    void unloadRegion(const Vec2u& rc);
    void registerRegionChunks(const Vec2u& rc, const RegionData* data);
    void unregisterRegionChunks(const Vec2u& rc);
};
