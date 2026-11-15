#pragma once

#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "camera_work_queue.hpp"
#include "chunk_mesher.hpp"
#include "consts.hpp"
#include "layers_generator.hpp"
#include "location_generator.hpp"
#include "render/ui_context.hpp"

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
    void setFocus(Vec3 value) { focus = value; }
    void update();
    void registerChunk(const ChunkCoord& coord, const ChunkCoord& regionOrigin,
                       const Region* region);
    Chunk* request(const ChunkCoord& coord);
    Chunk* requestLighting(const ChunkCoord& coord, ChunkCoord beneficiary);
    bool isRegistered(const ChunkCoord& coord) const;
    bool isReady(const ChunkCoord& coord) const;
    Chunk* getChunk(const ChunkCoord& coord);
    std::shared_ptr<Chunk> getChunkShared(const ChunkCoord& coord);
    const Tile* tileAt(int tx, int ty, int tz);
    void unregister(const ChunkCoord& coord);
    void markDirty(const ChunkCoord& coord, bool urgent = false);
    void markAllDirty();
    int getColumnTopY(unsigned cx, unsigned cz) const;
    void raiseColumnTopY(unsigned cx, unsigned cz, int worldY);
    int generatedSurfaceY(unsigned tileX, unsigned tileZ);
    size_t cachedColumnInputs() const { return columnInputs.size(); }
    void uploadReadyMeshes(IRenderer& renderer) {
        for (auto& result : readyMeshes) {
            auto it = chunks.find(result.coord);
            if (it == chunks.end() || it->second.identity != result.identity ||
                it->second.meshRevision != result.revision)
                continue;
            auto& handle = it->second;
            handle.vertices = std::move(result.mesh.vertices);
            handle.indices = std::move(result.mesh.indices);
            if (handle.vertices.empty()) {
                // Hide a previous mesh when geometry becomes empty.
                handle.mesh.release();
                handle.initialised = false;
                continue;
            }
            if (!handle.initialised) {
                handle.mesh.initDynamic<BlockVertex>(
                    &renderer, handle.vertices.size(), handle.indices.size());
                handle.initialised = true;
            }
            handle.mesh.update(handle.vertices.data(), handle.vertices.size(),
                               handle.indices.data(), handle.indices.size(),
                               false);
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
    };
    struct GenerationJob {
        ChunkCoord coord;
        uint64_t identity;
        std::future<std::shared_ptr<Chunk>> future;
    };
    struct MeshJob {
        ChunkCoord coord;
        uint64_t identity;
        std::future<FinishedMesh> future;
    };
    bool canMesh(ChunkCoord coord) const;
    void submitMesh(ChunkCoord coord);
    void collect();

    struct ColumnCache {
        size_t registrations = 0;
        std::shared_ptr<const ChunkColumnInputs> data;
    };
    std::shared_ptr<const ChunkColumnInputs> prepareColumns(
        ChunkCoord coord, const Region& region);
    const TerrainNeighborhood& terrainNeighborhood(Vec2u region);
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
    uint64_t nextIdentity = 0;
    std::unordered_map<Vec2u, std::vector<RegionLayer>> regionLayers;
    std::unordered_map<Vec2u, SoilProfile> regionSoils;
    std::unordered_map<Vec2u, TerrainNeighborhood> regionTerrain;
    std::unordered_map<Vec2u, size_t> regionRegistrations;
    std::unordered_map<uint64_t, ColumnCache> columnInputs;
    std::unordered_map<ChunkCoord, ChunkHandle> chunks;
    std::unordered_map<ChunkCoord, PendingInfo> pending;
    std::unordered_map<uint64_t, int> columnTopY;
    std::vector<GenerationJob> generationJobs;
    std::vector<MeshJob> meshJobs;
    std::vector<FinishedMesh> readyMeshes;
    std::mutex tileAtMutex;
};
