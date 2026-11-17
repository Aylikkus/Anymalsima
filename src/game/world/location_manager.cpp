#include "location_manager.hpp"

LocationManager::LocationManager(ChunkManager& chunks, const World& world,
                                 RegionStore* store)
    : chunks(chunks), world(world), store(store) {}

void LocationManager::loadRegion(const Vec2u& regionCoord, int radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            // X wraps around the world; Z ends at its edges.
            const int x =
                int(world.wrap().wrapRegion(int64_t(regionCoord.x) + dx));
            const int z = int(regionCoord.y) + dy;
            if (x < 0 || z < 0 || x >= world.width || z >= world.height)
                continue;
            Vec2u rc{unsigned(x), unsigned(z)};
            if (loadedRegions.count(rc) || opening.count(rc)) continue;

            if (!store) {
                registerRegionChunks(rc, nullptr);
                loadedRegions.insert(rc);
                continue;
            }
            store->open(rc);
            opening.insert(rc);
        }
    }
    // Cached or absent regions are usually ready immediately.
    update();
}

void LocationManager::update() {
    if (!store || opening.empty()) return;
    store->update();
    for (auto it = opening.begin(); it != opening.end();) {
        const auto data = store->ready(*it);
        if (!data) {
            ++it;
            continue;
        }
        registerRegionChunks(*it, data.get());
        loadedRegions.insert(*it);
        it = opening.erase(it);
    }
}

void LocationManager::unloadDistant(const Vec2u& centre, int keepRadius) {
    auto distant = [&](const Vec2u& rc) {
        const int dx = int(std::abs(world.wrap().deltaRegion(rc.x, centre.x)));
        const int dy = std::abs(int(rc.y) - int(centre.y));
        return dx > keepRadius || dy > keepRadius;
    };

    std::vector<Vec2u> toRemove;
    for (const auto& rc : loadedRegions)
        if (distant(rc)) toRemove.push_back(rc);
    for (const auto& rc : toRemove) unloadRegion(rc);

    for (auto it = opening.begin(); it != opening.end();) {
        if (!distant(*it)) {
            ++it;
            continue;
        }
        // Nothing was registered yet, so there is nothing to save.
        store->release(*it);
        it = opening.erase(it);
    }
}

void LocationManager::unloadRegion(const Vec2u& rc) {
    if (store) {
        // Moving the live chunks is safe: they are unregistered right below.
        auto snapshot = chunks.snapshotRegion(rc, false);
        unregisterRegionChunks(rc);
        store->store(rc, std::move(snapshot), true);
    } else {
        unregisterRegionChunks(rc);
    }
    loadedRegions.erase(rc);
}

void LocationManager::saveRegion(const Vec2u& rc) {
    if (!store || !loadedRegions.contains(rc)) return;
    store->store(rc, chunks.snapshotRegion(rc, true), false);
}

void LocationManager::saveAll(bool unload) {
    if (unload) {
        const auto regions = loaded();
        for (const auto& rc : regions) unloadRegion(rc);
        for (const auto& rc : opening) store->release(rc);
        opening.clear();
        return;
    }
    for (const auto& rc : loadedRegions) saveRegion(rc);
}

void LocationManager::registerRegionChunks(const Vec2u& rc,
                                           const RegionData* data) {
    const Region& region = world.regions[rc.y * world.width + rc.x];
    const ChunkCoord origin = regionOriginChunk(rc);

    for (unsigned int slabY = 0; slabY < Y_CHUNKS; ++slabY) {
        for (int cz = 0; cz < CHUNKS_PER_REGION; ++cz) {
            for (int cx = 0; cx < CHUNKS_PER_REGION; ++cx) {
                ChunkCoord coord{origin.x + cx, slabY, origin.z + cz};
                ChunkBlob stored, storedFlora;
                if (data) {
                    const auto index = RegionData::localIndex(coord);
                    const auto it = data->chunks.find(index);
                    if (it != data->chunks.end()) stored = it->second;
                    const auto plants = data->flora.find(index);
                    if (plants != data->flora.end())
                        storedFlora = plants->second;
                }
                chunks.registerChunk(coord, origin, &region, stored,
                                     storedFlora);
            }
        }
    }
    if (!data) return;
    // Saved terrain can rise above the generated estimate (future edits);
    // sunlight sweeps start from this ceiling.
    for (int column = 0; column < RegionData::Columns; ++column) {
        const int top = data->tops[column];
        if (top == INT_MIN) continue;
        chunks.raiseColumnTopY(origin.x + column % CHUNKS_PER_REGION,
                               origin.z + column / CHUNKS_PER_REGION, top);
    }
}

void LocationManager::unregisterRegionChunks(const Vec2u& rc) {
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
