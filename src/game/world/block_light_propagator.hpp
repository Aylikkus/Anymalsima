#pragma once

#include <array>
#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chunk_manager.hpp"
#include "light_sources.hpp"
#include "world_editor.hpp"

// Near-field artificial light: flood-fills light from emissive tiles (and
// entity lights) through the voxels, so walls, caves and translucent tiles
// shape it. Writes Tile::lightLevel.artificial_light (0-15) and, where a
// colored light reaches, a per-chunk chroma side buffer.
//
// Each emitter lights the cells within LightEmission::NearRange blocks. A
// cell's light uses an effective distance: the straight-line distance plus
// every detour and translucent-tile penalty of its cheapest voxel path. Open
// space therefore gets round falloff, and walls cast soft shadows. The
// search domain of an emitter is fixed by the emitter alone, so the chunks on
// both sides of a border compute identical values.
//
// Main-thread scheduler. Workers receive immutable per-chunk opacity and
// emitter snapshots; they never touch ChunkManager or live tiles. Light is
// derived data and never sets persistDirty.
class BlockLightPropagator {
   public:
    static constexpr int Range = LightEmission::NearRange;
    static constexpr int Cells = Chunk::SIZE_X * Chunk::SIZE_Y * Chunk::SIZE_Z;
    // Two bytes per cell (Co, Cg relative to luma, 128 = white); see
    // encodeChroma(). Chunks reached only by white light have none.
    using Chroma = std::array<uint8_t, Cells * 2>;
    // Cell of chunk-local tile (x, y, z) in light and Chroma arrays (two
    // bytes per cell there).
    static int index(int x, int y, int z) {
        return (z * Chunk::SIZE_Y + y) * Chunk::SIZE_X + x;
    }

    // registry: optional; receives far block lights and supplies entity
    // lights. Must outlive the propagator.
    BlockLightPropagator(ChunkManager& chunks, const DataStorage<Block>& blocks,
                         ThreadPool& pool,
                         LightSourceRegistry* registry = nullptr);
    ~BlockLightPropagator();
    BlockLightPropagator(const BlockLightPropagator&) = delete;
    BlockLightPropagator& operator=(const BlockLightPropagator&) = delete;

    // Relights around tiles changed through the editor.
    void watch(WorldEditor& editor);
    // Rescans a Ready chunk whose tiles changed without the editor.
    void markChunkChanged(const ChunkCoord& coord) { rescans.insert(coord); }
    void update();
    void setFocus(Vec3 value) { focus = value; }
    bool isIdle() const;
    size_t inFlightCount() const { return jobs.size(); }
    const Chroma* chroma(const ChunkCoord& coord) const;
    // Light emitted by a block id, or null.
    const LightEmission* emission(uint8_t block) const {
        return emissions[block];
    }

    // Chroma bytes of a color (any scale; black counts as white).
    static std::array<uint8_t, 2> encodeChroma(std::array<float, 3> color);

   private:
    struct LocalEmitter {
        uint16_t cell;  // index(x, y, z) inside the chunk
        uint8_t block;
    };
    // Immutable snapshot of one Ready chunk, shared with workers.
    struct Inputs {
        uint8_t uniform = 0;           // opacity when `opacity` is empty
        std::vector<uint8_t> opacity;  // Cells entries, or empty
        std::vector<LocalEmitter> emitters;
    };
    struct Emitter {
        int x = 0, y = 0, z = 0;  // cell relative to the target chunk origin
        LightEmission emission;
        std::array<float, 3> forward{0, -1, 0};
        std::array<uint8_t, 2> chroma{128, 128};
    };
    static constexpr int AreaSide = 5;  // chunks; target in the middle
    using Area = std::array<std::shared_ptr<const Inputs>,
                            AreaSide * AreaSide * AreaSide>;
    struct Input {
        ChunkCoord coord;
        uint64_t revision = 0;
        Area area;
        std::vector<Emitter> emitters;
    };
    struct Result {
        ChunkCoord coord;
        uint64_t revision = 0;
        std::array<uint8_t, Cells> light{};
        std::unique_ptr<Chroma> chroma;
    };
    struct Job {
        ChunkCoord coord;
        std::future<Result> future;
    };
    struct State {
        std::shared_ptr<const Inputs> inputs;
        uint64_t revision = 0;
        bool lit = false;  // published light is not all zero
    };

    static Result compute(Input input);
    // Near field of a far light: direct line of sight from the emitter to
    // every cell of the target chunk within `limit`.
    template <typename Opacity, typename Contribute>
    static void directLight(const Emitter& e, float limit,
                            const Opacity& opacity, Contribute&& contribute);
    bool offset(ChunkCoord coord, int dx, int dy, int dz,
                ChunkCoord& out) const;
    std::shared_ptr<const Inputs> scan(const ChunkCoord& coord);
    bool hasEmitters(const ChunkCoord& coord) const;
    bool entityLightsNear(const ChunkCoord& coord) const;
    // Queues every chunk whose light may depend on coord's tiles, given
    // whether coord itself has (or had) emitters.
    void invalidateAround(const ChunkCoord& coord, bool ownEmitters);
    void invalidateChunk(const ChunkCoord& coord);
    void ready(const ChunkCoord& coord);
    void removed(const ChunkCoord& coord);
    void entityChanged(const LightSource& source);
    // Mirrors the chunk's emissive tiles into the registry.
    void reconcileRegistry(const ChunkCoord& coord, const Inputs& inputs);
    // The chunk and its six face neighbors (their border emitters' faces).
    void reconcileAround(const ChunkCoord& coord);
    // LightSource::openFaces of the emitter at a chunk-local cell.
    uint8_t openFaces(const ChunkCoord& coord, int x, int y, int z) const;
    Input snapshot(const ChunkCoord& coord);
    void collect();
    void publish(const ChunkCoord& coord,
                 const std::array<uint8_t, Cells>& light,
                 std::unique_ptr<Chroma> chroma);

    ChunkManager& chunks;
    const DataStorage<Block>& blocks;
    ThreadPool& pool;
    LightSourceRegistry* registry;
    std::array<const LightEmission*, 256> emissions{};
    Signal<const ChunkCoord&>::Connection waterConnection, readyConnection;
    Signal<const ChunkCoord&>::Connection removedConnection;
    Signal<const LightSource&>::Connection entityConnection;
    WorldEditor* editor = nullptr;
    Signal<const BlockChange&>::Connection editorConnection;
    std::unordered_map<ChunkCoord, State> states;
    std::unordered_map<ChunkCoord, std::unique_ptr<Chroma>> chromas;
    std::unordered_set<ChunkCoord> rescans;
    CameraWorkQueue pending;
    std::vector<Job> jobs;
    uint64_t nextRevision = 0;
    Vec3 focus{0, 0, 0};
};
