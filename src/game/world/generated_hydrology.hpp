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

// Deterministic basins (generator v14) and carved reaches (v15).
// Region IDs are row-major.
// Body zero is dry and body one is the sea. Lake IDs derive from their seed.
class GeneratedHydrology {
   public:
    static constexpr uint32_t Sea = 1;
    struct Basin {
        uint32_t id = 0;
        int seed = -1;
        int outlet = -1;
        int sillY = -1;  // geometric boundary at the coarse spill saddle
        bool coastal = false;
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
        float strength;
    };

    static constexpr int MaxRiverDepth = 32;
    static constexpr int MaxRiverCut = 96;
    static constexpr int MaxFallDrop = 32;
    static constexpr int MinFallDrop = 8;

    enum class RiverEnd { Sea, Lake, Confluence, Plateau, ClosedBasin };
    struct Spring {
        Vec2i tile;
        int rockY;
        unsigned unitsPerStep;  // bounded source for the future fluid solver
    };
    struct Waterfall {
        Vec2i lip, landing;
        int lipY, landingY;
        float width;
        int drop() const { return lipY - landingY; }
    };
    struct LakeOutlet {
        uint32_t body;
        Vec2i tile;  // shore tile inside the verified lake footprint
    };
    struct Reach {
        uint32_t id;
        Spring spring;
        RiverEnd end;
        Vec2i outlet;
        uint32_t receiver = 0;  // body ID, or reach ID for a confluence
        std::vector<Waterfall> falls;
        std::optional<LakeOutlet> lakeSource = {};
    };
    struct Channel {
        WaterSurface water;
        int bedY;
        float distance;
        float radius;
    };

    static std::shared_ptr<const GeneratedHydrology> build(
        const World& world, const std::function<bool()>& cancelled = {},
        bool carveRivers = true, bool naturalRivers = false,
        bool boundRivers = false, bool riverSediment = false,
        bool connectLakeOutlets = false, bool connectCoastalBasins = false,
        bool oceanAcrossBasins = false);

    uint32_t candidateBody(int region) const;
    uint32_t catchment(int region) const;
    const Basin* basin(uint32_t id) const;
    Lake lake(uint32_t id) const;
    int ceiling(int region) const;
    WaterSurface waterAt(int x, int z, int groundY = -1) const;
    bool needsRefinement(int x0, int z0, int x1, int z1) const;
    int waterCeilingAt(int x, int z) const;
    WaterSurface certifiedSeaAt(int x, int z) const;
    bool potentialSeaAt(int x, int z) const;
    WaterSurface previewWaterAt(int x, int z, int groundY) const;
    bool uniformWater(int x0, int z0, int x1, int z1, uint32_t expectedBody,
                      bool certifiedOnly = false) const;

    const std::vector<Reach>& riverReaches() const { return reaches; }
    std::optional<Channel> riverAt(int x, int z) const;
    double carve(int x, int z, double originalY) const;
    std::pair<int, WaterSurface> sampleRiverColumn(int x, int z) const;
    bool springRockAt(int x, int y, int z) const;
    bool usesRiverSediment() const { return riverSediment; }
    bool hasChannel(int x0, int z0, int x1, int z1) const;
    int channelFloorLevel(int x, int z, int radius) const;

    const std::vector<std::vector<RiverBed>>& riverBeds() const { return beds; }

   private:
    static constexpr int Side = WorldWrap::RegionSize;
    static constexpr int Area = Side * Side;
    static constexpr int MaxLakeRegions = 24;
    static constexpr int SharedShore = -2;
    static constexpr size_t CacheSize = 256;
    static constexpr size_t OutletLakeBudget = 16u << 20;

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
    bool canCrossOcean(const RegionWater& region) const {
        return oceanAcrossBasins || region.body <= Sea;
    }
    bool canSurveyOcean(const RegionWater& region) const {
        return region.nearOcean && canCrossOcean(region);
    }
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
        bool ocean = false;
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
    bool buildReaches(const std::function<bool()>& cancelled,
                      bool naturalRivers, bool connectLakeOutlets);
    WaterSurface standingWaterAt(int x, int z, int groundY,
                                 bool probeOutlet = false) const;
    bool oceanConnectedAt(int region, Vec2i tile) const;
    struct Span {
        Vec2i from, to;  // unwrapped shortest image for this span
        int level0, level1;
        int depth0, depth1;
        float radius0, radius1;
        uint32_t reach;
    };
    int bankTop(int x, int z) const;
    void indexSpan(Span span);
    const std::vector<uint32_t>* spansAt(int x, int z) const;
    std::vector<Reach> reaches;
    std::vector<Span> spans;
    std::unordered_map<uint64_t, int> springRock;
    // Region ceilings and a finer sparse index, both including the banks.
    std::unordered_map<int, int> riverCeilings;
    static constexpr int RiverBucket = 16;
    std::unordered_map<uint64_t, std::vector<uint32_t>> riverIndex;

    void retainOutletLake(uint32_t body);
    Footprint refineCoastalBasin(const Basin& basin) const;
    Footprint refineLake(uint32_t body) const;
    Footprint refineOcean(int region) const;
    std::shared_ptr<const Footprint> footprint(uint64_t key) const;

    int width = 0, height = 0;
    bool naturalProfiles = false, boundedBanks = false;
    bool riverSediment = false;
    bool connectCoastalBasins = false;
    bool oceanAcrossBasins = false;
    uint32_t heightSeed = 0, featureSeed = 0;
    std::vector<Ground> ground;
    std::vector<RegionWater> regions;
    std::unordered_map<uint32_t, Basin> basins;
    std::vector<std::vector<RiverBed>> beds;

    // Outlet lakes are surveyed at world creation. Retain their exact masks
    // within a fixed budget so a distant river and its receiver can arrive
    // together.
    std::unordered_map<uint32_t, std::shared_ptr<const Footprint>> outletLakes;
    size_t outletLakeBytes = 0;

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
