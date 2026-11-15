#pragma once

#include <array>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chunk_manager.hpp"

// Main-thread scheduler. Workers receive owned opacity/direct-light snapshots;
// they never access ChunkManager, live tiles, or the propagator itself.
class SunLightPropagator {
   public:
    SunLightPropagator(ChunkManager& chunks, const DataStorage<Block>& blocks,
                       ThreadPool& pool);
    ~SunLightPropagator();
    SunLightPropagator(const SunLightPropagator&) = delete;
    SunLightPropagator& operator=(const SunLightPropagator&) = delete;

    void update();
    void setFocus(Vec3 value) { focus = value; }
    void markColumnDirty(unsigned cx, unsigned cz);
    bool isIdle() const;
    bool hasDirect(ChunkCoord coord) const;
    bool isComplete(ChunkCoord coord) const;
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

    struct State {
        Light direct{};
        uint64_t revision = 0;
        bool directKnown = false;
        bool complete = false;
    };
    struct Slab {
        ChunkCoord coord;
        Light opacity;
    };
    struct ChunkLight {
        ChunkCoord coord;
        Light light{};
    };
    struct DirectInput {
        uint64_t column;
        uint64_t revision;
        int topSlab;
        std::vector<Slab> slabs;
    };
    struct DirectResult {
        uint64_t column;
        uint64_t revision;
        std::vector<ChunkLight> chunks;
        std::vector<ChunkCoord> missing;
    };
    struct IndirectInput {
        ChunkCoord coord;
        uint64_t revision;
        std::vector<uint8_t> opacity;
        std::vector<uint8_t> light;
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
    static bool offset(ChunkCoord coord, int dx, int dy, int dz,
                       ChunkCoord& out);
    static DirectResult sweep(DirectInput input);
    static IndirectResult spread(IndirectInput input);
    uint8_t opacity(const Tile& tile) const;
    void ready(const ChunkCoord& coord);
    void removed(const ChunkCoord& coord);
    void invalidateAround(const ChunkCoord& coord);
    DirectInput snapshotColumn(uint64_t column);
    IndirectInput snapshotHalo(const ChunkCoord& coord);
    void collect();
    void publish(ChunkCoord coord, const Light& light);
    std::vector<uint64_t> dependencies(ChunkCoord coord) const;
    ChunkCoord beneficiary(ChunkCoord slab) const;

    ChunkManager& chunks;
    const DataStorage<Block>& blocks;
    ThreadPool& pool;
    Signal<const ChunkCoord&>::Connection readyConnection;
    Signal<const ChunkCoord&>::Connection removedConnection;
    std::unordered_map<ChunkCoord, State> states;
    std::unordered_map<uint64_t, uint64_t> columnRevisions;
    std::unordered_map<uint64_t, std::unordered_set<ChunkCoord>> columnChunks;
    std::unordered_set<uint64_t> pendingDirect;
    CameraWorkQueue pendingIndirect;
    std::unordered_set<ChunkCoord> requested;
    std::vector<DirectJob> directJobs;
    std::vector<IndirectJob> indirectJobs;
    uint64_t nextRevision = 0;
    uint64_t dispatches = 0;
    Vec3 focus{0, 0, 0};
};
