#pragma once

#include <array>
#include <future>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chunk_manager.hpp"
#include "sky_column.hpp"

class SkyHeightMap;

// Main-thread scheduler. Workers receive owned opacity/direct-light snapshots;
// they never access ChunkManager, live tiles, or the propagator itself.
//
// With a sky heightmap (docs/handoff/sky-light.md), a slab's direct light
// starts from the slab above when it is loaded and from the heightmap
// otherwise, so sunlight never needs the slabs between a chunk and the sky.
// A column waits for its heightmap; never-lit chunks keep their estimate
// (isEstimated) until their full light lands. Without one (headless
// fixtures without a world map), direct light sweeps from the column top
// through every slab and requests the missing ones.
class SunLightPropagator {
   public:
    // sky: optional; must outlive the propagator.
    SunLightPropagator(ChunkManager& chunks, const DataStorage<Block>& blocks,
                       ThreadPool& pool, SkyHeightMap* sky = nullptr);
    ~SunLightPropagator();
    SunLightPropagator(const SunLightPropagator&) = delete;
    SunLightPropagator& operator=(const SunLightPropagator&) = delete;

    void update();
    void setFocus(Vec3 value) { focus = value; }
    void markColumnDirty(unsigned cx, unsigned cz);
    void markEnvironmentChanged(const ChunkCoord& coord);
    bool isIdle() const;
    bool hasDirect(ChunkCoord coord) const;
    bool isComplete(ChunkCoord coord) const;
    // The chunk shows estimated sky light, not swept voxel light: it was
    // never fully lit, and its column's sweep has not run yet or stopped at
    // a slab above it that is not generated yet (a tall structure raised
    // the column top). Renderers darken such chunks with the structure
    // sky occlusion map (docs/handoff/structures.md).
    bool isEstimated(ChunkCoord coord) const;
    // What a chunk's light waits on (diagnostics, smoke harness).
    enum class Wait {
        None,        // fully lit
        NoState,     // not ready
        NoMap,       // its column's heightmap is not built (sky mode)
        Sweep,       // its column's sweep is queued or running
        Neighbours,  // a neighbour column's sweep is queued or running
        Spreading,   // its spread job is running
        Queued,      // queued for a spread, not dispatched yet
        Idle,        // not lit and not queued (blind, or dropped)
    };
    Wait waitingOn(ChunkCoord coord) const;
    size_t inFlightCount() const {
        return directJobs.size() + indirectJobs.size();
    }

   private:
    static constexpr int X = Chunk::SIZE_X;
    static constexpr int Y = Chunk::SIZE_Y;
    static constexpr int Z = Chunk::SIZE_Z;
    static constexpr int VOLUME = X * Y * Z;
    static constexpr int HALO = 14;
    static constexpr int HX = X + 2 * HALO;
    static constexpr int HY = Y + 2 * HALO;
    static constexpr int HZ = Z + 2 * HALO;
    using Light = std::array<uint8_t, VOLUME>;

    struct Opacity {
        Light material{};
        voxel_light::Shapes shapes;
        voxel_light::Cell at(int i) const {
            return shapes.cell(i, material[i]);
        }
    };

