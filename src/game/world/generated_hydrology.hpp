#pragma once

#include <array>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "water_surface.hpp"
#include "world.hpp"

struct TerrainNeighborhood;

// Frozen derivation for WorldMeta::HydrologyVersion. Region IDs are row-major.
// Body zero is dry and body one is the sea. Lake IDs derive from their seed.
class GeneratedHydrology {
   public:
    static constexpr uint32_t Sea = 1;
    struct Basin {
        uint32_t id = 0;
        int seed = -1;
        int outlet = -1;
        int sillY = -1;  // geometric boundary at the coarse spill saddle
    };
    struct Lake {
        int surfaceY = -1;
        int sillY = -1;
        std::optional<Vec2i> outlet;  // verified exit from the search area
    };
    struct RiverBed {
        Vec2i tile;
        int groundY;
        uint32_t catchment;
    };

    static std::shared_ptr<const GeneratedHydrology> build(
        const World& world, const std::function<bool()>& cancelled = {});

    uint32_t candidateBody(int region) const;
    uint32_t catchment(int region) const;
    const Basin* basin(uint32_t id) const;
    Lake lake(uint32_t id) const;
    int ceiling(int region) const;
    WaterSurface waterAt(int x, int z, int groundY = -1) const;
    bool needsRefinement(int x0, int z0, int x1, int z1) const;
    int waterCeilingAt(int x, int z) const;
    WaterSurface certifiedSeaAt(int x, int z) const;
    bool uniformWater(int x0, int z0, int x1, int z1, uint32_t expectedBody,
                      bool certifiedOnly = false) const;

    const std::vector<std::vector<RiverBed>>& riverBeds() const { return beds; }

   private:
    static constexpr int Side = WorldWrap::RegionSize;
    static constexpr int Area = Side * Side;
    static constexpr int MaxLakeRegions = 24;
    static constexpr int SharedShore = -2;
    static constexpr size_t CacheSize = 256;

    struct Ground {
        int y;
        LandType land;
        FeatureMask features;
    };
    struct RegionWater {
        uint32_t body = 0;
        uint32_t catchment = 0;
        int shore = -1;  // basin seed, or SharedShore for competing candidates
        bool ocean = false;
        bool deep = false;
        bool oceanSource = false;
        bool nearOcean = false;
    };
    struct Mask {
        static constexpr int WordSize = 64;
        static_assert(Side % WordSize == 0);
        std::array<uint64_t, Area / WordSize> bits{};
        void set(int index);
        bool test(int index) const;
        bool matches(int x0, int z0, int x1, int z1, bool wet) const;
    };
    struct Footprint {
        Lake lake;
        std::unordered_map<int, Mask> masks;
    };

    int regionAt(int x, int z) const;
    Vec2i center(int region) const;
    int lakeSeedAt(int region, Vec2i tile) const;
    std::array<int, 9> shoreNeighborhood(int region) const;
    std::array<int, 4> neighbors(int region) const;
    TerrainNeighborhood terrain(int region) const;
    bool buildOcean(const std::function<bool()>& cancelled);
    bool buildBasins(const std::function<bool()>& cancelled);
    bool buildRiverBeds(const World& world,
                        const std::function<bool()>& cancelled);
    Footprint refineLake(uint32_t body) const;
    Footprint refineOcean(int region) const;
    std::shared_ptr<const Footprint> footprint(uint64_t key) const;

    int width = 0, height = 0;
    uint32_t heightSeed = 0, featureSeed = 0;
    std::vector<Ground> ground;
    std::vector<RegionWater> regions;
    std::unordered_map<uint32_t, Basin> basins;
    std::vector<std::vector<RiverBed>> beds;

    // Workers share immutable results. A future coalesces concurrent misses.
    using Cached = std::shared_future<std::shared_ptr<const Footprint>>;
    struct CacheEntry {
        Cached value;
        uint64_t lastUsed;
    };
    mutable std::mutex cacheMutex;
    mutable uint64_t cacheClock = 0;
    mutable std::unordered_map<uint64_t, CacheEntry> cache;
};
