#pragma once

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
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
#include "save/region_file.hpp"

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
    MeshHandle mesh;
    bool initialised = false;
    bool uploaded = false;
    uint64_t uploadedRevision = 0;
    // Tile contents differ from what the region file holds (freshly
    // generated, or modified through markModified). Lighting never sets it.
    bool persistDirty = false;
    // Loaded from disk with solid/air different from generated terrain
    // (an earlier gameplay edit).
    bool edited = false;
    // Plants anchored in this chunk (null: none). Replaced, never mutated.
    FloraLayerPtr flora;
    std::vector<BlockVertex> vertices;
    std::vector<uint32_t> indices;
};

struct FinishedMesh {
    ChunkCoord coord;
    ChunkMesh mesh;
    uint64_t identity;
    uint64_t revision;
};

// Main-thread requests/dispatch/publication, with bounded immutable worker
// jobs.
class ChunkManager {
   public:
    ChunkManager(const LocationGenerator& gen, ThreadPool& pool,
                 const ChunkMesher* mesher, const World& world,
                 IAssets* assets);
    ~ChunkManager();

    Signal<const ChunkCoord&> becameReady;
    Signal<const ChunkCoord&> removed;
    // Tile light or opacity of a Ready chunk changed. Geometry may be
    // unchanged; listeners refresh light volumes around the chunk.
    Signal<const ChunkCoord&> lightChanged;
    // A chunk's drawn geometry changed (new mesh uploaded, or emptied).
    Signal<const ChunkCoord&> meshUploaded;
    // The plants of a Ready chunk changed (setFlora). Chunks becoming
    // ready announce their plants through becameReady.
    Signal<const ChunkCoord&> floraChanged;
    void setFocus(Vec3 value) { focus = value; }
    // X wrapping of the world this manager streams (disabled without a map).
    WorldWrap wrap() const { return terrainWorld.wrap(); }
    void update();
    // stored: saved chunk payload; the chunk is decoded instead of generated.
    // storedFlora: its saved plants; a saved chunk without them regrows its
    // generated plants.
    void registerChunk(const ChunkCoord& coord, const ChunkCoord& regionOrigin,
                       const Region* region, ChunkBlob stored = nullptr,
                       ChunkBlob storedFlora = nullptr);
    Chunk* request(const ChunkCoord& coord);
    Chunk* requestLighting(const ChunkCoord& coord, ChunkCoord beneficiary);
    bool isRegistered(const ChunkCoord& coord) const;
    bool isReady(const ChunkCoord& coord) const;
    // Requested and waiting for (or running) its generation job.
    bool isQueued(const ChunkCoord& coord) const;
    // Ready, read from a save, and its solid tiles differ from what the
    // generator makes there (see LocationGenerator).
    bool isEdited(const ChunkCoord& coord) const;
    // Generated surface inputs of a registered chunk column, or null for an
    // unregistered column or a world without a map (fixtures).
    std::shared_ptr<const ChunkColumnInputs> generatedColumns(unsigned chunkX,
                                                              unsigned chunkZ);
    bool isMeshUploaded(const ChunkCoord& coord) const;
    // An older uploaded revision remains drawable while its replacement is
    // queued (for example, after sunlight changes).
    bool hasUploadedMesh(const ChunkCoord& coord) const;
    Chunk* getChunk(const ChunkCoord& coord);
    std::shared_ptr<Chunk> getChunkShared(const ChunkCoord& coord);
    const Tile* tileAt(int tx, int ty, int tz);
    void unregister(const ChunkCoord& coord);
    void markDirty(const ChunkCoord& coord, bool urgent = false);
    void markAllDirty();
    // Announces new light values without remeshing.
    void markLightChanged(const ChunkCoord& coord);
    // Records a gameplay edit of a Ready chunk: it will be saved and remeshed.
    void markModified(const ChunkCoord& coord);
    bool isPersistDirty(const ChunkCoord& coord) const;
    // Plants of a Ready chunk (null when none or not ready).
    FloraLayerPtr flora(const ChunkCoord& coord) const;
    // Replaces a Ready chunk's plants and announces it (floraChanged).
    // persist: the change must be saved (false for derived bookkeeping such
    // as FloraLayer::resolvedDay, which is recomputed after a reload).
    void setFlora(const ChunkCoord& coord, FloraLayerPtr layer,
                  bool persist = true);
    // Species rules (empty without flora data).
    const FloraSpeciesTable& floraSpecies() const { return *floraTable; }
    // Seed of the world's plant placement (generateFlora).
    uint32_t floraSeed() const { return floraSeed_; }
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
    // Captures Ready chunks of a region that need saving and clears their
    // persistDirty flags. copy=false shares the live data and is only valid
    // when the region is unregistered right afterwards.
    RegionSnapshot snapshotRegion(Vec2u region, bool copy);
    int getColumnTopY(unsigned cx, unsigned cz) const;
    void raiseColumnTopY(unsigned cx, unsigned cz, int worldY);
    int generatedSurfaceY(unsigned tileX, unsigned tileZ);
    size_t cachedColumnInputs() const { return columnInputs.size(); }
    // Uploads finished meshes. Call on the main thread before drawing. Kept
    // inline: the headless test library does not link bgfx.
    void uploadReadyMeshes(IRenderer& renderer) {
        for (auto& result : readyMeshes) {
            auto it = chunks.find(result.coord);
            if (it == chunks.end() || it->second.identity != result.identity ||
                it->second.meshRevision != result.revision)
                continue;
            auto& handle = it->second;
            handle.uploaded = true;
            handle.uploadedRevision = result.revision;
            handle.vertices = std::move(result.mesh.vertices);
            handle.indices = std::move(result.mesh.indices);
            if (handle.vertices.empty()) {
                // Hide a previous mesh when geometry becomes empty.
                handle.mesh.release();
                handle.initialised = false;
                meshed.erase(result.coord);
                meshUploaded.emit(result.coord);
                continue;
            }
            if (!handle.initialised) {
                handle.mesh.initDynamic<BlockVertex>(
                    &renderer, handle.vertices.size(), handle.indices.size());
                handle.initialised = true;
            }
            handle.mesh.update(handle.vertices.data(), handle.vertices.size(),
                               handle.indices.data(), handle.indices.size(),
                               true);
            meshed.insert(result.coord);
            meshUploaded.emit(result.coord);
        }
        readyMeshes.clear();
    }
    MeshHandle* getMesh(const ChunkCoord& coord);

