#include "chunk_manager.hpp"

#include <climits>
#include <iostream>

#include "generated_hydrology.hpp"
#include "ground_cover_climate.hpp"
#include "save/chunk_codec.hpp"
#include "save/flora_codec.hpp"
#include "terrain_edits.hpp"
#include "world_clock.hpp"
#include "world_editor.hpp"

namespace {
constexpr int DamSpillRadius = 40;

// A hydraulic survey can cross a slab boundary while the adjacent wall is
// unloaded. Read its saved solid additions once per slab during the survey.
class SavedWaterTerrain {
   public:
    explicit SavedWaterTerrain(const TerrainEdits* source)
        : edits(source ? source->snapshot() : nullptr) {}
    const ColumnEdit* column(int x, int z) const {
        if (!edits || x < 0 || z < 0) return nullptr;
        const auto it =
            edits->find(editKey(x / Chunk::SIZE_X, z / Chunk::SIZE_Z));
        return it == edits->end() ? nullptr : it->second.get();
    }
    bool addedSolid(TilePos p) {
        const auto* edit = column(p.x, p.z);
        if (!edit || p.y < 0 || p.y > edit->structureTopY()) return false;
        const auto* blob = edit->slab(uint16_t(p.y / Chunk::SIZE_Y));
        if (!blob || !*blob) return false;
        auto& slab = slabs[blob->get()];
        if (!slab) {
            slab = std::make_unique<Chunk>();
            chunk_codec::decode(**blob, *slab);
        }
        return slab
            ->tiles[p.z % Chunk::SIZE_Z][p.y % Chunk::SIZE_Y]
                   [p.x % Chunk::SIZE_X]
            .isSolid();
    }

   private:
    std::shared_ptr<const EditMap> edits;
    std::unordered_map<const Bytes*, std::unique_ptr<Chunk>> slabs;
};

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
                    grown.test(slab->blockAt(i))) {
                    slab->bits.reset(i);
                    slab->setShape(uint16_t(i), {});
                }
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
    chunk.forEachShape(
        [&](uint16_t cell, TileShape shape) { result->setShape(cell, shape); });
    return result->bits.any() ? result : nullptr;
}

bool differsFromGeneration(const Chunk& chunk, ChunkCoord coord,
                           const ChunkColumnInputs& terrain,
                           const Region& region,
                           const std::vector<RegionLayer>& layers,
                           const structure_voxels::Slab* structures,
                           const GrownBlockSet& grown) {
    const int baseY = int(coord.y) * Chunk::SIZE_Y;
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int x = 0; x < Chunk::SIZE_X; ++x) {
            const auto& column = terrain.columns[z * Chunk::SIZE_X + x];
            for (int y = 0; y < Chunk::SIZE_Y; ++y) {
                const int worldY = baseY + y;
                const auto cell = Chunk::cell(x, y, z);
                const auto actual = chunk.cellAt(x, y, z);
                const auto* root = chunk.rootAt(cell);
                const uint8_t block = root ? root->block
                                      : grown.test(actual.tile.block_id)
                                          ? 0
                                          : actual.tile.block_id;
                const auto shape = root    ? root->shape
                                   : block ? actual.shape
                                           : TileShape{};
                const bool ground = worldY <= column.surface_y;
                const uint8_t expected = ground
                                             ? LocationGenerator::terrainBlock(
                                                   region, layers, column.soil,
                                                   column.surface_y, worldY)
                                         : structures ? structures->tileAt(cell)
                                                      : 0;
                const auto expectedShape =
                    ground ? (worldY == column.surface_y ? column.groundShape
                                                         : TileShape{})
                    : structures ? structures->shapeAt(cell)
                                 : TileShape{};
                if (block != expected || shape != expectedShape) return true;

                const auto liquid =
                    !expected && column.water.fills(worldY, column.surface_y)
                        ? column.water.kind()
                        : LiquidType::None;
                // Projected water is derived from the separate water ledger.
                if (actual.tile.waterOrigin != WaterOrigin::Independent)
                    continue;
                if (chunk.projectsWater &&
                    (column.water.body || column.water.reach) &&
                    !actual.tile.isLiquid())
                    continue;
                if (actual.tile.liquid != liquid) return true;
            }
        }
    return false;
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
                const auto i = uint16_t(cellIndex(x, y, z));
                const uint8_t old = previous ? previous->tileAt(i) : 0;
                const uint8_t next = current ? current->tileAt(i) : 0;
                const auto oldShape =
                    previous ? previous->shapeAt(i) : TileShape{};
                const auto nextShape =
                    current ? current->shapeAt(i) : TileShape{};
                if (old == next && oldShape == nextShape) continue;
                changed = true;
                const auto& tile = chunk.tiles[z][y][x];
                if (old ? tile.block_id == old && chunk.shapeAt(i) == oldShape
                        : tile.isAir())
                    apply(x, y, z, tile, next, nextShape);
            }
    return changed;
}

