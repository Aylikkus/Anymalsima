#include "chunk_manager.hpp"

#include <climits>
#include <iostream>

#include "ground_cover_climate.hpp"
#include "save/chunk_codec.hpp"
#include "save/flora_codec.hpp"

ChunkManager::ChunkManager(const LocationGenerator& gen, ThreadPool& pool,
                           const ChunkMesher* mesher, const World& world,
                           IAssets* assets)
    : terrainWorld(world),
      generator(gen),
      pool(pool),
      mesher(mesher),
      layersGenerator(world.genSettings, assets),
      soilAssets(assets),
      soilSeed(world.genSettings.feature_seed) {
    floraTable = assets ? std::make_shared<const FloraSpeciesTable>(*assets)
                        : std::make_shared<const FloraSpeciesTable>();
    floraSeed_ = world.genSettings.feature_seed ^ 0x5f10a7u;
    treeTable = assets ? std::make_shared<const TreeSpeciesTable>(*assets)
                       : std::make_shared<const TreeSpeciesTable>();
    treeSeed = world.genSettings.feature_seed ^ 0x7ee5u;
    landmarks = placeTreeLandmarks(*treeTable, world, treeSeed);
    for (const auto& site : landmarks)
        landmarkRegions.insert({unsigned(site.x / TerrainTuning::RegionX),
                                unsigned(site.z / TerrainTuning::RegionZ)});
    const size_t threads = std::max<size_t>(1, pool.get_thread_count());
    maxGenerationJobs = std::clamp<size_t>(threads * 2, 4, 32);
    maxMeshJobs = std::clamp<size_t>(threads, 2, 16);
}

ChunkManager::~ChunkManager() {
    for (auto& job : generationJobs) job.future.wait();
    for (auto& job : meshJobs) job.future.wait();
    for (auto& job : treeJobs) job.future.wait();
    for (auto& job : billboardJobs) job.future.wait();
}

void ChunkManager::registerChunk(const ChunkCoord& coord,
                                 const ChunkCoord& origin, const Region* region,
                                 ChunkBlob stored, ChunkBlob storedFlora) {
    if (chunks.contains(coord)) unregister(coord);
    auto& handle = chunks[coord];
    handle.identity = ++nextIdentity;
    handle.regionCoord = region->coord;
    pending[coord] = {origin, region, std::nullopt, std::move(stored),
                      std::move(storedFlora)};
    if (!regionLayers.contains(region->coord))
        regionLayers[region->coord] = layersGenerator.get(*region);
    if (!regionSoils.contains(region->coord))
        regionSoils[region->coord] =
            SoilGenerator::select(*region, *soilAssets, soilSeed);
    const uint64_t column = (uint64_t{coord.x} << 32) | coord.z;
    columnTopY.try_emplace(
        column, TerrainTuning::surfaceCeiling(region->land_height_m));
    ++columnInputs[column].registrations;
    ++regionRegistrations[region->coord];
}

Chunk* ChunkManager::request(const ChunkCoord& coord) {
    auto it = chunks.find(coord);
    if (it == chunks.end()) return nullptr;
    if (it->second.state == ChunkState::Ready) return it->second.data.get();
    it->second.state = ChunkState::Queued;
    // A submitted job is already represented by this handle's identity.
    if (std::none_of(generationJobs.begin(), generationJobs.end(),
                     [&](const auto& job) {
                         return job.coord == coord &&
                                job.identity == it->second.identity;
                     }))
        generationQueue.insert(coord);
    return nullptr;
}

Chunk* ChunkManager::requestLighting(const ChunkCoord& coord,
                                     ChunkCoord beneficiary) {
    if (auto it = pending.find(coord); it != pending.end())
        it->second.beneficiary = beneficiary;
    return request(coord);
}

