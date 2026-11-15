#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

#include "layers_generator.hpp"
#include "terrain_edits.hpp"
#include "terrain_sampler.hpp"

struct SurfaceColumn {
    int surfaceY = 0;
    int waterY = -1;
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
    // edits: gameplay changes to show instead of generated terrain (ground
    // height, top block, water). Structures above the generated surface are
    // not part of the height field; see structureTop().
    SurfaceSampler(const World& world, IAssets& assets,
                   std::shared_ptr<SurfaceInputCache> shared = nullptr,
                   std::shared_ptr<const EditMap> edits = nullptr)
        : world(world),
          assets(assets),
          layers(world.genSettings, &assets),
          shared(std::move(shared)),
          edits(std::move(edits)) {
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
    // Highest structure tile of the chunk column containing (x, z), or -1.
    int structureTop(int x, int z);
    // Replaces the edits and drops the derived caches.
    void setEdits(std::shared_ptr<const EditMap> value);
    int width() const { return world.width * TerrainTuning::RegionX; }
    int depth() const { return world.height * TerrainTuning::RegionZ; }

   private:
    using RegionInput = SurfaceRegionInput;
    const RegionInput& input(int x, int z);
    // Edit of the chunk column containing the clamped tile, or null.
    const ColumnEdit* editAt(int x, int z);
    SurfaceColumn generatedColumn(int x, int z);
    const World& world;
    IAssets& assets;
    LayersGenerator layers;
    std::shared_ptr<SurfaceInputCache> shared;
    std::shared_ptr<const EditMap> edits;
    uint64_t lastEditKey = UINT64_MAX;
    const ColumnEdit* lastEdit = nullptr;
    std::unordered_map<uint64_t, std::shared_ptr<const RegionInput>> regions;
    std::unordered_map<uint64_t, int> heights;
    std::unordered_map<uint64_t, SurfaceColumn> columns;
    uint16_t material(const RegionInput& data, const SurfaceColumn& column,
                      int y) const;
};
