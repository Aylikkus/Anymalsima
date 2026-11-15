#pragma once

#include <unordered_set>

#include "chunk_manager.hpp"
#include "consts.hpp"
#include "world.hpp"

class LocationManager {
   public:
    explicit LocationManager(ChunkManager& chunks, const World& world)
        : chunks(chunks), world(world) {}

    // Load the ring of regions centred on regionCoord.
    // radius=0 → just that region, radius=1 → 3x3 regions, etc.
    void loadRegion(const Vec2u& regionCoord, int radius = 1) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int x = int(regionCoord.x) + dx;
                const int z = int(regionCoord.y) + dy;
                if (x < 0 || z < 0 || x >= world.width || z >= world.height)
                    continue;
                Vec2u rc{unsigned(x), unsigned(z)};
                if (loadedRegions.count(rc)) continue;

                registerRegionChunks(rc);
                loadedRegions.insert(rc);
            }
        }
    }

    // Unload regions outside the keep radius around regionCoord.
    void unloadDistant(const Vec2u& centre, int keepRadius = 2) {
        std::vector<Vec2u> toRemove;

        for (const auto& rc : loadedRegions) {
            int dx = std::abs(int(rc.x) - int(centre.x));
            int dy = std::abs(int(rc.y) - int(centre.y));
            if (dx > keepRadius || dy > keepRadius) toRemove.push_back(rc);
        }

        for (const auto& rc : toRemove) {
            unregisterRegionChunks(rc);
            loadedRegions.erase(rc);
        }
    }

    // Call when the player crosses into a new region
    void onPlayerRegionChanged(const Vec2u& newRegion) {
        loadRegion(newRegion, 1);
        unloadDistant(newRegion, 2);
    }

   private:
    ChunkManager& chunks;
    const World& world;

    std::unordered_set<Vec2u> loadedRegions;

    // World-chunk origin for a given region
    static ChunkCoord regionOriginChunk(const Vec2u& rc) {
        return {rc.x * CHUNKS_PER_REGION, 0, rc.y * CHUNKS_PER_REGION};
    }

    void registerRegionChunks(const Vec2u& rc) {
        const Region& region = world.regions[rc.y * world.width + rc.x];
        const ChunkCoord origin = regionOriginChunk(rc);

        for (unsigned int slabY = 0; slabY < Y_CHUNKS; ++slabY) {
            for (int cz = 0; cz < CHUNKS_PER_REGION; ++cz) {
                for (int cx = 0; cx < CHUNKS_PER_REGION; ++cx) {
                    ChunkCoord coord{origin.x + cx, slabY, origin.z + cz};
                    chunks.registerChunk(coord, origin, &region);
                }
            }
        }
    }

    void unregisterRegionChunks(const Vec2u& rc) {
        const ChunkCoord origin = regionOriginChunk(rc);

        for (unsigned int slabY = 0; slabY < Y_CHUNKS; ++slabY) {
            for (int cz = 0; cz < CHUNKS_PER_REGION; ++cz) {
                for (int cx = 0; cx < CHUNKS_PER_REGION; ++cx) {
                    ChunkCoord coord{origin.x + cx, slabY, origin.z + cz};
                    chunks.unregister(coord);
                }
            }
        }
    }
};