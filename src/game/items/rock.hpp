#pragma once

#include <optional>
#include <string>

#include "enums.hpp"
#include "serialization/serializable_api.hpp"
#include "utils/enum_parser.hpp"
#include "utils/items_enum_maps.hpp"
#include "utils/region_enum_maps.hpp"

struct RockProperties {
    float porosity = 0.0f;
    float permeability = 0.0f;
};

struct RockPlacement {
    std::vector<LandType> allowed_land;
    std::vector<CrustType> allowed_crust;

    std::vector<FeatureMask> required_features;
    std::vector<FeatureMask> forbidden_features;

    bool allow_underwater = false;
    float weight = 1.0f;
};

struct RockTransform {
    std::string result;
    int min_depth_m;
};

struct Rock {
    static constexpr const std::string TypeName = "rocks";
    static inline uint8_t counter = 0;

    uint8_t id = 0;
    std::string name;
    std::string block_name;
    RockType category;

    RockProperties properties;
    RockPlacement placement;
    std::optional<std::pair<int, int>> body_size_m;

    std::vector<RockTransform> transforms;
};

inline void from_json(const Json &j, RockProperties &p) {
    j.at("porosity").get_to(p.porosity);
    j.at("permeability").get_to(p.permeability);
}

inline void from_json(const Json &j, RockPlacement &p) {
    if (j.contains("allowed_land"))
        p.allowed_land = EnumParser<LandType>::parseArray(
            RegionEnumMaps::land(), j["allowed_land"]);

    if (j.contains("allowed_crust"))
        p.allowed_crust = EnumParser<CrustType>::parseArray(
            RegionEnumMaps::crust(), j["allowed_crust"]);

    if (j.contains("required_features"))
        p.required_features = EnumParser<FeatureMask>::parseArray(
            RegionEnumMaps::features(), j["required_features"]);

    if (j.contains("forbidden_features"))
        p.forbidden_features = EnumParser<FeatureMask>::parseArray(
            RegionEnumMaps::features(), j["forbidden_features"]);

    if (j.contains("allow_underwater"))
        j.at("allow_underwater").get_to(p.allow_underwater);

    if (j.contains("weight")) j.at("weight").get_to(p.weight);
}


inline void from_json(const Json &j, RockTransform &t) {
    j.at("result").get_to(t.result);
    j.at("min_depth_m").get_to(t.min_depth_m);
}

inline void from_json(const Json &j, Rock &r) {
    j.at("name").get_to(r.name);
    j.at("block_name").get_to(r.block_name);
    r.category =
        EnumParser<RockType>::parse(ItemsEnumMaps::rockTypes(), j["category"]);

    j.at("properties").get_to(r.properties);

    if (j.contains("placement")) j.at("placement").get_to(r.placement);

    if (j.contains("body_size_m")) {
        r.body_size_m =
            std::make_pair(j["body_size_m"][0], j["body_size_m"][1]);
    }

    if (j.contains("transforms")) j.at("transforms").get_to(r.transforms);
}