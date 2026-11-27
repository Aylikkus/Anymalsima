#pragma once

#include <bitset>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "camera_work_queue.hpp"
#include "chunk_mesher.hpp"
#include "consts.hpp"
#include "flora_generator.hpp"
#include "layers_generator.hpp"
#include "location_generator.hpp"
#include "render/ui_context.hpp"
#include "reservoirs.hpp"
#include "save/region_file.hpp"
#include "structure_overrides.hpp"
#include "tree_wood_lod.hpp"

class WorldEditor;
class TerrainEdits;

enum class ChunkState { Unloaded, Queued, Ready };
enum class MeshState { None, Queued, Built };

struct ChunkHandle {
    Vec2u regionCoord{};
    uint64_t identity = 0;
    ChunkState state = ChunkState::Unloaded;
    std::shared_ptr<Chunk> data;
    MeshState meshState = MeshState::None;
    bool dirtyPending = false;
    bool meshUrgent = false;
    uint64_t meshRevision = 0;
    // Terrain/neighbor changes invalidate snapshots. Water-only changes can
    // leave a slightly older mesh drawable until its successor is ready.
    uint64_t meshTopologyRevision = 0;
    MeshHandle mesh, waterMesh;
    LakeMeshSurface lakeSurface;
    bool hasWaterGeometry = false;
    bool initialised = false;
    bool uploaded = false;
    uint64_t uploadedRevision = 0;
    // Tile contents differ from what the region file holds (freshly
    // generated, or modified through markModified). Lighting never sets it.
    bool persistDirty = false;
    // Loaded from disk with solid/air different from generated terrain
    // (an earlier gameplay edit).
    mutable std::optional<bool> edited;
    std::vector<uint32_t> waterBodies;
    std::vector<uint32_t> waterReaches;
    std::vector<TilePos> shoreIntakes;
    // Generated structure voxels at the revision used to prepare this chunk.
    // Tree wood is reconciled separately through TreeStamp.
    std::shared_ptr<const structure_voxels::Slab> structureBaseline;
    // Plants anchored in this chunk (null: none). Replaced, never mutated.
    FloraLayerPtr flora;
    // When its tree wood was last brought up to date (saved with plants).
    TreeStamp trees;
    std::vector<BlockVertex> vertices;
    std::vector<uint32_t> indices;

    bool canPublishMesh(uint64_t revision) const {
        return revision >= meshTopologyRevision &&
               (!uploaded || revision > uploadedRevision);
    }
};

struct FinishedMesh {
    ChunkCoord coord;
    ChunkMesh mesh;
    uint64_t identity;
    uint64_t revision;
};

struct SolidCellRevision {
    uint64_t chunkIdentity;
    uint64_t topology;
    uint16_t index;
    bool operator==(const SolidCellRevision&) const = default;
};

struct VersionedSolidCell {
    SolidCellSnapshot cell;
    SolidCellRevision revision;
};

// Main-thread requests/dispatch/publication, with bounded immutable worker
// jobs.
class ChunkManager {
   public:
    ChunkManager(const LocationGenerator& gen, ThreadPool& pool,
                 const ChunkMesher* mesher, const World& world, IAssets* assets,
                 std::optional<std::vector<TreeLandmark>> preparedLandmarks =
                     std::nullopt);
    ~ChunkManager();

