#include <filesystem>

#include "flora_assets.hpp"
#include "items/flora_validation.hpp"
#include "world/ground_cover_generator.hpp"

namespace {
Json coverJson(const std::string& name = "ground_covers.meadow") {
    return Json{{"name", name},
                {"overlay_material", "materials.cover_meadow"},
                {"placement", Json::array({Json::object()})}};
}
Json bushJson() {
    return Json::parse(R"({
        "name": "flora.bush",
        "kind": "bush",
        "footprint": { "tiles": [2, 2], "blocks_placement": true,
                       "collision": "soft", "min_spacing_m": 1.5 },
        "placement": {
            "soils": { "fertility": [0.3, 1] },
            "ground_cover": { "required": ["ground_covers.meadow"] },
            "climate": [ { "allowed_land": ["Plains"] } ],
            "density_per_100m2": [0.5, 2],
            "cluster": { "scale_m": 18, "threshold": 0.55 },
            "priority": 10
        },
        "growth": { "stages": { "seedling": [3, 6], "mature": 40 },
                    "perennial": true, "season_mask": null },
        "parts": [
            { "type": "leaf_clump", "count": [3, 5],
              "shell": { "radii_m": [[0.7, 1.0], 0.6, [0.7, 1.0]] },
              "material": "foliage.leaf",
              "stages": { "seedling": 0.2, "mature": 1.0 } },
            { "type": "fruit_cluster", "material": "foliage.leaf",
              "attach": "leaf_clump_surface", "stages": { "fruiting": 1 } }
        ],
        "forage": [ { "item": "items.berry", "count": [2, 6],
                      "stage": "fruiting" } ]
    })");
}
Region plains() {
    Region r{};
    r.land_height_m = SEA_LEVEL_Y + 20;
    r.land = LandType::PLAINS;
    r.vegetation = VegetationType::GRASS;
    r.humidity = .5f;
    r.temperature_c = 15;
    return r;
}
}  // namespace

TEST(flora_value_ranges) {
    using flora_json::range;
    const auto single = range(Json(0.5), "x");
    CHECK(single.min == 0.5f && single.max == 0.5f);
    const auto pair = range(Json::array({1, 3}), "x");
    CHECK(pair.min == 1 && pair.max == 3);
    EQUAL(pair.sample(0), 1.f, "range start");
    EQUAL(pair.sample(1), 3.f, "range end");
    EQUAL(pair.sample(0.5), 2.f, "range middle");
    // Integer sampling: every count in [1, 3] is reachable, none outside.
    std::array<int, 5> seen{};
    for (int i = 0; i < 300; ++i) {
        const int value = pair.sampleInt(i / 299.0);
        CHECK(value >= 1 && value <= 3);
        ++seen[value];
    }
    CHECK(seen[1] > 0 && seen[2] > 0 && seen[3] > 0);
    for (const auto& bad : {Json::array({3, 1}), Json::array({1}), Json("1"),
                            Json(nullptr), Json::array({1, "2"}), Json(1e30)})
        test::expectThrows([&] { range(bad, "x"); }, "invalid range");
    test::expectThrows([&] { range(Json(2), "x", 0, 1); }, "out of bounds");
    const auto c = flora_json::color(Json("#ff8000"), "c");
    CHECK(c.r == 1 && c.b == 0 && std::abs(c.g - 128 / 255.f) < 1e-6f);
    EQUAL(c.packed(), 0xff0080ffu, "packed RGBA8, red in the low byte");
    for (const auto& bad : {Json("ff8000"), Json("#ff80"), Json("#gg0000")})
        test::expectThrows([&] { flora_json::color(bad, "c"); }, "bad colour");
}

