#include "chunk_manager.hpp"

#include <climits>
#include <iostream>

#include "generated_hydrology.hpp"
#include "ground_cover_climate.hpp"
#include "save/chunk_codec.hpp"
#include "save/flora_codec.hpp"
#include "world_editor.hpp"

namespace {
using StructureSlab = std::shared_ptr<const structure_voxels::Slab>;

StructureSlab structureBaselineFor(std::optional<structure_voxels::Slab> slab,
                                   ChunkCoord coord,
                                   const ChunkColumnInputs* terrain,
                                   const GrownBlockSet& grown) {
    if (!slab) return nullptr;
    using namespace structure_voxels;
    for (int z = 0; z < Side; ++z)
        for (int y = 0; y < Side; ++y)
            for (int x = 0; x < Side; ++x) {
                const auto i = cellIndex(x, y, z);
                if (!slab->bits.test(i)) continue;
                const auto* column =
                    terrain ? &terrain->columns[z * Side + x] : nullptr;
                const int ceiling = column ? std::max(column->water.topTile(),
                                                      column->surface_y)
                                           : WaterSurface::sea(-1).topTile();
                if (int(coord.y) * Side + y <= ceiling ||
                    grown.test(slab->blockAt(i)))
                    slab->bits.reset(i);
            }
    return slab->bits.any() ? std::make_shared<const Slab>(std::move(*slab))
                            : nullptr;
}

StructureSlab decodeStructureBaseline(ByteView blob) {
    if (blob.empty()) return nullptr;
    Chunk chunk;
    chunk_codec::decode(blob, chunk);
    using namespace structure_voxels;
    auto result = std::make_shared<Slab>();
    result->blocks.resize(Cells);
    for (int z = 0; z < Side; ++z)
        for (int y = 0; y < Side; ++y)
            for (int x = 0; x < Side; ++x) {
                const auto block = chunk.tiles[z][y][x].block_id;
                if (!block) continue;
                const auto i = cellIndex(x, y, z);
                result->bits.set(i);
                result->blocks[i] = block;
            }
    return result->bits.any() ? result : nullptr;
}

template <typename Apply>
bool reconcileStructure(Chunk& chunk, const structure_voxels::Slab* previous,
                        const structure_voxels::Slab* current, Apply apply) {
    if (!previous && !current) return false;
    using namespace structure_voxels;
    bool changed = false;
    for (int z = 0; z < Side; ++z)
        for (int y = 0; y < Side; ++y)
            for (int x = 0; x < Side; ++x) {
                const auto i = cellIndex(x, y, z);
                const uint8_t old = previous ? previous->tileAt(i) : 0;
                const uint8_t next = current ? current->tileAt(i) : 0;
                if (old == next) continue;
                changed = true;
                auto& tile = chunk.tiles[z][y][x];
                if (old ? tile.block_id == old : tile.isAir())
                    apply(x, y, z, tile, next);
            }
    return changed;
}

bool rebaseStructure(Chunk& chunk, const structure_voxels::Slab* previous,
                     const structure_voxels::Slab* current) {
    return reconcileStructure(chunk, previous, current,
                              [](int, int, int, Tile& tile, uint8_t block) {
                                  tile.block_id = block;
                                  tile.clearCover();
                              });
}

std::vector<TileBox> changedStructureBoxes(
    const std::vector<structure_voxels::Kind>& older,
    const std::vector<structure_voxels::Kind>& current) {
    std::unordered_map<uint64_t, structure_voxels::SourcePtr> before;
    for (const auto& kind : older)
        for (const auto& entry : kind.instances())
            before.emplace(entry.source->key, entry.source);
    std::vector<TileBox> changed;
    for (const auto& kind : current)
        for (const auto& entry : kind.instances()) {
            const auto& source = *entry.source;
            const auto old = before.find(source.key);
            if (old == before.end()) {
                changed.push_back(source.bounds);
                continue;
            }
            if (source.revision != old->second->revision ||
                source.bounds != old->second->bounds) {
                if (auto local = source.changedSince(*old->second)) {
                    if (!local->empty()) changed.push_back(*local);
                } else {
                    changed.push_back(old->second->bounds);
                    changed.push_back(source.bounds);
                }
            }
            before.erase(old);
        }
    for (const auto& [key, source] : before) changed.push_back(source->bounds);
    return changed;
}
}  // namespace

ChunkManager::ChunkManager(
    const LocationGenerator& gen, ThreadPool& pool, const ChunkMesher* mesher,
    const World& world, IAssets* assets,
    std::optional<std::vector<TreeLandmark>> preparedLandmarks)
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
    treeSeed = treeGenerationSeed(world);
    landmarks = preparedLandmarks
                    ? std::move(*preparedLandmarks)
                    : placeTreeLandmarks(*treeTable, world, treeSeed);
    for (const auto& site : landmarks)
        landmarkRegions.insert({unsigned(site.x / TerrainTuning::RegionX),
                                unsigned(site.z / TerrainTuning::RegionZ)});
    // A chunk `extent` tiles from an anchor is at most this many regions
    // away (plus a margin for clumps beyond the declared crown).
    landmarkRadius =
        std::max(2, int(std::ceil((treeTable->maxLandmarkExtent() + 16) /
                                  std::min(TerrainTuning::RegionX,
                                           TerrainTuning::RegionZ))));
    const size_t threads = std::max<size_t>(1, pool.get_thread_count());
    maxGenerationJobs = std::clamp<size_t>(threads * 2, 4, 32);
    maxMeshJobs = std::clamp<size_t>(threads, 2, 16);
    maxTreeJobs = std::clamp<size_t>(threads / 2, 2, 8);
}

ChunkManager::~ChunkManager() {
    for (auto& job : generationJobs) job.future.wait();
    for (auto& job : meshJobs) job.future.wait();
    for (auto& job : treeJobs) job.future.wait();
    for (auto& job : structureJobs) job.future.wait();
    for (auto& job : billboardJobs) job.future.wait();
}

