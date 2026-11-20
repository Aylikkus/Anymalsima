#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

#include "layers_generator.hpp"
#include "terrain_edits.hpp"
#include "terrain_sampler.hpp"

// Ground cover of a distant column, as stored in TerrainLod::Mesh::covers:
// overlay layer + 1 (0 = none), density 0-255, tint RGB565 little endian.
struct CoverTexel {
    uint8_t layer = 0;
    uint8_t density = 0;
    uint16_t tint = 0;
};

struct SurfaceColumn {
    int surfaceY = 0;
    WaterSurface water;
    SoilColumn soil;
    uint16_t material = 0;
    uint16_t rockMaterial = 0;
};

// Per-region sampling inputs: captured terrain neighbourhood and rock layers.
// Immutable once built, so workers can share them.
struct SurfaceRegionInput {
    TerrainNeighborhood terrain;
    std::vector<RegionLayer> rocks;
};

// Thread-safe cache of SurfaceRegionInput shared by many samplers, so each
// LOD job does not re-capture the regions its neighbours just captured.
class SurfaceInputCache {
   public:
    static constexpr size_t MaxRegions = 1024;
    template <typename Make>
    std::shared_ptr<const SurfaceRegionInput> get(uint64_t key, Make&& make) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (auto it = regions.find(key); it != regions.end())
                return it->second;
        }
        // Built outside the lock; a concurrent duplicate is identical.
        auto value = std::make_shared<const SurfaceRegionInput>(make());
        std::lock_guard<std::mutex> lock(mutex);
        if (regions.size() >= MaxRegions) regions.clear();
        return regions.try_emplace(key, std::move(value)).first->second;
    }

   private:
    std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<const SurfaceRegionInput>>
        regions;
};

// Thread-confined cache over immutable world definitions. No chunk allocation,
// lighting, live voxel access or GPU calls. Owner must outlive the sampler.
class SurfaceSampler {
   public:
    enum class WaterDetail { Exact, CertifiedSea, Preview };
    // edits: gameplay changes to show instead of generated terrain (ground
    // height, top block, water). Structures above the generated surface are
    // not part of the height field; see structureTop().
    SurfaceSampler(const World& world, IAssets& assets,
                   std::shared_ptr<SurfaceInputCache> shared = nullptr,
                   std::shared_ptr<const EditMap> edits = nullptr,
                   WaterDetail waterDetail = WaterDetail::Exact)
        : world(world),
          assets(assets),
          layers(world.genSettings, &assets),
          shared(std::move(shared)),
          edits(std::move(edits)),
          waterDetail(waterDetail) {
        if (world.width <= 0 || world.height <= 0 ||
            world.regions.size() != size_t(world.width) * world.height)
            throw std::invalid_argument("Invalid surface sampling world");
    }
    // Surface after edits: the ground height, its block's material and water.
    SurfaceColumn column(int x, int z);
    // Generated strata at height y. Edits keep the original layers, so a dug
    // pit shows them on its walls.
    uint16_t materialAt(int x, int y, int z);
    // Ground height after edits.
    int height(int x, int z);
    WaterSurface waterAt(int x, int z);
    bool submerged(int x, int z, float y);
    int channelFloorLevel(int x, int z, int radius) const;
    bool hasChannel(int x0, int z0, int x1, int z1) const;
    bool hasUnrefinedWater(int x0, int z0, int x1, int z1) const;
    bool uniformWater(int x0, int z0, int x1, int z1, WaterSurface sample);
    // Ground cover of the column's surface after edits (none where an edit
    // moved the ground).
    CoverTexel cover(int x, int z);
    // Highest structure tile of the chunk column containing (x, z), or -1.
    int structureTop(int x, int z);
    // Replaces the edits and drops the derived caches.
    void setEdits(std::shared_ptr<const EditMap> value);
    bool provisionalWater() const {
        return world.hydrology && waterDetail != WaterDetail::Exact;
    }
    int width() const { return world.width * TerrainTuning::RegionX; }
    int depth() const { return world.height * TerrainTuning::RegionZ; }
    // X wraps around the world; Z clamps (see the clamps in the .cpp).
    int wrapX(int x) const { return int(WorldWrap::mod(x, width())); }

   private:
    bool hasEdits(int x0, int z0, int x1, int z1);
    using RegionInput = SurfaceRegionInput;
    const RegionInput& input(int x, int z);
    // Edit of the chunk column containing the wrapped/clamped tile, or null.
    const ColumnEdit* editAt(int x, int z);
    WaterSurface generatedWater(int x, int z, int groundY) const;
    SurfaceColumn generatedColumn(int x, int z);
    const World& world;
    IAssets& assets;
    LayersGenerator layers;
    std::shared_ptr<SurfaceInputCache> shared;
    std::shared_ptr<const EditMap> edits;
    WaterDetail waterDetail;
    uint64_t lastEditKey = UINT64_MAX;
    const ColumnEdit* lastEdit = nullptr;
    std::unordered_map<uint64_t, std::shared_ptr<const RegionInput>> regions;
    std::unordered_map<uint64_t, int> heights;
    std::unordered_map<uint64_t, WaterSurface> waterSamples;
    std::unordered_map<uint64_t, SurfaceColumn> columns;
    uint16_t material(const RegionInput& data, const SurfaceColumn& column,
                      int y) const;
};