TEST(ground_cover_parsing) {
    auto full = coverJson();
    full["faces"] = {{"top", true}, {"sides", false}, {"side_fringe_m", 0.3}};
    full["soils"] = {{"names", {"soils.loam"}}, {"fertility", {0.4, 1}}};
    full["vegetation"] = {"Grass", "Shrub"};
    full["density_noise"] = {
        {"scale_m", 16}, {"threshold", 0.3}, {"contrast", 2}};
    full["max_slope"] = 0.6;
    full["tint_gradient"] = {{"humid_warm", "#00ff00"},
                             {"dry_cold", "#808080"},
                             {"temperature_c", {-10, 20}}};
    const auto cover = full.get<GroundCover>();
    CHECK(cover.faces.top && !cover.faces.sides);
    CHECK(cover.faces.side_fringe_m == ValueRange(0.3f));
    EQUAL(cover.density_noise.scale_m, 16, "noise scale");
    EQUAL(cover.vegetation.size(), size_t(2), "vegetation list");
    // Tint corners: humid + warm, dry + cold, and clamped beyond the range.
    CHECK(cover.tint_gradient.at(1, 40) == (Color3{0, 1, 0}));
    const auto cold = cover.tint_gradient.at(0, -30);
    CHECK(std::abs(cold.r - 128 / 255.f) < 1e-6f);
    // Soil and vegetation rules.
    SoilProperties loam{.4f, .5f, .6f, .7f, .02f};
    CHECK(cover.acceptsSoil("soils.loam", loam));
    CHECK(!cover.acceptsSoil("soils.sand", loam));
    loam.fertility = .2f;
    CHECK(!cover.acceptsSoil("soils.loam", loam));
    CHECK(cover.acceptsVegetation(VegetationType::SHRUB));
    CHECK(!cover.acceptsVegetation(VegetationType::FOREST));
    CHECK(!coverJson().get<GroundCover>().acceptsVegetation(
        VegetationType::NONE));

    const auto rejects = [](const char* key, Json value) {
        auto bad = coverJson();
        if (value.is_null())
            bad.erase(key);
        else
            bad[key] = value;
        test::expectThrows([&] { bad.get<GroundCover>(); },
                           std::string("rejects ") + key);
    };
    rejects("placement", Json::array());
    rejects("placement", nullptr);
    rejects("overlay_material", "");
    rejects("faces", {{"top", false}, {"sides", false}});
    rejects("faces", {{"side_fringe_m", {0.5, 2}}});
    rejects("density_noise", {{"threshold", 1}});
    rejects("density_noise", {{"scale_m", 0.5}});
    rejects("max_slope", 0);
    rejects("vegetation", {"Jungle"});
    rejects("tint_gradient", {{"humid_warm", "green"}});
    rejects("typo_field", 1);
}

TEST(foliage_material_parsing) {
    const auto m = Json::parse(R"({
        "name": "foliage.leaf", "albedo": "foliage.leaf_broad",
        "palette": { "base": "#4c7d33", "tip": "#9bbf4a" },
        "gradient": "center_to_edge", "translucency": 0.35,
        "withered": ["#8a7a3a", "#5e4a2a"] })")
                       .get<FoliageMaterial>();
    CHECK(m.gradient == FoliageGradient::CenterToEdge);
    EQUAL(m.withered.size(), size_t(2), "withered colours");
    CHECK(m.palette.tip == flora_json::color(Json("#9bbf4a"), "tip"));
    for (const auto& [key, value] :
         std::initializer_list<std::pair<const char*, Json>>{
             {"gradient", "radial"},
             {"translucency", 2},
             {"withered", Json::array()},
             {"albedo", nullptr},
             {"colour", "#000000"}}) {
        auto bad = Json{{"name", "foliage.bad"}, {"albedo", "x"}};
        bad[key] = value;
        if (value.is_null()) bad.erase(key);
        test::expectThrows([&] { bad.get<FoliageMaterial>(); }, key);
    }
}