    Signal<const ChunkCoord&> becameReady;
    Signal<const ChunkCoord&> removed;
    // Tile light or opacity of a Ready chunk changed. Geometry may be
    // unchanged; listeners refresh light volumes around the chunk.
    Signal<const ChunkCoord&> lightChanged;
    Signal<const ChunkCoord&> waterChanged;
    Signal<uint64_t> columnWaterChanged;
    const World& world() const { return terrainWorld; }
    Reservoirs* reservoirs() const { return terrainWorld.reservoirs.get(); }
    WaterVolume transferWater(uint32_t body, WaterVolume amount,
                              ReservoirExchange reason);
    WaterVolume divertWater(uint32_t body, TilePos recipient,
                            WaterVolume amount);
    size_t flowWater(size_t cellBudget);
    // The standing body or reach owning a loaded reservoir water cell. The
    // dated weather input boundary uses it to route rain and meltwater.
    std::optional<Reservoirs::Sink> waterSinkAt(TilePos pos);
    // Recheck edited faces against live tiles before moving any water.
    void waterBlockEdited(TilePos pos);
    void commitWaterEdits();
    void setWaterTerrainEdits(const TerrainEdits* edits) {
        waterTerrainEdits = edits;
    }
    // Allows cheap steps to keep up with x720 at 60 FPS. The time budget
    // takes precedence when steps are expensive.
    static constexpr size_t MaxWaterStepsPerUpdate = 256;
    // Advance toward the clock's game seconds, leaving unprocessed seconds
    // on the ledger for later frames. Check the time budget between steps.
    void advanceWater(uint64_t targetStep,
                      std::chrono::steady_clock::duration workBudget =
                          std::chrono::milliseconds(3));
    // The aquifer saturation of the rock stratum at this tile, or 0 when the
    // tile is air, water, soil, or above the surface. Springs register an
    // outlet where an edit exposes such rock to open air.
    float aquiferAt(int x, int y, int z);
    // Emitted when an edit exposes or seals an aquifer-bearing rock face:
    // (tile, exposed). TerrainEditState routes these to Reservoirs outlets.
    Signal<std::pair<TilePos, bool>> aquiferExposed;
    // A chunk's drawn geometry changed (new mesh uploaded, or emptied).
    Signal<const ChunkCoord&> meshUploaded;
    // The plants of a Ready chunk changed (setFlora). Chunks becoming
    // ready announce their plants through becameReady.
    Signal<const ChunkCoord&> floraChanged;
    // Generated structures changed in a column: key (x << 32 | z).
    Signal<uint64_t> columnStructuresChanged;
    Signal<const ChunkCoord&> structureRebased;
    // A chunk column's last chunk was unregistered: column key.
    Signal<uint64_t> columnReleased;
    void setFocus(Vec3 value) { focus = value; }
    // X wrapping of the world this manager streams (disabled without a map).
    WorldWrap wrap() const { return terrainWorld.wrap(); }
    void update(WorldEditor* editor = nullptr);
    // stored: saved chunk payload; the chunk is decoded instead of generated.
    // storedFlora: its saved plants; a saved chunk without them regrows its
    // generated plants.
    void registerChunk(const ChunkCoord& coord, const ChunkCoord& regionOrigin,
                       const Region* region, ChunkBlob stored = nullptr,
                       ChunkBlob storedFlora = nullptr,
                       ChunkBlob storedBaseline = nullptr);
    Chunk* request(const ChunkCoord& coord);
    Chunk* requestLighting(const ChunkCoord& coord, ChunkCoord beneficiary);
    bool isRegistered(const ChunkCoord& coord) const;
    bool isReady(const ChunkCoord& coord) const;
    // Requested and waiting for (or running) its generation job.
    bool isQueued(const ChunkCoord& coord) const;
    // A ready slab differs from the terrain or structures it was built against.
    bool isEdited(const ChunkCoord& coord) const;
    uint8_t generatedGroundBlock(unsigned chunkX, unsigned chunkZ,
                                 const ColumnGenerationInput& column,
                                 int y) const;
    const structure_voxels::Slab* structureBaseline(
        const ChunkCoord& coord) const;
    // Generated surface inputs of a registered chunk column, or null for an
    // unregistered column or a world without a map (fixtures).
    std::shared_ptr<const ChunkColumnInputs> generatedColumns(unsigned chunkX,
                                                              unsigned chunkZ);
    // Uploaded for the current solid topology; water may have stepped again.
    bool isMeshUploaded(const ChunkCoord& coord) const;
    // An older uploaded revision remains drawable while its replacement is
    // queued (for example, after sunlight changes).
    bool hasUploadedMesh(const ChunkCoord& coord) const;
    Chunk* getChunk(const ChunkCoord& coord);
    std::shared_ptr<Chunk> getChunkShared(const ChunkCoord& coord);
    const Tile* tileAt(int tx, int ty, int tz);
    std::optional<ShapedTile> cellAt(int tx, int ty, int tz);
    // Owner-thread read. The returned value may outlive the loaded slab.
    std::optional<VersionedSolidCell> solidCellAt(TilePos pos);
    // The chunk's own solids without the environmental projection. Used by
    // the snow simulation as its terrain.
    std::optional<tile_geometry::CellGeometry> rawCellGeometryAt(TilePos pos);
    // Projected water at a loaded column. `ground` is the column's surface
    // tile. Missing columns return nullopt.
    std::optional<std::pair<int, WaterSurface>> projectedWaterAt(int x, int z);
    // Environmental solids projected over the world (deposited snow). They
    // participate in meshing and queries but never in saves or edit history.
    void setEnvironment(std::shared_ptr<const EnvironmentMap> environment);
    std::shared_ptr<const ChunkEnvironment> environmentFor(
        const ChunkCoord& coord) const;
    // In-memory storage edit for the owner thread. This invalidates mesh
    // snapshots. Gameplay edits go through WorldEditor for persistence
    // and notifications.
    CellPieceEdit setCellPieces(TilePos pos, SolidCellRevision expected,
                                std::span<const CellPiece> replacement,
                                bool playerEdit = false);
    // Main-thread view of the substrate under a living root.
    const RootVoxel* rootAt(int x, int y, int z);
    void unregister(const ChunkCoord& coord);
    void markDirty(const ChunkCoord& coord, bool urgent = false,
                   bool waterOnly = false);
    void markAllDirty();
    // Announces new light values without remeshing.
    void markLightChanged(const ChunkCoord& coord);
    // Records a gameplay edit of a Ready chunk, including shape-only changes.
    // This invalidates pending topology work and marks the chunk for saving.
    void markModified(const ChunkCoord& coord);
    bool isPersistDirty(const ChunkCoord& coord) const;
    // Plants of a Ready chunk (null when none or not ready).
    FloraLayerPtr flora(const ChunkCoord& coord) const;
    std::optional<float> floraAnchorHeight(const ChunkCoord& coord,
                                           const FloraInstance& plant,
                                           const FloraSpeciesRule& rule);
    // Replaces a Ready chunk's plants and announces it (floraChanged).
    // persist: the change must be saved (false for derived bookkeeping such
    // as FloraLayer::resolvedDay, which is recomputed after a reload).
    void setFlora(const ChunkCoord& coord, FloraLayerPtr layer,
                  bool persist = true);
    // Species rules (empty without flora data).
    const FloraSpeciesTable& floraSpecies() const { return *floraTable; }
    // Seed of the world's plant placement (generateFlora).
    uint32_t floraSeed() const { return floraSeed_; }
    // Tree species rules (empty without tree data).
    const TreeSpeciesTable& treeSpecies() const { return *treeTable; }
    // Living wood block ids: edit detection and distant structures treat
    // them as air (docs/handoff/trees.md).
    const GrownBlockSet& grownBlocks() const { return treeTable->grown(); }
    // Massive trees of the whole map (placeTreeLandmarks).
    const std::vector<TreeLandmark>& treeLandmarks() const { return landmarks; }
    // Trees over a registered chunk column, or null while its region tree
    // jobs are still running (or for an unregistered column).
    std::shared_ptr<const TreeColumn> treeColumn(unsigned chunkX,
                                                 unsigned chunkZ) const;
    // What a registered chunk column's generated content comes from:
    // terrain inputs and structure voxels. Prepared on demand (terrain
    // sampling, tree set jobs); std::nullopt until the trees are ready, for
    // unregistered columns and without a world map. Main thread.
    struct ColumnSources {
        std::shared_ptr<const ChunkColumnInputs> terrain;
        structure_voxels::ColumnPtr structures;
        bool hasTrees = false;
    };
    std::optional<ColumnSources> columnSources(unsigned chunkX,
                                               unsigned chunkZ);
    // Day tree wood is generated for (setFloraDay).
    int32_t woodDay() const { return floraDay; }
    // Capture a column's generation inputs without registering any slabs.
    // The returned work may generate missing tree sets on a worker.
    std::function<ColumnSources()> snapshotColumnSources(unsigned cx,
                                                         unsigned cz);
    // Replace the generated indexes for kinds other than trees. This affects
    // generation, sky and landmarks. Live tile changes go through WorldEditor.
    void setGeneratedStructureKinds(std::vector<structure_voxels::Kind> kinds);
    void appendStructureLandmarks(
        std::vector<structure_voxels::SourcePtr>& sources, const Vec3& eye,
        float range) const;
    void setStructureOverrides(structure_voxels::Overrides overrides);
    const structure_voxels::Overrides& structureOverrides() const {
        return savedStructures;
    }
    uint64_t structureOverrideRevision() const { return overrideRevision; }
    uint64_t generatedStructureRevision() const { return structureRevision; }
    // Tree stamp of a Ready chunk.
    TreeStamp treeStamp(const ChunkCoord& coord) const;
    // Records that a Ready chunk's wood is up to date with `stamp`.
    // persist: its tiles changed and must be saved with the stamp.
    void setTreeStamp(const ChunkCoord& coord, TreeStamp stamp, bool persist);
    // Cut and planted trees (trees.dat). Set the loaded overrides before
    // regions register; later changes come with updateTree.
    void setTreeOverrides(TreeOverridesPtr overrides);
    const TreeOverridesPtr& treeOverrides() const { return overrides; }
    // Seed of tree placement (RegionTreesInput::seed).
    uint32_t treePlacementSeed() const { return treeSeed; }
    // Replaces (or adds) one tree in the set of the region holding its
    // anchor and rebuilds the tree columns it covers (old and new bounds).
    // Chunk jobs that used an older column are redone.
    void updateTree(const TreeInstancePtr& tree,
                    const TreeTileBox& oldBounds = {});
    // Takes a tree out of the world (it died): its region set, landmark
    // cache, billboards and the tree lists of the columns it touched. Its
    // removal must already be in the overrides (TreeRecord::removed, or
    // the planted record erased), so rebuilt sets agree.
    void removeTree(const TreeInstancePtr& tree);
    // fn(chunkX, chunkZ, column) for every built tree column with trees.
    template <typename F>
    void forEachTreeColumn(F&& fn) const {
        for (const auto& [key, cache] : columnInputs)
            if (cache.trees && !cache.trees->trees.empty())
                fn(unsigned(key >> 32), unsigned(key & 0xFFFFFFFFu),
                   *cache.trees);
    }
    // fn(tree) for every tree of the built region sets (the regions around
    // the loaded area: distant trees).
    template <typename F>
    void forEachTree(F&& fn) const {
        for (const auto& [region, set] : treeSets)
            for (const auto& tree : *set) fn(tree);
    }
    // Landmarks within `range` tiles of (x, z), built ones only; the
    // regions of missing ones get tree jobs. Landmarks stay cached once
    // built (a handful per world), so they are seen from kilometres away.
    std::vector<TreeInstancePtr> landmarkTrees(double x, double z,
                                               double range);
    // Regions whose tree sets the renderer wants (crowns out to the distant
    // mesh range), nearest first: they get low-priority-order tree jobs
    // after the sets chunks wait for, and stay built while wanted.
    void wantTreeSets(const std::vector<Vec2u>& regions);
    bool hasTreeSet(Vec2u region) const { return treeSets.contains(region); }
    // Increases whenever a region tree set is built or released.
    uint64_t treeSetsRevision() const { return treeSetsRevision_; }
    // Far billboards of a region (regionBillboards), or null until built.
    // Cached until releaseBillboards.
    using BillboardSet = std::shared_ptr<const std::vector<TreeBillboard>>;
    BillboardSet billboards(Vec2u region) const;
    // Regions whose billboards are wanted, nearest first: update() keeps
    // MaxBillboardJobs low-priority jobs busy building them.
    void wantBillboards(std::vector<Vec2u> regions);
    // Increases whenever a billboard set is built.
    uint64_t billboardRevision() const { return billboardRevision_; }
    // Drops billboard sets of regions farther than `range` tiles.
    void releaseBillboards(double x, double z, double range);
    static constexpr size_t MaxBillboardJobs = 2;
    // World map size in regions along Z (0 without a map).
    int worldHeightRegions() const { return terrainWorld.height; }
    // Increases whenever a tree changes (renderers refresh).
    uint64_t treeRevision() const { return treeRevision_; }
    // Game day new chunks generate their plants for: the epoch's placement
    // (FloraSimulation::CatchUpDays long), ages counted back from this day.
    void setFloraDay(int32_t day) { floraDay = std::max(0, day); }
    static uint32_t floraEpochSalt(int32_t day) {
        return uint32_t(day / FloraEpochDays) * 0x9e3779b9u;
    }
    static constexpr int32_t FloraEpochDays = 365;
    // Plants of a Ready chunk as generation would place them with the world
    // seed varied by `salt`, on its live tiles. Synchronous (a few ms);
    // empty without generation inputs (map-less fixtures).
    FloraLayer regenerateFlora(const ChunkCoord& coord, uint32_t salt);
    // Captures immutable copies of dirty Ready chunks and clears their
    // persistDirty flags.
    RegionSnapshot snapshotRegion(Vec2u region);
    // The same for one chunk column (all slabs), appended to out.
    void snapshotColumn(unsigned cx, unsigned cz, RegionSnapshot& out);
    int getColumnTopY(unsigned cx, unsigned cz) const;
    int getStructureTopY(unsigned cx, unsigned cz) const;
    void raiseColumnTopY(unsigned cx, unsigned cz, int worldY);
    int generatedSurfaceY(unsigned tileX, unsigned tileZ);
    size_t cachedColumnInputs() const { return columnInputs.size(); }
    // Uploads finished meshes. Call on the main thread before drawing. Kept
    // inline: the headless test library does not link bgfx.
    void uploadReadyMeshes(IRenderer& renderer) {
        // Meshes whose GPU buffers could not be created (out of bgfx
        // handles) wait for the next frame; the chunk stays not uploaded, so
        // distant proxies and tree stand-ins keep covering it.
        decltype(readyMeshes) retry;
        for (auto& result : readyMeshes) {
            auto it = chunks.find(result.coord);
            if (it == chunks.end() || it->second.identity != result.identity ||
                !it->second.canPublishMesh(result.revision))
                continue;
            auto& handle = it->second;
            // Reserve both replacements before touching either drawn buffer.
            // A failed allocation keeps the previously complete presentation.
            MeshHandle ground, water;
            const auto reserve = [&](const ChunkGeometry& geometry,
                                     const MeshHandle& existing,
                                     MeshHandle& replacement) {
                if (geometry.vertices.empty()) return true;
                if (existing.drawable()) {
                    const auto& gpu = std::get<DynamicMesh>(existing.get());
                    if (geometry.vertices.size() <= gpu.capacityVerts &&
                        geometry.indices.size() <= gpu.capacityIndices)
                        return true;
                }
                replacement.initDynamic<BlockVertex>(&renderer,
                                                     geometry.vertices.size(),
                                                     geometry.indices.size());
                return replacement.drawable();
            };
            if (!reserve(result.mesh, handle.mesh, ground) ||
                !reserve(result.mesh.water, handle.waterMesh, water)) {
                retry.push_back(std::move(result));
                continue;
            }
            const auto upload = [](const ChunkGeometry& geometry,
                                   MeshHandle& existing,
                                   MeshHandle& replacement) {
                if (geometry.vertices.empty()) {
                    existing.release();
                    return;
                }
                if (replacement.drawable()) {
                    replacement.update(
                        geometry.vertices.data(), geometry.vertices.size(),
                        geometry.indices.data(), geometry.indices.size(), true);
                    existing = std::move(replacement);
                } else {
                    existing.update(
                        geometry.vertices.data(), geometry.vertices.size(),
                        geometry.indices.data(), geometry.indices.size(), true);
                }
            };
            upload(result.mesh, handle.mesh, ground);
            upload(result.mesh.water, handle.waterMesh, water);
            handle.initialised = !result.mesh.vertices.empty();
            handle.uploaded = true;
            handle.uploadedRevision = result.revision;
            handle.lakeSurface = result.mesh.lakeSurface;
            handle.vertices = std::move(result.mesh.vertices);
            handle.indices = std::move(result.mesh.indices);
            handle.hasWaterGeometry = !result.mesh.water.indices.empty();
            if (handle.initialised || handle.hasWaterGeometry)
                meshed.insert(result.coord);
            else
                meshed.erase(result.coord);
            meshUploaded.emit(result.coord);
        }
        readyMeshes = std::move(retry);
    }
    MeshHandle* getMesh(const ChunkCoord& coord);
    MeshHandle* getWaterMesh(const ChunkCoord& coord);
    const LakeMeshSurface& waterMeshSurface(ChunkCoord coord) const {
        return chunks.at(coord).lakeSurface;
    }

