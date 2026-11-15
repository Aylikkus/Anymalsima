#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

#include "layers_generator.hpp"
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
    SurfaceSampler(const World& world, IAssets& assets,
                   std::shared_ptr<SurfaceInputCache> shared = nullptr)
        : world(world),
          assets(assets),
          layers(world.genSettings, &assets),
          shared(std::move(shared)) {
        if (world.width <= 0 || world.height <= 0 ||
            world.regions.size() != size_t(world.width) * world.height)
            throw std::invalid_argument("Invalid surface sampling world");
    }
    SurfaceColumn column(int x, int z);
    uint16_t materialAt(int x, int y, int z);
    int height(int x, int z);
    int width() const { return world.width * TerrainTuning::RegionX; }
    int depth() const { return world.height * TerrainTuning::RegionZ; }

   private:
    using RegionInput = SurfaceRegionInput;
    const RegionInput& input(int x, int z);
    const World& world;
    IAssets& assets;
    LayersGenerator layers;
    std::shared_ptr<SurfaceInputCache> shared;
    std::unordered_map<uint64_t, std::shared_ptr<const RegionInput>> regions;
    std::unordered_map<uint64_t, int> heights;
    std::unordered_map<uint64_t, SurfaceColumn> columns;
    uint16_t material(const RegionInput& data, const SurfaceColumn& column,
                      int y) const;
};