TEST(flora_species_parsing) {
    const auto bush = bushJson().get<Flora>();
    CHECK(bush.kind == FloraKind::Bush);
    CHECK(bush.footprint.tiles == (std::array<int, 2>{2, 2}));
    CHECK(bush.footprint.collision == FloraCollision::Soft);
    EQUAL(bush.placement.priority, 10, "priority");
    EQUAL(bush.placement.cluster_scale_m, 18, "cluster scale");
    CHECK(bush.growth.stages[size_t(FloraStage::Mature)] == ValueRange(40));
    CHECK(!bush.growth.stages[size_t(FloraStage::Young)]);
    const auto& clump = bush.parts[0];
    CHECK(clump.normal_mode == FloraNormalMode::Spherical);
    CHECK(clump.shell->radii_m[1] == ValueRange(0.6f));
    EQUAL(clump.stageScale(FloraStage::Seedling), 0.2f, "listed stage");
    EQUAL(clump.stageScale(FloraStage::Withering), 0.f, "unlisted stage");
    EQUAL(bush.parts[1].stageScale(FloraStage::Fruiting), 1.f, "fruit");
    FloraPart plain;
    EQUAL(plain.stageScale(FloraStage::Young), 1.f, "no stages: always");
    CHECK(bush.forage[0].stage == FloraStage::Fruiting);
    // Seeds: defaults, and the seed stage.
    auto seeding = bushJson();
    seeding["growth"]["spread"] = {{"radius_m", 4},
                                   {"seeds", {2, 5}},
                                   {"germination", 0.7},
                                   {"stage", "mature"}};
    const auto seeded = seeding.get<Flora>();
    CHECK(seeded.growth.spread_seeds == ValueRange(2, 5));
    EQUAL(seeded.growth.spread_germination, 0.7f, "germination");
    EQUAL(seeded.growth.spread_chance_per_cycle, 1.f, "default chance");
    CHECK(seeded.growth.spread_stage == FloraStage::Mature);
    CHECK(!bush.growth.spread_stage);
    CHECK(!bush.placement.persistent_sites);
    auto landmark = bushJson();
    landmark["placement"]["persistent_sites"] = true;
    CHECK(landmark.get<Flora>().placement.persistent_sites);

    const auto rejects = [](auto mutate, const std::string& context) {
        auto bad = bushJson();
        mutate(bad);
        test::expectThrows([&] { bad.get<Flora>(); }, context);
    };
    rejects([](Json& j) { j["kind"] = "tree"; }, "unknown kind");
    rejects([](Json& j) { j["footprint"]["radius_m"] = 0.2; },
            "tiles and radius");
    rejects([](Json& j) { j["footprint"]["tiles"] = {0, 2}; }, "empty tiles");
    rejects(
        [](Json& j) {
            j["footprint"].erase("tiles");
            j["footprint"]["radius_m"] = 0.3;
        },
        "small plants cannot block placement");
    rejects([](Json& j) { j["placement"]["climate"] = Json::array(); },
            "no climate rule");
    rejects([](Json& j) { j["growth"]["stages"]["budding"] = 3; },
            "unknown stage");
    rejects([](Json& j) { j["growth"]["season_mask"] = {1, 0}; },
            "seasons not supported yet");
    rejects([](Json& j) { j["placement"]["persistent_sites"] = 1; },
            "persistent_sites is a flag");
    rejects([](Json& j) { j["growth"]["spread"] = {{"stage", "withering"}}; },
            "seeds drop before withering");
    rejects([](Json& j) { j["growth"]["spread"] = {{"stage", "fruiting"}}; },
            "seed stage the species lacks");
    rejects([](Json& j) { j["growth"]["spread"] = {{"germination", 2}}; },
            "germination is a probability");
    rejects([](Json& j) { j["growth"]["spread"] = {{"seeds", {5, 2}}}; },
            "reversed seed range");
    rejects([](Json& j) { j["parts"] = Json::array(); }, "no parts");
    rejects([](Json& j) { j["parts"][0].erase("shell"); },
            "leaf clump without shell");
    rejects([](Json& j) { j["parts"][0]["type"] = "branch"; }, "part type");
    rejects([](Json& j) { j["parts"][0]["stages"]["mature"] = 5; },
            "stage scale bound");
    rejects([](Json& j) { j["parts"][0]["type"] = "leaf_card"; },
            "fruit on a missing leaf clump");
    rejects([](Json& j) { j["parts"][1]["attach"] = "stem_tip"; },
            "stem attachment without stem");
    rejects([](Json& j) { j["parts"][1]["colour"] = 1; }, "unknown field");
}

TEST(flora_references_validated) {
    test::FloraAssets assets;
    test::loadMeadow(assets);
    assets.load<FoliageMaterial>(
        Json::array({{{"name", "foliage.leaf"}, {"albedo", "leaf"}}}));
    auto bush = bushJson();
    bush["placement"]["companions"] = {{{"species", "flora.bush"}}};
    assets.load<Flora>(Json::array({bush}));
    validateFloraReferences(assets);
    for (const auto& [mutate, context] :
         std::initializer_list<std::pair<void (*)(Json&), const char*>>{
             {[](Json& j) { j["parts"][0]["material"] = "foliage.none"; },
              "foliage material"},
             {[](Json& j) {
                  j["placement"]["ground_cover"]["forbidden"] = {"gc.none"};
              },
              "ground cover"},
             {[](Json& j) {
                  j["placement"]["companions"] = {{{"species", "flora.x"}}};
              },
              "companion"},
             {[](Json& j) { j["placement"]["soils"]["names"] = {"soils.x"}; },
              "soil"}}) {
        test::FloraAssets broken;
        test::loadMeadow(broken);
        broken.load<FoliageMaterial>(
            Json::array({{{"name", "foliage.leaf"}, {"albedo", "leaf"}}}));
        auto bad = bushJson();
        mutate(bad);
        broken.load<Flora>(Json::array({bad}));
        test::expectThrows([&] { validateFloraReferences(broken); }, context);
    }
    test::FloraAssets cover;
    test::loadMeadow(cover);
    auto missing = coverJson("ground_covers.missing");
    missing["overlay_material"] = "materials.none";
    cover.load<GroundCover>(Json::array({missing}));
    test::expectThrows([&] { validateFloraReferences(cover); },
                       "overlay material");
}

