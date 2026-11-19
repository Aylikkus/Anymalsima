#include <filesystem>

#include "flora_assets.hpp"
#include "items/flora_validation.hpp"

// Tree species data (docs/handoff/trees.md, docs/modding/trees.md).

namespace {
Json oakJson() {
    return Json::parse(R"({
        "name": "trees.oak",
        "form": "decurrent",
        "size": { "height_m": [15, 26], "trunk_radius_m": [1.2, 2.6],
                  "crown_radius_m": [6, 10] },
        "growth": { "maturity_days": [900, 1400],
                    "lifespan_days": [100000, 200000], "curve": 1.8 },
        "form_params": { "branch_spacing_m": [3, 5], "branch_angle_deg": 45,
                         "branches_per_node": [2, 3], "crown_base": 0.3,
                         "droop": 0.1, "taper": 0.7, "gnarl": 0.35,
                         "root_flare": 0.5, "branch_orders": 3 },
        "wood": { "block": "blocks.oak_wood", "bark": "foliage.bark" },
        "foliage": { "type": "leaf_clump", "material": "foliage.leaf",
                     "clump_size_m": [1.4, 2.2], "density": 1.2 },
        "placement": {
            "climate": [ { "allowed_land": ["Plains"] } ],
            "vegetation": ["Forest"],
            "density_per_100m2": [0.4, 1.0],
            "spacing_m": [7, 10],
            "priority": 15
        },
        "cut": { "stump_resprouts": true, "resprout_days": [20, 40] }
    })");
}

// Minimal assets the oak refers to.
void loadOakReferences(test::FloraAssets& a, bool naturalGrowth = true) {
    a.load<Material>(Json::array({{{"name", "materials.oak_wood"},
                                   {"texture_albedo", "oak"},
                                   {"texture_normal", "oak_normal"}}}));
    a.load<Block>(
        Json::array({{{"name", "blocks.oak_wood"},
                      {"material", "materials.oak_wood"},
                      {"render_category", "Vegetation"},
                      {"properties", {{"natural_growth", naturalGrowth}}}}}));
    a.load<FoliageMaterial>(Json::array(
        {{{"name", "foliage.leaf"}, {"albedo", "foliage.leaf_broad"}},
         {{"name", "foliage.bark"}, {"albedo", "foliage.blade"}}}));
}
}  // namespace

TEST(tree_species_parsing) {
    const auto oak = oakJson().get<Tree>();
    CHECK(oak.form == TreeForm::Decurrent);
    CHECK(oak.size.trunk_radius_m == ValueRange(1.2f, 2.6f));
    CHECK(oak.sizeClass() == TreeSizeClass::Medium);
    CHECK(!oak.landmark());
    EQUAL(oak.form_params.branch_orders, 3, "branch orders");
    CHECK(oak.foliage.type == TreeFoliageType::LeafClump);
    CHECK(oak.cut.stump_resprouts);
    CHECK(oak.cut.fell == TreeFell::Fall);  // the default
    // Spacing lives in placement next to the flora fields.
    CHECK(oak.spacing_m == ValueRange(7, 10));
    EQUAL(oak.placement.priority, 15, "placement priority");
    EQUAL(oak.placement.vegetation.size(), size_t(1), "vegetation list");
    // Extent: the crown, or flared roots when wider.
    EQUAL(oak.extent(), 10.f, "crown extent");
    EQUAL(oak.reach(), 20.f, "crown plus spacing");

    const auto rejects = [](const std::string& what, auto edit) {
        auto bad = oakJson();
        edit(bad);
        test::expectThrows([&] { bad.get<Tree>(); }, "rejects " + what);
    };
    rejects("unknown form", [](Json& j) { j["form"] = "bonsai"; });
    rejects("unknown fell", [](Json& j) { j["cut"]["fell"] = "explode"; });
    {
        auto settling = oakJson();
        settling["cut"]["fell"] = "settle";
        CHECK(settling.get<Tree>().cut.fell == TreeFell::Settle);
    }
    rejects("missing size", [](Json& j) { j.erase("size"); });
    rejects("missing trunk",
            [](Json& j) { j["size"].erase("trunk_radius_m"); });
    rejects("too tall",
            [](Json& j) { j["size"]["height_m"] = TreeMaxHeight + 1; });
    rejects("trunk wider than 64",
            [](Json& j) { j["size"]["trunk_radius_m"] = {32, 65}; });
    rejects("reversed range",
            [](Json& j) { j["size"]["height_m"] = {26, 15}; });
    rejects("dies before maturity",
            [](Json& j) { j["growth"]["lifespan_days"] = 1000; });
    rejects("branch orders",
            [](Json& j) { j["form_params"]["branch_orders"] = 5; });
    rejects("foliage type", [](Json& j) { j["foliage"]["type"] = "flowers"; });
    rejects("missing wood", [](Json& j) { j.erase("wood"); });
    rejects("companions", [](Json& j) {
        j["placement"]["companions"] =
            Json::array({{{"species", "flora.fern"}}});
    });
    rejects("vegetation", [](Json& j) {
        j["placement"]["vegetation"] = Json::array({"Jungle"});
    });
    // An ordinary tree must stay within the generation halo.
    rejects("reach of an ordinary tree",
            [](Json& j) { j["size"]["crown_radius_m"] = 90; });
    rejects("typo", [](Json& j) { j["form_param"] = Json::object(); });

    // Massive trees (landmarks) may reach further, up to their own limit.
    auto giant = oakJson();
    giant["form"] = "giant";
    giant["size"] = {{"height_m", {100, 120}},
                     {"trunk_radius_m", {28, 64}},
                     {"crown_radius_m", {120, 180}}};
    giant["placement"]["spacing_m"] = {1500, 2500};
    const auto elder = giant.get<Tree>();
    CHECK(elder.landmark());
    CHECK(elder.sizeClass() == TreeSizeClass::Massive);
    giant["size"]["crown_radius_m"] = TreeMaxLandmarkReach + 1;
    test::expectThrows([&] { giant.get<Tree>(); }, "landmark extent");
}