    bool hasGenerationWork() const {
        return !generationQueue.empty() || !generationJobs.empty() ||
               !treeJobs.empty() || !treeRequests.empty();
    }
    bool hasStructureWork() const {
        return !rebaseQueue.empty() || !structureJobs.empty();
    }
    size_t generationInFlight() const { return generationJobs.size(); }
    // Region tree-set jobs running, and regions waiting for a job slot.
    size_t treeJobsInFlight() const { return treeJobs.size(); }
    size_t pendingTreeSets() const { return treeRequests.size(); }
    // Caps concurrent tree-set jobs (0 holds every request; tests use it to
    // keep a region's trees from building).
    void setMaxTreeJobs(size_t value) { maxTreeJobs = value; }
    size_t meshInFlight() const { return meshJobs.size(); }
    // The worker pool (for systems that build on chunk data, like tree
    // wood updates).
    ThreadPool& threadPool() const { return pool; }
    bool meshInFlight(ChunkCoord coord) const {
        return std::any_of(meshJobs.begin(), meshJobs.end(),
                           [&](const auto& job) { return job.coord == coord; });
    }
    size_t pendingGeneration() const { return generationQueue.size(); }
    // Main-thread time per update() for choosing and submitting jobs (tests
    // use 0: exactly one pick per update).
    void setDispatchBudget(std::chrono::nanoseconds value) {
        dispatchBudget = value;
    }
    // Withdraws queued chunks (not yet submitted) outside the cube of
    // `radius` chunks around `centre`, unless they are the sunlight
    // prerequisite of a chunk inside it. Fast travel otherwise leaves an
    // ever-growing queue behind the camera.
    void withdrawOutside(ChunkCoord centre, int radius);

