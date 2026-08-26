#pragma once
#include <optional>
#include <string>

#include "enums.hpp"
#include "render/renderer_api.hpp"
#include "serialization/serializable_api.hpp"
#include "utils/enum_parser.hpp"
#include "utils/items_enum_maps.hpp"

struct BlockProperties {
    int hardness = 0;
    bool flammable = false;
    uint8_t opacity = 15; // 0 - 15
};

struct Block {
    static constexpr const std::string TypeName = "blocks";
    static inline uint8_t counter = 0;

    uint8_t id = 0;
    std::string name;
    std::string material;
    BlockRenderCategory renderCategory = BlockRenderCategory::TERRAIN_ROCK;
    BlockProperties properties;
};

inline void from_json(const Json& j, BlockProperties& p) {
    if (j.contains("hardness")) j.at("hardness").get_to(p.hardness);
    if (j.contains("flammable")) j.at("flammable").get_to(p.flammable);
    if (j.contains("opacity")) j.at("transparent").get_to(p.opacity);
}

inline void from_json(const Json& j, Block& b) {
    j.at("name").get_to(b.name);
    j.at("material").get_to(b.material);
    if (j.contains("properties")) j.at("properties").get_to(b.properties);
    if (j.contains("render_category")) {
        b.renderCategory = EnumParser<BlockRenderCategory>::parse(
            ItemsEnumMaps::blockRenderCategories(), j["render_category"]);
    }
}