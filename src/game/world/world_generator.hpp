#pragma once

#include <queue>

#include "core/assets_api.hpp"
#include "items/rock.hpp"
#include "world.hpp"

struct FeatureEncoded {
    uint8_t biome;        // R
    uint8_t moisture;     // G
    uint8_t temperature;  // B
};

class WorldGenerator {
   public:
    World generate(const WorldGenSettings& settings, IAssets* assets,
                   std::function<bool()> shouldCancel);

   private:
    void generateHeight(const WorldGenSettings& settings,
                        std::function<bool()> shouldCancel);
    void generateHumidity(const WorldGenSettings& settings,
                          std::function<bool()> shouldCancel);
    void generateRivers(const WorldGenSettings& settings,
                        std::function<bool()> shouldCancel);
    void generateTemperature(const WorldGenSettings& settings,
                             std::function<bool()> shouldCancel);
    void generateFeatures(const WorldGenSettings& settings, IAssets* assets,
                          std::function<bool()> shouldCancel);
    void computeDistanceToWater(World& world, std::queue<int>& waterQ);

    void buildRegionMeshes(std::function<bool()> shouldCancel);
    float sampleHeight(const Vec2& p);

    FeatureEncoded encodeFeatures(const Region& r);
    World tempWorld;
    // Refined lake tiles per region; the preview draws these, not the
    // coarse basin candidates.
    std::vector<uint16_t> lakeCoverage;
};