    template <typename F>
    void forEachReady(F&& fn) {
        for (auto& [coord, handle] : chunks)
            if (handle.state == ChunkState::Ready) fn(coord, *handle.data);
    }
    // Chunks that currently own an uploaded, non-empty GPU mesh. Much smaller
    // than the registered set, which covers every slab of resident regions.
    template <typename F>
    void forEachMeshed(F&& fn) {
        for (const auto& coord : meshed) fn(coord);
    }
    size_t meshedCount() const { return meshed.size(); }
    template <typename F>
    bool withChunkLocked(const ChunkCoord& coord, F&& fn) {
        std::lock_guard<std::mutex> lock(tileAtMutex);
        auto* chunk = getChunk(coord);
        if (!chunk) return false;
        fn(*chunk);
        return true;
    }

   private:
    struct PendingInfo {
        ChunkCoord regionOrigin;
        const Region* region;
        std::optional<ChunkCoord> beneficiary;
        ChunkBlob stored;
        ChunkBlob storedFlora;
        ChunkBlob storedBaseline;
    };
    struct GeneratedChunk {
        std::shared_ptr<Chunk> data;
        bool fromDisk = false;
        bool edited = false;
        FloraLayerPtr flora;
        // Plants were regrown for a saved chunk: save them.
        bool floraRegrown = false;
        TreeStamp trees = {};
        // Tree wood was added to a saved chunk: save it.
        bool treesPlaced = false;
        bool baselineChanged = false;
        bool iceApplied = false;
        // The source snapshot the job used; a replacement makes it stale.
        structure_voxels::ColumnPtr structuresUsed = {};
        std::shared_ptr<const structure_voxels::Slab> structureBaseline = {};
    };
    struct GenerationJob {
        ChunkCoord coord;
        uint64_t identity;
        std::future<GeneratedChunk> future;
    };
    struct MeshJob {
        ChunkCoord coord;
        uint64_t identity;
        std::future<FinishedMesh> future;
    };
    bool canMesh(ChunkCoord coord) const;
    // The greedy mesher would emit no geometry: the chunk is all air, or it
    // is solid and every face neighbour covers it. Reads live tiles.
    bool hasNoGeometry(ChunkCoord coord) const;
    void submitMesh(ChunkCoord coord);
    // Publishes an empty mesh without a worker job.
    void finishEmptyMesh(ChunkCoord coord);
    void dispatchGeneration();
    void dispatchMeshes();
    void publishGenerated(ChunkCoord coord, uint64_t identity,
                          GeneratedChunk data);
    void collect();

