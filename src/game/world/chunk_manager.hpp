#pragma once
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "chunk.hpp"
#include "chunk_coord.hpp"
#include "chunk_mesher.hpp"
#include "consts.hpp"
#include "layers_generator.hpp"
#include "location_generator.hpp"
#include "render/renderer_api.hpp"
#include "world/region.hpp"

enum class ChunkState { Unloaded, Queued, Ready };
enum class MeshState { None, Queued, Built };

struct ChunkHandle {
    ChunkState state = ChunkState::Unloaded;
    std::shared_ptr<Chunk> data;
    std::future<std::shared_ptr<Chunk>> future;

    /* MESH */
    MeshState meshState = MeshState::None;
    bool dirtyPending = false;

    MeshHandle mesh;
    bool initialised = false;

    std::vector<BlockVertex> vertices;
    std::vector<uint32_t> indices;

    bool all_above_requested = false;
};

struct FinishedMesh {
    ChunkCoord coord;
    ChunkMesh mesh;
};

class ChunkManager {
   public:
    explicit ChunkManager(const LocationGenerator& gen, ThreadPool& pool,
                          const ChunkMesher* mesher, const World& world,
                          IAssets* assets)
        : generator(gen),
          pool(pool),
          mesher(mesher),
          layersGenerator(world.genSettings, assets) {}

    Signal<const ChunkCoord&> becameReady;

    // -----------------------------------------------------------------------
    void registerChunk(const ChunkCoord& coord, const ChunkCoord& regionOrigin,
                       const Region* region) {
        auto& h = chunks[coord];
        h.state = ChunkState::Unloaded;
        h.meshState = MeshState::None;
        h.dirtyPending = false;
        h.data = nullptr;
        pending[coord] = {regionOrigin, region};

        if (regionLayers.find(region->coord) == regionLayers.end())
            regionLayers[region->coord] = layersGenerator.get(*region);

        // First writer wins — region assignment per column is deterministic,
        // so subsequent registerChunk calls for the same column (different Y)
        // don't need to touch this again.
        uint64_t colKey = ((uint64_t)coord.x << 32) | coord.z;
        if (columnTopY.find(colKey) == columnTopY.end())
            columnTopY[colKey] = region->land_height_m;
    }

    // -----------------------------------------------------------------------
    Chunk* request(const ChunkCoord& coord) {
        auto it = chunks.find(coord);
        if (it == chunks.end()) return nullptr;
        ChunkHandle& h = it->second;

        if (h.state == ChunkState::Ready) {
            if (h.dirtyPending) {
                enqueueMesh(coord, h);
            }
            return h.data.get();
        }

        if (h.state == ChunkState::Unloaded) {
            auto cit = chunks.find(coord);
            auto pit = pending.find(coord);

            PendingInfo info = pit->second;
            pending.erase(pit);
            cit->second.state = ChunkState::Queued;

            auto& layers = regionLayers[info.region->coord];

            cit->second.future = pool.submit_task(
                [this, coord, info, layers]() -> std::shared_ptr<Chunk> {
                    auto chunk = std::make_shared<Chunk>();
                    generator.generateChunk(*chunk, coord, info.regionOrigin,
                                            *info.region, layers);
                    return chunk;
                });

            return nullptr;
        }

        if (h.state == ChunkState::Queued) {
            if (h.future.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready)
                return nullptr;

            bool shouldEmitReady = false;

            auto data = h.future.get();
            {
                // This is the exact state tileAt()/getChunkShared() read under
                // this same lock — must be written under it too, or those reads
                // race with this write.
                std::lock_guard<std::mutex> lk(tileAtMutex);
                h.data = data;
                h.state = ChunkState::Ready;
                shouldEmitReady = true;
            }

            if (shouldEmitReady) {
                becameReady.emit(coord);
            }

            h.dirtyPending = true;

            return h.data.get();
        }

        return nullptr;
    }

