#pragma once

#include <string>
#include <tuple>
#include <vector>

#include "flora_json.hpp"

// Surface of plant parts (docs/handoff/flora.md): the textures carry shape
// (alpha cutout) and detail, the palette carries colour, so one leaf texture
// serves many species. Texture keys are resolved by the foliage texture array
// once plants are rendered; until then they are only parsed.
enum class FoliageGradient : uint8_t {
    Uniform,      // base colour everywhere
    RootToTip,    // shade at the root, base, tip at the end
    CenterToEdge  // shade at the clump centre, tip on its shell
};

struct FoliagePalette {
    Color3 base{0.30f, 0.49f, 0.20f};
    Color3 shade{0.17f, 0.29f, 0.15f};
    Color3 tip{0.61f, 0.75f, 0.29f};
    Color3 dry{0.54f, 0.48f, 0.23f};
};

struct FoliageMaterial {
    // Too long for a constexpr std::string (small-string buffer).
    static inline const std::string TypeName = "foliage_materials";
    static inline uint8_t counter = 0;

    uint8_t id = 0;
    std::string name;
    std::string albedo;
    std::string normal;
    FoliagePalette palette;
    FoliageGradient gradient = FoliageGradient::RootToTip;
    float hue_jitter = 0.04f;
    float value_jitter = 0.12f;
    float translucency = 0.3f;
    // Colours a withering plant fades through, in order.
    std::vector<Color3> withered;
};

inline void from_json(const Json& j, FoliageMaterial& m) {
    using namespace flora_json;
    known(j, "foliage material",
          {"name", "albedo", "normal", "palette", "gradient", "hue_jitter",
           "value_jitter", "translucency", "withered"});
    m.name = name(j, "name");
    try {
        m.albedo = name(j, "albedo");
        if (j.contains("normal")) m.normal = name(j, "normal");
        if (j.contains("palette")) {
            const auto& p = j.at("palette");
            known(p, "palette", {"base", "shade", "tip", "dry"});
            for (const auto& [key, target] :
                 std::initializer_list<std::pair<const char*, Color3*>>{
                     {"base", &m.palette.base},
                     {"shade", &m.palette.shade},
                     {"tip", &m.palette.tip},
                     {"dry", &m.palette.dry}})
                if (p.contains(key)) *target = color(p.at(key), key);
        }
        if (j.contains("gradient")) {
            const auto& g = j.at("gradient");
            const auto text = g.is_string() ? g.get<std::string>() : "";
            if (text == "uniform")
                m.gradient = FoliageGradient::Uniform;
            else if (text == "root_to_tip")
                m.gradient = FoliageGradient::RootToTip;
            else if (text == "center_to_edge")
                m.gradient = FoliageGradient::CenterToEdge;
            else
                fail("gradient",
                     "expected uniform, root_to_tip or center_to_edge");
        }
        for (const auto& [key, target, hi] :
             std::initializer_list<std::tuple<const char*, float*, float>>{
                 {"hue_jitter", &m.hue_jitter, 0.5f},
                 {"value_jitter", &m.value_jitter, 1.f},
                 {"translucency", &m.translucency, 1.f}})
            if (j.contains(key)) {
                *target = finite(j.at(key), key);
                if (*target < 0 || *target > hi)
                    fail(key, "expected [0, " + std::to_string(hi) + "]");
            }
        if (j.contains("withered")) {
            const auto& w = j.at("withered");
            if (!w.is_array() || w.empty())
                fail("withered", "expected an array of colours");
            for (const auto& c : w) m.withered.push_back(color(c, "withered"));
        }
    } catch (const std::exception& error) {
        throw std::runtime_error(m.name + ": " + error.what());
    }
}