    std::shared_ptr<const EnvironmentMap> environment =
        std::make_shared<const EnvironmentMap>();
    bool reconcileWater(Chunk& chunk, ChunkCoord coord,
                        const ChunkColumnInputs* columns,
                        bool refreshLocal = false,
                        const std::bitset<Chunk::SIZE_X * Chunk::SIZE_Z>*
                            dirty = nullptr) const;
    Reservoirs::CellAccess waterAccess(TilePos pos);
    // Geometry of a cell that needs sub-cell water capacity. Nullopt means a
    // uniform open cell (full 1 m capacity).
    std::optional<fluid::Profile> fluidProfileAt(TilePos pos);
    // The standing body or reach owning a loaded reservoir water cell.
    WaterSurface waterSourceAt(TilePos pos);
    // Certifies dug channels whose local water links a standing body to a
    // lower receiver; bounded per call.
    void surveyChannels(Reservoirs& water, size_t mouths);
    void surveyChannel(Reservoirs& water, TilePos mouth);
    const ColumnGenerationInput* waterColumnAt(TilePos pos) const;
    bool waterIntakeCandidate(TilePos pos);
    void updateWaterTopology(Reservoirs& water);
    void detectLakeSplit(Reservoirs& water, TilePos outlet);
    void reconnectLakes(Reservoirs& water);
    std::optional<std::pair<uint32_t, Reservoirs::BreachSurvey>> riverDamAt(
        TilePos pos, bool existing = false);
    std::optional<TilePos> riverMouthAnchor(Vec2i source, uint32_t reach) const;
    std::vector<TilePos> riverWallCandidates(TilePos source, uint32_t reach);
    void findShoreIntakes(ChunkCoord coord, const Reservoirs& water);
    void tickWater(Reservoirs& water, uint64_t step);
    void reservoirChanged(const ReservoirChange& change);
    void indexWater(ChunkCoord coord, const ChunkColumnInputs& columns);
    std::shared_ptr<const ChunkColumnInputs> projectedColumns(
        std::shared_ptr<const ChunkColumnInputs> columns, unsigned cx,
        unsigned cz, const ReservoirSnapshot& snapshot) const;
    Signal<const ReservoirChange&>::Connection reservoirConnection;
    std::unordered_map<uint32_t, std::unordered_set<ChunkCoord>> waterChunks;
    std::unordered_map<uint32_t, std::unordered_set<ChunkCoord>>
        waterReachChunks;
    std::unordered_set<ChunkCoord> pendingShoreChecks;
    bool lakeJoinsPending = false;
    std::set<TilePos, TilePosLess> waterEdits;
    std::set<TilePos, TilePosLess> waterTopologyEdits;
    std::set<TilePos, TilePosLess> waterDirectEdits;
    const TerrainEdits* waterTerrainEdits = nullptr;
    std::optional<TilePos> waterEditCursor;
    std::optional<TilePos> channelCursor;
    std::unordered_map<TilePos, uint64_t> lastChannelSurvey;