bool ChunkManager::isRegistered(const ChunkCoord& coord) const {
    return chunks.contains(coord);
}
bool ChunkManager::isReady(const ChunkCoord& coord) const {
    auto it = chunks.find(coord);
    return it != chunks.end() && it->second.state == ChunkState::Ready;
}
bool ChunkManager::isQueued(const ChunkCoord& coord) const {
    auto it = chunks.find(coord);
    return it != chunks.end() && it->second.state == ChunkState::Queued;
}
bool ChunkManager::isEdited(const ChunkCoord& coord) const {
    auto it = chunks.find(coord);
    return it != chunks.end() && it->second.state == ChunkState::Ready &&
           it->second.edited;
}
std::shared_ptr<const ChunkColumnInputs> ChunkManager::generatedColumns(
    unsigned chunkX, unsigned chunkZ) {
    if (terrainWorld.regions.empty()) return {};
    const auto it = chunks.find({chunkX, 0, chunkZ});
    if (it == chunks.end()) return {};
    const auto rc = it->second.regionCoord;
    return prepareColumns(
        {chunkX, 0, chunkZ},
        terrainWorld.regions[rc.y * terrainWorld.width + rc.x]);
}
bool ChunkManager::hasUploadedMesh(const ChunkCoord& coord) const {
    const auto it = chunks.find(coord);
    return it != chunks.end() && it->second.state == ChunkState::Ready &&
           it->second.uploaded;
}
bool ChunkManager::isMeshUploaded(const ChunkCoord& coord) const {
    const auto it = chunks.find(coord);
    return it != chunks.end() && it->second.state == ChunkState::Ready &&
           it->second.uploaded &&
           it->second.uploadedRevision == it->second.meshRevision &&
           !it->second.dirtyPending;
}
Chunk* ChunkManager::getChunk(const ChunkCoord& coord) {
    auto it = chunks.find(coord);
    return it != chunks.end() && it->second.state == ChunkState::Ready
               ? it->second.data.get()
               : nullptr;
}
std::shared_ptr<Chunk> ChunkManager::getChunkShared(const ChunkCoord& coord) {
    std::lock_guard<std::mutex> lock(tileAtMutex);
    auto it = chunks.find(coord);
    return it != chunks.end() && it->second.state == ChunkState::Ready
               ? it->second.data
               : nullptr;
}
const Tile* ChunkManager::tileAt(int x, int y, int z) {
    x = int(wrap().wrapTile(x));
    if (x < 0 || y < 0 || z < 0) return nullptr;
    std::lock_guard<std::mutex> lock(tileAtMutex);
    auto* chunk = getChunk({static_cast<unsigned>(x / Chunk::SIZE_X),
                            static_cast<unsigned>(y / Chunk::SIZE_Y),
                            static_cast<unsigned>(z / Chunk::SIZE_Z)});
    return chunk ? &chunk->tiles[z % Chunk::SIZE_Z][y % Chunk::SIZE_Y]
                                [x % Chunk::SIZE_X]
                 : nullptr;
}
void ChunkManager::unregister(const ChunkCoord& coord) {
    const auto found = chunks.find(coord);
    const bool existed = found != chunks.end();
    if (existed) {
        const auto rc = found->second.regionCoord;
        chunks.erase(found);
        meshed.erase(coord);
        if (--regionRegistrations.at(rc) == 0) {
            regionRegistrations.erase(rc);
            regionTerrain.erase(rc);
            regionLayers.erase(rc);
            regionSoils.erase(rc);
            releaseTreeSets();
        }
        const uint64_t column = (uint64_t{coord.x} << 32) | coord.z;
        if (--columnInputs.at(column).registrations == 0) {
            columnInputs.erase(column);
            columnTopY.erase(column);
        }
    }
    pending.erase(coord);
    generationQueue.erase(coord);
    meshQueue.erase(coord);
    // Submitted jobs own all their inputs. Their identities prevent publication
    // into a replacement chunk, and unloading never waits for a worker.
    if (existed) removed.emit(coord);
}
void ChunkManager::markDirty(const ChunkCoord& coord, bool urgent) {
    auto it = chunks.find(coord);
    if (it == chunks.end() || it->second.state != ChunkState::Ready) return;
    auto& handle = it->second;
    handle.dirtyPending = true;
    handle.meshUrgent |= urgent;
    ++handle.meshRevision;
    if (mesher) meshQueue.insert(coord);
}
void ChunkManager::markModified(const ChunkCoord& coord) {
    auto it = chunks.find(coord);
    if (it == chunks.end() || it->second.state != ChunkState::Ready) return;
    it->second.persistDirty = true;
    markDirty(coord, true);
    // Edited cells change opacity, which the light volumes encode.
    lightChanged.emit(coord);
}
void ChunkManager::markLightChanged(const ChunkCoord& coord) {
    auto it = chunks.find(coord);
    if (it == chunks.end() || it->second.state != ChunkState::Ready) return;
    lightChanged.emit(coord);
}
FloraLayer ChunkManager::regenerateFlora(const ChunkCoord& coord,
                                         uint32_t salt) {
    const auto it = chunks.find(coord);
    if (it == chunks.end() || it->second.state != ChunkState::Ready ||
        floraTable->empty() || terrainWorld.regions.empty())
        return {};
    const auto columns = generatedColumns(coord.x, coord.z);
    if (!columns) return {};
    const auto terrain = terrainShared(it->second.regionCoord);
    const auto tiles = getChunkShared(coord);
    const auto trees = treeColumn(coord.x, coord.z);
    FloraGenerationInput input{
        floraTable.get(),  terrain.get(), columns.get(), coord,
        floraSeed_ ^ salt, tiles.get(),   floraSeed_};
    if (trees) input.treeDiscs = &trees->discs;
    return generateFlora(input);
}

std::shared_ptr<const TreeColumn> ChunkManager::treeColumn(
    unsigned chunkX, unsigned chunkZ) const {
    const auto it = columnInputs.find((uint64_t{chunkX} << 32) | chunkZ);
    return it == columnInputs.end() ? nullptr : it->second.trees;
}
void ChunkManager::setTreeStamp(const ChunkCoord& coord, TreeStamp stamp,
                                bool persist) {
    auto it = chunks.find(coord);
    if (it == chunks.end() || it->second.state != ChunkState::Ready) return;
    it->second.trees = stamp;
    if (persist) it->second.persistDirty = true;
}

void ChunkManager::setTreeOverrides(TreeOverridesPtr value) {
    overrides =
        value ? std::move(value) : std::make_shared<const TreeOverrideMap>();
    ++treeRevision_;
}

ChunkManager::BillboardSet ChunkManager::billboards(Vec2u region) const {
    const auto it = billboardSets.find(region);
    return it == billboardSets.end() ? nullptr : it->second;
}

void ChunkManager::wantBillboards(std::vector<Vec2u> regions) {
    billboardWishes = std::move(regions);
    billboardWishNext = 0;
}

