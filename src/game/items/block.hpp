#pragma once
#include <bitset>
#include <optional>
#include <stdexcept>
#include <string>

#include "enums.hpp"
#include "light_emission.hpp"
#include "render/renderer_api.hpp"
#include "serialization/serializable_api.hpp"
#include "thermal_emission.hpp"
#include "utils/enum_parser.hpp"
#include "utils/items_enum_maps.hpp"

struct BlockProperties {
    int hardness = 0;
    bool flammable = false;
    uint8_t opacity = 15;  // 0 - 15
    // Living wood of trees (docs/handoff/trees.md): placed only by tree
    // growth, never by players. Edit detection and distant structures treat
    // it as air, because trees are regenerated from the seed.
    bool natural_growth = false;
};

struct Block {
    static constexpr const std::string TypeName = "blocks";
    static inline uint8_t counter = 0;

    uint8_t id = 0;
    std::string name;
    std::string material;
    BlockRenderCategory renderCategory = BlockRenderCategory::TERRAIN_ROCK;
    BlockProperties properties;
    // Present when the block emits light (see light_emission.hpp).
    std::optional<LightEmission> light;
    // Present when the block warms or cools nearby space (thermal_emission).
    std::optional<ThermalEmission> thermal;
};

inline void from_json(const Json& j, BlockProperties& p) {
    if (j.contains("hardness")) j.at("hardness").get_to(p.hardness);
    if (j.contains("flammable")) j.at("flammable").get_to(p.flammable);
    if (j.contains("opacity")) j.at("opacity").get_to(p.opacity);
    if (j.contains("natural_growth"))
        j.at("natural_growth").get_to(p.natural_growth);
}

inline void from_json(const Json& j, Block& b) {
    j.at("name").get_to(b.name);
    j.at("material").get_to(b.material);
    if (j.contains("properties")) j.at("properties").get_to(b.properties);
    if (j.contains("light")) {
        try {
            b.light = j.at("light").get<LightEmission>();
        } catch (const std::exception& e) {
            throw std::runtime_error(b.name + ": " + e.what());
        }
        if (!b.light->emits()) b.light.reset();
    }
    if (j.contains("thermal")) {
        try {
            b.thermal = j.at("thermal").get<ThermalEmission>();
        } catch (const std::exception& e) {
            throw std::runtime_error(b.name + ": " + e.what());
        }
        if (!b.thermal->emits()) b.thermal.reset();
    }
    if (j.contains("render_category")) {
        b.renderCategory = EnumParser<BlockRenderCategory>::parse(
            ItemsEnumMaps::blockRenderCategories(), j["render_category"]);
    }
}

// Block ids with natural_growth set (tree wood), indexed by Tile::block_id.
using GrownBlockSet = std::bitset<256>;
template <typename Blocks>
GrownBlockSet naturalGrowthBlocks(const Blocks& blocks) {
    GrownBlockSet out;
    for (const Block& block : blocks)
        if (block.properties.natural_growth) out.set(block.id);
    return out;
}