    struct ColumnCache {
        size_t registrations = 0;
        std::shared_ptr<const ChunkColumnInputs> data;
        std::shared_ptr<const TreeColumn> trees;
        structure_voxels::ColumnPtr structures;
        int32_t structuresDay = -1;
    };
    std::shared_ptr<const ChunkColumnInputs> prepareColumns(
        ChunkCoord coord, const Region& region);
    // Trees of a column once every region tree set it needs is ready
    // (starting the missing jobs).
    std::shared_ptr<const TreeColumn> prepareTrees(ChunkCoord coord,
                                                   Vec2u region);
    structure_voxels::ColumnPtr prepareStructures(ChunkCoord coord,
                                                  const TreeColumn& trees);
    // Region tree set, or null while its job runs (requested if missing:
    // dispatchTreeJobs starts the nearest requests).
    const TreeSet* regionTrees(Vec2u region);
    // Columns meeting any of the boxes rebuild their tree lists.
    void refreshTreeColumns(std::initializer_list<const TreeTileBox*> boxes);
    // Landmarks within two regions of `region` (the generator's input).
    std::vector<TreeLandmark> landmarksNear(Vec2u region) const;
    // Sky light for a chunk becoming Ready before its sunlight sweep:
    // full sky down each column from an open top to the first solid tile.
    static void estimateSkyLight(
        Chunk& chunk, ChunkCoord coord,
        const std::function<std::pair<int, WaterSurface>(int x, int z)>&
            surface);
    // Regions whose sets the column trees of a chunk in `region` need.
    std::vector<Vec2u> treeRegionsAround(Vec2u region) const;
    // True when prepareTrees would succeed now; starts no job.
    bool treesReady(ChunkCoord coord, Vec2u region) const;
    // Starts tree jobs for the nearest requests, up to maxTreeJobs, and
    // drops requests no registered region needs any more.
    void dispatchTreeJobs();
    // Drops tree sets no registered region within two regions needs.
    void releaseTreeSets();
    const TerrainNeighborhood& terrainNeighborhood(Vec2u region) {
        return *terrainShared(region);
    }
    // Shared so generation jobs can keep it after the region unloads.
    std::shared_ptr<const TerrainNeighborhood> terrainShared(Vec2u region);
    const World& terrainWorld;
    const LocationGenerator& generator;
    ThreadPool& pool;
    const ChunkMesher* mesher;
    LayersGenerator layersGenerator;
    const IAssets* soilAssets;
    uint8_t iceBlockId = 0;
    uint8_t snowBlockId = 0;
    uint32_t soilSeed;
    Vec3 focus{0, 0, 0};
    CameraWorkQueue generationQueue;
    CameraWorkQueue meshQueue;
    uint64_t generationDispatches = 0;
    uint64_t meshDispatches = 0;
    // In-flight worker caps, derived from the pool size.
    size_t maxGenerationJobs = 4;
    size_t maxMeshJobs = 2;
    // Main-thread time per update() spent choosing and submitting jobs.
    // Every update makes progress: at least one pick runs even past the
    // budget (ordering a huge queue must never use it all up).
    std::chrono::nanoseconds dispatchBudget = std::chrono::milliseconds(2);
    // Picks considered per update: nearest first, the rest wait.
    static constexpr size_t DispatchWindow = 1024;
    // Air chunks published synchronously per update(), without a job.
    static constexpr int AirChunksPerUpdate = 256;
    uint64_t nextIdentity = 0;
    std::unordered_map<Vec2u, std::vector<RegionLayer>> regionLayers;
    std::unordered_map<Vec2u, SoilProfile> regionSoils;
    std::unordered_map<Vec2u, std::shared_ptr<const TerrainNeighborhood>>
        regionTerrain;
    std::shared_ptr<const FloraSpeciesTable> floraTable;
    std::shared_ptr<const TreeSpeciesTable> treeTable;
    using StructureKinds = std::vector<structure_voxels::Kind>;
    std::shared_ptr<const StructureKinds> structureKinds =
        std::make_shared<const StructureKinds>();
    void installStructureKinds(std::shared_ptr<const StructureKinds> kinds);
    StructureKinds generatedKinds;
    structure_voxels::Overrides savedStructures;
    uint64_t overrideRevision = 0;
    void updateStructureBaselines(WorldEditor* editor);
    struct StructureJob {
        ChunkCoord coord;
        uint64_t identity, revision;
        std::future<std::shared_ptr<const structure_voxels::Slab>> future;
    };
    CameraWorkQueue rebaseQueue;
    std::vector<StructureJob> structureJobs;
    uint64_t structureRevision = 0;
    struct CachedTreeVoxels {
        const TreeInstance* tree = nullptr;
        float age = 0;
        std::weak_ptr<const structure_voxels::Source> source;
    };
    std::unordered_map<uint64_t, CachedTreeVoxels> treeVoxels;
    std::vector<TreeLandmark> landmarks;
    std::unordered_set<Vec2u> landmarkRegions;
    // Regions around a chunk where a landmark anchor can still reach it
    // (at least 2): from the widest landmark species.
    int landmarkRadius = 2;
    uint32_t treeSeed = 0;
    std::unordered_map<Vec2u, std::shared_ptr<const TreeSet>> treeSets;
    struct TreeJob {
        Vec2u region;
        std::future<TreeSet> future;
        TreeOverridesPtr overrides;  // the overrides it was built with
    };
    std::vector<TreeJob> treeJobs;
    // Regions waiting for a tree job. Capped and nearest-first so one
    // region's set cannot starve behind a flood of jobs while travelling.
    std::unordered_set<Vec2u> treeRequests;
    std::unordered_set<Vec2u> treeWishes;  // wantTreeSets
    uint64_t treeSetsRevision_ = 0;
    size_t maxTreeJobs = 2;
    TreeOverridesPtr overrides = std::make_shared<const TreeOverrideMap>();
    std::unordered_map<uint64_t, TreeInstancePtr> landmarkCache;
    std::unordered_map<Vec2u, BillboardSet> billboardSets;
    struct BillboardJob {
        Vec2u region;
        std::future<std::vector<TreeBillboard>> future;
        TreeOverridesPtr overrides;  // removed trees it left out
    };
    std::vector<BillboardJob> billboardJobs;
    std::vector<Vec2u> billboardWishes;  // nearest first
    size_t billboardWishNext = 0;
    uint64_t billboardRevision_ = 0;
    void dispatchBillboards();
    // Regions whose set is built: landmark sites without a tree there were
    // dropped by generation (no ground) and are not asked for again.
    std::unordered_set<Vec2u> landmarkRegionsBuilt;
    uint64_t treeRevision_ = 0;
    uint32_t floraSeed_ = 0;
    int32_t floraDay = 0;
    std::unordered_map<Vec2u, size_t> regionRegistrations;
    std::unordered_map<uint64_t, ColumnCache> columnInputs;
    std::unordered_map<ChunkCoord, ChunkHandle> chunks;
    std::unordered_map<ChunkCoord, PendingInfo> pending;
    std::unordered_map<uint64_t, int> columnTopY;
    std::vector<GenerationJob> generationJobs;
    std::vector<MeshJob> meshJobs;
    std::vector<FinishedMesh> readyMeshes;
    std::unordered_set<ChunkCoord> meshed;
    std::mutex tileAtMutex;
};
