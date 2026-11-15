#include <fstream>

#include "items/soil_validation.hpp"
#include "world/soil_generator.hpp"
#include "world_fixture.hpp"

namespace {
class SoilAssets final : public IAssets {
   public:
    SoilAssets() {
        registerDataType<Soil>();
        registerDataType<Block>();
        registerDataType<Material>();
    }
    template <typename T>
    void load(const Json& j) {
        loaders.at(T::TypeName)(*this, j);
    }
    Vec2u getTextureSize(const std::string&) override { return {16, 16}; }
    uint16_t getMaterialAlbedoLayer(const std::string&) const override {
        return 0;
    }
    uint16_t getMaterialNormalLayer(const std::string&) const override {
        return 0;
    }
    bgfx::TextureHandle getMaterialAlbedoArray() const override {
        return BGFX_INVALID_HANDLE;
    }
    bgfx::TextureHandle getMaterialNormalArray() const override {
        return BGFX_INVALID_HANDLE;
    }
};
Json soilJson(std::string name = "soils.loam") {
    return Json{{"name", name},
                {"block_name", "blocks.loam"},
                {"topsoil_block_name", "blocks.loam_topsoil"},
                {"properties",
                 {{"porosity", .4},
                  {"permeability", .5},
                  {"water_retention", .6},
                  {"fertility", .7},
                  {"organic_content", .02}}},
                {"placement", Json::array({Json::object()})}};
}
template <typename F>
void rejects(F fn) {
    bool threw = false;
    try {
        fn();
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}
Region soilRegion(int height = SEA_LEVEL_Y + 16) {
    auto r = test::regionAt(height);
    r.land = LandType::PLAINS;
    r.vegetation = VegetationType::GRASS;
    r.humidity = .5;
    r.temperature_c = 15;
    return r;
}
SoilProfile profile() { return {1, 2, 3, {4, 4}, {1, 1}, 42}; }
void loadBlocks(SoilAssets& a) {
    a.load<Material>(Json::array({{{"name", "materials.loam"},
                                   {"texture_albedo", "loam"},
                                   {"texture_normal", "normal"}}}));
    a.load<Block>(Json::array({{{"name", "blocks.loam"},
                                {"material", "materials.loam"},
                                {"render_category", "Terrain Soil"}},
                               {{"name", "blocks.loam_topsoil"},
                                {"material", "materials.loam"},
                                {"render_category", "Terrain Soil"}}}));
}
}  // namespace

TEST(soil_data_validation) {
    auto valid = soilJson();
    const auto soil = valid.get<Soil>();
    CHECK(soil.depth_m == (std::array<int, 2>{1, 4}));
    CHECK(soil.propertiesFor(true).fertility == soil.properties.fertility);
    for (const auto& key : {"porosity", "permeability", "water_retention",
                            "fertility", "organic_content"}) {
        auto bad = valid;
        bad["properties"][key] = 1.1;
        rejects([&] { bad.get<Soil>(); });
        bad["properties"][key] = "bad";
        rejects([&] { bad.get<Soil>(); });
        bad["properties"].erase(key);
        rejects([&] { bad.get<Soil>(); });
    }
    for (const auto& bounds :
         {Json::array({4, 1}), Json::array({-1, 2}), Json::array({1.5, 2}),
          Json::array({1}), Json::array({1, 1e30})}) {
        auto bad = valid;
        bad["depth_m"] = bounds;
        rejects([&] { bad.get<Soil>(); });
    }
    for (const auto& invalid : {Json::array(), Json::object(), Json("bad")}) {
        auto bad = valid;
        bad["placement"] = invalid;
        rejects([&] { bad.get<Soil>(); });
    }
    for (auto rule : {Json{{"weight", 0}}, Json{{"humidity_range", {-1, 1}}},
                      Json{{"allowed_land", {"PLAINS"}}},
                      Json{{"required_features", "Cliff"}}}) {
        auto bad = valid;
        bad["placement"] = Json::array({rule});
        rejects([&] { bad.get<Soil>(); });
    }
}

TEST(soil_loading_references_capacity) {
    SoilAssets assets;
    auto data = Json::array({soilJson()});
    assets.load<Soil>(
        data);  // Forward block references are permitted at parse time.
    loadBlocks(assets);
    validateSoilReferences(assets);
    rejects([&] { assets.load<Soil>(data); });
    auto missing = soilJson("soils.missing");
    missing["block_name"] = "blocks.missing";
    assets.load<Soil>(Json::array({missing}));
    rejects([&] { validateSoilReferences(assets); });
    SoilAssets capacity;
    for (int i = 0; i < 255; ++i)
        capacity.load<Soil>(
            Json::array({soilJson("soil" + std::to_string(i))}));
    EQUAL(int(capacity.getStorage<Soil>().items.back().id), 255,
          "last valid ID");
    rejects([&] { capacity.load<Soil>(Json::array({soilJson("overflow")})); });
    EQUAL(capacity.getStorage<Soil>().items.size(), 255u,
          "overflow leaves storage intact");
}

TEST(soil_placement_priority) {
    SoilAssets assets;
    loadBlocks(assets);
    auto ordinary = soilJson("soils.ordinary");
    auto special = soilJson("soils.special");
    special["placement"] = Json::array(
        {{{"priority", 10}, {"required_features", {"Floodplain"}}}});
    assets.load<Soil>(Json::array({ordinary, special}));
    auto r = soilRegion();
    EQUAL(int(SoilGenerator::select(r, assets, 42).soil_id), 1,
          "ordinary terrain");
    r.addFeature(FeatureMask::FLOODPLAIN);
    EQUAL(int(SoilGenerator::select(r, assets, 42).soil_id), 2, "deposit wins");
    r.land_height_m = SEA_LEVEL_Y - 1;
    CHECK(SoilGenerator::select(r, assets, 42).block_id == 0);
    SoilPlacement p{};
    p.allow_underwater = true;
    CHECK(SoilGenerator::allowed(p, r));
    p.forbidden_features = {FeatureMask::FLOODPLAIN};
    CHECK(!SoilGenerator::allowed(p, r));
    p.forbidden_features.clear();
    p.humidity_range = {.6, 1};
    CHECK(!SoilGenerator::allowed(p, r));
    p.humidity_range = {0, 1};
    p.temperature_range_c = {-5, 0};
    CHECK(!SoilGenerator::allowed(p, r));
}

TEST(soil_selection_order_topsoil) {
    SoilAssets a, b;
    loadBlocks(a);
    loadBlocks(b);
    const auto one = soilJson("soils.a"), two = soilJson("soils.b");
    a.load<Soil>(Json::array({one, two}));
    b.load<Soil>(Json::array({two, one}));
    for (unsigned x = 0; x < 30; ++x) {
        auto r = soilRegion();
        r.coord = {x, 5};
        auto first = SoilGenerator::select(r, a, 77);
        auto second = SoilGenerator::select(r, b, 77);
        EQUAL(a.getStorage<Soil>().get(first.soil_id).name,
              b.getStorage<Soil>().get(second.soil_id).name, "file order");
        CHECK(first.topsoil_block_id != 0);
        r.vegetation = VegetationType::NONE;
        CHECK(SoilGenerator::select(r, a, 77).topsoil_block_id == 0);
    }
    auto wet = soilJson("soils.wet");
    wet["placement"][0]["allow_underwater"] = true;
    SoilAssets underwater;
    loadBlocks(underwater);
    underwater.load<Soil>(Json::array({wet}));
    CHECK(SoilGenerator::select(soilRegion(SEA_LEVEL_Y - 2), underwater, 42)
              .topsoil_block_id == 0);
}

TEST(soil_horizons_exposure) {
    auto r = soilRegion();
    r.addFeature(
        FeatureMask::DESERT_DUNES);  // Exclude outcrops for exact boundaries.
    auto p = profile();
    auto column = SoilGenerator::sample(p, r, 5, 7);
    EQUAL(column.count, 2, "two horizons");
    EQUAL(column.layers[0].thickness_m, 1, "topsoil");
    EQUAL(column.layers[1].depth_from_surface_m, 1, "subsoil start");
    EQUAL(column.layers[1].thickness_m, 3, "subsoil thickness");
    CHECK(column.layers[0].type == RegionLayerType::Soil);
    CHECK(column.layers[0].topsoil);
    p.depth_m = {1, 1};
    column = SoilGenerator::sample(p, r, 5, 7);
    CHECK(column.count == 1 && column.layers[0].topsoil);
    p.topsoil_block_id = 0;
    column = SoilGenerator::sample(p, r, 5, 7);
    CHECK(column.count == 1 && !column.layers[0].topsoil);
    CHECK(SoilGenerator::sample({}, r, 5, 7).count == 0);
    p.depth_m = {0, 0};
    CHECK(SoilGenerator::sample(p, r, 5, 7).count == 0);
    r.removeFeature(FeatureMask::DESERT_DUNES);
    int bare = 0, covered = 0;
    p = profile();
    r.land = LandType::MOUNTAINS;
    for (int x = -64; x < 128; ++x) {
        column = SoilGenerator::sample(p, r, x, 9);
        if (column.depth_m == 0)
            ++bare;
        else
            ++covered;
        CHECK(column.depth_m <= 2);
    }
    CHECK(bare > 0 && covered > 0);
}

TEST(soil_generation_seams_geology) {
    auto r = soilRegion(SEA_LEVEL_Y + 16);
    auto p = profile();
    LocationGenerator gen;
    const std::vector<RegionLayer> rocks = {
        {RegionLayerType::Rock, 1, 2, 0, .8f, 1},
        {RegionLayerType::Rock, 1, 1000, 2, .4f, 1}};
    // Generate upper chunks first, then lower chunks, then repeat the upper.
    for (unsigned cx : {0u, 1u, 7u, 8u}) {
        Chunk upper, lower, repeat;
        gen.generateChunk(upper, {cx, 25, 0}, {}, r, rocks, p);
        gen.generateChunk(lower, {cx, 24, 0}, {}, r, rocks, p);
        gen.generateChunk(repeat, {cx, 25, 0}, {}, r, rocks, p);
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int x = 0; x < Chunk::SIZE_X; ++x) {
                const auto cover = SoilGenerator::sample(
                    p, r, int64_t(cx) * Chunk::SIZE_X + x, z);
                for (int y = 0; y < Chunk::SIZE_Y; ++y) {
                    const int depth = 16 - y;
                    int expected = 1;
                    if (depth < cover.depth_m) expected = depth == 0 ? 3 : 2;
                    EQUAL(int(lower.tiles[z][y][x].block_id), expected,
                          "lower slab profile and preserved rock");
                    EQUAL(int(upper.tiles[z][y][x].block_id),
                          int(repeat.tiles[z][y][x].block_id), "repeat");
                }
                EQUAL(int(upper.tiles[z][0][x].block_id), cover.count ? 3 : 1,
                      "surface on slab boundary");
            }
    }
    CHECK(rocks[0].aquifer_saturation == .8f);
    CHECK(rocks[1].depth_from_surface_m == 2);
}