void ChunkManager::dispatchBillboards() {
    if (treeTable->empty() || terrainWorld.regions.empty()) return;
    while (billboardJobs.size() < MaxBillboardJobs &&
           billboardWishNext < billboardWishes.size()) {
        const Vec2u region = billboardWishes[billboardWishNext++];
        if (billboardSets.contains(region) ||
            std::any_of(
                billboardJobs.begin(), billboardJobs.end(),
                [&](const BillboardJob& j) { return j.region == region; }))
            continue;
        billboardJobs.push_back(
            {region, pool.submit_task(
                         [world = &terrainWorld, assets = soilAssets,
                          table = treeTable, region, seed = treeSeed] {
                             const auto terrain = TerrainNeighborhood::capture(
                                 *world, region, *assets);
                             return regionBillboards(
                                 {table.get(), &terrain, region, seed});
                         },
                         BS::pr::low)});
    }
}

void ChunkManager::releaseBillboards(double x, double z, double range) {
    const auto wrapX = wrap();
    const double tiles = TerrainTuning::RegionX;
    std::erase_if(billboardSets, [&](const auto& entry) {
        const double cx = (entry.first.x + 0.5) * tiles;
        const double cz = (entry.first.y + 0.5) * tiles;
        const double dx = wrapX.deltaX(cx, x), dz = cz - z;
        return dx * dx + dz * dz > (range + tiles) * (range + tiles);
    });
}

std::vector<TreeInstancePtr> ChunkManager::landmarkTrees(double x, double z,
                                                         double range) {
    std::vector<TreeInstancePtr> out;
    const auto wrapX = wrap();
    int jobs = 0;
    for (const auto& site : landmarks) {
        const double dx = wrapX.deltaX(double(site.x), x);
        const double dz = double(site.z) - z;
        if (dx * dx + dz * dz > range * range) continue;
        const uint64_t key = (uint64_t(0xFF) << 56) |
                             (uint64_t(site.x) & 0xFFFFFFF) << 28 |
                             (uint64_t(site.z) & 0xFFFFFFF);
        if (const auto it = landmarkCache.find(key);
            it != landmarkCache.end()) {
            out.push_back(it->second);
            continue;
        }
        // At most a couple of new region jobs per call.
        const Vec2u region{unsigned(site.x / TerrainTuning::RegionX),
                           unsigned(site.z / TerrainTuning::RegionZ)};
        if (jobs >= 2 || landmarkRegionsBuilt.contains(region)) continue;
        if (std::none_of(
                treeJobs.begin(), treeJobs.end(),
                [&](const TreeJob& j) { return j.region == region; })) {
            ++jobs;
            regionTrees(region);
        }
    }
    return out;
}

void ChunkManager::updateTree(const TreeInstancePtr& tree,
                              const TreeTileBox& oldBounds) {
    if (const auto it = landmarkCache.find(tree->key);
        it != landmarkCache.end())
        it->second = tree;
    const Vec2u region{unsigned(tree->anchor.x / TerrainTuning::RegionX),
                       unsigned(tree->anchor.z / TerrainTuning::RegionZ)};
    if (const auto it = treeSets.find(region); it != treeSets.end()) {
        auto set = std::make_shared<TreeSet>(*it->second);
        const auto at = std::find_if(
            set->begin(), set->end(),
            [&](const TreeInstancePtr& t) { return t->key == tree->key; });
        if (at != set->end())
            *at = tree;
        else
            set->push_back(tree);
        std::sort(set->begin(), set->end(),
                  [](const auto& a, const auto& b) { return a->key < b->key; });
        it->second = std::move(set);
    }
    // Columns under the old and new bounds rebuild their tree lists now
    // (their sets are ready), so column tops rise before sunlight runs.
    const auto wrapX = wrap();
    for (const auto* box : {&oldBounds, &tree->bounds}) {
        if (box->empty()) continue;
        for (int64_t cz = box->z0 / Chunk::SIZE_Z;
             cz <= (box->z1 - 1) / Chunk::SIZE_Z; ++cz)
            for (int64_t cx =
                     int64_t(std::floor(double(box->x0) / Chunk::SIZE_X));
                 cx <= int64_t(std::floor(double(box->x1 - 1) / Chunk::SIZE_X));
                 ++cx) {
                if (cz < 0) continue;
                const int64_t wx = wrapX.enabled() ? wrapX.wrapChunk(cx) : cx;
                if (wx < 0) continue;
                const uint64_t key = (uint64_t(wx) << 32) | uint64_t(cz);
                const auto column = columnInputs.find(key);
                if (column == columnInputs.end() || !column->second.trees)
                    continue;
                column->second.trees.reset();
                const auto handle =
                    chunks.find({unsigned(wx), 0, unsigned(cz)});
                if (handle != chunks.end())
                    prepareTrees({unsigned(wx), 0, unsigned(cz)},
                                 handle->second.regionCoord);
            }
    }
    ++treeRevision_;
}

TreeStamp ChunkManager::treeStamp(const ChunkCoord& coord) const {
    const auto it = chunks.find(coord);
    return it != chunks.end() && it->second.state == ChunkState::Ready
               ? it->second.trees
               : TreeStamp{};
}