TEST(ground_cover_selection_rules) {
    test::FloraAssets assets;
    test::loadMeadow(assets);
    auto dry = coverJson("ground_covers.dry");
    dry["placement"] = {{{"humidity_range", {0, 0.3}}, {"priority", 5}}};
    auto picky = coverJson("ground_covers.picky");
    picky["soils"] = {{"names", {"soils.loam"}}, {"fertility", {0.9, 1}}};
    picky["placement"] = {{{"priority", 50}}};
    assets.load<GroundCover>(Json::array({dry, picky}));
    const auto name = [&](const GroundCoverProfile& p) {
        return p.cover_id
                   ? assets.getStorage<GroundCover>().get(p.cover_id).name
                   : std::string("none");
    };
    auto r = plains();
    // picky's priority never applies: loam fertility 0.7 is outside it.
    EQUAL(name(GroundCoverGenerator::select(r, 1, assets, 7)),
          std::string("ground_covers.meadow"), "humid plains");
    r.humidity = .2f;
    EQUAL(name(GroundCoverGenerator::select(r, 1, assets, 7)),
          std::string("ground_covers.dry"), "dry rule has priority");
    r.land = LandType::HILLS;  // meadow is Plains-only; dry still applies
    EQUAL(name(GroundCoverGenerator::select(r, 1, assets, 7)),
          std::string("ground_covers.dry"), "hills");
    r.vegetation = VegetationType::NONE;
    EQUAL(name(GroundCoverGenerator::select(r, 1, assets, 7)),
          std::string("none"), "bare region");
    EQUAL(name(GroundCoverGenerator::select(plains(), 0, assets, 7)),
          std::string("none"), "no soil");

    // Density: deterministic, 0-15, and zero on slopes past max_slope.
    const auto profile = GroundCoverGenerator::select(plains(), 1, assets, 7);
    int low = 15, high = 0;
    for (int x = 0; x < 256; ++x) {
        const auto d = GroundCoverGenerator::density(profile, x, 3, 0);
        EQUAL(d, GroundCoverGenerator::density(profile, x, 3, 0), "repeat");
        low = std::min<int>(low, d);
        high = std::max<int>(high, d);
        EQUAL(int(GroundCoverGenerator::density(profile, x, 3, 0.8)), 0,
              "too steep");
    }
    CHECK(low < high && high <= 15);
    // The pattern is periodic in X when the world wraps.
    for (int x = 0; x < 64; ++x)
        EQUAL(GroundCoverGenerator::density(profile, x, 9, 0, 1024),
              GroundCoverGenerator::density(profile, x + 1024, 9, 0, 1024),
              "periodic density");
}

TEST(flora_runtime_content) {
    test::FloraAssets assets;
    for (const auto* file : {"materials/rocks.json", "materials/soils.json",
                             "materials/vegetation.json"})
        assets.loadContent<Material>(file);
    for (const auto* file : {"blocks/rocks.json", "blocks/soils.json"})
        assets.loadContent<Block>(file);
    assets.loadContent<Soil>("soils/soils.json");
    assets.loadContent<GroundCover>("ground_covers/ground_covers.json");
    assets.loadContent<FoliageMaterial>(
        "foliage_materials/foliage_materials.json");
    assets.loadContent<Flora>("flora/flora.json");
    validateFloraReferences(assets);
    // Foliage texture keys name generated cards: foliage.<card> is
    // assets/materials/foliage/<card>.png.
    validateFoliageTextures(assets, [](const std::string& key) {
        auto path = key;
        std::replace(path.begin(), path.end(), '.', '/');
        return std::filesystem::exists(std::string(SOIL_CONTENT_DIR) +
                                       "/../materials/" + path + ".png");
    });
    test::expectThrows(
        [&] {
            validateFoliageTextures(assets,
                                    [](const std::string&) { return false; });
        },
        "missing foliage texture");
    CHECK(assets.getStorage<GroundCover>().items.size() >= 3);
    CHECK(assets.getStorage<Flora>().items.size() >= 2);
    // Every shipped cover is chosen somewhere for some soil, and temperate
    // humid loam gets grass.
    const auto& soils = assets.getStorage<Soil>();
    std::vector<bool> used(assets.getStorage<GroundCover>().items.size() + 1);
    for (const auto& soil : soils.items)
        for (float humidity : {.1f, .5f, .9f})
            for (float temperature : {-10.f, 15.f, 30.f})
                for (auto land : {LandType::PLAINS, LandType::PEAK}) {
                    auto r = plains();
                    r.humidity = humidity;
                    r.temperature_c = temperature;
                    r.land = land;
                    used[GroundCoverGenerator::select(r, soil.id, assets, 3)
                             .cover_id] = true;
                }
    for (size_t id = 1; id < used.size(); ++id) CHECK(used[id]);
    const auto loam = soils.get(std::string("soils.loam")).id;
    EQUAL(assets.getStorage<GroundCover>()
              .get(GroundCoverGenerator::select(plains(), loam, assets, 3)
                       .cover_id)
              .name,
          std::string("ground_covers.meadow_grass"), "temperate loam");
}
