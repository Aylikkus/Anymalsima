#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "flora_json.hpp"
#include "soil.hpp"

// A plant species: placement rules, growth and a recipe of parts from which
// procedural meshes are built (docs/handoff/flora.md, sections 3 to 5).
// Instances are not blocks; they live in a per-chunk side store. Every
// numeric field accepts a number or a [min, max] range sampled per instance.

enum class FloraKind : uint8_t {
    GrassTuft,
    Flower,
    Fern,
    Herb,
    Bush,
    Shrub,
    Succulent,
};

enum class FloraCollision : uint8_t { None, Soft, Solid };

enum class FloraStage : uint8_t {
    Seedling,
    Young,
    Mature,
    Flowering,
    Fruiting,
    Withering,
};
inline constexpr int FloraStageCount = 6;

enum class FloraPartType : uint8_t {
    Stem,
    Blade,
    LeafCard,
    LeafClump,
    PetalRing,
    FlowerHead,
    FruitCluster,
    Frond,
};

enum class FloraNormalMode : uint8_t { Card, Spherical, Cylindrical };

enum class FloraAttach : uint8_t {
    Ground,            // the plant's anchor point
    Stem,              // along a stem part
    StemTip,           // the end of a stem part
    LeafClumpSurface,  // on the shell of a leaf clump part
};

struct FloraFootprint {
    // Large plants occupy whole tiles; small ones a radius on a shared tile.
    std::optional<std::array<int, 2>> tiles;
    ValueRange radius_m{0.2f};
    bool blocks_placement = false;
    FloraCollision collision = FloraCollision::None;
    float min_spacing_m = 0;
};

struct FloraCompanion {
    std::string species;
    float radius_m = 2;
    float chance = 0.5f;
};

struct FloraSoilRule {
    std::vector<std::string> names;
    ValueRange fertility{0, 1};
    ValueRange water_retention{0, 1};
};

struct FloraRules {
    FloraSoilRule soils;
    std::vector<std::string> ground_cover_required;
    std::vector<std::string> ground_cover_forbidden;
    std::vector<SoilPlacement> climate;
    // Region vegetation types the species grows in; empty: any.
    std::vector<VegetationType> vegetation;
    float max_slope = 1;
    bool underwater = false;
    ValueRange density_per_100m2{1};
    int cluster_scale_m = 0;  // 0: no clustering
    float cluster_threshold = 0;
    std::vector<FloraCompanion> companions;
    int priority = 0;
    // Rare and landmark species: keep the generated sites in every epoch,
    // so a long-unvisited area re-draws their ages but not their places
    // (FloraSimulation re-sampling).
    bool persistent_sites = false;
};

struct FloraGrowth {
    // Duration of each stage in game days; absent stages are skipped.
    std::array<std::optional<ValueRange>, FloraStageCount> stages{};
    bool perennial = false;
    ValueRange lifespan_days{365};
    // Reproduction: once per seeding cycle, at the end of spread_stage, the
    // plant drops `spread_seeds` seeds within spread_radius_m (with
    // probability spread_chance_per_cycle). Each seed germinates with
    // probability spread_germination, lowered by local crowding. Plants
    // never regrow in place: populations live on through seeds.
    float spread_radius_m = 0;
    float spread_chance_per_cycle = 0;
    ValueRange spread_seeds{3};
    float spread_germination = 0.5f;
    // Default: fruiting, else flowering, else the last growing stage.
    std::optional<FloraStage> spread_stage;
    std::optional<ValueRange> regrow_after_harvest_days;
};

struct FloraShell {
    std::array<ValueRange, 3> radii_m{ValueRange{0.5f}, ValueRange{0.5f},
                                      ValueRange{0.5f}};
    ValueRange lift_m{0};
};

struct FloraPart {
    FloraPartType type = FloraPartType::LeafCard;
    ValueRange count{1};
    std::optional<ValueRange> height_m, length_m, width_m, bend, droop,
        spread_angle, phyllotaxis, cards_per_clump, card_size_m;
    std::optional<FloraShell> shell;
    FloraNormalMode normal_mode = FloraNormalMode::Card;
    std::string material;
    float color_variation = 0;
    FloraAttach attach = FloraAttach::Ground;
    // Scale per growth stage; a stage not listed hides the part. Empty
    // means every stage at scale 1.
    std::array<std::optional<float>, FloraStageCount> stages{};

    float stageScale(FloraStage stage) const {
        bool any = false;
        for (const auto& s : stages) any = any || s.has_value();
        if (!any) return 1;
        return stages[size_t(stage)].value_or(0);
    }
};

struct FloraForage {
    std::string item;  // resolved once an items data type exists
    ValueRange count{1};
    FloraStage stage = FloraStage::Mature;
};