bool rebaseStructure(Chunk& chunk, const structure_voxels::Slab* previous,
                     const structure_voxels::Slab* current) {
    return reconcileStructure(
        chunk, previous, current,
        [&](int x, int y, int z, Tile tile, uint8_t block, TileShape shape) {
            tile.block_id = block;
            tile.clearCover();
            chunk.setCell(Chunk::cell(x, y, z), tile, shape);
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
    if (world.reservoirs)
        reservoirConnection = world.reservoirs->changed.connect(
            [this](const ReservoirChange& change) {
                reservoirChanged(change);
            });
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
    if (terrainWorld.reservoirs)
        terrainWorld.reservoirs->changed.disconnect(reservoirConnection);
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
    const auto it = chunks.find(coord);
    if (it == chunks.end() || it->second.state != ChunkState::Ready)
        return false;
    auto& handle = it->second;
    if (!handle.edited.has_value()) {
        const auto& column = columnInputs.at(editKey(coord.x, coord.z)).data;
        handle.edited =
            column
                ? differsFromGeneration(
                      *handle.data, coord, *column,
                      terrainWorld
                          .regions[handle.regionCoord.y * terrainWorld.width +
                                   handle.regionCoord.x],
                      regionLayers.at(handle.regionCoord),
                      handle.structureBaseline.get(), grownBlocks())
                : handle.data->hasShapes();
    }
    return *handle.edited;
}

uint8_t ChunkManager::generatedGroundBlock(unsigned chunkX, unsigned chunkZ,
                                           const ColumnGenerationInput& column,
                                           int y) const {
    if (y < 0 || y > column.surface_y) return 0;
    const auto& region =
        terrainWorld.regions[(chunkZ / CHUNKS_PER_REGION) * terrainWorld.width +
                             chunkX / CHUNKS_PER_REGION];
    return LocationGenerator::terrainBlock(region,
                                           regionLayers.at(region.coord),
                                           column.soil, column.surface_y, y);
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
    auto terrain = prepareColumns({chunkX, 0, chunkZ}, region);
    if (const auto* water = reservoirs())
        terrain = projectedColumns(terrain, chunkX, chunkZ, *water->snapshot());
    return ColumnSources{terrain,
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
    return
        [world = &terrainWorld, assets = soilAssets, table = treeTable,
         seed = treeSeed, overrides = overrides, inputs = std::move(inputs),
         kinds = structureKinds, day = floraDay, region, cx, cz,
         water = reservoirs() ? reservoirs()->snapshot() : nullptr]() mutable {
            const auto terrain =
                TerrainNeighborhood::capture(*world, region, *assets);
            std::vector<const TreeSet*> sets;
            for (auto& input : inputs) {
                if (!input.set) {
                    const auto halo = TerrainNeighborhood::capture(
                        *world, input.region, *assets);
                    const RegionTreesInput treeInput{
                        table.get(),      &halo,          input.region, seed,
                        &input.landmarks, overrides.get()};
                    input.set = std::make_shared<const TreeSet>(
                        generateRegionTrees(treeInput));
                }
                sets.push_back(input.set.get());
            }
            auto columns = TerrainSampler(terrain).columns(cx, cz);
            if (water)
                for (int z = 0; z < Chunk::SIZE_Z; ++z)
                    for (int x = 0; x < Chunk::SIZE_X; ++x) {
                        auto& c = columns.columns[z * Chunk::SIZE_X + x];
                        c.water = water->project(
                            world->hydrology.get(), int(cx) * Chunk::SIZE_X + x,
                            int(cz) * Chunk::SIZE_Z + z, c.surface_y, c.water);
                    }
            return ColumnSources{
                std::make_shared<const ChunkColumnInputs>(std::move(columns)),
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
           it->second.uploadedRevision >= it->second.meshTopologyRevision;
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
std::optional<ShapedTile> ChunkManager::cellAt(int x, int y, int z) {
    x = int(wrap().wrapTile(x));
    if (x < 0 || y < 0 || z < 0) return std::nullopt;
    std::lock_guard<std::mutex> lock(tileAtMutex);
    const auto* chunk =
        getChunk({unsigned(x / Chunk::SIZE_X), unsigned(y / Chunk::SIZE_Y),
                  unsigned(z / Chunk::SIZE_Z)});
    if (!chunk) return std::nullopt;
    return chunk->cellAt(x % Chunk::SIZE_X, y % Chunk::SIZE_Y,
                         z % Chunk::SIZE_Z);
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
namespace {
TilePos waterEditPosition(ChunkCoord coord, uint16_t cell) {
    return {
        int(coord.x) * Chunk::SIZE_X + cell % Chunk::SIZE_X,
        int(coord.y) * Chunk::SIZE_Y + (cell / Chunk::SIZE_X) % Chunk::SIZE_Y,
        int(coord.z) * Chunk::SIZE_Z + cell / (Chunk::SIZE_X * Chunk::SIZE_Y)};
}
}  // namespace

void ChunkManager::unregister(const ChunkCoord& coord) {
    const auto found = chunks.find(coord);
    const bool existed = found != chunks.end();
    if (existed) {
        if (const auto& chunk = found->second.data; chunk && chunk->rootTerrain)
            for (auto cell : chunk->rootTerrain->edited) {
                const auto p = waterEditPosition(coord, cell);
                waterEdits.erase(p);
                lastChannelSurvey.erase(p);
            }
        for (const auto p : found->second.shoreIntakes) {
            waterEdits.erase(p);
            lastChannelSurvey.erase(p);
        }
        pendingShoreChecks.erase(coord);
        const auto rc = found->second.regionCoord;
        for (const auto body : found->second.waterBodies) {
            auto it = waterChunks.find(body);
            it->second.erase(coord);
            if (it->second.empty()) waterChunks.erase(it);
        }
        for (const auto reach : found->second.waterReaches) {
            auto it = waterReachChunks.find(reach);
            it->second.erase(coord);
            if (it->second.empty()) waterReachChunks.erase(it);
        }
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
void ChunkManager::markDirty(const ChunkCoord& coord, bool urgent,
                             bool waterOnly) {
    auto it = chunks.find(coord);
    if (it == chunks.end() || it->second.state != ChunkState::Ready) return;
    auto& handle = it->second;
    handle.dirtyPending = true;
    handle.meshUrgent |= urgent;
    ++handle.meshRevision;
    if (!waterOnly) handle.meshTopologyRevision = handle.meshRevision;
    if (mesher) meshQueue.insert(coord);
}
void ChunkManager::markModified(const ChunkCoord& coord) {
    auto it = chunks.find(coord);
    if (it == chunks.end() || it->second.state != ChunkState::Ready) return;
    it->second.persistDirty = true;
    it->second.edited.reset();
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

std::optional<float> ChunkManager::floraAnchorHeight(
    const ChunkCoord& coord, const FloraInstance& plant,
    const FloraSpeciesRule& rule) {
    const int x = int(coord.x) * Chunk::SIZE_X + plant.tileX();
    const int y = int(coord.y) * Chunk::SIZE_Y + plant.y;
    const int z = int(coord.z) * Chunk::SIZE_Z + plant.tileZ();
    const auto anchor = cellAt(x, y, z);
    if (!anchor) return std::nullopt;
    const auto height =
        floraSupportHeight(rule, *anchor, plant.localX() - plant.tileX(),
                           plant.localZ() - plant.tileZ());
    if (!height || !rule.large) return height;

    const auto footprint = floraVolume(plant, coord, rule);
    for (int dz = 0; dz < footprint.sizeZ; ++dz)
        for (int dx = 0; dx < footprint.sizeX; ++dx) {
            const int px = int(footprint.minX) + dx;
            const int pz = int(footprint.minZ) + dz;
            if (px == x && pz == z) continue;
            const auto cell = cellAt(px, y, pz);
            // Unloaded footprint tiles are rechecked once they are ready.
            if (cell && !floraSupportHeight(rule, *cell, 0, 0))
                return std::nullopt;
        }
    return height;
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
        out.chunks.emplace_back(
            index, std::make_shared<const Chunk>(*it->second.data));
        out.flora.emplace_back(index, it->second.flora);
        out.trees.emplace_back(index, it->second.trees);
        out.structureBaselines.emplace_back(index,
                                            it->second.structureBaseline);
    }
}

RegionSnapshot ChunkManager::snapshotRegion(Vec2u region) {
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
                    index, std::make_shared<const Chunk>(*it->second.data));
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
    std::shared_ptr<const Chunk> data;
    std::unordered_map<ChunkCoord, std::shared_ptr<const Chunk>> neighbors;
    const auto wrapX = wrap();
    {
        // Read center and border tiles with their shape tables under one lock.
        std::lock_guard<std::mutex> lock(tileAtMutex);
        data = std::make_shared<const Chunk>(*handle.data);
        const int offsets[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        for (const auto& offset : offsets) {
            const int64_t x = wrapX.wrapChunk(int64_t{coord.x} + offset[0]);
            const int64_t y = int64_t{coord.y} + offset[1];
            const int64_t z = int64_t{coord.z} + offset[2];
            if (x < 0 || y < 0 || z < 0) continue;
            ChunkCoord neighbor{static_cast<unsigned>(x),
                                static_cast<unsigned>(y),
                                static_cast<unsigned>(z)};
            if (auto* chunk = getChunk(neighbor))
                neighbors[neighbor] = std::make_shared<const Chunk>(*chunk);
        }
    }
    const auto column = columnInputs.find((uint64_t{coord.x} << 32) | coord.z);
    auto terrain = column == columnInputs.end() ? nullptr : column->second.data;
    const auto water = reservoirs() ? reservoirs()->snapshot() : nullptr;
    if (water) terrain = projectedColumns(terrain, coord.x, coord.z, *water);
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
         terrain, structures, water, hydrology = terrainWorld.hydrology]() {
            const unsigned aboveY = coord.y + 1;
            const std::optional<structure_voxels::Slab> above =
                !neighbors.contains({coord.x, aboveY, coord.z}) && structures &&
                        aboveY < Y_CHUNKS && structures->mayContain(aboveY)
                    ? structures->rasterize(aboveY)
                    : std::nullopt;
            auto mesh = builder.build(
                *data, coord,
                [&](int x, int y, int z) -> ChunkMesher::Neighbour {
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
                    if (it == neighbors.end()) return nullptr;
                    const int lx = x % Chunk::SIZE_X, ly = y % Chunk::SIZE_Y,
                              lz = z % Chunk::SIZE_Z;
                    const auto& chunk = *it->second;
                    return {&chunk.tiles[lz][ly][lx],
                            chunk.shapeAt(Chunk::cell(lx, ly, lz))};
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
                    if (!hydrology) return {-1, {}};
                    x = int(wrapX.wrapTile(x));
                    if (!water ||
                        (water->localTop(x, z) < 0 &&
                         std::none_of(
                             water->levels.begin(), water->levels.end(),
                             [&](const auto& changed) {
                                 return hydrology->mayContainBody(
                                     changed.first, x, z, x + 1, z + 1);
                             }))) {
                        auto [ground, surface] =
                            hydrology->sampleRiverColumn(x, z);
                        if (water)
                            surface = water->project(hydrology.get(), x, z,
                                                     ground, surface);
                        return {ground, surface};
                    }
                    const auto [ground, baseline] =
                        hydrology->sampleColumn(x, z);
                    return {ground, water->project(hydrology.get(), x, z,
                                                   ground, baseline)};
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
        if (!handle.canPublishMesh(result.revision)) {
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
            // surface. Water seen from the top keeps the sweep's falloff.
            const auto [ground, water] = surface(x, z);
            const int top = base + Chunk::SIZE_Y - 1;
            if (top <= ground) continue;
            const Tile& first = chunk.tiles[z][Chunk::SIZE_Y - 1][x];
            const int above =
                first.isLiquid() ? std::max(0, water.topTile() - top) : 0;
            int value = above ? std::max(0, 15 - waterOpacity(water.topFill) -
                                                8 * (above - 1))
                              : 15;
            for (int y = Chunk::SIZE_Y - 1; y >= 0 && value > 0; --y) {
                Tile& tile = chunk.tiles[z][y][x];
                if (tile.isSolid()) break;
                if (tile.lightLevel.sun_exposure < value)
                    tile.lightLevel.sun_exposure = uint8_t(value);
                if (tile.liquid == LiquidType::Water)
                    value = std::max(0, value - waterOpacity(tile.liquidLevel));
                else if (tile.isLiquid())
                    value = std::max(0, value - 8);
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
    const auto inputs =
        column != columnInputs.end() ? column->second.data : nullptr;
    if (data.data && reservoirs() && inputs) {
        if (!data.fromDisk) {
            data.data->projectsWater = true;
            for (int z = 0; z < Chunk::SIZE_Z; ++z)
                for (int x = 0; x < Chunk::SIZE_X; ++x) {
                    const auto baseline =
                        inputs->columns[z * Chunk::SIZE_X + x].water;
                    if (!baseline.body && !baseline.reach) continue;
                    for (auto& row : data.data->tiles[z])
                        if (row[x].liquid == LiquidType::Water)
                            row[x].waterOrigin = WaterOrigin::Reservoir;
                }
        }
        reconcileWater(*data.data, coord, inputs.get());
        if (data.data->projectsWater) indexWater(coord, *inputs);
    }
    // Start new chunks with estimated sunlight until their sweep arrives.
    if (data.data) {
        auto projected = inputs;
        if (const auto* water = reservoirs())
            projected =
                projectedColumns(inputs, coord.x, coord.z, *water->snapshot());
        const int top = getColumnTopY(coord.x, coord.z);
        estimateSkyLight(*data.data, coord, [&](int x, int z) {
            if (!projected) return std::pair{top, WaterSurface::sea(top)};
            const auto& sample = projected->columns[z * Chunk::SIZE_X + x];
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
    if (auto* water = reservoirs()) {
        const TilePos base{int(coord.x) * Chunk::SIZE_X,
                           int(coord.y) * Chunk::SIZE_Y,
                           int(coord.z) * Chunk::SIZE_Z};
        water->wakeLocalBox({base.x - 1, base.y - 1, base.z - 1},
                            {base.x + Chunk::SIZE_X, base.y + Chunk::SIZE_Y,
                             base.z + Chunk::SIZE_Z},
                            wrap().tiles);
        lakeJoinsPending |= water->snapshot()->poolIntersects(
            base.x - 1, base.z - 1, base.x + Chunk::SIZE_X + 1,
            base.z + Chunk::SIZE_Z + 1, wrap().tiles);
    }
    if (const auto* chunk = getChunk(coord);
        reservoirs() && chunk && chunk->rootTerrain)
        for (auto cell : chunk->rootTerrain->edited) {
            const auto p = waterEditPosition(coord, cell);
            const auto* baseline = waterColumnAt(p);
            if (baseline && (baseline->water.reach || baseline->water.body) &&
                baseline->water.fills(p.y, baseline->surface_y)) {
                if (const auto* tile = tileAt(p.x, p.y, p.z);
                    tile && tile->isSolid())
                    waterTopologyEdits.insert(p);
                if (baseline->water.reach ||
                    baseline->water.body > GeneratedHydrology::Sea) {
                    reservoirs()->clearChannelMouth(p);
                    continue;
                }
            }
            if (reservoirs()->hasBreach(p)) continue;
            if (waterIntakeCandidate(p)) {
                waterEdits.insert(p);
                waterTopologyEdits.insert(p);
            }
        }
    if (reservoirs()) pendingShoreChecks.insert(coord);
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
                        const bool edited =
                            columns ? differsFromGeneration(
                                          *result, coord, *columns, region,
                                          layers, baseline.get(), grown)
                                    : result->hasShapes();
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
    if (it->second.data->hasShapes() ||
        !std::all_of(first, last, [](const Tile& t) { return t.isSolid(); }))
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
        const auto& neighbor = *n->second.data;
        for (int a = 0; a < Chunk::SIZE_X; ++a)
            for (int b = 0; b < Chunk::SIZE_X; ++b) {
                const int lx = o[0] ? (o[0] > 0 ? 0 : Chunk::SIZE_X - 1) : b;
                const int ly =
                    o[1] ? (o[1] > 0 ? 0 : Chunk::SIZE_Y - 1) : (o[0] ? b : a);
                const int lz = o[2] ? (o[2] > 0 ? 0 : Chunk::SIZE_Z - 1) : a;
                const auto cell = neighbor.cellAt(lx, ly, lz);
                if (!cell.tile.isSolid() || cell.shape.code()) return false;
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
                [&](int x, int y, int z, Tile tile, uint8_t block,
                    TileShape shape) {
                    if (editor) {
                        edits.push_back({.pos = {int(job.coord.x) * Side + x,
                                                 int(job.coord.y) * Side + y,
                                                 int(job.coord.z) * Side + z},
                                         .mode = EditMode::Replace,
                                         .block = block,
                                         .shape = shape});
                    } else {
                        tile.block_id = block;
                        tile.clearCover();
                        chunk.setCell(Chunk::cell(x, y, z), tile, shape);
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

WaterVolume ChunkManager::transferWater(uint32_t body, WaterVolume amount,
                                        ReservoirExchange reason) {
    if (!terrainWorld.reservoirs) return 0;
    return terrainWorld.reservoirs->transfer(body, amount, reason);
}

WaterVolume ChunkManager::divertWater(uint32_t body, TilePos p,
                                      WaterVolume amount) {
    if (!terrainWorld.reservoirs) return 0;
    p.x = int(wrap().wrapTile(p.x));
    if (p.x < 0 || p.y < 0 || p.z < 0) return 0;
    const ChunkCoord coord{unsigned(p.x / Chunk::SIZE_X),
                           unsigned(p.y / Chunk::SIZE_Y),
                           unsigned(p.z / Chunk::SIZE_Z)};
    const auto* chunk = getChunk(coord);
    if (!chunk || !chunk->projectsWater) return 0;
    const auto& tile = chunk->tiles[p.z % Chunk::SIZE_Z][p.y % Chunk::SIZE_Y]
                                   [p.x % Chunk::SIZE_X];
    if (tile.isSolid() ||
        (tile.isLiquid() && tile.waterOrigin != WaterOrigin::Local))
        return 0;
    const auto& columns =
        columnInputs.at((uint64_t{coord.x} << 32) | coord.z).data;
    if (!columns) return 0;
    const auto& c = columns->columns[(p.z % Chunk::SIZE_Z) * Chunk::SIZE_X +
                                     p.x % Chunk::SIZE_X];
    if (c.water.fills(p.y, c.surface_y)) return 0;
    return terrainWorld.reservoirs->divert(body, p, amount);
}

float ChunkManager::aquiferAt(int x, int y, int z) {
    x = int(wrap().wrapTile(x));
    if (x < 0 || y < 0 || z < 0) return 0;
    const auto coord =
        ChunkCoord{unsigned(x / Chunk::SIZE_X), unsigned(y / Chunk::SIZE_Y),
                   unsigned(z / Chunk::SIZE_Z)};
    const auto it = chunks.find(coord);
    if (it == chunks.end() || !it->second.data) return 0;
    const auto layers = regionLayers.find(it->second.regionCoord);
    if (layers == regionLayers.end()) return 0;
    const auto& tile =
        it->second.data
            ->tiles[z % Chunk::SIZE_Z][y % Chunk::SIZE_Y][x % Chunk::SIZE_X];
    if (!tile.isSolid()) return 0;
    const auto& region =
        terrainWorld.regions[it->second.regionCoord.y * terrainWorld.width +
                             it->second.regionCoord.x];
    const int depth = region.land_height_m - y;
    if (depth < 0) return 0;
    for (const auto& layer : layers->second)
        if (layer.type == RegionLayerType::Rock &&
            layer.block_id == tile.block_id) {
            const int top = layer.depth_from_surface_m;
            const int bottom = top + layer.thickness_m;
            if (depth >= top && depth < bottom) return layer.aquifer_saturation;
            break;
        }
    return 0;
}

Reservoirs::CellAccess ChunkManager::waterAccess(TilePos p) {
    using Access = Reservoirs::CellAccess;
    if (p.x < 0 || p.y < 0 || p.y >= WORLD_HEIGHT_Y || p.z < 0 ||
        (terrainWorld.height && p.z >= int(terrainWorld.height) * 128))
        return Access::Blocked;
    const ChunkCoord coord{unsigned(p.x / Chunk::SIZE_X),
                           unsigned(p.y / Chunk::SIZE_Y),
                           unsigned(p.z / Chunk::SIZE_Z)};
    const auto* chunk = getChunk(coord);
    if (!chunk) return Access::Unknown;
    const auto& tile = chunk->tiles[p.z % Chunk::SIZE_Z][p.y % Chunk::SIZE_Y]
                                   [p.x % Chunk::SIZE_X];
    if (tile.isSolid() ||
        (tile.isLiquid() && tile.waterOrigin != WaterOrigin::Local))
        return Access::Blocked;
    return Access::Open;
}

WaterSurface ChunkManager::waterSourceAt(TilePos p) {
    p.x = int(wrap().wrapTile(p.x));
    if (p.x < 0 || p.y < 0 || p.z < 0 || !reservoirs()) return {};
    const auto* tile = tileAt(p.x, p.y, p.z);
    if (!tile || tile->isSolid() ||
        (tile->isLiquid() && tile->liquid != LiquidType::Water))
        return {};
    const auto* baseline = waterColumnAt(p);
    if (!baseline) return {};
    const auto snapshot = reservoirs()->snapshot();
    const auto* hydrology = terrainWorld.hydrology.get();
    auto surface = snapshot->projectStanding(
        hydrology, p.x, p.z, baseline->surface_y, baseline->water);
    int floor = baseline->surface_y;
    const auto pool = snapshot->projectLocal(p.x, p.z, floor, {}, &floor);
    if (pool.fills(p.y, floor))
        surface = pool;
    else if (!surface.fills(p.y, baseline->surface_y))
        return {};
    else if (const auto stage = snapshot->riverStages.find(surface.reach);
             stage != snapshot->riverStages.end()) {
        uint32_t sourceReach = surface.reach;
        const int head = stage->second.backwaterAt(
            p.x, p.z, (baseline->surface_y + 1) * 255,
            baseline->water.topTile() * 255 + baseline->water.topFill,
            &sourceReach);
        if (head > p.y * 255 &&
            (!stage->second.damUpstream ||
             stage->second.belowCut(p.x, p.z, baseline->water.surfaceY))) {
            surface.surfaceY = (head + 254) / 255;
            surface.topFill = uint8_t((head - 1) % 255 + 1);
            surface.reach = sourceReach;
        }
    }
    // Legacy slabs preserve independent water; use a projected face only
    // where the slab participates in projection or actually contains water.
    const auto* chunk =
        getChunk({unsigned(p.x / Chunk::SIZE_X), unsigned(p.y / Chunk::SIZE_Y),
                  unsigned(p.z / Chunk::SIZE_Z)});
    if (chunk && !surface.reach && surface.body <= GeneratedHydrology::Sea &&
        !pool.body &&
        chunk->rootEdited(Chunk::cell(p.x % Chunk::SIZE_X, p.y % Chunk::SIZE_Y,
                                      p.z % Chunk::SIZE_Z)))
        return {};
    return tile->liquid == LiquidType::Water || (chunk && chunk->projectsWater)
               ? surface
               : WaterSurface{};
}

std::optional<Reservoirs::Sink> ChunkManager::waterSinkAt(TilePos p) {
    const auto surface = waterSourceAt(p);
    if (!surface.body && !surface.reach) return {};
    return Reservoirs::Sink{surface.reach ? 0 : surface.body, surface.reach,
                            surface.surfaceY};
}

void ChunkManager::surveyChannel(Reservoirs& water, TilePos mouth) {
    const auto canonical = [&](TilePos p) {
        p.x = int(wrap().wrapTile(p.x));
        return p;
    };
    const auto faces = [](TilePos p) {
        return std::array<TilePos, 6>{{{p.x, p.y + 1, p.z},
                                       {p.x, p.y - 1, p.z},
                                       {p.x - 1, p.y, p.z},
                                       {p.x + 1, p.y, p.z},
                                       {p.x, p.y, p.z - 1},
                                       {p.x, p.y, p.z + 1}}};
    };
    const auto local = [&](TilePos p) {
        const auto* tile = tileAt(p.x, p.y, p.z);
        return tile && !tile->isSolid() && tile->liquid == LiquidType::Water &&
               tile->waterOrigin == WaterOrigin::Local;
    };
    // The source: a standing body beside or above the mouth.
    std::optional<Reservoirs::Sink> source;
    TilePos intake = mouth;
    for (auto q : faces(mouth)) {
        if (q.y < mouth.y) continue;
        if (const auto s = waterSinkAt(canonical(q)); s && s->body && !s->reach)
            if (!source || s->surfaceY > source->surfaceY) {
                source = s;
                intake = canonical(q);
            }
    }
    // A mouth above the lowered surface no longer feeds; keep its record.
    using Access = Reservoirs::CellAccess;
    if (!source || waterAccess(mouth) != Access::Open) return;
    // Trace gravity through loaded cells. A fast-forwarded downspout may
    // have no stored water between its lip and the pool at the bottom.
    std::queue<TilePos> pending;
    std::unordered_map<TilePos, TilePos> parent{{mouth, mouth}};
    pending.push(mouth);
    constexpr size_t Budget = 8192;
    bool unknown = false;
    std::optional<TilePos> runoff;
    const auto pathTo = [&](TilePos end, TilePos previous) {
        std::vector<TilePos> path{end};
        for (auto at = previous; at != mouth; at = parent.at(at))
            path.push_back(at);
        path.push_back(mouth);
        path.push_back(intake);
        return path;
    };
    while (!pending.empty()) {
        const auto p = pending.front();
        pending.pop();
        const TilePos below{p.x, p.y - 1, p.z};
        const bool supported =
            waterAccess(below) == Access::Blocked || local(below);
        // A substantial downhill route onto open terrain can discharge into
        // surface runoff even without another mapped water body. Require a
        // loaded natural surface and room to spread; a shaft or a sealed
        // tunnel remains finite storage, not a drain.
        if (!runoff && p.y <= mouth.y - 4 &&
            std::abs(WorldWrap::delta(p.x, mouth.x, wrap().tiles)) +
                    std::abs(p.z - mouth.z) >=
                12) {
            const auto* ground = waterColumnAt(p);
            const auto* floor = tileAt(below.x, below.y, below.z);
            const auto* above = tileAt(p.x, p.y + 1, p.z);
            unsigned sides = 0;
            if (ground && !ground->water.wet() &&
                p.y == ground->surface_y + 1 && floor && floor->isSolid() &&
                above && !above->isSolid())
                for (auto q : faces(p)) {
                    if (q.y != p.y) continue;
                    q = canonical(q);
                    const auto* bank = waterColumnAt(q);
                    if (bank && bank->surface_y < q.y &&
                        waterAccess(q) == Access::Open)
                        ++sides;
                }
            if (sides >= 3) runoff = p;
        }
        for (auto q : faces(p)) {
            q = canonical(q);
            if (q.y < 0 || q.z < 0 || q.y > p.y || (q.y == p.y && !supported) ||
                parent.contains(q))
                continue;
            const auto* tile = tileAt(q.x, q.y, q.z);
            if (!tile) {
                unknown = true;
                continue;
            }
            if (const auto target = waterSinkAt(q);
                target && (target->reach || target->body != source->body) &&
                target->surfaceY < source->surfaceY) {
                // Flow continues while the source stands above the mouth,
                // the path sill and the receiver.
                water.setChannelMouth(source->body, *target, mouth,
                                      std::max(mouth.y, target->surfaceY),
                                      pathTo(q, p));
                return;
            }
            if (waterAccess(q) != Access::Open) continue;
            if (parent.size() >= Budget) {
                unknown = true;
                continue;
            }
            parent.emplace(q, p);
            pending.push(q);
        }
    }
    // A tunnel may exit much farther downstream than the original bank.
    // Use the confirmed passage to reach the valley, then survey its bowl.
    if (runoff && source->body == GeneratedHydrology::Sea) {
        std::vector<TilePos> route;
        Reservoirs::ValleyOutlet outlet;
        auto bowl = Reservoirs::surveyBreach(
            {runoff->x, runoff->z}, source->surfaceY, wrap().tiles,
            [&](Vec2i at) -> std::optional<int> {
                const auto* baseline = waterColumnAt({at.x, 0, at.y});
                if (!baseline) return {};
                if (baseline->water.body == source->body)
                    return source->surfaceY;
                if (baseline->water.wet()) return {};
                // Read loaded voxels to include dams, excavation and ledges.
                // A missing slab leaves this branch unverified.
                for (int y = source->surfaceY - 1; y >= 0; --y) {
                    const auto* tile = tileAt(at.x, y, at.y);
                    if (!tile) return {};
                    if (tile->isSolid()) return y + 1;
                }
                return {};
            },
            &route, &outlet,
            [&](TilePos edge) -> std::optional<Reservoirs::Sink> {
                const auto sink = waterSinkAt(edge);
                if (sink && sink->reach) return sink;
                const auto* column = waterColumnAt(edge);
                if (!column || !column->water.reach ||
                    column->water.surfaceY > edge.y)
                    return {};
                for (int y = column->water.topTile(); y <= edge.y; ++y) {
                    const auto* tile = tileAt(edge.x, y, edge.z);
                    if (!tile || tile->isSolid()) return {};
                }
                return Reservoirs::Sink{0, column->water.reach,
                                        column->water.surfaceY};
            });
        if (bowl) {
            auto passage = pathTo(*runoff, parent.at(*runoff));
            route.insert(route.end(), passage.begin(), passage.end() - 2);
            if (water.connectValley(source->body, mouth, intake,
                                    std::move(*bowl), std::move(route),
                                    std::move(outlet)))
                return;
        }
    }
    // Fully surveyed without a receiver: the channel is closed or pooled.
    if (runoff && source->body != GeneratedHydrology::Sea)
        water.setChannelMouth(source->body, {0, 0, runoff->y}, mouth, mouth.y,
                              pathTo(*runoff, parent.at(*runoff)));
    else if (!unknown)
        water.clearChannelMouth(mouth);
}

void ChunkManager::surveyChannels(Reservoirs& water, size_t mouths) {
    if (waterEdits.empty()) return;
    auto it = channelCursor ? waterEdits.upper_bound(*channelCursor)
                            : waterEdits.begin();
    for (size_t i = 0; i < std::min(mouths, waterEdits.size()); ++i) {
        if (it == waterEdits.end()) it = waterEdits.begin();
        const auto p = *it++;
        channelCursor = p;
        if (water.feedsPool(p)) continue;
        const auto last = lastChannelSurvey.find(p);
        if (last != lastChannelSurvey.end() && water.step() - last->second < 60)
            continue;
        surveyChannel(water, p);
        lastChannelSurvey[p] = water.step();
    }
}

const ColumnGenerationInput* ChunkManager::waterColumnAt(TilePos p) const {
    if (p.x < 0 || p.z < 0) return nullptr;
    const auto key =
        (uint64_t(p.x / Chunk::SIZE_X) << 32) | unsigned(p.z / Chunk::SIZE_Z);
    const auto it = columnInputs.find(key);
    if (it == columnInputs.end() || !it->second.data) return nullptr;
    return &it->second.data->columns[(p.z % Chunk::SIZE_Z) * Chunk::SIZE_X +
                                     p.x % Chunk::SIZE_X];
}

bool ChunkManager::waterIntakeCandidate(TilePos p) {
    if (waterAccess(p) != Reservoirs::CellAccess::Open) return false;
    const auto* hydrology = terrainWorld.hydrology.get();
    if (!hydrology) return false;
    for (auto q : {TilePos{p.x, p.y + 1, p.z}, TilePos{p.x - 1, p.y, p.z},
                   TilePos{p.x + 1, p.y, p.z}, TilePos{p.x, p.y, p.z - 1},
                   TilePos{p.x, p.y, p.z + 1}}) {
        q.x = int(wrap().wrapTile(q.x));
        if (!hydrology->containsColumn(q.x, q.z)) continue;
        if (const auto* source = waterColumnAt(q)) {
            if (source->water.fills(q.y, source->surface_y)) return true;
        } else if (q.y < hydrology->waterCeilingAt(q.x, q.z)) {
            const auto [ground, water] = hydrology->sampleColumn(q.x, q.z);
            if (water.fills(q.y, ground)) return true;
        }
    }
    return false;
}

void ChunkManager::waterBlockEdited(TilePos p) {
    if (!reservoirs()) return;
    reservoirs()->wakeLocalBox({p.x - 1, p.y - 1, p.z - 1},
                               {p.x + 1, p.y + 1, p.z + 1}, wrap().tiles);
    waterTopologyEdits.insert(p);
    waterDirectEdits.insert(p);
    const auto* chunk =
        getChunk({unsigned(p.x / Chunk::SIZE_X), unsigned(p.y / Chunk::SIZE_Y),
                  unsigned(p.z / Chunk::SIZE_Z)});
    if (chunk && waterIntakeCandidate(p) &&
        chunk->rootEdited(Chunk::cell(p.x % Chunk::SIZE_X, p.y % Chunk::SIZE_Y,
                                      p.z % Chunk::SIZE_Z)))
        waterEdits.insert(p);
    else if (waterEdits.erase(p))
        reservoirs()->clearChannelMouth(p);
}

void ChunkManager::commitWaterEdits() {
    if (auto* water = reservoirs()) updateWaterTopology(*water);
}

void ChunkManager::detectLakeSplit(Reservoirs& water, TilePos outlet) {
    const auto& hydro = *terrainWorld.hydrology;
    constexpr size_t MaxPocketCells = 1024;
    const auto wrapX = wrap();
    const auto canonical = [&](TilePos p) {
        p.x = int(wrapX.wrapTile(p.x));
        return p;
    };
    for (auto seed : {TilePos{outlet.x, outlet.y + 1, outlet.z},
                      TilePos{outlet.x - 1, outlet.y, outlet.z},
                      TilePos{outlet.x + 1, outlet.y, outlet.z},
                      TilePos{outlet.x, outlet.y, outlet.z - 1},
                      TilePos{outlet.x, outlet.y, outlet.z + 1}}) {
        seed = canonical(seed);
        const auto* baseline = waterColumnAt(seed);
        if (!baseline || !baseline->water.body || baseline->water.reach)
            continue;
        const uint32_t parent = baseline->water.body;
        const auto snapshot = water.snapshot();
        const auto surface = snapshot->projectStanding(
            &hydro, seed.x, seed.z, baseline->surface_y, baseline->water);
        if (!surface.fills(seed.y, baseline->surface_y)) continue;
        const auto* first = tileAt(seed.x, seed.y, seed.z);
        if (!first || first->waterOrigin != WaterOrigin::Reservoir ||
            first->liquid != LiquidType::Water)
            continue;
        const int head = surface.surfaceY;
        const auto fill = snapshot->surfaceFills.find(parent);
        struct Column {
            int floor = WORLD_HEIGHT_Y;
            int cells = 0;
        };
        std::map<std::pair<int, int>, Column> columns;
        std::vector<TilePos> work{seed};
        std::unordered_set<TilePos> seen{seed};
        WaterVolume trapped = 0;
        bool wall = false, verified = true;
        for (size_t i = 0; verified && i < work.size(); ++i) {
            const auto p = work[i];
            const auto* tile = tileAt(p.x, p.y, p.z);
            if (!tile || tile->waterOrigin != WaterOrigin::Reservoir ||
                tile->liquid != LiquidType::Water) {
                verified = false;
                break;
            }
            auto& column = columns[{p.x, p.z}];
            column.floor = std::min(column.floor, p.y);
            ++column.cells;
            trapped += p.y == head - 1 && fill != snapshot->surfaceFills.end()
                           ? fill->second
                           : WaterUnitsPerBlock;
            for (auto q :
                 {TilePos{p.x, p.y + 1, p.z}, TilePos{p.x, p.y - 1, p.z},
                  TilePos{p.x - 1, p.y, p.z}, TilePos{p.x + 1, p.y, p.z},
                  TilePos{p.x, p.y, p.z - 1}, TilePos{p.x, p.y, p.z + 1}}) {
                q = canonical(q);
                if (q == outlet || q.y >= head || q.y < 0 ||
                    !hydro.containsColumn(q.x, q.z) || !seen.insert(q).second)
                    continue;
                const auto* other = tileAt(q.x, q.y, q.z);
                const auto* base = waterColumnAt(q);
                if (!other || !base) {
                    verified = false;
                    break;
                }
                if (other->isSolid()) {
                    wall |= base->water.body == parent &&
                            base->water.fills(q.y, base->surface_y);
                    continue;
                }
                if (waterEdits.contains(q) &&
                    !base->water.fills(q.y, base->surface_y))
                    continue;  // another cut of this outlet bank
                if (work.size() >= MaxPocketCells ||
                    !snapshot
                         ->projectStanding(&hydro, q.x, q.z, base->surface_y,
                                           base->water)
                         .fills(q.y, base->surface_y) ||
                    base->water.body != parent) {
                    verified = false;
                    break;
                }
                work.push_back(q);
            }
        }
        if (!verified || !wall) continue;
        Reservoirs::BreachSurvey survey{head, {}};
        for (const auto& [pos, column] : columns) {
            if (column.cells != head - column.floor) {
                verified = false;  // no overhang/cavity in this shortcut
                break;
            }
            survey.columns.push_back(
                {{pos.first, pos.second}, column.floor, column.floor});
        }
        if (verified &&
            water.detachLake(parent, seed, std::move(survey), trapped))
            return;
    }
}

void ChunkManager::reconnectLakes(Reservoirs& water) {
    const auto snapshot = water.snapshot();
    for (const auto& [body, pool] : snapshot->pools) {
        if (!pool.parent) continue;
        const auto& columns = pool.footprint->columns;
        int bottom = WORLD_HEIGHT_Y;
        for (const auto& [key, heights] : columns)
            bottom = std::min(bottom, heights.first);

        // Rejoin only through a loaded face at the compartment's floor.
        // A higher notch can leave water trapped below its sill.
        const auto connected = [&](const auto& entry) {
            const auto& [key, heights] = entry;
            if (heights.first != bottom || heights.second != bottom)
                return false;
            const TilePos p{int(key >> 32), bottom, int(uint32_t(key))};
            const auto* base = waterColumnAt(p);
            if (!base || base->water.body != pool.parent || base->water.reach ||
                !base->water.fills(p.y, base->surface_y))
                return false;  // Flooded valleys have their own storage.
            const auto* tile = tileAt(p.x, p.y, p.z);
            if (!tile || tile->isSolid() ||
                (tile->isLiquid() && tile->liquid != LiquidType::Water))
                return false;
            for (const auto d :
                 std::array<Vec2i, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                const TilePos q{int(wrap().wrapTile(p.x + d.x)), p.y,
                                p.z + d.y};
                if (waterSourceAt(q).body == pool.parent) return true;
            }
            return false;
        };
        if (std::any_of(columns.begin(), columns.end(), connected))
            water.rejoinLake(body);
    }
}

void ChunkManager::updateWaterTopology(Reservoirs& water) {
    if (!terrainWorld.hydrology || !soilAssets) return;
    if (waterTopologyEdits.empty()) {
        if (std::exchange(lakeJoinsPending, false)) reconnectLakes(water);
        return;
    }
    const auto& hydro = *terrainWorld.hydrology;
    SavedWaterTerrain saved(waterTerrainEdits);
    const int depth = int(terrainWorld.height) * WorldWrap::RegionSize;
    std::unordered_map<Vec2u, TerrainNeighborhood> neighborhoods;
    const auto floor = [&](Vec2i v, int head) {
        if (const auto* edit = saved.column(v.x, v.y)) {
            int y = edit->groundY[ColumnEdit::index(v.x % Chunk::SIZE_X,
                                                    v.y % Chunk::SIZE_Z)] +
                    1;
            while (y < head && saved.addedSolid({v.x, y, v.y})) ++y;
            return y;
        }
        const Vec2u region{unsigned(v.x / WorldWrap::RegionSize),
                           unsigned(v.y / WorldWrap::RegionSize)};
        auto it = neighborhoods.find(region);
        if (it == neighborhoods.end())
            it = neighborhoods
                     .emplace(region, TerrainNeighborhood::capture(
                                          terrainWorld, region, *soilAssets))
                     .first;
        return int(std::floor(TerrainSampler(it->second).surface(v.x, v.y))) +
               1;
    };
    const auto installDam = [&](uint32_t reach, TilePos wall,
                                Reservoirs::BreachSurvey survey) {
        return water.openDam(reach, wall, std::move(survey), [&](TilePos p) {
            const auto* tile = tileAt(p.x, p.y, p.z);
            return tile ? tile->isSolid() : saved.addedSolid(p);
        });
    };
    const auto nearby = [&](TilePos a, TilePos b, int radius) {
        return std::abs(WorldWrap::delta(a.x, b.x, wrap().tiles)) <= radius &&
               std::abs(a.z - b.z) <= radius;
    };
    std::unordered_set<uint32_t> surveyedMouths;
    for (const auto p : waterTopologyEdits) {
        const auto* cell = tileAt(p.x, p.y, p.z);
        if (!cell) continue;
        bool springMouth = false;
        if (hydro.springRockAt(p.x, p.y - 1, p.z))
            for (const auto& reach : hydro.riverReaches()) {
                const auto& spring = reach.spring;
                if (reach.lakeSource || spring.tile != Vec2i{p.x, p.z} ||
                    p.y != spring.rockY + 1)
                    continue;
                water.setSpringOpen(reach.id, !cell->isSolid());
                springMouth = true;
                // Sealing the supply does not open the dam it feeds.
                waterDirectEdits.erase(p);
                // An already built wall may only become certifiable when
                // its nearby spring is unsealed.
                if (!cell->isSolid() && !water.damAt(reach.id))
                    for (const auto q : riverWallCandidates(p, reach.id))
                        if (auto dam = riverDamAt(q);
                            dam &&
                            installDam(reach.id, q, std::move(dam->second)))
                            break;
            }
        const auto* baseline = waterColumnAt(p);
        const bool inRiver = baseline && baseline->water.reach &&
                             baseline->water.fills(p.y, baseline->surface_y);
        if (cell->isSolid()) {
            water.blockChannelCell(p);
            if (!inRiver || water.damAt(baseline->water.reach) != p)
                water.closeBreach(p);
            if (inRiver) {
                const auto reach = baseline->water.reach;
                if (!springMouth && !water.damAt(reach))
                    if (auto dam = riverDamAt(p))
                        installDam(reach, p, std::move(dam->second));
                waterEdits.erase(p);
            } else if (baseline && baseline->water.body &&
                       baseline->water.fills(p.y, baseline->surface_y)) {
                const auto body = baseline->water.body;
                const auto surveyMouth = [&](const auto& reach, Vec2i mouth) {
                    if (!nearby(p, {mouth.x, p.y, mouth.y},
                                ReservoirSnapshot::RiverStage::DamRadius) ||
                        water.damAt(reach.id) ||
                        !surveyedMouths.insert(reach.id).second)
                        return;
                    if (const auto anchor = riverMouthAnchor(mouth, reach.id))
                        if (auto dam = riverDamAt(*anchor))
                            installDam(reach.id, *anchor,
                                       std::move(dam->second));
                };
                if (const auto index = water.lakeOutletIndex(body)) {
                    const auto& outlet = hydro.riverReaches()[*index];
                    surveyMouth(outlet, outlet.lakeSource->tile);
                }
                for (const auto& inlet : hydro.riverReaches())
                    if (inlet.end == GeneratedHydrology::RiverEnd::Lake &&
                        inlet.receiver == body)
                        surveyMouth(inlet, inlet.outlet);
            }
            continue;
        }
        if (inRiver ||
            (baseline && baseline->water.body > GeneratedHydrology::Sea &&
             baseline->water.fills(p.y, baseline->surface_y))) {
            water.clearChannelMouth(p);
            // Restoring the generated cross-section is not a new diversion.
            waterEdits.erase(p);
            ReservoirChange change;
            change.local = p;
            reservoirChanged(change);
            continue;
        }
        if (water.hasBreach(p)) {
            waterEdits.erase(p);
            continue;
        }
        if (!baseline) continue;
        if (p.y != baseline->surface_y || baseline->water.reach) continue;
        if (const auto* above = tileAt(p.x, p.y + 1, p.z);
            above && above->isSolid())
            continue;
        for (auto q :
             {TilePos{p.x - 1, p.y, p.z}, TilePos{p.x + 1, p.y, p.z},
              TilePos{p.x, p.y, p.z - 1}, TilePos{p.x, p.y, p.z + 1}}) {
            q.x = int(wrap().wrapTile(q.x));
            if (q.z < 0 || q.z >= depth) continue;
            const auto* inlet = waterColumnAt(q);
            const auto source = inlet
                                    ? std::pair{inlet->surface_y, inlet->water}
                                    : hydro.sampleColumn(q.x, q.z);
            if (!source.second.fills(p.y, source.first)) continue;
            if (!source.second.reach && !source.second.body) continue;
            const auto* neighbor = tileAt(q.x, p.y, q.z);
            if (neighbor && neighbor->isSolid()) continue;
            const int head = source.second.surfaceY;
            std::vector<TilePos> route;
            Reservoirs::ValleyOutlet outlet;
            const auto survey = Reservoirs::surveyBreach(
                {p.x, p.z}, head, wrap().tiles,
                [&](Vec2i v) -> std::optional<int> {
                    if (v.y < 0 || v.y >= depth) return {};
                    if (v == Vec2i{p.x, p.z}) return p.y;
                    const auto water = hydro.waterAt(v.x, v.y);
                    if (source.second.reach ? water.reach == source.second.reach
                                            : water.body == source.second.body)
                        return head;
                    if (water.wet()) return {};
                    return floor(v, head);
                },
                source.second.reach ? nullptr : &route,
                source.second.reach ? nullptr : &outlet,
                [&](TilePos edge) -> std::optional<Reservoirs::Sink> {
                    if (!hydro.containsColumn(edge.x, edge.z)) return {};
                    const auto reach = hydro.waterAt(edge.x, edge.z);
                    if (!reach.reach || floor({edge.x, edge.z}, head) > edge.y)
                        return {};
                    return Reservoirs::Sink{0, reach.reach, reach.surfaceY};
                });
            if (survey && !source.second.reach) {
                water.connectValley(source.second.body, p, q, *survey,
                                    std::move(route), std::move(outlet));
                break;
            }
            if (survey && water.openBreach(source.second.reach, p, *survey)) {
                waterEdits.erase(p);
                break;
            }
        }
    }
    for (const auto& [reach, wall] : water.damSites()) {
        const bool direct = std::any_of(
            waterDirectEdits.begin(), waterDirectEdits.end(),
            [&](TilePos p) { return nearby(p, wall, DamSpillRadius); });
        if (std::none_of(
                waterTopologyEdits.begin(), waterTopologyEdits.end(),
                [&](TilePos p) { return nearby(p, wall, DamSpillRadius); }))
            continue;
        const auto active = water.damAt(reach);
        if (active && *active != wall) continue;
        if (auto dam = riverDamAt(wall, active.has_value()))
            installDam(reach, wall, std::move(dam->second));
        else if (active && direct)
            water.closeBreach(wall);
    }
    lastChannelSurvey.clear();
    waterTopologyEdits.clear();
    waterDirectEdits.clear();
    lakeJoinsPending = false;
    reconnectLakes(water);
    size_t tested = 0;
    for (const auto p : waterEdits) {
        if (tested++ == 64) break;
        detectLakeSplit(water, p);
    }
}

// The mouth station is on the lake shore. Anchor the survey on the
// nearest reach voxel, including when the wall is entirely inside the lake.
std::optional<TilePos> ChunkManager::riverMouthAnchor(Vec2i source,
                                                      uint32_t reach) const {
    constexpr int radius = ReservoirSnapshot::RiverStage::DamRadius;
    const auto& hydro = *terrainWorld.hydrology;
    for (int r = 0; r <= radius; ++r)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
                TilePos q{int(wrap().wrapTile(source.x + dx)), 0,
                          source.y + dz};
                if (!hydro.containsColumn(q.x, q.z)) continue;
                const auto* column = waterColumnAt(q);
                const auto surface =
                    column ? column->water : hydro.waterAt(q.x, q.z);
                if (surface.reach == reach) {
                    q.y = surface.topTile();
                    return q;
                }
            }
    return {};
}

std::vector<TilePos> ChunkManager::riverWallCandidates(TilePos source,
                                                       uint32_t reach) {
    std::vector<TilePos> result;
    for (int dz = -16; dz <= 16; ++dz)
        for (int dx = -16; dx <= 16; ++dx) {
            TilePos q{int(wrap().wrapTile(source.x + dx)), 0, source.z + dz};
            const auto* column = waterColumnAt(q);
            if (!column || column->water.reach != reach) continue;
            for (q.y = column->surface_y + 1; q.y < column->water.surfaceY;
                 ++q.y)
                if (const auto* tile = tileAt(q.x, q.y, q.z);
                    tile && tile->isSolid()) {
                    result.push_back(q);
                    break;
                }
        }
    return result;
}

std::optional<std::pair<uint32_t, Reservoirs::BreachSurvey>>
ChunkManager::riverDamAt(TilePos p, bool existing) {
    const auto& hydro = *terrainWorld.hydrology;
    const auto sample = [&](int x, int z) -> std::pair<int, WaterSurface> {
        if (const auto* column = waterColumnAt({x, 0, z}))
            return {column->surface_y, column->water};
        if (hydro.containsColumn(x, z)) return hydro.sampleColumn(x, z);
        return {-1, {}};
    };
    const auto [ground, water] = sample(p.x, p.z);
    if (!water.reach || !water.fills(p.y, ground)) return {};
    const auto flow = water.flow;
    if (flow.x == 0 && flow.y == 0) return {};
    const auto wrapX = wrap();
    constexpr int Radius = ReservoirSnapshot::RiverStage::DamRadius;
    constexpr float Port = 9;
    const auto along = [&](TilePos q) {
        return WorldWrap::delta(q.x, p.x, wrapX.tiles) * flow.x +
               (q.z - p.z) * flow.y;
    };
    SavedWaterTerrain saved(waterTerrainEdits);
    const auto solid = [&](TilePos q) {
        if (const auto* cell = tileAt(q.x, q.y, q.z)) return cell->isSolid();
        return saved.addedSolid(q);
    };
    std::unordered_map<uint64_t, std::pair<int, WaterSurface>> columns;
    const auto columnAt = [&](TilePos q) {
        const auto key = ReservoirSnapshot::columnKey(q.x, q.z);
        if (const auto it = columns.find(key); it != columns.end())
            return it->second;
        return columns.emplace(key, sample(q.x, q.z)).first->second;
    };
    std::vector<TilePos> pending;
    std::unordered_set<TilePos> seen;
    // Near a headwater the wet approach can be only one voxel long. Use
    // the actual open spring as a source instead of requiring nine blocks
    // of upstream channel (or inventing a source behind a plugged spring).
    const auto& reaches = hydro.riverReaches();
    const auto route = std::find_if(
        reaches.begin(), reaches.end(),
        [&](const auto& reach) { return reach.id == water.reach; });
    if (route != reaches.end() && !route->lakeSource) {
        const TilePos source{route->spring.tile.x, route->spring.rockY + 1,
                             route->spring.tile.y};
        if (std::abs(WorldWrap::delta(source.x, p.x, wrapX.tiles)) <= Radius &&
            std::abs(source.z - p.z) <= Radius && along(source) < Port) {
            const auto [bed, surface] = columnAt(source);
            if (surface.reach == water.reach && surface.fills(source.y, bed) &&
                (existing || !solid(source))) {
                seen.insert(source);
                pending.push_back(source);
            }
        }
    }
    // At a lake outlet the upstream port can be in standing water even
    // when there are fewer than nine river tiles before the wall.
    const uint32_t lakeBody = route != reaches.end() && route->lakeSource
                                  ? route->lakeSource->body
                                  : 0;
    const uint32_t receivingLake =
        route != reaches.end() &&
                route->end == GeneratedHydrology::RiverEnd::Lake
            ? route->receiver
            : 0;
    const bool atInlet = receivingLake &&
                         std::abs(WorldWrap::delta(route->outlet.x, p.x,
                                                   wrapX.tiles)) < Radius &&
                         std::abs(route->outlet.y - p.z) < Radius;
    const auto perimeter = [&](TilePos q) {
        return std::max<int64_t>(
                   std::abs(WorldWrap::delta(q.x, p.x, wrapX.tiles)),
                   std::abs(q.z - p.z)) == Radius;
    };
    // A final one-voxel bend can point across the inlet. At the mouth
    // the river and receiving lake identify the two ports directly.
    const auto downstreamPort = [&](TilePos q, WaterSurface surface) {
        if (atInlet) return surface.body == receivingLake && perimeter(q);
        return surface.reach == water.reach && along(q) >= Port;
    };
    // The surveyed lake perimeter supplies the outlet. Starting at every
    // lake/river face would inject water on the dry side of an inland wall.
    if (lakeBody)
        for (int dz = -Radius; dz <= Radius; ++dz)
            for (int dx = -Radius; dx <= Radius; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != Radius) continue;
                TilePos q{int(wrapX.wrapTile(p.x + dx)), 0, p.z + dz};
                const auto [bed, surface] = columnAt(q);
                if (surface.body != lakeBody) continue;
                for (q.y = bed + 1; q.y < surface.surfaceY; ++q.y)
                    if (!solid(q) && seen.insert(q).second)
                        pending.push_back(q);
            }
    bool downstream = false;
    for (int dz = -Radius; dz <= Radius; ++dz)
        for (int dx = -Radius; dx <= Radius; ++dx) {
            TilePos q{int(wrapX.wrapTile(p.x + dx)), 0, p.z + dz};
            if (!hydro.containsColumn(q.x, q.z)) continue;
            const float distance = along(q);
            if (atInlet ? !perimeter(q) : std::abs(distance) < Port) continue;
            const auto [bed, surface] = columnAt(q);
            const bool target = downstreamPort(q, surface);
            if (!target && (surface.reach != water.reach ||
                            (!atInlet && distance > -Port)))
                continue;
            for (q.y = bed + 1; q.y < surface.surfaceY; ++q.y) {
                if (solid(q)) continue;
                if (target) {
                    downstream = true;
                    break;
                }
                if (seen.insert(q).second) pending.push_back(q);
            }
        }
    if (pending.empty() || !downstream) return {};

    const auto neighbors = [&](TilePos q) {
        std::array<TilePos, 6> cells{{{q.x - 1, q.y, q.z},
                                      {q.x + 1, q.y, q.z},
                                      {q.x, q.y, q.z - 1},
                                      {q.x, q.y, q.z + 1},
                                      {q.x, q.y - 1, q.z},
                                      {q.x, q.y + 1, q.z}}};
        for (auto& cell : cells) cell.x = int(wrapX.wrapTile(cell.x));
        return cells;
    };
    const auto inside = [&](TilePos q) {
        return std::abs(WorldWrap::delta(q.x, p.x, wrapX.tiles)) <= Radius &&
               std::abs(q.z - p.z) <= Radius;
    };
    const auto wetNeighbor = [&](TilePos from,
                                 TilePos to) -> std::optional<TilePos> {
        if (!hydro.containsColumn(to.x, to.z)) return {};
        const auto [bed, surface] = columnAt(to);
        if (inside(to) && to.y >= 0 && to.y < water.surfaceY &&
            to.y >= p.y - GeneratedHydrology::MaxRiverDepth &&
            !surface.fills(to.y, bed)) {
            // Loaded cuts in a bank or floor can open a route around or
            // underneath the wall. Don't invent such openings offscreen.
            const auto* chunk = getChunk({unsigned(to.x / Chunk::SIZE_X),
                                          unsigned(to.y / Chunk::SIZE_Y),
                                          unsigned(to.z / Chunk::SIZE_Z)});
            if (chunk &&
                chunk->rootEdited(Chunk::cell(to.x % Chunk::SIZE_X,
                                              to.y % Chunk::SIZE_Y,
                                              to.z % Chunk::SIZE_Z)) &&
                !solid(to))
                return to;
        }
        if (surface.body &&
            (surface.body == lakeBody || surface.body == receivingLake) &&
            surface.fills(to.y, bed))
            return to;
        if (surface.reach != water.reach) return {};
        if (surface.fills(to.y, bed)) return to;
        if (from.y != to.y || to.y < surface.surfaceY ||
            !surface.fills(surface.topTile(), bed))
            return {};
        // A downhill step connects through air above the lower wet cell.
        // Count that drop only if the edited passage is open.
        for (int y = to.y; y >= surface.surfaceY; --y)
            if (solid({to.x, y, to.z})) return {};
        to.y = surface.topTile();
        return to;
    };
    // Flood the generated wet voxels, not a single line across them. An
    // unloaded wet voxel is conservatively passable, never part of the wall.
    bool touchesWall = false;
    for (size_t i = 0; i < pending.size(); ++i) {
        const auto q = pending[i];
        if (downstreamPort(q, columnAt(q).second)) return {};
        for (const auto neighbor : neighbors(q)) {
            const auto adjacent = wetNeighbor(q, neighbor);
            if (!adjacent) continue;
            const auto next = *adjacent;
            if (!inside(next)) {
                if (!atInlet && columnAt(next).second.reach == water.reach &&
                    std::abs(along(next)) < Port && !solid(next))
                    return {};
                continue;
            }
            if (solid(next)) {
                if (neighbor.y == q.y &&
                    (atInlet || std::abs(along(next)) < Port))
                    touchesWall = true;
            } else if (seen.insert(next).second) {
                pending.push_back(next);
            }
        }
    }
    if (!touchesWall) return {};

    // Search column saddles across the banks as well as the channel. The
    // upstream flood already checked submerged openings through the wall.
    using Column = std::pair<int, int>;
    using Cost = std::pair<int, int>;  // highest floor crossed, path length
    using Step = std::pair<Cost, Column>;
    std::priority_queue<Step, std::vector<Step>, std::greater<>> spill;
    std::map<Column, Cost> costs;
    std::map<Column, Column> parents;
    std::map<Column, int> floors;
    const auto floorAt = [&](TilePos q) {
        const Column key{q.x, q.z};
        if (const auto it = floors.find(key); it != floors.end())
            return it->second;
        q.y = columnAt(q).first + 1;
        if (const auto* edit = saved.column(q.x, q.z);
            edit && !tileAt(q.x, q.y - 1, q.z))
            q.y = edit->groundY[ColumnEdit::index(q.x % Chunk::SIZE_X,
                                                  q.z % Chunk::SIZE_Z)] +
                  1;
        while (q.y > 0) {
            const auto* below = tileAt(q.x, q.y - 1, q.z);
            if (!below || below->isSolid()) break;
            --q.y;
        }
        // The wet-voxel flood has already ruled out a submerged passage.
        // A blind pocket underneath a wall is not its spill elevation.
        const int waterline = columnAt(q).second.surfaceY;
        for (int y = waterline - 1; y >= q.y; --y) {
            if (solid({q.x, y, q.z})) {
                q.y = y + 1;
                break;
            }
        }
        while (q.y < WORLD_HEIGHT_Y && solid(q)) ++q.y;
        floors.emplace(key, q.y);
        return q.y;
    };
    std::map<Column, Reservoirs::BasinColumn> upstream;
    auto side = std::make_shared<std::vector<uint64_t>>();
    for (const auto q : pending) {
        const Column key{q.x, q.z};
        side->push_back(ReservoirSnapshot::columnKey(q.x, q.z));
        // A submerged pocket can be reachable from upstream without
        // exposing a free surface on top of the wall.
        const int floor = floorAt(q);
        if (q.y < floor || !costs.emplace(key, Cost{0, 0}).second) continue;
        spill.push({{0, 0}, key});
        const int surface = std::max(floor, columnAt(q).second.surfaceY);
        upstream.emplace(key,
                         Reservoirs::BasinColumn{{q.x, q.z}, surface, surface});
    }
    std::sort(side->begin(), side->end());
    side->erase(std::unique(side->begin(), side->end()), side->end());
    while (!spill.empty()) {
        const auto [cost, key] = spill.top();
        spill.pop();
        if (costs.at(key) != cost) continue;
        const TilePos q{key.first, 0, key.second};
        const auto [crest, length] = cost;
        if (downstreamPort(q, columnAt(q).second) &&
            floorAt(q) < columnAt(q).second.surfaceY) {
            if (!crest || crest >= WORLD_HEIGHT_Y) return {};
            Reservoirs::BreachSurvey survey{crest + 1, {}, std::move(side)};
            // Below the spill saddle, water can occupy every connected
            // low bank, including depressions reached across a lower sill.
            // Dijkstra has settled every cost below crest at this point.
            for (const auto& [bank, cost] : costs) {
                if (cost.first >= crest || upstream.contains(bank)) continue;
                const TilePos tile{bank.first, 0, bank.second};
                const auto base = columnAt(tile).second;
                if (base.wet() && base.reach != water.reach) continue;
                const int floor = std::max(floorAt(tile), base.surfaceY);
                if (floor >= crest) continue;
                upstream.emplace(
                    bank,
                    Reservoirs::BasinColumn{
                        {tile.x, tile.z}, floor, std::max(floor, cost.first)});
            }
            for (auto at = key; parents.contains(at); at = parents.at(at)) {
                const TilePos tile{at.first, 0, at.second};
                const auto [bed, surface] = columnAt(tile);
                const int floor = floorAt(tile);
                if (surface.wet() && floor < surface.surfaceY) continue;
                upstream.insert_or_assign(
                    at,
                    Reservoirs::BasinColumn{{tile.x, tile.z}, floor, crest});
            }
            for (const auto& [pos, c] : upstream)
                if (c.floor <= crest) survey.columns.push_back(c);
            if (!survey.columns.empty())
                return std::pair{water.reach, std::move(survey)};
            return {};
        }
        for (const auto neighbor : neighbors(q)) {
            if (neighbor.y ||
                std::abs(WorldWrap::delta(neighbor.x, p.x, wrapX.tiles)) >
                    DamSpillRadius ||
                std::abs(neighbor.z - p.z) > DamSpillRadius ||
                !hydro.containsColumn(neighbor.x, neighbor.z))
                continue;
            const Column target{neighbor.x, neighbor.z};
            const Cost next{std::max(crest, floorAt(neighbor)), length + 1};
            if (const auto it = costs.find(target);
                it != costs.end() && it->second <= next)
                continue;
            costs.insert_or_assign(target, next);
            parents.insert_or_assign(target, key);
            spill.push({next, target});
        }
    }
    return {};
}

// Wake newly exposed shoreline faces, including across a loaded slab edge.
void ChunkManager::findShoreIntakes(ChunkCoord coord, const Reservoirs& water) {
    const auto wetSlab = [&](int dx, int dz) {
        const auto cx = wrap().wrapChunk(int(coord.x) + dx);
        const int cz = int(coord.z) + dz;
        if (cx < 0 || cz < 0) return false;
        const auto it = chunks.find({unsigned(cx), coord.y, unsigned(cz)});
        return it != chunks.end() && (!it->second.waterBodies.empty() ||
                                      !it->second.waterReaches.empty());
    };
    if (!wetSlab(0, 0) && !wetSlab(-1, 0) && !wetSlab(1, 0) &&
        !wetSlab(0, -1) && !wetSlab(0, 1))
        return;
    const int y0 = int(coord.y) * Chunk::SIZE_Y;
    const auto inspect = [&](TilePos from, TilePos to) {
        const auto* a = waterColumnAt(from);
        const auto* b = waterColumnAt(to);
        if (!a || !b || !a->water.wet() ||
            (a->water.body == b->water.body &&
             a->water.reach == b->water.reach))
            return;
        // The lake and its outlet already exchange water through the graph.
        // Only excavated passages should also draw from this connection.
        if (water.hasLakeOutletFlow(a->water)) return;
        const int floor = std::max({y0, a->surface_y + 1, b->surface_y + 1});
        const int top = std::min(y0 + Chunk::SIZE_Y, a->water.surfaceY);
        for (int y = floor; y < top; ++y) {
            from.y = to.y = y;
            if (waterAccess(to) != Reservoirs::CellAccess::Open ||
                waterSourceAt(to).wet() || !waterSourceAt(from).wet())
                continue;
            if (waterEdits.insert(to).second)
                chunks
                    .at({unsigned(to.x / Chunk::SIZE_X), coord.y,
                         unsigned(to.z / Chunk::SIZE_Z)})
                    .shoreIntakes.push_back(to);
            break;
        }
    };
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int x = 0; x < Chunk::SIZE_X; ++x) {
            const TilePos p{int(coord.x) * Chunk::SIZE_X + x, y0,
                            int(coord.z) * Chunk::SIZE_Z + z};
            for (const auto d :
                 std::array<Vec2i, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
                const TilePos q{int(wrap().wrapTile(p.x + d.x)), y0, p.z + d.y};
                inspect(p, q);
                if (x + d.x < 0 || x + d.x >= Chunk::SIZE_X || z + d.y < 0 ||
                    z + d.y >= Chunk::SIZE_Z)
                    inspect(q, p);
            }
        }
}

void ChunkManager::advanceWater(
    uint64_t targetStep, std::chrono::steady_clock::duration workBudget) {
    auto* water = reservoirs();
    if (!water) return;
    updateWaterTopology(*water);
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < MaxWaterStepsPerUpdate && water->step() < targetStep;
         ++i) {
        for (auto coord : pendingShoreChecks) findShoreIntakes(coord, *water);
        pendingShoreChecks.clear();
        surveyChannels(*water, 4);
        if (!water->needsFixedSteps()) {
            if (waterEdits.empty()) {
                water->advanceTo(targetStep);
                return;
            }
            // A surveyed channel has a known sink and a storage curve. Only
            // that case can skip local steps without skipping a flow front.
            if (targetStep - water->step() > MaxWaterStepsPerUpdate &&
                std::all_of(
                    waterEdits.begin(), waterEdits.end(),
                    [&](TilePos p) { return water->hasChannelMouth(p); })) {
                tickWater(*water, targetStep);
                return;
            }
        }
        tickWater(*water, water->step() + 1);
        if (std::chrono::steady_clock::now() - start >= workBudget) break;
    }
}

void ChunkManager::tickWater(Reservoirs& water, uint64_t step) {
    const auto elapsed = step - water.step();
    std::vector<Reservoirs::RiverIntake> intakes;
    auto it = waterEditCursor ? waterEdits.upper_bound(*waterEditCursor)
                              : waterEdits.begin();
    const size_t EditBudget = elapsed > 1 ? 8 : 64;
    for (size_t i = 0; i < std::min(EditBudget, waterEdits.size()); ++i) {
        if (it == waterEdits.end()) it = waterEdits.begin();
        const auto p = *it++;
        waterEditCursor = p;
        if (water.hasBreach(p) || water.feedsPool(p)) continue;
        using Access = Reservoirs::CellAccess;
        const auto access = waterAccess(p);
        if (access != Access::Open) continue;
        if (const auto* baseline = waterColumnAt(p);
            baseline && baseline->water.reach &&
            baseline->water.fills(p.y, baseline->surface_y) &&
            waterSourceAt(p).wet())
            continue;
        // A wet face above or beside a cut is evidence of a connection.
        WaterSurface source;
        bool belowDam = false;
        const auto state = water.snapshot();
        for (auto q : {TilePos{p.x, p.y + 1, p.z}, TilePos{p.x - 1, p.y, p.z},
                       TilePos{p.x + 1, p.y, p.z}, TilePos{p.x, p.y, p.z - 1},
                       TilePos{p.x, p.y, p.z + 1}}) {
            auto face = waterSourceAt(q);
            if (face.reach)
                face.topFill = face.surfaceFill(
                    q.x + .5f, q.z + .5f,
                    float(step % 86400) *
                        (WorldClock::DayLengthSeconds / 86400.));
            if (!face.body && !face.reach) continue;
            if (face.height() <= source.height()) continue;
            source = face;
            q.x = int(wrap().wrapTile(q.x));
            const auto stage = state->riverStages.find(face.reach);
            const auto* column = waterColumnAt(q);
            belowDam = column && stage != state->riverStages.end() &&
                       stage->second.damAge &&
                       stage->second.belowCut(q.x, q.z, column->water.surfaceY);
        }
        if (source.reach)
            intakes.push_back({source.reach, p, false, source, belowDam});
        else if (source.body)
            water.drainCut(
                source.body, p, water.hasChannelMouth(p) ? 1 : elapsed,
                [&](TilePos cell) { return waterAccess(cell); }, wrap().tiles);
    }
    water.advanceTo(
        step, intakes, [&](TilePos p) { return waterAccess(p); }, wrap().tiles);
    water.reconcile();
    flowWater(512);
}

size_t ChunkManager::flowWater(size_t cellBudget) {
    if (!reservoirs()) return 0;
    return reservoirs()->flow(
        cellBudget, [&](TilePos p) { return waterAccess(p); }, wrap().tiles,
        [&](TilePos p) { return waterSinkAt(p); });
}

std::shared_ptr<const ChunkColumnInputs> ChunkManager::projectedColumns(
    std::shared_ptr<const ChunkColumnInputs> columns, unsigned cx, unsigned cz,
    const ReservoirSnapshot& snapshot) const {
    if (!columns || (snapshot.levels.empty() && snapshot.cells.empty() &&
                     snapshot.riverStages.empty() && snapshot.pools.empty() &&
                     snapshot.streams.empty()))
        return columns;
    auto result = std::make_shared<ChunkColumnInputs>(*columns);
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int x = 0; x < Chunk::SIZE_X; ++x) {
            auto& c = result->columns[z * Chunk::SIZE_X + x];
            c.water = snapshot.project(
                terrainWorld.hydrology.get(), int(cx) * Chunk::SIZE_X + x,
                int(cz) * Chunk::SIZE_Z + z, c.surface_y, c.water);
        }
    return result;
}

bool ChunkManager::reconcileWater(
    Chunk& chunk, ChunkCoord coord, const ChunkColumnInputs* columns,
    bool refreshLocal,
    const std::bitset<Chunk::SIZE_X * Chunk::SIZE_Z>* dirty) const {
    if (!columns || !reservoirs()) return false;
    const auto snapshot = reservoirs()->snapshot();
    bool changed = false;
    const int y0 = int(coord.y) * Chunk::SIZE_Y;
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int x = 0; x < Chunk::SIZE_X; ++x) {
            if (dirty && !dirty->test(z * Chunk::SIZE_X + x)) continue;
            const auto& c = columns->columns[z * Chunk::SIZE_X + x];
            const int wx = int(coord.x) * Chunk::SIZE_X + x;
            const int wz = int(coord.z) * Chunk::SIZE_Z + z;
            const bool hasReservoir =
                chunk.projectsWater && (c.water.body || c.water.reach) &&
                c.water.surfaceY > y0 && c.surface_y + 1 < y0 + Chunk::SIZE_Y;
            int poolGround = c.surface_y;
            const auto pool = snapshot->pools.empty() || !chunk.projectsWater
                                  ? WaterSurface{}
                                  : snapshot->projectLocal(wx, wz, c.surface_y,
                                                           {}, &poolGround);
            const auto stream =
                snapshot->streams.find(ReservoirSnapshot::columnKey(wx, wz));
            if (stream == snapshot->streams.end() && !hasReservoir &&
                !refreshLocal && snapshot->localTop(wx, wz) <= y0 &&
                pool.surfaceY <= y0)
                continue;
            auto standing = hasReservoir ? snapshot->projectStanding(
                                               terrainWorld.hydrology.get(), wx,
                                               wz, c.surface_y, c.water)
                                         : WaterSurface{};
            if (hasReservoir && !standing.wet())
                standing = snapshot->projectLakeInflow(
                    terrainWorld.hydrology.get(), wx, wz, c.surface_y,
                    c.water.body);
            for (int y = 0; y < Chunk::SIZE_Y; ++y) {
                auto& tile = chunk.tiles[z][y][x];
                const bool oldProjectedRiver =
                    hasReservoir && c.water.reach &&
                    c.water.fills(y0 + y, c.surface_y) &&
                    tile.liquid == LiquidType::Water &&
                    tile.liquidLevel == 255 &&
                    !chunk.rootEdited(Chunk::cell(x, y, z));
                if (tile.isSolid() ||
                    (tile.isLiquid() &&
                     tile.waterOrigin == WaterOrigin::Independent &&
                     !oldProjectedRiver))
                    continue;
                const auto local = snapshot->cells.fill({wx, y0 + y, wz});
                uint8_t fill = 0;
                if (c.water.reach || c.water.body > GeneratedHydrology::Sea ||
                    !chunk.rootEdited(Chunk::cell(x, y, z)))
                    fill = standing.fillAt(y0 + y, c.surface_y);
                if (pool.body) {
                    // The pool's storage starts above the river's top
                    // voxel. Submerging it also covers its partial top.
                    if (fill && y0 + y == poolGround &&
                        pool.surfaceY > poolGround + 1)
                        fill = 255;
                    fill = std::max(fill, pool.fillAt(y0 + y, poolGround));
                }
                if (stream != snapshot->streams.end() &&
                    y0 + y >= stream->second.bottom &&
                    y0 + y <= stream->second.top)
                    fill = std::max<uint8_t>(
                        fill, y0 + y == stream->second.top ? 64 : 255);
                // A submerged local credit still belongs to the ledger,
                // but the voxel's surface is the higher of the two fills.
                fill = std::max(fill, local);
                const auto origin = local  ? WaterOrigin::Local
                                    : fill ? WaterOrigin::Reservoir
                                           : WaterOrigin::Independent;
                if (origin == tile.waterOrigin && fill == tile.liquidLevel)
                    continue;
                tile.liquid = fill ? LiquidType::Water : LiquidType::None;
                tile.liquidLevel = fill;
                tile.waterOrigin = origin;
                changed = true;
            }
        }
    return changed;
}

void ChunkManager::indexWater(ChunkCoord coord,
                              const ChunkColumnInputs& columns) {
    auto& bodies = chunks.at(coord).waterBodies;
    const int y0 = int(coord.y) * Chunk::SIZE_Y;
    for (const auto& c : columns.columns)
        if (c.water.body && !c.water.reach && c.water.surfaceY > y0 &&
            c.surface_y + 1 < y0 + Chunk::SIZE_Y &&
            std::find(bodies.begin(), bodies.end(), c.water.body) ==
                bodies.end())
            bodies.push_back(c.water.body);
    for (auto body : bodies) waterChunks[body].insert(coord);
    auto& reaches = chunks.at(coord).waterReaches;
    for (const auto& c : columns.columns)
        if (c.water.reach && c.water.surfaceY > y0 &&
            c.surface_y + 1 < y0 + Chunk::SIZE_Y &&
            std::find(reaches.begin(), reaches.end(), c.water.reach) ==
                reaches.end()) {
            reaches.push_back(c.water.reach);
            waterReachChunks[c.water.reach].insert(coord);
        }
}

void ChunkManager::reservoirChanged(const ReservoirChange& change) {
    std::unordered_set<ChunkCoord> affected;
    for (const auto body : change.bodies)
        if (const auto it = waterChunks.find(body); it != waterChunks.end())
            affected.insert(it->second.begin(), it->second.end());
    for (const auto reach : change.reaches)
        if (const auto it = waterReachChunks.find(reach);
            it != waterReachChunks.end())
            affected.insert(it->second.begin(), it->second.end());
    std::unordered_set<uint64_t> poolColumns;
    if (change.pools)
        for (const auto body : change.bodies)
            if (const auto it = change.pools->find(body);
                it != change.pools->end())
                for (const auto p : it->second)
                    poolColumns.insert((uint64_t(p.x / Chunk::SIZE_X) << 32) |
                                       unsigned(p.y / Chunk::SIZE_Z));
    for (const auto key : poolColumns)
        if (columnInputs.contains(key))
            for (unsigned y = 0; y < Y_CHUNKS; ++y)
                affected.insert({unsigned(key >> 32), y, unsigned(key)});
    std::unordered_map<ChunkCoord, std::bitset<Chunk::SIZE_X * Chunk::SIZE_Z>>
        dirty;
    const bool localOnly = change.bodies.empty() && change.reaches.empty();
    const auto mark = [&](TilePos p) {
        const ChunkCoord coord{unsigned(p.x / Chunk::SIZE_X),
                               unsigned(p.y / Chunk::SIZE_Y),
                               unsigned(p.z / Chunk::SIZE_Z)};
        affected.insert(coord);
        if (localOnly)
            dirty[coord].set((p.z % Chunk::SIZE_Z) * Chunk::SIZE_X +
                             p.x % Chunk::SIZE_X);
    };
    if (change.local) mark(*change.local);
    for (const auto p : change.locals) mark(p);
    std::unordered_set<uint64_t> columns;
    std::unordered_set<ChunkCoord> remesh;
    for (const auto coord : affected) {
        const auto key = (uint64_t{coord.x} << 32) | coord.z;
        const auto it = columnInputs.find(key);
        if (it == columnInputs.end()) continue;
        bool changed = false;
        withChunkLocked(coord, [&](Chunk& chunk) {
            changed = reconcileWater(chunk, coord, it->second.data.get(),
                                     change.local || !change.locals.empty() ||
                                         poolColumns.contains(key),
                                     localOnly ? &dirty.at(coord) : nullptr);
        });
        if (!changed) continue;
        columns.insert(key);
        remesh.insert(coord);
        for (const auto& offset :
             std::array<std::array<int, 3>, 6>{{{-1, 0, 0},
                                                {1, 0, 0},
                                                {0, -1, 0},
                                                {0, 1, 0},
                                                {0, 0, -1},
                                                {0, 0, 1}}}) {
            const auto x = wrap().wrapChunk(int64_t(coord.x) + offset[0]);
            const auto y = int64_t(coord.y) + offset[1];
            const auto z = int64_t(coord.z) + offset[2];
            if (x >= 0 && y >= 0 && z >= 0)
                remesh.insert({unsigned(x), unsigned(y), unsigned(z)});
        }
        markLightChanged(coord);
        waterChanged.emit(coord);
    }
    if (!change.reaches.empty() || !change.bodies.empty())
        for (const auto coord : affected)
            if (getChunk(coord)) pendingShoreChecks.insert(coord);
    for (const auto coord : remesh) markDirty(coord, true, true);
    for (const auto key : columns) columnWaterChanged.emit(key);
}
