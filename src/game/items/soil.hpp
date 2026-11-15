#pragma once

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "serialization/serializable_api.hpp"
#include "utils/enum_parser.hpp"
#include "utils/region_enum_maps.hpp"

struct SoilProperties {
    float porosity = 0;
    float permeability = 0;
    float water_retention = 0;
    float fertility = 0;
    float organic_content = 0;
};

struct SoilPlacement {
    std::vector<LandType> allowed_land;
    std::vector<BiomeType> allowed_biomes;
    std::vector<FeatureMask> required_features;
    std::vector<FeatureMask> forbidden_features;
    std::array<float, 2> humidity_range{0, 1};
    std::array<float, 2> temperature_range_c{-273.15f, 1000};
    bool allow_underwater = false;
    int priority = 0;
    float weight = 1;
};

struct Soil {
    static constexpr const std::string TypeName = "soils";
    static inline uint8_t counter = 0;
    uint8_t id = 0;
    std::string name;
    std::string block_name;
    std::optional<std::string> topsoil_block_name;
    std::array<int, 2> depth_m{1, 4};
    std::array<int, 2> topsoil_depth_m{1, 1};
    SoilProperties properties;
    std::optional<SoilProperties> topsoil_properties;
    std::vector<SoilPlacement> placement;

    const SoilProperties& propertiesFor(bool topsoil) const {
        return topsoil && topsoil_properties ? *topsoil_properties : properties;
    }
};

namespace soil_json {
template <typename T>
void range(const Json& j, const char* key, std::array<T, 2>& out) {
    if (!j.contains(key)) return;
    const auto& value = j.at(key);
    if (!value.is_array() || value.size() != 2)
        throw std::runtime_error(std::string(key) + ": expected two bounds");
    for (int i = 0; i < 2; ++i) {
        if (!value[i].is_number() ||
            (std::is_integral_v<T> && !value[i].is_number_integer()))
            throw std::runtime_error(std::string(key) + ": invalid bound");
        const double number = value[i].get<double>();
        if (!std::isfinite(number) ||
            number < std::numeric_limits<T>::lowest() ||
            number > std::numeric_limits<T>::max())
            throw std::runtime_error(std::string(key) + ": invalid bound");
        out[i] = value[i].get<T>();
    }
    if (out[0] > out[1])
        throw std::runtime_error(std::string(key) + ": reversed bounds");
}
template <typename E>
void enums(const Json& j, const char* key,
           const std::unordered_map<std::string, E>& map, std::vector<E>& out) {
    if (!j.contains(key)) return;
    if (!j.at(key).is_array())
        throw std::runtime_error(std::string(key) + ": expected array");
    out = EnumParser<E>::parseArray(map, j.at(key));
}
}  // namespace soil_json

inline void from_json(const Json& j, SoilProperties& p) {
    for (const auto& [key, target] :
         std::initializer_list<std::pair<const char*, float*>>{
             {"porosity", &p.porosity},
             {"permeability", &p.permeability},
             {"water_retention", &p.water_retention},
             {"fertility", &p.fertility},
             {"organic_content", &p.organic_content}}) {
        const auto& value = j.at(key);
        if (!value.is_number())
            throw std::runtime_error(std::string(key) + ": expected number");
        *target = value.get<float>();
        if (!std::isfinite(*target) || *target < 0 || *target > 1)
            throw std::runtime_error(std::string(key) + ": expected [0,1]");
    }
}

inline void from_json(const Json& j, SoilPlacement& p) {
    soil_json::enums(j, "allowed_land", RegionEnumMaps::land(), p.allowed_land);
    soil_json::enums(j, "allowed_biomes", RegionEnumMaps::biome(),
                     p.allowed_biomes);
    soil_json::enums(j, "required_features", RegionEnumMaps::features(),
                     p.required_features);
    soil_json::enums(j, "forbidden_features", RegionEnumMaps::features(),
                     p.forbidden_features);
    soil_json::range(j, "humidity_range", p.humidity_range);
    soil_json::range(j, "temperature_range_c", p.temperature_range_c);
    if (p.humidity_range[0] < 0 || p.humidity_range[1] > 1)
        throw std::runtime_error("humidity_range: expected [0,1]");
    p.allow_underwater = j.value("allow_underwater", false);
    if (j.contains("priority")) {
        std::array<int, 2> priority{};
        soil_json::range(Json{{"priority", Json::array({j.at("priority"),
                                                        j.at("priority")})}},
                         "priority", priority);
        p.priority = priority[0];
    }
    p.weight = j.value("weight", 1.0f);
    if (!std::isfinite(p.weight) || p.weight <= 0)
        throw std::runtime_error("soil placement weight must be positive");
}

inline void from_json(const Json& j, Soil& s) {
    j.at("name").get_to(s.name);
    j.at("block_name").get_to(s.block_name);
    if (s.name.empty() || s.block_name.empty())
        throw std::runtime_error("soil names must not be empty");
    if (j.contains("topsoil_block_name"))
        s.topsoil_block_name = j.at("topsoil_block_name").get<std::string>();
    soil_json::range(j, "depth_m", s.depth_m);
    soil_json::range(j, "topsoil_depth_m", s.topsoil_depth_m);
    if (s.depth_m[0] < 0 || s.topsoil_depth_m[0] < 0)
        throw std::runtime_error("soil depth must be nonnegative");
    j.at("properties").get_to(s.properties);
    if (j.contains("topsoil_properties"))
        s.topsoil_properties = j.at("topsoil_properties").get<SoilProperties>();
    if (s.topsoil_properties && !s.topsoil_block_name)
        throw std::runtime_error("topsoil properties require a topsoil block");
    if (!j.at("placement").is_array() || j.at("placement").empty())
        throw std::runtime_error(
            "soil placement must contain at least one rule");
    j.at("placement").get_to(s.placement);
}