struct Flora {
    static constexpr const std::string TypeName = "flora";
    static inline uint8_t counter = 0;

    uint8_t id = 0;
    std::string name;
    FloraKind kind = FloraKind::Herb;
    FloraFootprint footprint;
    FloraRules placement;
    FloraGrowth growth;
    std::vector<FloraPart> parts;
    std::vector<FloraForage> forage;
};

namespace flora_json {

template <typename E, size_t N>
E keyword(const Json& value, std::string_view key,
          const std::array<std::pair<std::string_view, E>, N>& options) {
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        for (const auto& [word, result] : options)
            if (word == text) return result;
    }
    std::string expected;
    for (const auto& [word, _] : options)
        expected += (expected.empty() ? "" : ", ") + std::string(word);
    fail(key, "expected one of " + expected);
}

inline constexpr std::array<std::pair<std::string_view, FloraStage>,
                            FloraStageCount>
    StageNames{{{"seedling", FloraStage::Seedling},
                {"young", FloraStage::Young},
                {"mature", FloraStage::Mature},
                {"flowering", FloraStage::Flowering},
                {"fruiting", FloraStage::Fruiting},
                {"withering", FloraStage::Withering}}};

inline FloraStage stage(const std::string& text, std::string_view key) {
    return keyword(Json(text), key, StageNames);
}

inline void optionalRange(const Json& j, const char* key,
                          std::optional<ValueRange>& out, float lo = -1e9f,
                          float hi = 1e9f) {
    if (j.contains(key)) out = range(j.at(key), key, lo, hi);
}

inline float number(const Json& j, const char* key, float fallback, float lo,
                    float hi) {
    if (!j.contains(key)) return fallback;
    const float value = finite(j.at(key), key);
    if (value < lo || value > hi)
        fail(key, "expected [" + std::to_string(lo) + ", " +
                      std::to_string(hi) + "]");
    return value;
}

inline void soilRule(const Json& j, FloraSoilRule& out) {
    known(j, "soils", {"names", "fertility", "water_retention"});
    out.names = names(j, "names");
    range(j, "fertility", out.fertility, 0, 1);
    range(j, "water_retention", out.water_retention, 0, 1);
}

// The "placement" object shared by flora and trees (docs/modding/plants.md,
// docs/modding/trees.md).
inline void placement(const Json& p, FloraRules& r) {
    known(p, "placement",
          {"soils", "ground_cover", "climate", "max_slope", "underwater",
           "density_per_100m2", "cluster", "companions", "priority",
           "persistent_sites", "vegetation"});
    if (p.contains("soils")) soilRule(p.at("soils"), r.soils);
    if (p.contains("ground_cover")) {
        const auto& g = p.at("ground_cover");
        known(g, "ground_cover", {"required", "forbidden"});
        r.ground_cover_required = names(g, "required");
        r.ground_cover_forbidden = names(g, "forbidden");
    }
    if (!p.contains("climate") || !p.at("climate").is_array() ||
        p.at("climate").empty())
        fail("climate", "expected at least one rule");
    p.at("climate").get_to(r.climate);
    soil_json::enums(p, "vegetation", RegionEnumMaps::vegetation(),
                     r.vegetation);
    r.max_slope = number(p, "max_slope", 1, 0, 64);
    r.underwater = boolean(p, "underwater", false);
    range(p, "density_per_100m2", r.density_per_100m2, 0, 10000);
    if (p.contains("cluster")) {
        const auto& c = p.at("cluster");
        known(c, "cluster", {"scale_m", "threshold"});
        if (!c.contains("scale_m") || !c.at("scale_m").is_number_integer() ||
            c.at("scale_m").get<int64_t>() < 1 ||
            c.at("scale_m").get<int64_t>() > 4096)
            fail("scale_m", "expected an integer in [1, 4096]");
        r.cluster_scale_m = c.at("scale_m").get<int>();
        r.cluster_threshold = number(c, "threshold", 0.5f, 0, 0.999f);
    }
    if (p.contains("companions")) {
        const auto& list = p.at("companions");
        if (!list.is_array()) fail("companions", "expected an array");
        for (const auto& c : list) {
            known(c, "companion", {"species", "radius_m", "chance"});
            r.companions.push_back({name(c, "species"),
                                    number(c, "radius_m", 2, 0, 64),
                                    number(c, "chance", 0.5f, 0, 1)});
        }
    }
    r.persistent_sites = boolean(p, "persistent_sites", false);
    if (p.contains("priority")) {
        if (!p.at("priority").is_number_integer())
            fail("priority", "expected an integer");
        r.priority = p.at("priority").get<int>();
    }
}

}  // namespace flora_json

