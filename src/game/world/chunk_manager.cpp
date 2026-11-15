#include "chunk_manager.hpp"

#include <iostream>

#include "save/chunk_codec.hpp"

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
    const size_t threads = std::max<size_t>(1, pool.get_thread_count());
    maxGenerationJobs = std::clamp<size_t>(threads * 2, 4, 32);
    maxMeshJobs = std::clamp<size_t>(threads, 2, 16);
}

ChunkManager::~ChunkManager() {
    for (auto& job : generationJobs) job.future.wait();
    for (auto& job : meshJobs) job.future.wait();
}

void ChunkManager::registerChunk(const ChunkCoord& coord,
                                 const ChunkCoord& origin, const Region* region,
                                 ChunkBlob stored) {
    if (chunks.contains(coord)) unregister(coord);
    auto& handle = chunks[coord];
    handle.identity = ++nextIdentity;
    handle.regionCoord = region->coord;
    pending[coord] = {origin, region, std::nullopt, std::move(stored)};
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
                snapshot.chunks.emplace_back(
                    RegionData::localIndex(coord),
                    copy ? std::make_shared<const Chunk>(*it->second.data)
                         : std::shared_ptr<const Chunk>(it->second.data));
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
    const ChunkCoord neighbors[6] = {
        {coord.x + 1, coord.y, coord.z}, {coord.x - 1, coord.y, coord.z},
        {coord.x, coord.y + 1, coord.z}, {coord.x, coord.y - 1, coord.z},
        {coord.x, coord.y, coord.z + 1}, {coord.x, coord.y, coord.z - 1}};
    return std::all_of(
        std::begin(neighbors), std::end(neighbors), [&](auto neighbor) {
            return (terrainWorld.width > 0 && terrainWorld.height > 0 &&
                    (neighbor.x >=
                         unsigned(terrainWorld.width * CHUNKS_PER_REGION) ||
                     neighbor.z >=
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
    for (const auto& offset : offsets) {
        const int64_t x = int64_t{coord.x} + offset[0];
        const int64_t y = int64_t{coord.y} + offset[1];
        const int64_t z = int64_t{coord.z} + offset[2];
        if (x < 0 || y < 0 || z < 0) continue;
        ChunkCoord neighbor{static_cast<unsigned>(x), static_cast<unsigned>(y),
                            static_cast<unsigned>(z)};
        if (auto* chunk = getChunk(neighbor))
            neighbors[neighbor] = std::make_shared<Chunk>(*chunk);
    }
    const auto identity = handle.identity, revision = handle.meshRevision;
    const auto priority = handle.meshUrgent ? BS::pr::high : BS::pr::normal;
    auto future = pool.submit_task(
        [coord, data, neighbors = std::move(neighbors), builder = *mesher,
         identity, revision, width = terrainWorld.width * 128,
         depth = terrainWorld.height * 128]() {
            auto mesh = builder.build(
                *data, coord, [&](int x, int y, int z) -> const Tile* {
                    static const Tile outside{};
                    if (width > 0 && depth > 0 &&
                        (x < 0 || z < 0 || y < 0 || x >= width || z >= depth ||
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
                });
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
    {
        std::lock_guard<std::mutex> lock(tileAtMutex);
        it->second.data = std::move(data.data);
        it->second.state = ChunkState::Ready;
        it->second.persistDirty = !data.fromDisk;
        it->second.edited = data.edited;
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
        return CameraWorkQueue::distance(coord, focus) <
                       CameraWorkQueue::distance(target, focus)
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
        if (!info.stored && columns && generator.isAirChunk(coord, *columns)) {
            // The oldest pick always proceeds so the starvation guard cannot
            // spin on an air chunk.
            if (air >= AirChunksPerUpdate && !oldest) continue;
            ++air;
            generationQueue.erase(coord);
            publishGenerated(coord, identity,
                             {std::make_shared<Chunk>(), false});
            continue;
        }
        if (!slots) continue;
        auto future = pool.submit_task(
            [generator = generator, coord, columns, origin = info.regionOrigin,
             region = *info.region,
             layers = regionLayers.at(info.region->coord),
             soil = regionSoils.at(info.region->coord), stored = info.stored] {
                auto result = std::make_shared<Chunk>();
                if (stored) {
                    try {
                        chunk_codec::decode(*stored, *result);
                        // Generated tiles are solid exactly up to the
                        // surface; anything else is an earlier edit.
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
                                        if (result->tiles[z][y][x].isSolid() !=
                                            (worldY <= surface)) {
                                            edited = true;
                                            break;
                                        }
                                    }
                        return GeneratedChunk{result, true, edited};
                    } catch (const std::exception& e) {
                        std::cerr << "Saved chunk " << coord.x << ',' << coord.y
                                  << ',' << coord.z << " is unreadable ("
                                  << e.what() << "); regenerating\n";
                    }
                }
                generator.generateChunk(*result, coord, origin, region, layers,
                                        soil, columns.get());
                return GeneratedChunk{result, false};
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
    for (const auto& o : offsets) {
        const int64_t x = int64_t{coord.x} + o[0];
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

const TerrainNeighborhood& ChunkManager::terrainNeighborhood(Vec2u region) {
    auto it = regionTerrain.find(region);
    if (it == regionTerrain.end())
        it = regionTerrain
                 .emplace(region, TerrainNeighborhood::capture(
                                      terrainWorld, region, *soilAssets))
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
    const Vec2u rc{tileX / TerrainTuning::RegionX,
                   tileZ / TerrainTuning::RegionZ};
    return TerrainSampler(terrainNeighborhood(rc))
        .sample(tileX, tileZ)
        .surface_y;
}