FloraLayerPtr ChunkManager::flora(const ChunkCoord& coord) const {
    const auto it = chunks.find(coord);
    return it != chunks.end() && it->second.state == ChunkState::Ready
               ? it->second.flora
               : nullptr;
}
void ChunkManager::setFlora(const ChunkCoord& coord, FloraLayerPtr layer,
                            bool persist) {
    auto it = chunks.find(coord);
    if (it == chunks.end() || it->second.state != ChunkState::Ready) return;
    if (layer && layer->empty()) layer = nullptr;
    it->second.flora = std::move(layer);
    if (persist) it->second.persistDirty = true;
    floraChanged.emit(coord);
}
bool ChunkManager::isPersistDirty(const ChunkCoord& coord) const {
    const auto it = chunks.find(coord);
    return it != chunks.end() && it->second.persistDirty;
}
RegionSnapshot ChunkManager::snapshotRegion(Vec2u region, bool copy) {
    RegionSnapshot snapshot;
    const ChunkCoord origin{region.x * CHUNKS_PER_REGION, 0,
                            region.y * CHUNKS_PER_REGION};
    std::lock_guard<std::mutex> lock(tileAtMutex);
    for (unsigned y = 0; y < Y_CHUNKS; ++y)
        for (unsigned z = 0; z < CHUNKS_PER_REGION; ++z)
            for (unsigned x = 0; x < CHUNKS_PER_REGION; ++x) {
                const ChunkCoord coord{origin.x + x, y, origin.z + z};
                auto it = chunks.find(coord);
                if (it == chunks.end() ||
                    it->second.state != ChunkState::Ready ||
                    !it->second.persistDirty)
                    continue;
                it->second.persistDirty = false;
                const auto index = RegionData::localIndex(coord);
                snapshot.chunks.emplace_back(
                    index, copy
                               ? std::make_shared<const Chunk>(*it->second.data)
                               : std::shared_ptr<const Chunk>(it->second.data));
                // Layers are immutable: sharing is a snapshot.
                snapshot.flora.emplace_back(index, it->second.flora);
                snapshot.trees.emplace_back(index, it->second.trees);
            }
    return snapshot;
}
void ChunkManager::markAllDirty() {
    for (auto& [coord, handle] : chunks)
        if (handle.state == ChunkState::Ready) markDirty(coord);
}
int ChunkManager::getColumnTopY(unsigned x, unsigned z) const {
    auto it = columnTopY.find((uint64_t{x} << 32) | z);
    return it == columnTopY.end() ? Y_CHUNKS * Chunk::SIZE_Y - 1 : it->second;
}
void ChunkManager::raiseColumnTopY(unsigned x, unsigned z, int y) {
    auto& top = columnTopY[(uint64_t{x} << 32) | z];
    top = std::max(top, y);
}

bool ChunkManager::canMesh(ChunkCoord coord) const {
    const auto it = chunks.find(coord);
    if (it == chunks.end() || it->second.state != ChunkState::Ready ||
        it->second.meshState == MeshState::Queued)
        return false;
    if (it->second.meshState == MeshState::Built) return true;
    const auto wrapX = wrap();
    const auto east = unsigned(wrapX.wrapChunk(int64_t{coord.x} + 1));
    const auto west = unsigned(wrapX.wrapChunk(int64_t{coord.x} - 1));
    const ChunkCoord neighbors[6] = {
        {east, coord.y, coord.z},        {west, coord.y, coord.z},
        {coord.x, coord.y + 1, coord.z}, {coord.x, coord.y - 1, coord.z},
        {coord.x, coord.y, coord.z + 1}, {coord.x, coord.y, coord.z - 1}};
    return std::all_of(
        std::begin(neighbors), std::end(neighbors), [&](auto neighbor) {
            // X wraps, so only Z and Y have world edges.
            return (terrainWorld.width > 0 && terrainWorld.height > 0 &&
                    (neighbor.z >=
                         unsigned(terrainWorld.height * CHUNKS_PER_REGION) ||
                     neighbor.y >= Y_CHUNKS)) ||
                   isReady(neighbor);
        });
}

