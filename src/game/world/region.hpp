#pragma once

#include <climits>
#include <type_traits>
#include <variant>
#include <vector>

#include "core/geometry.hpp"
#include "enums.hpp"
#include "items/rock.hpp"
#include "world_entity.hpp"

struct RiverInfo {
    RiverFlowDirection flow = RiverFlowDirection::NO;
    float strength = 0.0f;
};

enum class RegionLayerType { Rock, Soil };

struct RegionLayer {
    RegionLayerType type;
    uint8_t block_id;
    int thickness_m;
    int depth_from_surface_m;
    float aquifer_saturation = 0.0f;
    uint8_t data_id;
    bool topsoil = false;
};

struct Region {
    Vec2u coord;

    float height;           // высота
    int land_height_m;      // высота суши в метрах - 256 = 0, 512 = 1
    int land_height_sea_m;  // высота земли отн. уровня моря 0 = 0.5, -256 = 0,
                            // 256 = 1
    float temperature;
    float temperature_c;  // конвертируется автоматически из temperature
    float humidity;

    int water_distance = INT_MAX;

    float energy;  // потом

    BiomeType biome;
    LandType land;
    VegetationType vegetation;
    FeatureMask features;
    RiverInfo river;
    CrustType crust;

    std::vector<WorldEntity> entities;

    bool hasFeature(FeatureMask f) const {
        return (features & f) != FeatureMask::None;
    }

    void addFeature(FeatureMask f) { features = features | f; }

    void removeFeature(FeatureMask f) { features = features & ~f; }
};