    bool isRegistered(const ChunkCoord& coord) const {
        return chunks.find(coord) != chunks.end();
    }

    void uploadReadyMeshes(IRenderer& renderer) {
        std::vector<FinishedMesh> batch;
        {
            std::lock_guard<std::mutex> lk(readyMutex);
            int take = readyQueue.size();
            batch.insert(batch.end(),
                         std::make_move_iterator(readyQueue.begin()),
                         std::make_move_iterator(readyQueue.begin() + take));
            readyQueue.erase(readyQueue.begin(), readyQueue.begin() + take);
        }

        for (auto& fm : batch) {
            auto it = chunks.find(fm.coord);
            if (it == chunks.end()) continue;
            ChunkHandle& h = it->second;

            // Full remesh path
            h.vertices = std::move(fm.mesh.vertices);
            h.indices = std::move(fm.mesh.indices);

            if (h.vertices.empty()) {
                h.meshState = MeshState::Built;
                continue;
            }

            if (!h.initialised) {
                h.mesh.initDynamic<BlockVertex>(&renderer, h.vertices.size(),
                                                h.indices.size());
                h.initialised = true;
            }

            h.mesh.update(h.vertices.data(), h.vertices.size(),
                          h.indices.data(), h.indices.size(), false);
            h.meshState = MeshState::Built;
        }
    }

    MeshHandle* getMesh(const ChunkCoord& coord) {
        auto it = chunks.find(coord);
        if (it == chunks.end()) return nullptr;
        ChunkHandle& h = it->second;
        if (h.meshState != MeshState::Built) return nullptr;
        if (!h.initialised) return nullptr;
        return &h.mesh;
    }

    Chunk* getChunk(const ChunkCoord& coord) {
        auto it = chunks.find(coord);
        if (it == chunks.end()) return nullptr;
        if (it->second.state != ChunkState::Ready) return nullptr;
        return it->second.data.get();
    }

    void markDirty(const ChunkCoord& coord) {
        auto it = chunks.find(coord);
        if (it == chunks.end()) return;
        ChunkHandle& h = it->second;
        if (h.state != ChunkState::Ready) return;
        h.dirtyPending = true;
    }

    void markAllDirty() {
        for (auto& [coord, h] : chunks)
            if (h.state == ChunkState::Ready) markDirty(coord);
    }

    const Tile* tileAt(int tx, int ty, int tz) {
        if (ty < 0) return nullptr;

        unsigned cx = (unsigned)(tx / Chunk::SIZE_X);
        unsigned cy = (unsigned)(ty / Chunk::SIZE_Y);
        unsigned cz = (unsigned)(tz / Chunk::SIZE_Z);

        unsigned lx = (unsigned)tx - cx * Chunk::SIZE_X;
        unsigned ly = (unsigned)ty - cy * Chunk::SIZE_Y;
        unsigned lz = (unsigned)tz - cz * Chunk::SIZE_Z;

        ChunkCoord c{cx, cy, cz};

        {
            std::lock_guard<std::mutex> lk(tileAtMutex);
            auto it = chunks.find(c);
            if (it == chunks.end() || it->second.state != ChunkState::Ready)
                return nullptr;
            return &it->second.data->tiles[lz][ly][lx];
        }
    }

    bool isReady(const ChunkCoord& coord) const {
        auto it = chunks.find(coord);
        return it != chunks.end() && it->second.state == ChunkState::Ready;
    }

    template <typename F>
    void forEachReady(F&& fn) {
        for (auto& [coord, handle] : chunks)
            if (handle.state == ChunkState::Ready) fn(coord, *handle.data);
    }

    void unregister(const ChunkCoord& coord) {
        auto it = chunks.find(coord);
        if (it != chunks.end()) {
            if (it->second.state == ChunkState::Queued) {
                it->second.future.wait();
            }
        }

        chunks.erase(coord);
        pending.erase(coord);
    }