inline void from_json(const Json& j, FloraPart& p) {
    using namespace flora_json;
    known(j, "part",
          {"type", "count", "height_m", "length_m", "width_m", "bend", "droop",
           "spread_angle", "phyllotaxis", "shell", "cards_per_clump",
           "card_size_m", "normal_mode", "material", "color_variation",
           "attach", "stages"});
    if (!j.contains("type")) fail("type", "missing part type");
    p.type = keyword(j.at("type"), "type",
                     std::array<std::pair<std::string_view, FloraPartType>, 8>{
                         {{"stem", FloraPartType::Stem},
                          {"blade", FloraPartType::Blade},
                          {"leaf_card", FloraPartType::LeafCard},
                          {"leaf_clump", FloraPartType::LeafClump},
                          {"petal_ring", FloraPartType::PetalRing},
                          {"flower_head", FloraPartType::FlowerHead},
                          {"fruit_cluster", FloraPartType::FruitCluster},
                          {"frond", FloraPartType::Frond}}});
    range(j, "count", p.count, 0, 4096);
    optionalRange(j, "height_m", p.height_m, 0, 64);
    optionalRange(j, "length_m", p.length_m, 0, 64);
    optionalRange(j, "width_m", p.width_m, 0, 64);
    optionalRange(j, "bend", p.bend, -1, 1);
    optionalRange(j, "droop", p.droop, -1, 1);
    optionalRange(j, "spread_angle", p.spread_angle, 0, 180);
    optionalRange(j, "phyllotaxis", p.phyllotaxis, 0, 360);
    optionalRange(j, "cards_per_clump", p.cards_per_clump, 1, 1024);
    optionalRange(j, "card_size_m", p.card_size_m, 0.001f, 16);
    if (j.contains("shell")) {
        const auto& s = j.at("shell");
        known(s, "shell", {"radii_m", "lift_m"});
        FloraShell shell;
        if (s.contains("radii_m")) {
            const auto& radii = s.at("radii_m");
            if (!radii.is_array() || radii.size() != 3)
                fail("radii_m", "expected three radii (x, y, z)");
            for (size_t i = 0; i < 3; ++i)
                shell.radii_m[i] = range(radii[i], "radii_m", 0.001f, 32);
        }
        range(s, "lift_m", shell.lift_m, -16, 64);
        p.shell = shell;
    }
    if (p.type == FloraPartType::LeafClump && !p.shell)
        fail("shell", "a leaf_clump needs a shell");
    if (j.contains("normal_mode"))
        p.normal_mode =
            keyword(j.at("normal_mode"), "normal_mode",
                    std::array<std::pair<std::string_view, FloraNormalMode>, 3>{
                        {{"card", FloraNormalMode::Card},
                         {"spherical", FloraNormalMode::Spherical},
                         {"cylindrical", FloraNormalMode::Cylindrical}}});
    else if (p.type == FloraPartType::LeafClump)
        p.normal_mode = FloraNormalMode::Spherical;
    p.material = name(j, "material");
    p.color_variation = number(j, "color_variation", 0, 0, 1);
    if (j.contains("attach"))
        p.attach = keyword(
            j.at("attach"), "attach",
            std::array<std::pair<std::string_view, FloraAttach>, 4>{
                {{"ground", FloraAttach::Ground},
                 {"stem", FloraAttach::Stem},
                 {"stem_tip", FloraAttach::StemTip},
                 {"leaf_clump_surface", FloraAttach::LeafClumpSurface}}});
    if (j.contains("stages")) {
        const auto& s = j.at("stages");
        if (!s.is_object() || s.empty())
            fail("stages", "expected {stage: scale}");
        for (const auto& [key, value] : s.items()) {
            const float scale = finite(value, key);
            if (scale < 0 || scale > 4) fail(key, "expected a scale in [0, 4]");
            p.stages[size_t(stage(key, "stages"))] = scale;
        }
    }
}

inline void from_json(const Json& j, FloraForage& f) {
    using namespace flora_json;
    known(j, "forage", {"item", "count", "stage"});
    f.item = name(j, "item");
    range(j, "count", f.count, 0, 1024);
    if (j.contains("stage"))
        f.stage = keyword(j.at("stage"), "stage", StageNames);
}