TEST(tree_size_classes) {
    CHECK(treeSizeClass(0.3f) == TreeSizeClass::Thin);
    CHECK(treeSizeClass(TreeVoxelRadius) == TreeSizeClass::Small);
    CHECK(treeSizeClass(1) == TreeSizeClass::Small);
    CHECK(treeSizeClass(3) == TreeSizeClass::Medium);
    CHECK(treeSizeClass(8) == TreeSizeClass::Large);
    CHECK(treeSizeClass(15) == TreeSizeClass::Large);
    CHECK(treeSizeClass(16) == TreeSizeClass::Massive);
    CHECK(treeSizeClass(64) == TreeSizeClass::Massive);
}

TEST(tree_references_validated) {
    {
        test::FloraAssets assets;
        loadOakReferences(assets);
        assets.load<Tree>(Json::array({oakJson()}));
        validateTreeReferences(assets);
        const auto grown =
            naturalGrowthBlocks(assets.getStorage<Block>().items);
        EQUAL(grown.count(), size_t(1), "one natural growth block");
        CHECK(grown.test(assets.getStorage<Block>().items[0].id));
    }
    // Wood that players could place (no natural_growth) is refused: edit
    // detection would hide it.
    {
        test::FloraAssets assets;
        loadOakReferences(assets, false);
        assets.load<Tree>(Json::array({oakJson()}));
        test::expectThrows([&] { validateTreeReferences(assets); },
                           "wood without natural_growth");
    }
    for (const auto* key : {"block", "bark"}) {
        test::FloraAssets assets;
        loadOakReferences(assets);
        auto tree = oakJson();
        tree["wood"][key] = std::string("missing.") + key;
        assets.load<Tree>(Json::array({tree}));
        test::expectThrows([&] { validateTreeReferences(assets); },
                           std::string("unknown wood ") + key);
    }
    {
        test::FloraAssets assets;
        loadOakReferences(assets);
        auto tree = oakJson();
        tree["foliage"]["material"] = "foliage.missing";
        assets.load<Tree>(Json::array({tree}));
        test::expectThrows([&] { validateTreeReferences(assets); },
                           "unknown foliage material");
    }
}

TEST(tree_runtime_content) {
    test::FloraAssets assets;
    test::loadAllContent(assets);
    validateFloraReferences(assets);
    validateTreeReferences(assets);
    validateFoliageTextures(assets, [](const std::string& key) {
        auto path = key;
        std::replace(path.begin(), path.end(), '.', '/');
        return std::filesystem::exists(std::string(SOIL_CONTENT_DIR) +
                                       "/../materials/" + path + ".png");
    });
    // Wood textures are terrain materials (blocks.wood.<name>).
    for (const auto& material : assets.getStorage<Material>().items) {
        if (!material.name.ends_with("_wood")) continue;
        for (const auto* key :
             {&material.texture_albedo, &material.texture_normal}) {
            auto path = *key;
            std::replace(path.begin(), path.end(), '.', '/');
            CHECK(std::filesystem::exists(std::string(SOIL_CONTENT_DIR) +
                                          "/../materials/" + path + ".png"));
        }
    }
    // The shipped species range from thin, mesh-only trees (mountain pine,
    // palm, oak) through voxel trunks to the massive landmark.
    std::array<int, 5> classes{};
    for (const auto& tree : assets.getStorage<Tree>().items)
        ++classes[size_t(tree.sizeClass())];
    CHECK(classes[size_t(TreeSizeClass::Thin)] > 0);
    CHECK(classes[size_t(TreeSizeClass::Small)] +
              classes[size_t(TreeSizeClass::Medium)] +
              classes[size_t(TreeSizeClass::Large)] >
          0);
    CHECK(classes[size_t(TreeSizeClass::Massive)] > 0);
    // Every tree wood block is natural growth, and nothing else is.
    const auto grown = naturalGrowthBlocks(assets.getStorage<Block>().items);
    EQUAL(grown.count(), assets.getStorage<Tree>().items.size(),
          "one living wood per species");
    for (const auto& block : assets.getStorage<Block>().items)
        CHECK(grown.test(block.id) == block.name.ends_with("_wood"));
}