    struct State {
        Light direct{};
        uint64_t revision = 0;
        bool directKnown = false;
        bool complete = false;
        // Full (direct + indirect) light was published at least once. Later
        // recomputations keep showing it instead of a darker direct-only
        // lower bound, so edits never flash bounced light off.
        bool lit = false;
        // The last sweep stopped at a missing slab above this chunk. Its
        // tiles keep what they showed (the generation estimate or earlier
        // full light) and serve as its direct light for neighbours; it gets
        // no spread job until a complete sweep.
        bool blind = false;
        // Showed the estimate since it became ready (a blind sweep). Until
        // full light lands it keeps doing so rather than drop to the direct
        // lower bound, which is black under a wide structure.
        bool estimated = false;
        // Tile opacity as of the last sweep snapshot (every opacity change
        // sweeps the column again before a spread may run), shared with
        // spread jobs so halos are assembled on the workers.
        std::shared_ptr<const Opacity> opacity;
        // Tiles changed since `opacity` was taken (an edit): the next sweep
        // copies them again. Untouched chunks reuse their snapshot (sky
        // mode), so a column sweep reads only new or edited chunks.
        bool opacityStale = true;
    };
    struct Slab {
        ChunkCoord coord;
        std::shared_ptr<const Opacity> opacity;
    };
    struct ChunkLight {
        ChunkCoord coord;
        Light light{};
        // Below a missing slab: `light` is only a lower bound of zero.
        bool blind = false;
    };
    struct DirectInput {
        uint64_t column;
        uint64_t revision;
        int topSlab;
        std::vector<Slab> slabs;
        // The column's sky heightmap (null: sweep from topSlab).
        sky::TilePtr sky;
    };
    struct DirectResult {
        uint64_t column;
        uint64_t revision;
        std::vector<ChunkLight> chunks;
        std::vector<ChunkCoord> missing;
    };
    // The target and its 26 neighbours, by LightVolume::neighbor order. A
    // loaded neighbour brings its opacity and direct light; a missing one
    // its column's heightmap (sky mode), from which spread() fills the
    // halo: exact down to the highest opaque tile, unknown (opaque) below.
    // Neither: opaque padding.
    struct IndirectInput {
        ChunkCoord coord;
        uint64_t revision;
        std::array<std::shared_ptr<const Opacity>, 27> opacity;
        std::array<std::shared_ptr<const Light>, 27> direct;
        std::array<sky::TilePtr, 27> sky;
        std::array<int, 27> baseY{};  // world Y of each neighbour slab
    };
    struct IndirectResult {
        ChunkCoord coord;
        uint64_t revision;
        Light light;
    };
    struct DirectJob {
        uint64_t column;
        std::future<DirectResult> future;
    };
    struct IndirectJob {
        ChunkCoord coord;
        std::future<IndirectResult> future;
    };

    static int index(int x, int y, int z) { return (z * Y + y) * X + x; }
    static int haloIndex(int x, int y, int z) { return (z * HY + y) * HX + x; }
    static uint64_t columnKey(unsigned x, unsigned z) {
        return (uint64_t{x} << 32) | z;
    }
    // Neighbor chunk coordinate; X wraps around the world.
    bool offset(ChunkCoord coord, int dx, int dy, int dz,
                ChunkCoord& out) const;
    static DirectResult sweep(DirectInput input);
    static IndirectResult spread(IndirectInput input);
    void ready(const ChunkCoord& coord);
    void removed(const ChunkCoord& coord);
    void invalidateAround(const ChunkCoord& coord);
    // std::nullopt while the column's heightmap is not built (sky mode).
    std::optional<DirectInput> snapshotColumn(uint64_t column);
    // Sky mode: sweeps waiting for no chunk's spread (heightmap changes).
    void dispatchSweeps(size_t capacity);
    IndirectInput snapshotHalo(const ChunkCoord& coord);
    void collect();
    void publish(ChunkCoord coord, const Light& light);
    // Indirect spreading cannot change the direct light: it is 15 everywhere,
    // or the chunk and the touching layers of its six face neighbours are
    // fully opaque. Such chunks are finished without a worker job.
    bool spreadIsIdentity(const ChunkCoord& coord);
    bool hasQueuedNeighbor(const ChunkCoord& coord) const;
    // The column has a sweep waiting or running.
    bool sweepPending(uint64_t column) const;
    std::vector<uint64_t> dependencies(ChunkCoord coord) const;
    ChunkCoord beneficiary(ChunkCoord slab) const;

    ChunkManager& chunks;
    const DataStorage<Block>& blocks;
    ThreadPool& pool;
    SkyHeightMap* sky;
    Signal<uint64_t>::Connection skyConnection;
    Signal<uint64_t>::Connection waterConnection;
    Signal<const ChunkCoord&>::Connection readyConnection;
    Signal<const ChunkCoord&>::Connection removedConnection;
    std::unordered_map<ChunkCoord, State> states;
    std::unordered_map<uint64_t, uint64_t> columnRevisions;
    std::unordered_map<uint64_t, std::unordered_set<ChunkCoord>> columnChunks;
    std::unordered_set<uint64_t> pendingDirect;
    // Columns that lost a chunk since the last update (see removed()).
    std::unordered_set<uint64_t> removedColumns;
    CameraWorkQueue pendingIndirect;
    std::unordered_set<ChunkCoord> requested;
    std::vector<DirectJob> directJobs;
    std::vector<IndirectJob> indirectJobs;
    uint64_t nextRevision = 0;
    uint64_t dispatches = 0;
    Vec3 focus{0, 0, 0};
};
