#pragma once

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "camera_work_queue.hpp"
#include "chunk_manager.hpp"
#include "core/signal.hpp"
#include "sky_column.hpp"
#include "sky_occlusion_map.hpp"
#include "structure_voxels.hpp"
#include "terrain_edits.hpp"

class WorldEditor;
class SunLightPropagator;
struct BlockChange;

// The sky heightmap (docs/handoff/sky-light.md): the source of truth for
// sky light. Sky light comes from straight overhead, so it is a function of
// how each tile column looks from above, which this service knows without
// the chunks in between:
// - terrain from the generation inputs (ChunkManager::columnSources);
// - trees from their skeletons, projected (treeWoodTops), at the day the
//   chunks grow wood for (ChunkManager::woodDay);
// - gameplay edits recorded in TerrainEdits;
// - overrides: columns whose loaded tiles differ from all of that (holes
//   dug into tree wood, which TerrainEdits does not record), observed after
//   WorldEditor changes and saved (sky.dat).
//
// Maps are kept per chunk column (sky::Tile) for requested columns while
// they are registered, built on the pool and rebuilt when their inputs
// change. Main thread only.
class SkyHeightMap {
   public:
    using Overrides = sky::Overrides;

    SkyHeightMap(ChunkManager& chunks, TerrainEdits& edits,
                 const DataStorage<Block>& blocks, ThreadPool& pool);
    ~SkyHeightMap();
    SkyHeightMap(const SkyHeightMap&) = delete;
    SkyHeightMap& operator=(const SkyHeightMap&) = delete;

    // Observes this editor's changes (overrides). Call unwatch() before the
    // editor is destroyed if it dies first.
    void watch(WorldEditor& editor);
    void unwatch();
    void setFocus(Vec3 value) { focus = value; }
    // Keeps the chunk column's map built while it is registered.
    void request(unsigned chunkX, unsigned chunkZ);
    // Dispatches builds (nearest first), collects them and applies
    // observations. Call once per frame after ChunkManager::update.
    void update();

    // Built map of a chunk column with overrides applied, or null.
    sky::TilePtr tile(unsigned chunkX, unsigned chunkZ) const;
    // One tile column (X wraps), or std::nullopt while not built.
    std::optional<sky::Column> column(int64_t x, int64_t z) const;
    // Highest tile attenuating sky light (sky::None: none).
    std::optional<int> top(int64_t x, int64_t z) const;
    // Direct sunlight 0-15 at a tile: 15 minus the opacity of the tiles
    // above it. Gameplay "direct sunlight" (plants) reads this.
    std::optional<uint8_t> directSky(int64_t x, int y, int64_t z) const;

    // Request a heightmap column without generating any voxel slabs.
    // Call for adjacent columns as needed for indirect samples near edges.
    // A query returns null while its own column is pending.
    void requestUnloaded(int64_t x, int64_t z);
    // Published voxel light, or an estimate from the heightmap.
    std::optional<uint8_t> skyAt(int64_t x, int y, int64_t z) const;

    // Capture immutable inputs and refresh the renderer map on the pool.
    // World and assets must remain alive and immutable during builds.
    void updateOcclusion(Vec3 eye,
                         std::vector<structure_voxels::SourcePtr> structures,
                         const World& world, const IAssets& assets);
    std::shared_ptr<const SkyOcclusionMap> skyOcclusion() const {
        return occlusion;
    }

    // Loaded overrides (before any column is built), and the current ones
    // for saving.
    void setOverrides(Overrides value);
    const Overrides& overrides() const { return overridden; }
    // Overrides changed since the owner last saved them.
    bool dirty = false;

    bool isIdle() const;
    size_t builtCount() const { return entries.size(); }
    size_t jobCount() const { return jobs.size(); }

    // A chunk column's map changed (built, rebuilt, overridden): its key.
    Signal<uint64_t> changed;

    static uint64_t columnKey(unsigned chunkX, unsigned chunkZ) {
        return (uint64_t(chunkX) << 32) | chunkZ;
    }

   private:
    friend class SunLightPropagator;
    const SunLightPropagator* voxelLight = nullptr;

    struct Entry {
        sky::TilePtr derived;     // from the sources alone
        sky::TilePtr effective;   // with overrides
        uint64_t generation = 0;  // of the newest dispatched build
        bool hasTrees = false;
        bool queried = false;
        uint64_t lastQuery = 0;
    };
    struct Job {
        uint64_t key;
        uint64_t generation;
        std::future<sky::Tile> future;
        bool unloaded = false;
    };

    void dispatch();
    void collect();
    void observe();
    void refreshEffective(uint64_t key, Entry& entry);
    sky::Column scanLoaded(unsigned cx, unsigned cz, int lx, int lz,
                           const sky::Column& known);
    void invalidate(uint64_t key);

    ChunkManager& chunks;
    TerrainEdits& edits;
    const DataStorage<Block>& blocks;
    ThreadPool& pool;
    std::shared_ptr<const sky::OpacityTable> opacity;
    WorldEditor* editor = nullptr;
    Signal<const BlockChange&>::Connection editConnection;
    Signal<uint64_t>::Connection editsConnection;
    Signal<uint64_t>::Connection treesConnection;
    Signal<uint64_t>::Connection releasedConnection;
    Signal<const ChunkCoord&>::Connection readyConnection;
    std::unordered_map<uint64_t, Entry> entries;
    // Requested chunk columns waiting for a (re)build, as {x, 0, z}.
    CameraWorkQueue queue;
    std::vector<Job> jobs;
    // Tile columns to observe: chunk column key -> tile indices.
    std::unordered_map<uint64_t, std::unordered_set<int>> observations;
    Overrides overridden;
    int32_t day = -1;
    uint64_t nextGeneration = 0;
    uint64_t queryClock = 0;
    uint64_t treeRevision = 0;
    uint64_t revision = 0;
    uint64_t occlusionRevision = 0;
    uint64_t occlusionEditRevision = 0;
    std::chrono::steady_clock::time_point nextOcclusionBuild{};
    std::shared_ptr<const SkyOcclusionMap> occlusion;
    struct Projection {
        structure_voxels::SourcePtr source;
        TileBox box;
        std::vector<int> tops;
    };
    using Projections = std::vector<std::shared_ptr<const Projection>>;
    struct TerrainRaster {
        int x0, z0;
        std::vector<float> heights;
    };
    struct OcclusionInputs {
        Vec3 eye;
        std::vector<structure_voxels::SourcePtr> structures;
        Projections previous;
        std::unordered_map<uint64_t, sky::TilePtr> tiles;
        std::shared_ptr<const EditMap> edits;
        Overrides overrides;
        std::shared_ptr<const sky::OpacityTable> opacity;
        WorldWrap wrap;
        std::shared_ptr<const TerrainRaster> terrain;
    };
    struct OcclusionResult {
        std::shared_ptr<const SkyOcclusionMap> map;
        Projections projections;
        std::shared_ptr<const TerrainRaster> terrain;
    };
    static std::shared_ptr<const TerrainRaster> sampleTerrain(
        int x0, int z0, const World& world, const IAssets& assets,
        WorldWrap wrap, std::shared_ptr<const TerrainRaster> previous);
    static OcclusionResult buildOcclusion(OcclusionInputs inputs,
                                          const World& world,
                                          const IAssets& assets);

    Projections projections;
    std::shared_ptr<const TerrainRaster> occlusionTerrain;
    std::optional<std::future<OcclusionResult>> occlusionJob;
    std::vector<structure_voxels::SourcePtr> occlusionSources;
    Vec3 focus{0, 0, 0};
};