inline void from_json(const Json& j, Flora& f) {
    using namespace flora_json;
    known(j, "flora",
          {"name", "kind", "footprint", "placement", "growth", "parts",
           "forage"});
    f.name = name(j, "name");
    try {
        if (!j.contains("kind")) fail("kind", "missing plant kind");
        f.kind = keyword(j.at("kind"), "kind",
                         std::array<std::pair<std::string_view, FloraKind>, 7>{
                             {{"grass_tuft", FloraKind::GrassTuft},
                              {"flower", FloraKind::Flower},
                              {"fern", FloraKind::Fern},
                              {"herb", FloraKind::Herb},
                              {"bush", FloraKind::Bush},
                              {"shrub", FloraKind::Shrub},
                              {"succulent", FloraKind::Succulent}}});

        if (j.contains("footprint")) {
            const auto& fp = j.at("footprint");
            known(fp, "footprint",
                  {"tiles", "radius_m", "blocks_placement", "collision",
                   "min_spacing_m"});
            if (fp.contains("tiles") && fp.contains("radius_m"))
                fail("footprint", "use either tiles or radius_m");
            if (fp.contains("tiles")) {
                const auto& t = fp.at("tiles");
                if (!t.is_array() || t.size() != 2 ||
                    !t[0].is_number_integer() || !t[1].is_number_integer() ||
                    t[0].get<int64_t>() < 1 || t[0].get<int64_t>() > 16 ||
                    t[1].get<int64_t>() < 1 || t[1].get<int64_t>() > 16)
                    fail("tiles", "expected [x, z] tile counts in [1, 16]");
                f.footprint.tiles =
                    std::array<int, 2>{t[0].get<int>(), t[1].get<int>()};
            }
            range(fp, "radius_m", f.footprint.radius_m, 0.01f, 8);
            f.footprint.blocks_placement =
                boolean(fp, "blocks_placement", false);
            if (fp.contains("collision"))
                f.footprint.collision = keyword(
                    fp.at("collision"), "collision",
                    std::array<std::pair<std::string_view, FloraCollision>, 3>{
                        {{"none", FloraCollision::None},
                         {"soft", FloraCollision::Soft},
                         {"solid", FloraCollision::Solid}}});
            f.footprint.min_spacing_m = number(fp, "min_spacing_m", 0, 0, 64);
        }
        if (f.footprint.blocks_placement && !f.footprint.tiles)
            fail("blocks_placement", "only tile footprints block placement");

        if (!j.contains("placement")) fail("placement", "missing placement");
        placement(j.at("placement"), f.placement);

        if (j.contains("growth")) {
            const auto& g = j.at("growth");
            known(g, "growth",
                  {"stages", "perennial", "lifespan_days", "season_mask",
                   "spread", "regrow_after_harvest_days"});
            if (g.contains("stages")) {
                const auto& s = g.at("stages");
                if (!s.is_object()) fail("stages", "expected {stage: days}");
                for (const auto& [key, value] : s.items())
                    f.growth.stages[size_t(stage(key, "stages"))] =
                        range(value, key, 0, 100000);
            }
            f.growth.perennial = boolean(g, "perennial", false);
            range(g, "lifespan_days", f.growth.lifespan_days, 0, 1000000);
            // Seasons arrive with the calendar; only null is accepted now.
            if (g.contains("season_mask") && !g.at("season_mask").is_null())
                fail("season_mask", "seasons are not supported yet");
            if (g.contains("spread")) {
                const auto& s = g.at("spread");
                known(s, "spread",
                      {"radius_m", "chance_per_cycle", "seeds", "germination",
                       "stage"});
                f.growth.spread_radius_m = number(s, "radius_m", 0, 0, 256);
                f.growth.spread_chance_per_cycle =
                    number(s, "chance_per_cycle", 1, 0, 1);
                range(s, "seeds", f.growth.spread_seeds, 0, 64);
                f.growth.spread_germination =
                    number(s, "germination", 0.5f, 0, 1);
                if (s.contains("stage")) {
                    f.growth.spread_stage =
                        keyword(s.at("stage"), "stage", StageNames);
                    if (*f.growth.spread_stage == FloraStage::Withering)
                        fail("stage", "seeds drop before withering");
                    if (!f.growth.stages[size_t(*f.growth.spread_stage)])
                        fail("stage", "the species has no such stage");
                }
            }
            optionalRange(g, "regrow_after_harvest_days",
                          f.growth.regrow_after_harvest_days, 0, 100000);
        }

        if (!j.contains("parts") || !j.at("parts").is_array() ||
            j.at("parts").empty())
            fail("parts", "expected at least one part");
        j.at("parts").get_to(f.parts);
        const auto has = [&](FloraPartType type) {
            for (const auto& part : f.parts)
                if (part.type == type) return true;
            return false;
        };
        for (const auto& part : f.parts) {
            if (part.attach == FloraAttach::LeafClumpSurface &&
                !has(FloraPartType::LeafClump))
                fail("attach", "leaf_clump_surface needs a leaf_clump part");
            if ((part.attach == FloraAttach::Stem ||
                 part.attach == FloraAttach::StemTip) &&
                !has(FloraPartType::Stem))
                fail("attach", "stem attachment needs a stem part");
        }
        if (j.contains("forage")) {
            if (!j.at("forage").is_array()) fail("forage", "expected an array");
            j.at("forage").get_to(f.forage);
        }
    } catch (const std::exception& error) {
        throw std::runtime_error(f.name + ": " + error.what());
    }
}