    Chunk* getChunkFast(const ChunkCoord& coord) {
        auto it = chunks.find(coord);

        if (it == chunks.end()) return nullptr;

        if (it->second.state != ChunkState::Ready) return nullptr;

        return it->second.data.get();
    }

    template <typename F>
    bool withChunkLocked(const ChunkCoord& coord, F&& fn) {
        std::lock_guard<std::mutex> lk(tileAtMutex);
        Chunk* chunk = getChunkFast(coord);
        if (!chunk) return false;
        fn(*chunk);
        return true;
    }

    std::shared_ptr<Chunk> getChunkShared(const ChunkCoord& coord) {
        std::lock_guard<std::mutex> lk(tileAtMutex);
        auto it = chunks.find(coord);
        if (it == chunks.end() || it->second.state != ChunkState::Ready)
            return nullptr;
        return it->second
            .data;  // shared_ptr copy — safe to use lock-free after this
    }

    int getColumnTopY(unsigned cx, unsigned cz) const {
        uint64_t k = ((uint64_t)cx << 32) | cz;
        auto it = columnTopY.find(k);
        return it != columnTopY.end() ? it->second
                                      : (int)(Y_CHUNKS * Chunk::SIZE_Y - 1);
    }

    // Call this wherever block-placement logic lives, whenever a placed block's
    // world Y exceeds the recorded top for its column (e.g. player builds a
    // tower, or you later add trees taller than land_height_m). Monotonic —
    // only ever raises the bound, per your "up not down" rule.
    void raiseColumnTopY(unsigned cx, unsigned cz, int worldY) {
        uint64_t k = ((uint64_t)cx << 32) | cz;
        auto& top = columnTopY[k];  // zero-initialised on first access
        if (worldY > top) top = worldY;
    }

   private:
    // Safe enqueue: no-op if already enqueued or currently being built
    // (sets dirtyPending in that case so it re-meshes after).
    void enqueueMesh(const ChunkCoord& coord, ChunkHandle& h) {
        if (h.meshState == MeshState::Queued) return;
        if (h.meshState == MeshState::None) {
            bool neighboursReady = true;
            const ChunkCoord neighbours[6] = {
                {coord.x + 1, coord.y, coord.z},
                {coord.x - 1, coord.y, coord.z},
                {coord.x, coord.y + 1, coord.z},
                {coord.x, coord.y - 1, coord.z},
                {coord.x, coord.y, coord.z + 1},
                {coord.x, coord.y, coord.z - 1},
            };
            for (const auto& n : neighbours) {
                auto nit = chunks.find(n);
                if (nit == chunks.end() ||
                    nit->second.state != ChunkState::Ready) {
                    neighboursReady = false;
                    break;
                }
            }
            if (!neighboursReady) {
                return;
            }
        }

        h.meshState = MeshState::Queued;

        const Chunk* ch = h.data.get();
        auto chunkData = h.data;

        pool.detach_task([coord, chunkData, this]() mutable {
            ChunkMesh mesh = mesher->build(
                *chunkData, coord,
                [this](int tx, int ty, int tz) { return tileAt(tx, ty, tz); });
            {
                std::lock_guard<std::mutex> lk(readyMutex);
                readyQueue.push_back({coord, std::move(mesh)});
            }
        });

        h.dirtyPending = false;
    }

    // -----------------------------------------------------------------------
    struct PendingInfo {
        ChunkCoord regionOrigin;
        const Region* region = nullptr;
    };

    const LocationGenerator& generator;
    ThreadPool& pool;
    const ChunkMesher* mesher = nullptr;

    std::unordered_map<Vec2u, std::vector<RegionLayer>> regionLayers;
    std::unordered_map<ChunkCoord, ChunkHandle> chunks;
    std::unordered_map<ChunkCoord, PendingInfo> pending;
    LayersGenerator layersGenerator;

    std::mutex readyMutex;
    std::mutex tileAtMutex;
    std::vector<FinishedMesh> readyQueue;

    std::unordered_map<uint64_t, int> columnTopY;
};