TEST(soil_runtime_content) {
    SoilAssets assets;
    for (const auto& type : {"soils", "blocks", "materials"}) {
        std::ifstream input(std::string(SOIL_CONTENT_DIR) + "/" + type +
                            "/soils.json");
        CHECK(input.good());
        const auto data = Json::parse(input);
        if (std::string(type) == "soils") assets.load<Soil>(data);
        if (std::string(type) == "blocks") assets.load<Block>(data);
        if (std::string(type) == "materials") assets.load<Material>(data);
    }
    validateSoilReferences(assets);
    EQUAL(assets.getStorage<Soil>().items.size(), 14u, "profiles");
    EQUAL(assets.getStorage<Block>().items.size(), 26u, "blocks");
    EQUAL(assets.getStorage<Material>().items.size(), 26u, "materials");
}

TEST(soil_streaming_profile) {
    SoilAssets assets;
    loadBlocks(assets);
    assets.registerDataType<Rock>();
    assets.load<Rock>(Json::array(
        {{{"name", "rocks.test"},
          {"block_name", "blocks.loam"},
          {"category", "Igneous Intrusive"},
          {"body_size_m", {128, 128}},
          {"properties", {{"porosity", .1}, {"permeability", .1}}}}}));
    auto soil = soilJson();
    soil["depth_m"] = {4, 4};
    assets.load<Soil>(Json::array({soil}));
    World world{};
    world.genSettings.feature_seed = 42;
    auto region = soilRegion(SEA_LEVEL_Y + 16);
    region.addFeature(FeatureMask::DESERT_DUNES);
    LocationGenerator generator;
    ThreadPool pool(2);
    ChunkMesher mesher(assets);
    ChunkManager chunks(generator, pool, &mesher, world, &assets);
    const ChunkCoord coord{0, 25, 0};
    const auto generate = [&] {
        chunks.registerChunk(coord, {}, &region);
        chunks.request(coord);
        chunks.update();
        CHECK(pool.wait_for(std::chrono::seconds(3)));
        chunks.update();
        auto data = chunks.getChunkShared(coord);
        CHECK(data != nullptr);
        return data;
    };
    auto first = generate();
    EQUAL(int(first->tiles[0][0][0].block_id), 2, "worker topsoil block");
    chunks.unregister(coord);
    auto again = generate();
    EQUAL(int(again->tiles[0][0][0].block_id), 2, "reloaded topsoil block");
    CHECK(first != again);
    CHECK(pool.wait_for(std::chrono::seconds(3)));
}