void ChunkManager::registerChunk(const ChunkCoord& coord,
                                 const ChunkCoord& origin, const Region* region,
                                 ChunkBlob stored, ChunkBlob storedFlora,
                                 ChunkBlob storedBaseline) {
    if (chunks.contains(coord)) unregister(coord);
    auto& handle = chunks[coord];
    handle.identity = ++nextIdentity;
    handle.regionCoord = region->coord;
    pending[coord] = {origin,
                      region,
                      std::nullopt,
                      std::move(stored),
                      std::move(storedFlora),
                      std::move(storedBaseline)};
    if (!regionLayers.contains(region->coord))
        regionLayers[region->coord] = layersGenerator.get(*region);
    if (!regionSoils.contains(region->coord))
        regionSoils[region->coord] =
            SoilGenerator::select(*region, *soilAssets, soilSeed);
    const uint64_t column = (uint64_t{coord.x} << 32) | coord.z;
    // The surface ceiling blends across region borders
    // (TerrainSampler::surface), so the top bound is the highest ceiling of
    // the region and its neighbours.
    int ceiling = TerrainTuning::surfaceCeiling(region->land_height_m);
    if (!terrainWorld.regions.empty()) {
        const auto wrapX = wrap();
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const int64_t z = int64_t(region->coord.y) + dz;
                int64_t x = int64_t(region->coord.x) + dx;
                if (z < 0 || z >= terrainWorld.height) continue;
                if (wrapX.enabled())
                    x = wrapX.wrapRegion(x);
                else if (x < 0 || x >= terrainWorld.width)
                    continue;
                ceiling = std::max(
                    ceiling,
                    TerrainTuning::surfaceCeiling(
                        terrainWorld.regions[size_t(z) * terrainWorld.width + x]
                            .land_height_m));
            }
    }
    if (terrainWorld.hydrology)
        ceiling = std::max(ceiling, terrainWorld.hydrology->ceiling(
                                        region->coord.y * terrainWorld.width +
                                        region->coord.x));
    columnTopY.try_emplace(column, ceiling);
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
const structure_voxels::Slab* ChunkManager::structureBaseline(
    const ChunkCoord& coord) const {
    const auto it = chunks.find(coord);
    return it == chunks.end() ? nullptr : it->second.structureBaseline.get();
}
std::optional<ChunkManager::ColumnSources> ChunkManager::columnSources(
    unsigned chunkX, unsigned chunkZ) {
    if (terrainWorld.regions.empty()) return std::nullopt;
    const auto it = chunks.find({chunkX, 0, chunkZ});
    if (it == chunks.end()) return std::nullopt;
    const auto rc = it->second.regionCoord;
    const auto& region = terrainWorld.regions[rc.y * terrainWorld.width + rc.x];
    auto trees = prepareTrees({chunkX, 0, chunkZ}, rc);
    if (!trees) return std::nullopt;
    return ColumnSources{prepareColumns({chunkX, 0, chunkZ}, region),
                         prepareStructures({chunkX, 0, chunkZ}, *trees),
                         !trees->trees.empty()};
}

void ChunkManager::setGeneratedStructureKinds(StructureKinds kinds) {
    auto effective = std::make_shared<const StructureKinds>(
        structure_voxels::applyOverrides(kinds, savedStructures));
    generatedKinds = std::move(kinds);
    installStructureKinds(std::move(effective));
}

void ChunkManager::setStructureOverrides(structure_voxels::Overrides value) {
    if (savedStructures == value) return;
    auto effective = std::make_shared<const StructureKinds>(
        structure_voxels::applyOverrides(generatedKinds, value));
    savedStructures = std::move(value);
    ++overrideRevision;
    installStructureKinds(std::move(effective));
}

void ChunkManager::installStructureKinds(
    std::shared_ptr<const StructureKinds> kinds) {
    const auto changed = changedStructureBoxes(*structureKinds, *kinds);
    structureKinds = std::move(kinds);
    ++structureRevision;
    if (changed.empty()) return;
    const auto wrapX = wrap();
    const auto overlaps = [&](const TileBox& box, unsigned cx, unsigned cz) {
        const int x = int(cx) * Chunk::SIZE_X, z = int(cz) * Chunk::SIZE_Z;
        if (box.z0 >= z + Chunk::SIZE_Z || box.z1 <= z) return false;
        if (!wrapX.enabled()) return box.x0 < x + Chunk::SIZE_X && box.x1 > x;
        const auto dx = wrapX.wrapTile(int64_t(box.x0) - x);
        return dx < Chunk::SIZE_X || dx + box.sizeX() > wrapX.tiles;
    };
    for (auto& [key, column] : columnInputs) {
        const auto cx = unsigned(key >> 32), cz = unsigned(key);
        bool dirty = false;
        for (const auto& box : changed) {
            if (!overlaps(box, cx, cz)) continue;
            dirty = true;
            const int begin = std::max(0, box.y0 / Chunk::SIZE_Y);
            const int end = std::min(
                Y_CHUNKS, (box.y1 + Chunk::SIZE_Y - 1) / Chunk::SIZE_Y);
            for (int y = begin; y < end; ++y) {
                const ChunkCoord coord{cx, unsigned(y), cz};
                if (isReady(coord)) rebaseQueue.insert(coord);
            }
        }
        if (!dirty) continue;
        column.structures.reset();
        columnStructuresChanged.emit(key);
    }
}

void ChunkManager::appendStructureLandmarks(
    std::vector<structure_voxels::SourcePtr>& sources, const Vec3& eye,
    float range) const {
    for (const auto& kind : *structureKinds)
        kind.appendLandmarks(sources, wrap(), eye, range);
}

