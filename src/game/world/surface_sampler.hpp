#pragma once

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

// Thread-confined cache over immutable world definitions. No chunk allocation,
// lighting, live voxel access or GPU calls. Owner must outlive the sampler.
class SurfaceSampler {
   public:
    SurfaceSampler(const World& world, IAssets& assets)
        : world(world), assets(assets), layers(world.genSettings, &assets) {
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
    struct RegionInput {
        TerrainNeighborhood terrain;
        std::vector<RegionLayer> rocks;
    };
    RegionInput& input(int x, int z);
    const World& world;
    IAssets& assets;
    LayersGenerator layers;
    std::unordered_map<uint64_t, RegionInput> regions;
    std::unordered_map<uint64_t, int> heights;
    std::unordered_map<uint64_t, SurfaceColumn> columns;
    uint16_t material(const RegionInput& data, const SurfaceColumn& column,
                      int y) const;
};