void ChunkManager::submitMesh(ChunkCoord coord) {
    auto& handle = chunks.at(coord);
    auto data = std::make_shared<Chunk>(*handle.data);
    std::unordered_map<ChunkCoord, std::shared_ptr<Chunk>> neighbors;
    const int offsets[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                               {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    const auto wrapX = wrap();
    for (const auto& offset : offsets) {
        const int64_t x = wrapX.wrapChunk(int64_t{coord.x} + offset[0]);
        const int64_t y = int64_t{coord.y} + offset[1];
        const int64_t z = int64_t{coord.z} + offset[2];
        if (x < 0 || y < 0 || z < 0) continue;
        ChunkCoord neighbor{static_cast<unsigned>(x), static_cast<unsigned>(y),
                            static_cast<unsigned>(z)};
        if (auto* chunk = getChunk(neighbor))
            neighbors[neighbor] = std::make_shared<Chunk>(*chunk);
    }
    CoverClimate climate;
    if (!terrainWorld.regions.empty())
        for (int corner = 0; corner < 4; ++corner) {
            const auto [humidity, temperature] = ground_cover_climate::at(
                terrainWorld, double(coord.x + (corner & 1)) * Chunk::SIZE_X,
                double(coord.z + (corner >> 1)) * Chunk::SIZE_Z);
            climate.humidity[corner] = humidity;
            climate.temperature_c[corner] = temperature;
        }
    const auto identity = handle.identity, revision = handle.meshRevision;
    const auto priority = handle.meshUrgent ? BS::pr::high : BS::pr::normal;
    auto future = pool.submit_task(
        [coord, data, neighbors = std::move(neighbors), builder = *mesher,
         identity, revision, wrapX, depth = terrainWorld.height * 128,
         climate]() {
            auto mesh = builder.build(
                *data, coord,
                [&](int x, int y, int z) -> const Tile* {
                    static const Tile outside{};
                    // X wraps; Z and Y end at the world edge.
                    x = int(wrapX.wrapTile(x));
                    if (wrapX.enabled() && (z < 0 || y < 0 || z >= depth ||
                                            y >= Y_CHUNKS * Chunk::SIZE_Y))
                        return &outside;
                    if (x < 0 || y < 0 || z < 0) return nullptr;
                    ChunkCoord c{static_cast<unsigned>(x / Chunk::SIZE_X),
                                 static_cast<unsigned>(y / Chunk::SIZE_Y),
                                 static_cast<unsigned>(z / Chunk::SIZE_Z)};
                    auto it = neighbors.find(c);
                    return it == neighbors.end()
                               ? nullptr
                               : &it->second->tiles[z % Chunk::SIZE_Z]
                                                   [y % Chunk::SIZE_Y]
                                                   [x % Chunk::SIZE_X];
                },
                climate);
            return FinishedMesh{coord, std::move(mesh), identity, revision};
        },
        priority);
    meshJobs.push_back({coord, identity, std::move(future)});
    handle.meshState = MeshState::Queued;
    handle.dirtyPending = false;
    handle.meshUrgent = false;
    meshQueue.erase(coord);
}

void ChunkManager::collect() {
    using namespace std::chrono_literals;
    for (size_t i = 0; i < billboardJobs.size();) {
        if (billboardJobs[i].future.wait_for(0s) != std::future_status::ready) {
            ++i;
            continue;
        }
        auto job = std::move(billboardJobs[i]);
        billboardJobs.erase(billboardJobs.begin() + i);
        billboardSets[job.region] =
            std::make_shared<const std::vector<TreeBillboard>>(
                job.future.get());
        ++billboardRevision_;
    }
    dispatchBillboards();
    for (size_t i = 0; i < treeJobs.size();) {
        if (treeJobs[i].future.wait_for(0s) != std::future_status::ready) {
            ++i;
            continue;
        }
        auto job = std::move(treeJobs[i]);
        treeJobs.erase(treeJobs.begin() + i);
        auto set = job.future.get();
        // A tree was cut or planted meanwhile: build the set again.
        if (job.overrides != overrides) {
            regionTrees(job.region);
            continue;
        }
        for (const auto& tree : set)
            if (const auto* rule = treeTable->find(tree->species);
                rule && rule->landmark)
                landmarkCache[tree->key] = tree;
        if (landmarkRegions.contains(job.region))
            landmarkRegionsBuilt.insert(job.region);
        treeSets[job.region] = std::make_shared<const TreeSet>(std::move(set));
    }
    for (size_t i = 0; i < generationJobs.size();) {
        if (generationJobs[i].future.wait_for(0s) !=
            std::future_status::ready) {
            ++i;
            continue;
        }
        auto job = std::move(generationJobs[i]);
        generationJobs.erase(generationJobs.begin() + i);
        GeneratedChunk data;
        try {
            data = job.future.get();
        } catch (...) {
            if (auto it = chunks.find(job.coord);
                it != chunks.end() && it->second.identity == job.identity)
                generationQueue.insert(job.coord);
            throw;
        }
        publishGenerated(job.coord, job.identity, std::move(data));
    }
    for (size_t i = 0; i < meshJobs.size();) {
        if (meshJobs[i].future.wait_for(0s) != std::future_status::ready) {
            ++i;
            continue;
        }
        auto job = std::move(meshJobs[i]);
        meshJobs.erase(meshJobs.begin() + i);
        FinishedMesh result;
        try {
            result = job.future.get();
        } catch (...) {
            if (auto it = chunks.find(job.coord);
                it != chunks.end() && it->second.identity == job.identity) {
                it->second.meshState =
                    it->second.initialised ? MeshState::Built : MeshState::None;
                markDirty(job.coord);
            }
            throw;
        }
        auto it = chunks.find(result.coord);
        if (it == chunks.end() || it->second.identity != result.identity)
            continue;
        auto& handle = it->second;
        handle.meshState = MeshState::Built;
        if (handle.meshRevision != result.revision) {
            meshQueue.insert(result.coord);
            continue;
        }
        readyMeshes.push_back(std::move(result));
    }
}

void ChunkManager::publishGenerated(ChunkCoord coord, uint64_t identity,
                                    GeneratedChunk data) {
    auto it = chunks.find(coord);
    if (it == chunks.end() || it->second.identity != identity) return;
    // A tree changed while the job ran (a cut, a planted tree): redo it
    // with the new column rather than publish wood of the old tree.
    if (data.treesUsed) {
        const auto current = treeColumn(coord.x, coord.z);
        if (current && current.get() != data.treesUsed) {
            generationQueue.insert(coord);
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(tileAtMutex);
        it->second.data = std::move(data.data);
        it->second.state = ChunkState::Ready;
        it->second.persistDirty =
            !data.fromDisk || data.floraRegrown || data.treesPlaced;
        it->second.edited = data.edited;
        it->second.flora = std::move(data.flora);
        it->second.trees = data.trees;
    }
    pending.erase(coord);
    markDirty(coord);
    becameReady.emit(coord);
}

void ChunkManager::dispatchGeneration() {
    if (generationQueue.empty()) return;
    const auto started = std::chrono::steady_clock::now();
    const auto anchor = [&](ChunkCoord coord) {
        const auto target = pending.at(coord).beneficiary.value_or(coord);
        // A camera moving into a former sky prerequisite must not demote
        // that chunk to the old beneficiary's distant height.
        return generationQueue.distance(coord, focus) <
                       generationQueue.distance(target, focus)
                   ? coord
                   : target;
    };
    const auto order = generationQueue.ordered(focus, false, anchor);
    size_t next = 0;
    int air = 0;
    while (std::chrono::steady_clock::now() - started < DispatchBudget) {
        const bool slots = generationJobs.size() < maxGenerationJobs;
        if (!slots && air >= AirChunksPerUpdate) break;
        std::optional<ChunkCoord> picked;
        bool oldest = false;
        if (slots && (generationDispatches + 1) % 16 == 0) {
            picked = generationQueue.best(
                focus, true, [](auto) { return true; }, anchor);
            oldest = picked.has_value();
        }
        if (!picked) {
            while (next < order.size() &&
                   !generationQueue.contains(order[next]))
                ++next;
            if (next == order.size()) break;
            picked = order[next++];
        }
        const auto coord = *picked;
        const auto& info = pending.at(coord);
        const auto identity = chunks.at(coord).identity;
        const auto columns = prepareColumns(coord, *info.region);
        // Trees first: a chunk waits until the tree sets of the regions
        // around it are built (it stays queued meanwhile).
        const auto trees = prepareTrees(coord, info.region->coord);
        if (!trees) continue;
        const TreeStamp stamp{floraDay, TreeVersion};
        if (!info.stored && columns &&
            generator.isAirChunk(coord, *columns, trees->woodTop)) {
            // The oldest pick always proceeds so the starvation guard cannot
            // spin on an air chunk.
            if (air >= AirChunksPerUpdate && !oldest) continue;
            ++air;
            generationQueue.erase(coord);
            GeneratedChunk empty{std::make_shared<Chunk>(), false, false,
                                 nullptr};
            empty.trees = stamp;
            publishGenerated(coord, identity, std::move(empty));
            continue;
        }
        if (!slots) continue;
        // Plants are generated for slabs holding a surface tile.
        std::shared_ptr<const TerrainNeighborhood> terrain;
        if (columns && !floraTable->empty()) {
            int low = INT_MAX, high = INT_MIN;
            for (const auto& c : columns->columns) {
                low = std::min(low, c.surface_y);
                high = std::max(high, c.surface_y);
            }
            const int base = int(coord.y) * Chunk::SIZE_Y;
            if (high >= base && low < base + Chunk::SIZE_Y)
                terrain = terrainShared(info.region->coord);
        }
        auto future = pool.submit_task(
            [generator = generator, coord, columns, origin = info.regionOrigin,
             region = *info.region,
             layers = regionLayers.at(info.region->coord),
             soil = regionSoils.at(info.region->coord), stored = info.stored,
             storedFlora = info.storedFlora, terrain, rules = floraTable,
             seed = floraSeed_, day = floraDay, trees, treeRules = treeTable,
             stamp] {
                const auto plants = [&](const Chunk* tiles) -> FloraLayerPtr {
                    if (!terrain) return nullptr;
                    FloraGenerationInput input{rules.get(),
                                               terrain.get(),
                                               columns.get(),
                                               coord,
                                               seed ^ floraEpochSalt(day),
                                               tiles,
                                               seed};
                    input.treeDiscs = &trees->discs;
                    auto layer = generateFlora(input);
                    for (auto& p : layer.instances) p.birthDay += day;
                    layer.resolvedDay = day;
                    return layer.empty() ? nullptr
                                         : std::make_shared<const FloraLayer>(
                                               std::move(layer));
                };
                auto result = std::make_shared<Chunk>();
                if (stored) {
                    try {
                        chunk_codec::decode(*stored, *result);
                        // Generated tiles are solid exactly up to the
                        // surface (plus tree wood, which only grows into
                        // air); anything else is an earlier edit.
                        const auto& grown = treeRules->grown();
                        bool edited = false;
                        if (columns)
                            for (int z = 0; z < Chunk::SIZE_Z && !edited; ++z)
                                for (int y = 0; y < Chunk::SIZE_Y && !edited;
                                     ++y)
                                    for (int x = 0; x < Chunk::SIZE_X; ++x) {
                                        const int surface =
                                            columns
                                                ->columns[z * Chunk::SIZE_X + x]
                                                .surface_y;
                                        const int worldY =
                                            int(coord.y) * Chunk::SIZE_Y + y;
                                        const Tile& tile =
                                            result->tiles[z][y][x];
                                        const bool solid =
                                            tile.isSolid() &&
                                            !grown.test(tile.block_id);
                                        if (solid != (worldY <= surface)) {
                                            edited = true;
                                            break;
                                        }
                                    }
                        GeneratedChunk out{result, true, edited, nullptr};
                        out.treesUsed = trees.get();
                        // Chunks saved before trees existed get whole trees
                        // once, into their air.
                        // Chunks saved before trees existed get whole trees
                        // once; later ones catch up on growth and on cuts
                        // made while they were unloaded (only wood of the
                        // same tree generator version).
                        const auto addTrees = [&] {
                            if (!out.trees.placed()) {
                                placeTreeWood(*result, coord, *trees,
                                              *treeRules, day);
                                out.trees = stamp;
                                out.treesPlaced = true;
                                return;
                            }
                            if (out.trees.version != TreeVersion ||
                                out.trees.day >= day)
                                return;
                            const auto changes =
                                treeWoodChanges(*result, coord, *trees,
                                                *treeRules, out.trees.day, day);
                            applyTreeWoodChanges(*result, changes);
                            out.trees.day = day;
                            out.treesPlaced = !changes.empty();
                        };
                        if (storedFlora) {
                            try {
                                auto layer = flora_codec::decode(*storedFlora,
                                                                 &out.trees);
                                if (!layer.empty())
                                    out.flora =
                                        std::make_shared<const FloraLayer>(
                                            std::move(layer));
                                addTrees();
                                return out;
                            } catch (const std::exception& e) {
                                std::cerr << "Saved plants of chunk " << coord.x
                                          << ',' << coord.y << ',' << coord.z
                                          << " are unreadable (" << e.what()
                                          << "); regrowing\n";
                                out.trees = {};
                            }
                        }
                        addTrees();
                        // Older saves: regrow generated plants where the
                        // ground still allows them.
                        out.flora = plants(result.get());
                        out.floraRegrown = out.flora != nullptr;
                        return out;
                    } catch (const std::exception& e) {
                        std::cerr << "Saved chunk " << coord.x << ',' << coord.y
                                  << ',' << coord.z << " is unreadable ("
                                  << e.what() << "); regenerating\n";
                    }
                }
                generator.generateChunk(*result, coord, origin, region, layers,
                                        soil, columns.get());
                placeTreeWood(*result, coord, *trees, *treeRules, day);
                GeneratedChunk out{result, false, false, plants(nullptr)};
                out.trees = stamp;
                out.treesUsed = trees.get();
                return out;
            },
            info.beneficiary ? BS::pr::high : BS::pr::normal);
        generationJobs.push_back({coord, identity, std::move(future)});
        generationQueue.erase(coord);
        ++generationDispatches;
    }
}

bool ChunkManager::hasNoGeometry(ChunkCoord coord) const {
    const auto it = chunks.find(coord);
    if (it == chunks.end() || it->second.state != ChunkState::Ready ||
        !it->second.data)
        return false;
    const Tile* first = &it->second.data->tiles[0][0][0];
    const Tile* last = first + Chunk::SIZE_X * Chunk::SIZE_Y * Chunk::SIZE_Z;
    if (std::all_of(first, last, [](const Tile& t) { return t.isAir(); }))
        return true;
    if (!std::all_of(first, last, [](const Tile& t) { return t.isSolid(); }))
        return false;
    // A solid chunk has no visible face only when every face neighbour is
    // loaded and its touching layer is solid too. World edges and missing
    // neighbours take the regular mesher path.
    const int offsets[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                               {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    const auto wrapX = wrap();
    for (const auto& o : offsets) {
        const int64_t x = wrapX.wrapChunk(int64_t{coord.x} + o[0]);
        const int64_t y = int64_t{coord.y} + o[1];
        const int64_t z = int64_t{coord.z} + o[2];
        if (x < 0 || y < 0 || z < 0) return false;
        const auto n = chunks.find({unsigned(x), unsigned(y), unsigned(z)});
        if (n == chunks.end() || n->second.state != ChunkState::Ready ||
            !n->second.data)
            return false;
        const auto& tiles = n->second.data->tiles;
        for (int a = 0; a < 16; ++a)
            for (int b = 0; b < 16; ++b) {
                const Tile& t = o[0]   ? tiles[a][b][o[0] > 0 ? 0 : 15]
                                : o[1] ? tiles[a][o[1] > 0 ? 0 : 15][b]
                                       : tiles[o[2] > 0 ? 0 : 15][a][b];
                if (!t.isSolid()) return false;
            }
    }
    return true;
}

void ChunkManager::finishEmptyMesh(ChunkCoord coord) {
    auto& handle = chunks.at(coord);
    readyMeshes.push_back(
        FinishedMesh{coord, {}, handle.identity, handle.meshRevision});
    handle.meshState = MeshState::Built;
    handle.dirtyPending = false;
    handle.meshUrgent = false;
    meshQueue.erase(coord);
}

void ChunkManager::dispatchMeshes() {
    if (!mesher || meshQueue.empty()) return;
    const auto started = std::chrono::steady_clock::now();
    const auto eligible = [&](ChunkCoord coord) { return canMesh(coord); };
    const auto order = meshQueue.ordered(focus, false);
    size_t next = 0;
    while (std::chrono::steady_clock::now() - started < DispatchBudget) {
        const bool slots = meshJobs.size() < maxMeshJobs;
        std::optional<ChunkCoord> picked;
        if (slots && (meshDispatches + 1) % 16 == 0)
            picked = meshQueue.best(focus, true, eligible,
                                    [](auto coord) { return coord; });
        if (!picked) {
            while (next < order.size() &&
                   (!meshQueue.contains(order[next]) || !canMesh(order[next])))
                ++next;
            if (next == order.size()) break;
            picked = order[next++];
        }
        // Empty results skip the job and the seven chunk copies; they do
        // not occupy a worker slot.
        if (hasNoGeometry(*picked)) {
            finishEmptyMesh(*picked);
            continue;
        }
        if (!slots) continue;
        submitMesh(*picked);
        ++meshDispatches;
    }
}

void ChunkManager::update() {
    generationQueue.setPeriodX(wrap().tiles);
    meshQueue.setPeriodX(wrap().tiles);
    collect();
    dispatchGeneration();
    dispatchMeshes();
}

MeshHandle* ChunkManager::getMesh(const ChunkCoord& coord) {
    auto it = chunks.find(coord);
    // Keep the current uploaded mesh visible while its replacement is built.
    return it == chunks.end() || !it->second.initialised ? nullptr
                                                         : &it->second.mesh;
}

std::shared_ptr<const TerrainNeighborhood> ChunkManager::terrainShared(
    Vec2u region) {
    auto it = regionTerrain.find(region);
    if (it == regionTerrain.end())
        it = regionTerrain
                 .emplace(region, std::make_shared<const TerrainNeighborhood>(
                                      TerrainNeighborhood::capture(
                                          terrainWorld, region, *soilAssets)))
                 .first;
    return it->second;
}

std::shared_ptr<const ChunkColumnInputs> ChunkManager::prepareColumns(
    ChunkCoord coord, const Region& region) {
    // Synthetic/headless fixtures may deliberately omit the world map.
    if (terrainWorld.regions.empty()) return {};
    const uint64_t key = (uint64_t{coord.x} << 32) | coord.z;
    auto& cache = columnInputs.at(key);
    if (!cache.data)
        cache.data = std::make_shared<const ChunkColumnInputs>(
            TerrainSampler(terrainNeighborhood(region.coord))
                .columns(coord.x, coord.z));
    return cache.data;
}

int ChunkManager::generatedSurfaceY(unsigned tileX, unsigned tileZ) {
    tileX = unsigned(wrap().wrapTile(tileX));
    const Vec2u rc{tileX / TerrainTuning::RegionX,
                   tileZ / TerrainTuning::RegionZ};
    return TerrainSampler(terrainNeighborhood(rc))
        .sample(tileX, tileZ)
        .surface_y;
}

const TreeSet* ChunkManager::regionTrees(Vec2u region) {
    if (const auto it = treeSets.find(region); it != treeSets.end())
        return it->second.get();
    if (std::none_of(treeJobs.begin(), treeJobs.end(), [&](const TreeJob& job) {
            return job.region == region;
        })) {
        std::vector<TreeLandmark> near;
        const int width = terrainWorld.width;
        for (const auto& site : landmarks) {
            const int64_t dx = WorldWrap::delta(site.x / TerrainTuning::RegionX,
                                                region.x, width);
            const int64_t dz =
                site.z / TerrainTuning::RegionZ - int64_t(region.y);
            if (std::abs(dx) <= 2 && std::abs(dz) <= 2) near.push_back(site);
        }
        // The neighbourhood is captured on the worker: World and assets are
        // read-only while chunks stream.
        treeJobs.push_back(
            {region,
             pool.submit_task(
                 [world = &terrainWorld, assets = soilAssets, table = treeTable,
                  region, seed = treeSeed, near = std::move(near),
                  overrides = overrides] {
                     const auto terrain =
                         TerrainNeighborhood::capture(*world, region, *assets);
                     RegionTreesInput input{table.get(), &terrain, region, seed,
                                            &near};
                     input.overrides = overrides.get();
                     return generateRegionTrees(input);
                 },
                 BS::pr::high),
             overrides});
    }
    return nullptr;
}

std::shared_ptr<const TreeColumn> ChunkManager::prepareTrees(ChunkCoord coord,
                                                             Vec2u region) {
    static const auto none = std::make_shared<const TreeColumn>();
    if (terrainWorld.regions.empty() || treeTable->empty()) return none;
    const uint64_t key = (uint64_t{coord.x} << 32) | coord.z;
    auto& cache = columnInputs.at(key);
    if (cache.trees) return cache.trees;
    // Ordinary trees reach at most one region from their anchor; landmarks
    // two (only regions holding one are needed).
    std::vector<const TreeSet*> sets;
    bool ready = true;
    const auto wrapX = wrap();
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx) {
            const int64_t z = int64_t(region.y) + dz;
            if (z < 0 || z >= terrainWorld.height) continue;
            const Vec2u q{unsigned(wrapX.enabled() ? wrapX.wrapRegion(
                                                         int64_t(region.x) + dx)
                                                   : int64_t(region.x) + dx),
                          unsigned(z)};
            if (!wrapX.enabled() &&
                (int64_t(region.x) + dx < 0 ||
                 int64_t(region.x) + dx >= terrainWorld.width))
                continue;
            const bool near = std::abs(dx) <= 1 && std::abs(dz) <= 1;
            if (!near && !landmarkRegions.contains(q)) continue;
            const auto* set = regionTrees(q);
            if (!set) ready = false;
            sets.push_back(set);
        }
    if (!ready) return nullptr;
    cache.trees = std::make_shared<const TreeColumn>(
        makeTreeColumn(coord.x, coord.z, sets, wrapX.tiles));
    // Sunlight must see the wood before any slab of the column is lit.
    if (cache.trees->woodTop > INT_MIN)
        raiseColumnTopY(coord.x, coord.z,
                        std::min(cache.trees->woodTop, WORLD_HEIGHT_Y - 1));
    return cache.trees;
}

void ChunkManager::releaseTreeSets() {
    const int width = terrainWorld.width;
    std::erase_if(treeSets, [&](const auto& entry) {
        const Vec2u q = entry.first;
        for (const auto& [rc, count] : regionRegistrations) {
            const int64_t dx = WorldWrap::delta(rc.x, q.x, width);
            const int64_t dz = int64_t(rc.y) - int64_t(q.y);
            if (std::abs(dx) <= 2 && std::abs(dz) <= 2) return false;
        }
        return true;
    });
}