std::function<ChunkManager::ColumnSources()>
ChunkManager::snapshotColumnSources(unsigned cx, unsigned cz) {
    const Vec2u region{cx / CHUNKS_PER_REGION, cz / CHUNKS_PER_REGION};
    if (terrainWorld.regions.empty() ||
        region.x >= unsigned(terrainWorld.width) ||
        region.y >= unsigned(terrainWorld.height))
        return {};

    struct TreeInput {
        Vec2u region;
        std::shared_ptr<const TreeSet> set;
        std::vector<TreeLandmark> landmarks;
    };
    std::vector<TreeInput> inputs;
    if (!treeTable->empty())
        for (const auto q : treeRegionsAround(region)) {
            const auto found = treeSets.find(q);
            if (found != treeSets.end())
                inputs.push_back({q, found->second, {}});
            else
                inputs.push_back({q, {}, landmarksNear(q)});
        }
    return [world = &terrainWorld, assets = soilAssets, table = treeTable,
            seed = treeSeed, overrides = overrides, inputs = std::move(inputs),
            kinds = structureKinds, day = floraDay, region, cx, cz]() mutable {
        const auto terrain =
            TerrainNeighborhood::capture(*world, region, *assets);
        std::vector<const TreeSet*> sets;
        for (auto& input : inputs) {
            if (!input.set) {
                const auto halo =
                    TerrainNeighborhood::capture(*world, input.region, *assets);
                const RegionTreesInput treeInput{
                    table.get(),      &halo,          input.region, seed,
                    &input.landmarks, overrides.get()};
                input.set = std::make_shared<const TreeSet>(
                    generateRegionTrees(treeInput));
            }
            sets.push_back(input.set.get());
        }
        return ColumnSources{
            std::make_shared<const ChunkColumnInputs>(
                TerrainSampler(terrain).columns(cx, cz)),
            tree_wood_lod::columnVoxels(
                cx, cz, makeTreeColumn(cx, cz, sets, world->wrap().tiles),
                *table, day, world->wrap(), *kinds)};
    };
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
const RootVoxel* ChunkManager::rootAt(int x, int y, int z) {
    x = int(wrap().wrapTile(x));
    if (x < 0 || y < 0 || z < 0) return nullptr;
    const auto* chunk =
        getChunk({unsigned(x / Chunk::SIZE_X), unsigned(y / Chunk::SIZE_Y),
                  unsigned(z / Chunk::SIZE_Z)});
    return chunk ? chunk->rootAt(Chunk::cell(
                       x % Chunk::SIZE_X, y % Chunk::SIZE_Y, z % Chunk::SIZE_Z))
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
            columnReleased.emit(column);
        }
    }
    pending.erase(coord);
    generationQueue.erase(coord);
    rebaseQueue.erase(coord);
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
            {region,
             pool.submit_task(
                 [world = &terrainWorld, assets = soilAssets, table = treeTable,
                  region, seed = treeSeed, near = landmarksNear(region),
                  overrides = overrides] {
                     const auto terrain =
                         TerrainNeighborhood::capture(*world, region, *assets);
                     RegionTreesInput input{table.get(), &terrain, region, seed,
                                            &near};
                     input.overrides = overrides.get();
                     return regionBillboards(input);
                 },
                 BS::pr::low),
             overrides});
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
        if (!treeRequests.contains(region) &&
            std::none_of(
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
    refreshTreeColumns({&oldBounds, &tree->bounds});
    ++treeRevision_;
}

void ChunkManager::removeTree(const TreeInstancePtr& tree) {
    landmarkCache.erase(tree->key);
    const Vec2u region{unsigned(tree->anchor.x / TerrainTuning::RegionX),
                       unsigned(tree->anchor.z / TerrainTuning::RegionZ)};
    if (const auto it = treeSets.find(region); it != treeSets.end()) {
        auto set = std::make_shared<TreeSet>(*it->second);
        std::erase_if(*set, [&](const TreeInstancePtr& t) {
            return t->key == tree->key;
        });
        it->second = std::move(set);
    }
    // Its billboard goes too (rebuilt on the renderer's next wish).
    billboardSets.erase(region);
    ++billboardRevision_;
    refreshTreeColumns({&tree->bounds});
    ++treeRevision_;
}

void ChunkManager::refreshTreeColumns(
    std::initializer_list<const TreeTileBox*> boxes) {
    // Rebuild the tree lists while their region sets are ready.
    const auto wrapX = wrap();
    for (const auto* box : boxes) {
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
                column->second.structures.reset();
                const auto handle =
                    chunks.find({unsigned(wx), 0, unsigned(cz)});
                if (handle != chunks.end())
                    prepareTrees({unsigned(wx), 0, unsigned(cz)},
                                 handle->second.regionCoord);
                columnStructuresChanged.emit(key);
            }
    }
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
void ChunkManager::snapshotColumn(unsigned cx, unsigned cz,
                                  RegionSnapshot& out) {
    std::lock_guard<std::mutex> lock(tileAtMutex);
    for (unsigned y = 0; y < Y_CHUNKS; ++y) {
        const ChunkCoord coord{cx, y, cz};
        auto it = chunks.find(coord);
        if (it == chunks.end() || it->second.state != ChunkState::Ready ||
            !it->second.persistDirty)
            continue;
        it->second.persistDirty = false;
        const auto index = RegionData::localIndex(coord);
        out.chunks.emplace_back(index,
                                std::shared_ptr<const Chunk>(it->second.data));
        out.flora.emplace_back(index, it->second.flora);
        out.trees.emplace_back(index, it->second.trees);
        out.structureBaselines.emplace_back(index,
                                            it->second.structureBaseline);
    }
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
                snapshot.structureBaselines.emplace_back(
                    index, it->second.structureBaseline);
            }
    return snapshot;
}
void ChunkManager::markAllDirty() {
    for (auto& [coord, handle] : chunks)
        if (handle.state == ChunkState::Ready) markDirty(coord);
}
int ChunkManager::getColumnTopY(unsigned x, unsigned z) const {
    auto it = columnTopY.find((uint64_t{x} << 32) | z);
    return it == columnTopY.end()
               ? WORLD_HEIGHT_Y - 1
               : std::max(it->second, getStructureTopY(x, z));
}
int ChunkManager::getStructureTopY(unsigned x, unsigned z) const {
    const auto it = columnInputs.find((uint64_t{x} << 32) | z);
    return it == columnInputs.end() || !it->second.structures
               ? INT_MIN
               : it->second.structures->projectedTop().value_or(INT_MIN);
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
    const auto column = columnInputs.find((uint64_t{coord.x} << 32) | coord.z);
    const auto terrain =
        column == columnInputs.end() ? nullptr : column->second.data;
    const auto structures =
        column == columnInputs.end() ? nullptr : column->second.structures;
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
         identity, revision, wrapX, depth = terrainWorld.height * 128, climate,
         terrain, structures, hydrology = terrainWorld.hydrology]() {
            const unsigned aboveY = coord.y + 1;
            const std::optional<structure_voxels::Slab> above =
                !neighbors.contains({coord.x, aboveY, coord.z}) && structures &&
                        aboveY < Y_CHUNKS && structures->mayContain(aboveY)
                    ? structures->rasterize(aboveY)
                    : std::nullopt;
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
                climate,
                [&](int x, int y, int z) -> const Tile* {
                    if (!terrain) return nullptr;
                    static const Tile solid{.block_id = 1, .lightLevel = {}};
                    static const Tile water{.liquid = LiquidType::Water,
                                            .liquidLevel = 255,
                                            .lightLevel = {}};
                    static const Tile air{};
                    const int lx = x % Chunk::SIZE_X, lz = z % Chunk::SIZE_Z;
                    const auto& column =
                        terrain->columns[lz * Chunk::SIZE_X + lx];
                    if (y <= column.surface_y) return &solid;
                    if (above &&
                        above->tileAt(structure_voxels::cellIndex(lx, 0, lz)))
                        return &solid;
                    return column.water.fills(y, column.surface_y) ? &water
                                                                   : &air;
                },
                [&](int x, int z) -> std::pair<int, WaterSurface> {
                    const int lx = x - int(coord.x) * Chunk::SIZE_X;
                    const int lz = z - int(coord.z) * Chunk::SIZE_Z;
                    if (terrain && lx >= 0 && lx < Chunk::SIZE_X && lz >= 0 &&
                        lz < Chunk::SIZE_Z) {
                        const auto& column =
                            terrain->columns[lz * Chunk::SIZE_X + lx];
                        return {column.surface_y, column.water};
                    }
                    return hydrology ? hydrology->sampleRiverColumn(x, z)
                                     : std::pair<int, WaterSurface>{-1, {}};
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
    for (size_t i = 0; i < billboardJobs.size();) {
        if (billboardJobs[i].future.wait_for(0s) != std::future_status::ready) {
            ++i;
            continue;
        }
        auto job = std::move(billboardJobs[i]);
        billboardJobs.erase(billboardJobs.begin() + i);
        // A tree was removed meanwhile: the renderer asks again.
        if (job.overrides != overrides) continue;
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
        ++treeSetsRevision_;
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
                    it->second.uploaded ? MeshState::Built : MeshState::None;
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

void ChunkManager::estimateSkyLight(
    Chunk& chunk, ChunkCoord coord,
    const std::function<std::pair<int, WaterSurface>(int x, int z)>& surface) {
    const int base = int(coord.y) * Chunk::SIZE_Y;
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int x = 0; x < Chunk::SIZE_X; ++x) {
            // Open sky above the chunk: its top is above the column's
            // surface. Water seen from the top keeps the sweep's falloff
            // (opacity 8: 15, 7, then dark).
            const auto [ground, water] = surface(x, z);
            const int top = base + Chunk::SIZE_Y - 1;
            if (top <= ground) continue;
            const Tile& first = chunk.tiles[z][Chunk::SIZE_Y - 1][x];
            const int above =
                first.isLiquid() ? std::max(0, water.topTile() - top) : 0;
            int value = std::max(0, 15 - 8 * above);
            for (int y = Chunk::SIZE_Y - 1; y >= 0 && value > 0; --y) {
                Tile& tile = chunk.tiles[z][y][x];
                if (tile.isSolid()) break;
                if (tile.lightLevel.sun_exposure < value)
                    tile.lightLevel.sun_exposure = uint8_t(value);
                if (tile.isLiquid()) value = std::max(0, value - 8);
            }
        }
}

void ChunkManager::publishGenerated(ChunkCoord coord, uint64_t identity,
                                    GeneratedChunk data) {
    auto it = chunks.find(coord);
    if (it == chunks.end() || it->second.identity != identity) return;
    const uint64_t key = (uint64_t{coord.x} << 32) | coord.z;
    const auto column = columnInputs.find(key);
    if (data.structuresUsed) {
        if (column == columnInputs.end() ||
            column->second.structures != data.structuresUsed) {
            generationQueue.insert(coord);
            return;
        }
    }
    // Sunlight sweeps can lag far behind generation (wide chunk radii, fast
    // travel): start from an estimate so a new chunk never shows unlit
    // (black) until its sweep arrives; the sweep then refines it.
    if (data.data) {
        const auto* inputs =
            column != columnInputs.end() ? column->second.data.get() : nullptr;
        const int top = getColumnTopY(coord.x, coord.z);
        estimateSkyLight(*data.data, coord, [&](int x, int z) {
            if (!inputs) return std::pair{top, WaterSurface::sea(top)};
            const auto& sample = inputs->columns[z * Chunk::SIZE_X + x];
            return std::pair{sample.surface_y, sample.water};
        });
    }
    {
        std::lock_guard<std::mutex> lock(tileAtMutex);
        it->second.data = std::move(data.data);
        it->second.state = ChunkState::Ready;
        it->second.persistDirty = !data.fromDisk || data.floraRegrown ||
                                  data.treesPlaced || data.baselineChanged;
        it->second.edited = data.edited;
        it->second.structureBaseline = std::move(data.structureBaseline);
        it->second.flora = std::move(data.flora);
        it->second.trees = data.trees;
    }
    pending.erase(coord);
    markDirty(coord);
    becameReady.emit(coord);
    if (data.baselineChanged) structureRebased.emit(coord);
}

void ChunkManager::dispatchGeneration() {
    if (generationQueue.empty()) return;
    auto started = std::chrono::steady_clock::now();
    const auto anchor = [&](ChunkCoord coord) {
        const auto target = pending.at(coord).beneficiary.value_or(coord);
        // A camera moving into a former sky prerequisite must not demote
        // that chunk to the old beneficiary's distant height.
        return generationQueue.distance(coord, focus) <
                       generationQueue.distance(target, focus)
                   ? coord
                   : target;
    };
    const auto order = generationQueue.nearest(focus, anchor, DispatchWindow);
    started = std::chrono::steady_clock::now();
    // Whether the tree sets around a region are built, once per region and
    // update: with a landmark in the world a chunk reads sets up to
    // landmarkRadius regions away (hundreds of lookups), and the queue
    // holds thousands of chunks of a few regions.
    struct RegionTrees {
        bool ready = false, asked = false;
    };
    std::unordered_map<uint64_t, RegionTrees> regionReady;
    const auto regionState = [&](ChunkCoord c, Vec2u region) -> RegionTrees* {
        const uint64_t column = (uint64_t{c.x} << 32) | c.z;
        if (const auto it = columnInputs.find(column);
            it != columnInputs.end() && it->second.trees)
            return nullptr;  // the column is built
        const auto [it, inserted] =
            regionReady.try_emplace((uint64_t{region.x} << 32) | region.y);
        if (inserted) it->second.ready = treesReady(c, region);
        return &it->second;
    };
    const auto treesReadyMemo = [&](ChunkCoord c, Vec2u region) {
        const auto* state = regionState(c, region);
        return !state || state->ready;
    };
    size_t next = 0;
    int air = 0;
    // The starvation guard picks the oldest chunk at most once per update
    // and only among chunks whose tree sets are built: a chunk waiting for
    // trees would otherwise be picked again and again (generationDispatches
    // only grows on a submit), spending the whole budget without progress.
    uint64_t oldestTriedAt = UINT64_MAX;
    // Past the budget, picks go on until one chunk made progress (while a
    // job slot is free), so an update never ends empty-handed.
    bool progressed = false;
    while (std::chrono::steady_clock::now() - started < dispatchBudget ||
           (!progressed && generationJobs.size() < maxGenerationJobs)) {
        const bool slots = generationJobs.size() < maxGenerationJobs;
        if (!slots && air >= AirChunksPerUpdate) break;
        std::optional<ChunkCoord> picked;
        bool oldest = false;
        if (slots && (generationDispatches + 1) % 16 == 0 &&
            oldestTriedAt != generationDispatches) {
            oldestTriedAt = generationDispatches;
            picked = generationQueue.best(
                focus, true,
                [&](ChunkCoord c) {
                    return treesReadyMemo(c, pending.at(c).region->coord);
                },
                anchor);
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
        // Waiting for tree sets: ask for them once per region and move on
        // before computing the column (a whole window of waiting chunks
        // used to sample its terrain every update).
        if (auto* state = regionState(coord, info.region->coord);
            state && !state->ready) {
            if (!state->asked) prepareTrees(coord, info.region->coord);
            state->asked = true;
            continue;
        }
        const auto columns = prepareColumns(coord, *info.region);
        // Trees first: a chunk waits until the tree sets of the regions
        // around it are built (it stays queued meanwhile).
        const auto trees = prepareTrees(coord, info.region->coord);
        if (!trees) continue;
        const auto structures = prepareStructures(coord, *trees);
        const TreeStamp stamp{floraDay, TreeVersion};
        const bool terrainAir =
            columns && generator.isAirChunk(coord, *columns);
        if (!info.stored && terrainAir && !structures->mayContain(coord.y) &&
            structures->projectedTop()) {
            // The oldest pick always proceeds so the starvation guard cannot
            // spin on an air chunk.
            if (air >= AirChunksPerUpdate && !oldest) continue;
            ++air;
            generationQueue.erase(coord);
            GeneratedChunk empty{std::make_shared<Chunk>(), false, false,
                                 nullptr};
            empty.trees = stamp;
            publishGenerated(coord, identity, std::move(empty));
            progressed = true;
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
             storedFlora = info.storedFlora,
             savedBaseline = info.storedBaseline, terrain, rules = floraTable,
             seed = floraSeed_, day = floraDay, trees, treeRules = treeTable,
             structures, hasOtherKinds = !structureKinds->empty(), terrainAir,
             stamp] {
                // The first worker for this column computes its actual top
                // before any chunk is published. Sky jobs share the projection.
                if (!structures->empty()) structures->skyTops();
                auto slab = !stored || hasOtherKinds
                                ? structures->rasterize(coord.y)
                                : std::optional<structure_voxels::Slab>{};
                const auto baseline =
                    hasOtherKinds
                        ? structureBaselineFor(slab, coord, columns.get(),
                                               treeRules->grown())
                        : nullptr;
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
                        // Terrain retains its solid/air comparison. In
                        // buildable air the structure's block ID is the
                        // baseline.
                        const auto& grown = treeRules->grown();
                        const bool reshapeRoots = result->legacyRootShape;
                        if (reshapeRoots) result->restoreRootSubstrate(grown);
                        bool baselineChanged = !savedBaseline && baseline;
                        if (savedBaseline) {
                            try {
                                const auto previous =
                                    decodeStructureBaseline(*savedBaseline);
                                baselineChanged = rebaseStructure(
                                    *result, previous.get(), baseline.get());
                            } catch (const SerializationError& e) {
                                std::cerr << "Ignoring unreadable structure "
                                             "baseline: "
                                          << e.what() << '\n';
                                baselineChanged = true;
                            }
                        }
                        bool edited = false;
                        if (columns)
                            for (int z = 0; z < Chunk::SIZE_Z && !edited; ++z)
                                for (int y = 0; y < Chunk::SIZE_Y && !edited;
                                     ++y)
                                    for (int x = 0; x < Chunk::SIZE_X; ++x) {
                                        const auto& column =
                                            columns->columns[z * Chunk::SIZE_X +
                                                             x];
                                        const int surface = column.surface_y;
                                        const int worldY =
                                            int(coord.y) * Chunk::SIZE_Y + y;
                                        const Tile& tile =
                                            result->tiles[z][y][x];
                                        const auto* root = result->rootAt(
                                            Chunk::cell(x, y, z));
                                        const auto actual =
                                            root ? root->block
                                            : grown.test(tile.block_id)
                                                ? 0
                                                : tile.block_id;
                                        const auto expected =
                                            baseline
                                                ? baseline->tileAt(
                                                      structure_voxels::
                                                          cellIndex(x, y, z))
                                                : 0;
                                        const auto liquid =
                                            !expected && column.water.fills(
                                                             worldY, surface)
                                                ? LiquidType::Water
                                                : LiquidType::None;
                                        if ((worldY <= surface
                                                 ? actual == 0
                                                 : actual != expected) ||
                                            tile.liquid != liquid) {
                                            edited = true;
                                            break;
                                        }
                                    }
                        GeneratedChunk out{result, true, edited, nullptr};
                        out.structuresUsed = structures;
                        out.baselineChanged = baselineChanged;
                        const auto growRoots = [&](double since,
                                                   bool legacyBark = false) {
                            if (!columns) return false;
                            const auto roots = treeRootChanges(
                                *result, coord, *trees, *treeRules, *columns,
                                since, day, legacyBark);
                            applyTreeWoodChanges(*result, roots);
                            return !roots.empty();
                        };
                        // Chunks saved before trees existed get whole trees
                        // once; later ones catch up on growth and on cuts
                        // made while they were unloaded (only wood of the
                        // same tree generator version).
                        const auto addTrees = [&] {
                            if (reshapeRoots) {
                                growRoots(-INFINITY);
                                out.treesPlaced = true;
                            }
                            if (!out.trees.placed()) {
                                placeTreeWood(*result, coord, *trees,
                                              *treeRules, day);
                                if (!reshapeRoots) growRoots(-INFINITY);
                                out.trees = stamp;
                                out.treesPlaced = true;
                                return;
                            }
                            // The same day still catches up on cuts: the
                            // chunk may have been saved earlier that day,
                            // before the cut (its wood would stay, unseen
                            // high up, and shade the ground below).
                            const bool legacyBark =
                                out.trees.version == TreeBarkOnlyVersion;
                            const bool addRootCollar =
                                out.trees.version ==
                                TreeBeforeRootCollarVersion;
                            if ((!legacyBark && !addRootCollar &&
                                 out.trees.version != TreeVersion) ||
                                out.trees.day > day)
                                return;
                            const auto changes = treeWoodChanges(
                                *result, coord, *trees, *treeRules,
                                out.trees.day, day, legacyBark);
                            applyTreeWoodChanges(*result, changes);
                            const bool backfill = legacyBark || addRootCollar;
                            const bool rootsChanged =
                                !reshapeRoots &&
                                growRoots(backfill ? -INFINITY : out.trees.day,
                                          backfill);
                            out.treesPlaced = reshapeRoots || rootsChanged ||
                                              legacyBark || !changes.empty();
                            out.trees = stamp;
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
                                out.structureBaseline = baseline;
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
                        out.structureBaseline = baseline;
                        return out;
                    } catch (const std::exception& e) {
                        std::cerr << "Saved chunk " << coord.x << ',' << coord.y
                                  << ',' << coord.z << " is unreadable ("
                                  << e.what() << "); regenerating\n";
                    }
                }
                if (!terrainAir)
                    generator.generateChunk(*result, coord, origin, region,
                                            layers, soil, columns.get());
                if (columns && !terrainAir)
                    applyTreeWoodChanges(
                        *result,
                        treeRootChanges(*result, coord, *trees, *treeRules,
                                        *columns, -INFINITY, day));
                if (stored && !hasOtherKinds)
                    slab = structures->rasterize(coord.y);
                if (slab) structure_voxels::stamp(*result, *slab);
                GeneratedChunk out{result, false, false, plants(nullptr)};
                out.trees = stamp;
                out.structuresUsed = structures;
                out.structureBaseline = baseline;
                return out;
            },
            info.beneficiary ? BS::pr::high : BS::pr::normal);
        generationJobs.push_back({coord, identity, std::move(future)});
        generationQueue.erase(coord);
        ++generationDispatches;
        progressed = true;
    }
}

void ChunkManager::withdrawOutside(ChunkCoord centre, int radius) {
    const auto wrapX = wrap();
    const auto inside = [&](ChunkCoord c) {
        const int64_t dx = wrapX.enabled() ? wrapX.deltaChunk(c.x, centre.x)
                                           : int64_t(c.x) - int64_t(centre.x);
        return std::abs(dx) <= radius &&
               std::abs(int64_t(c.y) - int64_t(centre.y)) <= radius &&
               std::abs(int64_t(c.z) - int64_t(centre.z)) <= radius;
    };
    std::vector<ChunkCoord> stale;
    generationQueue.forEach([&](ChunkCoord c) {
        if (inside(c)) return;
        const auto& info = pending.at(c);
        if (info.beneficiary && inside(*info.beneficiary)) return;
        stale.push_back(c);
    });
    for (const auto c : stale) {
        generationQueue.erase(c);
        auto& handle = chunks.at(c);
        if (handle.state == ChunkState::Queued)
            handle.state = ChunkState::Unloaded;
        pending.at(c).beneficiary.reset();
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
    auto started = std::chrono::steady_clock::now();
    const auto eligible = [&](ChunkCoord coord) { return canMesh(coord); };
    const auto order = meshQueue.nearest(
        focus, [](ChunkCoord coord) { return coord; }, DispatchWindow);
    started = std::chrono::steady_clock::now();
    size_t next = 0;
    bool progressed = false;
    while (std::chrono::steady_clock::now() - started < dispatchBudget ||
           (!progressed && meshJobs.size() < maxMeshJobs)) {
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
            progressed = true;
            continue;
        }
        if (!slots) continue;
        submitMesh(*picked);
        ++meshDispatches;
        progressed = true;
    }
}

void ChunkManager::updateStructureBaselines(WorldEditor* editor) {
    using namespace std::chrono_literals;
    using namespace structure_voxels;
    for (size_t i = 0; i < structureJobs.size();) {
        if (structureJobs[i].future.wait_for(0s) != std::future_status::ready) {
            ++i;
            continue;
        }
        auto job = std::move(structureJobs[i]);
        structureJobs.erase(structureJobs.begin() + i);
        const auto found = chunks.find(job.coord);
        if (found == chunks.end() || found->second.identity != job.identity)
            continue;
        if (job.revision != structureRevision) {
            rebaseQueue.insert(job.coord);
            continue;
        }
        StructureSlab baseline;
        try {
            baseline = job.future.get();
        } catch (const std::exception& error) {
            std::cerr << "Structure revision failed: " << error.what() << '\n';
            rebaseQueue.insert(job.coord);
            continue;
        }
        const auto& old = found->second.structureBaseline;
        if (!old && !baseline) continue;
        std::vector<EditRequest> edits;
        bool changed = false;
        withChunkLocked(job.coord, [&](Chunk& chunk) {
            changed = reconcileStructure(
                chunk, old.get(), baseline.get(),
                [&](int x, int y, int z, Tile& tile, uint8_t block) {
                    if (editor) {
                        edits.push_back({{int(job.coord.x) * Side + x,
                                          int(job.coord.y) * Side + y,
                                          int(job.coord.z) * Side + z},
                                         EditMode::Replace,
                                         block});
                    } else {
                        tile.block_id = block;
                        tile.clearCover();
                    }
                });
        });
        if (!changed) continue;
        found->second.structureBaseline = std::move(baseline);
        markModified(job.coord);
        if (editor && !edits.empty()) editor->apply(edits);
        structureRebased.emit(job.coord);
    }

    if (rebaseQueue.empty() || structureJobs.size() >= 2) return;
    rebaseQueue.setPeriodX(wrap().tiles);
    for (const auto& coord : rebaseQueue.ordered(focus, false)) {
        if (structureJobs.size() >= 2) break;
        const auto found = chunks.find(coord);
        if (found == chunks.end() || found->second.state != ChunkState::Ready) {
            rebaseQueue.erase(coord);
            continue;
        }
        const auto trees = prepareTrees(coord, found->second.regionCoord);
        if (!trees) continue;
        const auto column = prepareStructures(coord, *trees);
        const auto terrain =
            columnInputs.at((uint64_t{coord.x} << 32) | coord.z).data;
        rebaseQueue.erase(coord);
        if (!found->second.structureBaseline &&
            (structureKinds->empty() || !column->mayContain(coord.y)))
            continue;
        structureJobs.push_back(
            {coord, found->second.identity, structureRevision,
             pool.submit_task([column, coord, terrain, table = treeTable,
                               hasKinds = !structureKinds->empty()] {
                 return hasKinds ? structureBaselineFor(
                                       column->rasterize(coord.y), coord,
                                       terrain.get(), table->grown())
                                 : nullptr;
             })});
    }
}

void ChunkManager::update(WorldEditor* editor) {
    generationQueue.setPeriodX(wrap().tiles);
    meshQueue.setPeriodX(wrap().tiles);
    collect();
    dispatchGeneration();
    dispatchTreeJobs();
    updateStructureBaselines(editor);
    dispatchMeshes();
}

MeshHandle* ChunkManager::getWaterMesh(const ChunkCoord& coord) {
    const auto it = chunks.find(coord);
    return it != chunks.end() && it->second.hasWaterGeometry
               ? &it->second.waterMesh
               : nullptr;
}

std::span<const float> ChunkManager::uploadedWaterTops(
    const ChunkCoord& coord) const {
    const auto it = chunks.find(coord);
    if (it == chunks.end() || !it->second.uploaded) return {};
    return it->second.waterTops;
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
    if (std::none_of(treeJobs.begin(), treeJobs.end(),
                     [&](const TreeJob& job) { return job.region == region; }))
        treeRequests.insert(region);
    return nullptr;
}

void ChunkManager::dispatchTreeJobs() {
    if (treeRequests.empty()) return;
    const int width = terrainWorld.width;
    // Requests no registered region needs any more (the player moved on)
    // are dropped; landmark regions stay, they are wanted from afar.
    std::erase_if(treeRequests, [&](Vec2u q) {
        if (landmarkRegions.contains(q) || treeWishes.contains(q)) return false;
        for (const auto& [rc, count] : regionRegistrations) {
            const int64_t dx = WorldWrap::delta(rc.x, q.x, width);
            const int64_t dz = int64_t(rc.y) - int64_t(q.y);
            if (std::abs(dx) <= 2 && std::abs(dz) <= 2) return false;
        }
        return true;
    });
    if (treeJobs.size() >= maxTreeJobs || treeRequests.empty()) return;
    const auto wrapX = wrap();
    std::vector<std::pair<double, Vec2u>> order;
    order.reserve(treeRequests.size());
    for (const Vec2u q : treeRequests) {
        const double cx = (q.x + 0.5) * TerrainTuning::RegionX;
        const double cz = (q.y + 0.5) * TerrainTuning::RegionZ;
        const double dx = wrapX.deltaX(cx, focus.x), dz = cz - focus.z;
        order.push_back({dx * dx + dz * dz, q});
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        return std::tie(a.first, a.second.x, a.second.y) <
               std::tie(b.first, b.second.x, b.second.y);
    });
    for (const auto& [distance, region] : order) {
        if (treeJobs.size() >= maxTreeJobs) break;
        treeRequests.erase(region);
        // The neighbourhood is captured on the worker: World and assets are
        // read-only while chunks stream.
        treeJobs.push_back(
            {region,
             pool.submit_task(
                 [world = &terrainWorld, assets = soilAssets, table = treeTable,
                  region, seed = treeSeed, near = landmarksNear(region),
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
}

std::vector<TreeLandmark> ChunkManager::landmarksNear(Vec2u region) const {
    std::vector<TreeLandmark> near;
    for (const auto& site : landmarks) {
        const int64_t dx = WorldWrap::delta(site.x / TerrainTuning::RegionX,
                                            region.x, terrainWorld.width);
        const int64_t dz = site.z / TerrainTuning::RegionZ - int64_t(region.y);
        if (std::abs(dx) <= landmarkRadius && std::abs(dz) <= landmarkRadius)
            near.push_back(site);
    }
    return near;
}

std::vector<Vec2u> ChunkManager::treeRegionsAround(Vec2u region) const {
    std::vector<Vec2u> out;
    std::unordered_set<Vec2u> seen;
    const auto wrapX = wrap();
    // Ordinary trees reach one region from their anchor, including the
    // plant exclusion margin. Farther sets must hold a landmark.
    constexpr int ordinaryRadius =
        int((TreeMaxReach + TreeDiscMargin + WorldWrap::RegionSize - 1) /
            WorldWrap::RegionSize);
    for (int dz = -landmarkRadius; dz <= landmarkRadius; ++dz)
        for (int dx = -landmarkRadius; dx <= landmarkRadius; ++dx) {
            const int64_t z = int64_t(region.y) + dz;
            if (z < 0 || z >= terrainWorld.height) continue;
            if (!wrapX.enabled() &&
                (int64_t(region.x) + dx < 0 ||
                 int64_t(region.x) + dx >= terrainWorld.width))
                continue;
            const Vec2u q{unsigned(wrapX.enabled() ? wrapX.wrapRegion(
                                                         int64_t(region.x) + dx)
                                                   : int64_t(region.x) + dx),
                          unsigned(z)};
            const bool near = std::abs(dx) <= ordinaryRadius &&
                              std::abs(dz) <= ordinaryRadius;
            if (!near && !landmarkRegions.contains(q)) continue;
            if (!wrapX.enabled() || wrapX.regions() > 2 * landmarkRadius ||
                seen.insert(q).second)
                out.push_back(q);
        }
    return out;
}

bool ChunkManager::treesReady(ChunkCoord coord, Vec2u region) const {
    if (terrainWorld.regions.empty() || treeTable->empty()) return true;
    const uint64_t key = (uint64_t{coord.x} << 32) | coord.z;
    if (const auto it = columnInputs.find(key);
        it != columnInputs.end() && it->second.trees)
        return true;
    for (const Vec2u q : treeRegionsAround(region))
        if (!treeSets.contains(q)) return false;
    return true;
}

std::shared_ptr<const TreeColumn> ChunkManager::prepareTrees(ChunkCoord coord,
                                                             Vec2u region) {
    static const auto none = std::make_shared<const TreeColumn>();
    if (terrainWorld.regions.empty() || treeTable->empty()) return none;
    const uint64_t key = (uint64_t{coord.x} << 32) | coord.z;
    auto& cache = columnInputs.at(key);
    if (cache.trees) return cache.trees;
    std::vector<const TreeSet*> sets;
    bool ready = true;
    const auto wrapX = wrap();
    for (const Vec2u q : treeRegionsAround(region)) {
        const auto* set = regionTrees(q);
        if (!set) ready = false;
        sets.push_back(set);
    }
    if (!ready) return nullptr;
    cache.trees = std::make_shared<const TreeColumn>(
        makeTreeColumn(coord.x, coord.z, sets, wrapX.tiles));
    return cache.trees;
}

structure_voxels::ColumnPtr ChunkManager::prepareStructures(
    ChunkCoord coord, const TreeColumn& trees) {
    auto& cache = columnInputs.at((uint64_t{coord.x} << 32) | coord.z);
    if (!cache.structures || cache.structuresDay != floraDay) {
        cache.structures = tree_wood_lod::columnVoxels(
            coord.x, coord.z, trees, *treeTable, floraDay, wrap(),
            *structureKinds, [this](const tree_wood_lod::Source& tree) {
                auto& cached = treeVoxels[tree.tree->key];
                if (cached.tree == tree.tree.get() && cached.age == tree.age)
                    if (const auto source = cached.source.lock()) return source;
                auto source = tree_wood_lod::voxels(tree);
                cached = {tree.tree.get(), tree.age, source};
                return source;
            });
        cache.structuresDay = floraDay;
    }
    return cache.structures;
}

void ChunkManager::wantTreeSets(const std::vector<Vec2u>& regions) {
    treeWishes = {regions.begin(), regions.end()};
    for (const Vec2u q : regions) regionTrees(q);
    releaseTreeSets();
}

void ChunkManager::releaseTreeSets() {
    const int width = terrainWorld.width;
    const size_t before = treeSets.size();
    std::erase_if(treeSets, [&](const auto& entry) {
        const Vec2u q = entry.first;
        if (treeWishes.contains(q)) return false;
        // Chunks of registered regions read sets two regions around, and
        // landmark sets up to landmarkRadius.
        const int keep = landmarkRegions.contains(q) ? landmarkRadius : 2;
        for (const auto& [rc, count] : regionRegistrations) {
            const int64_t dx = WorldWrap::delta(rc.x, q.x, width);
            const int64_t dz = int64_t(rc.y) - int64_t(q.y);
            if (std::abs(dx) <= keep && std::abs(dz) <= keep) return false;
        }
        return true;
    });
    if (treeSets.size() != before) {
        ++treeSetsRevision_;
        std::erase_if(treeVoxels, [](const auto& entry) {
            return entry.second.source.expired();
        });
    }
}
