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
            // Back before it finished unloading: finish (and save) it first,
            // then load it like any other region.
            if (const auto it = unloading.find(rc); it != unloading.end()) {
                advanceUnload(rc, it->second, std::nullopt);
                unloading.erase(it);
            }
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

bool LocationManager::advanceUnload(
    const Vec2u& rc, Unloading& state,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    const ChunkCoord origin = regionOriginChunk(rc);
    // Whole columns: sunlight then finds each column empty instead of
    // recomputing it once per slab.
    while (state.column < CHUNKS_PER_REGION * CHUNKS_PER_REGION) {
        if (deadline && std::chrono::steady_clock::now() >= *deadline)
            return false;
        const unsigned c = unsigned(state.column++);
        const unsigned cx = origin.x + c % CHUNKS_PER_REGION,
                       cz = origin.z + c / CHUNKS_PER_REGION;
        if (store) chunks.snapshotColumn(cx, cz, state.snapshot);
        for (unsigned y = 0; y < Y_CHUNKS; ++y) chunks.unregister({cx, y, cz});
    }
    if (store) store->store(rc, std::move(state.snapshot), true);
    return true;
}

bool LocationManager::continueUnloading(
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    for (auto it = unloading.begin(); it != unloading.end();) {
        if (!advanceUnload(it->first, it->second, deadline)) return false;
        it = unloading.erase(it);
    }
    return true;
}

void LocationManager::finishUnloading() { continueUnloading(std::nullopt); }

void LocationManager::update() {
    if (!unloading.empty())
        continueUnloading(std::chrono::steady_clock::now() + UnloadBudget);
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

void LocationManager::unloadDistant(const Vec2u& centre, int keepRadius,
                                    bool gradual) {
    auto distant = [&](const Vec2u& rc) {
        const int dx = int(std::abs(world.wrap().deltaRegion(rc.x, centre.x)));
        const int dy = std::abs(int(rc.y) - int(centre.y));
        return dx > keepRadius || dy > keepRadius;
    };

    std::vector<Vec2u> toRemove;
    for (const auto& rc : loadedRegions)
        if (distant(rc)) toRemove.push_back(rc);
    for (const auto& rc : toRemove) {
        if (!gradual) {
            unloadRegion(rc);
            continue;
        }
        loadedRegions.erase(rc);
        unloading.emplace(rc, Unloading{});
    }

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
        auto snapshot = chunks.snapshotRegion(rc);
        unregisterRegionChunks(rc);
        store->store(rc, std::move(snapshot), true);
    } else {
        unregisterRegionChunks(rc);
    }
    loadedRegions.erase(rc);
}

void LocationManager::saveRegion(const Vec2u& rc) {
    if (!store || !loadedRegions.contains(rc)) return;
    store->store(rc, chunks.snapshotRegion(rc), false);
}

void LocationManager::saveAll(bool unload) {
    // Regions half unloaded are saved as they finish.
    finishUnloading();
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
                ChunkBlob stored, storedFlora, storedBaseline;
                if (data) {
                    const auto index = RegionData::localIndex(coord);
                    const auto it = data->chunks.find(index);
                    if (it != data->chunks.end()) stored = it->second;
                    const auto plants = data->flora.find(index);
                    if (plants != data->flora.end())
                        storedFlora = plants->second;
                    if (const auto baseline =
                            data->structureBaselines.find(index);
                        baseline != data->structureBaselines.end())
                        storedBaseline = baseline->second;
                }
                chunks.registerChunk(coord, origin, &region, stored,
                                     storedFlora, storedBaseline);
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