    bool hasGenerationWork() const {
        return !generationQueue.empty() || !generationJobs.empty();
    }
    size_t generationInFlight() const { return generationJobs.size(); }
    size_t meshInFlight() const { return meshJobs.size(); }
    bool meshInFlight(ChunkCoord coord) const {
        return std::any_of(meshJobs.begin(), meshJobs.end(),
                           [&](const auto& job) { return job.coord == coord; });
    }
    size_t pendingGeneration() const { return generationQueue.size(); }

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
    };
    struct GeneratedChunk {
        std::shared_ptr<Chunk> data;
        bool fromDisk = false;
        bool edited = false;
        FloraLayerPtr flora;
        // Plants were regrown for a saved chunk: save them.
        bool floraRegrown = false;
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

    struct ColumnCache {
        size_t registrations = 0;
        std::shared_ptr<const ChunkColumnInputs> data;
    };
    std::shared_ptr<const ChunkColumnInputs> prepareColumns(
        ChunkCoord coord, const Region& region);
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
    static constexpr auto DispatchBudget = std::chrono::milliseconds(2);
    // Air chunks published synchronously per update(), without a job.
    static constexpr int AirChunksPerUpdate = 256;
    uint64_t nextIdentity = 0;
    std::unordered_map<Vec2u, std::vector<RegionLayer>> regionLayers;
    std::unordered_map<Vec2u, SoilProfile> regionSoils;
    std::unordered_map<Vec2u, std::shared_ptr<const TerrainNeighborhood>>
        regionTerrain;
    std::shared_ptr<const FloraSpeciesTable> floraTable;
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
