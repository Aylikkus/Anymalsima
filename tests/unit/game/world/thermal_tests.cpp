#include <cmath>
#include <filesystem>

#include "items/thermal_item.hpp"
#include "test.hpp"
#include "world/local_temperature.hpp"
#include "world/save/save_catalog.hpp"
#include "world/thermal_source_tracker.hpp"
#include "world/thermal_sources.hpp"
#include "world/world.hpp"
#include "world/world_editor.hpp"
#include "world/world_weather.hpp"
#include "world_fixture.hpp"

namespace {

World thermalWorld(int width = 8, int height = 4, float meanC = 10.f,
                   int surfaceY = 64) {
    World world;
    world.width = width;
    world.height = height;
    world.min_temp_c = -40;
    world.max_temp_c = 50;
    world.regions.resize(size_t(width) * size_t(height));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            auto& region = world.regions[size_t(y) * width + size_t(x)];
            region.coord = {unsigned(x), unsigned(y)};
            region.temperature_c = meanC;
            region.land_height_m = surfaceY;
        }
    return world;
}

bool near(double actual, double expected, double tolerance = 1e-4) {
    return std::abs(actual - expected) <= tolerance;
}

ThermalEmission emission(float delta, float radius, float falloff = 1.f) {
    ThermalEmission value;
    value.deltaC = delta;
    value.radius = radius;
    value.falloff = falloff;
    return value;
}

struct TrackerFixture : test::SunlightFixture {
    ThermalSourceRegistry registry;
    // The tracker disconnects from the editor, so it must be destroyed first.
    WorldEditor editor{chunks, &sunlight, assets.getStorage<Block>()};
    ThermalSourceTracker tracker{chunks, assets.getStorage<Block>(), registry};

    TrackerFixture() { tracker.watch(editor); }
    ~TrackerFixture() { pool.wait(); }

    static TilePos at(int x, int y, int z) { return {x, y + BASE_Y, z}; }
    void place(int x, int y, int z, uint8_t block) {
        EQUAL(int(editor.setBlock(at(x, y, z), block)),
              int(EditStatus::Applied), "thermal block edit");
    }
};

}  // namespace

TEST(thermal_emission_defaults) {
    const auto defaults = Json::object().get<ThermalEmission>();
    EQUAL(defaults.deltaC, 10.f, "default delta");
    EQUAL(defaults.radius, 6.f, "default radius");
    EQUAL(defaults.falloff, 1.5f, "default falloff");
    CHECK(defaults.emits());
    CHECK(near(defaults.offsetAt(0), 10.0));
    CHECK(near(defaults.offsetAt(3), 10.0 * std::pow(.5, 1.5)));

    ThermalEmission disabled;
    disabled.enabled = false;
    CHECK(!disabled.emits());
    EQUAL(disabled.offsetAt(0), 0.f, "disabled emits nothing");
}

TEST(thermal_emission_rejects_invalid) {
    const auto throws = [](Json j) {
        try {
            (void)j.get<ThermalEmission>();
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    CHECK(throws({{"radius", 0}}));
    CHECK(throws({{"radius", 513}}));
    CHECK(throws({{"falloff", 0}}));
    CHECK(throws({{"delta_c", 501}}));
    CHECK(throws("not an object"));
    CHECK(!throws({{"delta_c", -15}, {"radius", 5}, {"falloff", 2}}));
}

TEST(thermal_block_parsing) {
    const auto block = Json{{"name", "blocks.heater"},
                            {"material", "materials.quartzite"},
                            {"thermal", {{"delta_c", 20}, {"radius", 6}}}}
                           .get<Block>();
    CHECK(block.thermal.has_value());
    EQUAL(block.thermal->deltaC, 20.f, "block delta");
    EQUAL(block.thermal->radius, 6.f, "block radius");

    const auto disabled =
        Json{{"name", "blocks.cold"},
             {"material", "materials.basalt"},
             {"thermal", {{"enabled", false}, {"delta_c", -15}}}}
            .get<Block>();
    CHECK(!disabled.thermal);
}

TEST(thermal_item_parsing) {
    const auto item =
        Json{{"name", "items.heater"},
             {"thermal", {{"delta_c", 20}, {"radius", 6}, {"falloff", 2}}}}
            .get<ThermalItem>();
    EQUAL(item.thermal.deltaC, 20.f, "item delta");
    EQUAL(item.thermal.radius, 6.f, "item radius");
    EQUAL(item.thermal.falloff, 2.f, "item falloff");

    bool threw = false;
    try {
        (void)Json{{"name", "items.broken"}}.get<ThermalItem>();
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(thermal_registry_lifecycle) {
    ThermalSourceRegistry registry;
    const TilePos tile{12, 40, -3};
    registry.setBlockThermal(tile, 8, emission(20.f, 8.f));
    EQUAL(registry.size(), size_t(1), "one block source");
    const auto* block = registry.blockThermal(tile);
    CHECK(block != nullptr);
    const auto blockKey = block->key;
    CHECK(blockKey != 0);
    CHECK(registry.containsKey(blockKey));
    EQUAL(registry.blockThermalsIn({10, 39, -4}, {15, 41, -2}).size(),
          size_t(1), "block source in bounds");
    EQUAL(registry.blockThermalsIn({13, 39, -4}, {15, 41, -2}).size(),
          size_t(0), "block source outside bounds");

    registry.dirty = false;
    ThermalSource item;
    item.owner = ThermalSource::Owner::Item;
    item.item = 2;
    item.emission = emission(-15.f, 5.f);
    item.position = {12.5, 40.5, -2.5};
    const auto id = registry.add(item);
    EQUAL(registry.itemCount(), size_t(1), "one item source");
    CHECK(registry.dirty);
    CHECK(registry.containsKey(registry.find(id)->key));
    registry.place(id, {20.5, 41.5, 4.5});
    CHECK(near(registry.find(id)->position[0], 20.5));
    registry.remove(id);
    EQUAL(registry.itemCount(), size_t(0), "item removed");
    EQUAL(registry.size(), size_t(1), "block source remains");
    registry.removeBlockThermal(tile);
    EQUAL(registry.size(), size_t(0), "block removed");
    CHECK(!registry.containsKey(blockKey));
}

TEST(thermal_local_temperature_attenuation) {
    const auto world = thermalWorld();
    ThermalSourceRegistry registry;
    ThermalSource heater;
    heater.owner = ThermalSource::Owner::Item;
    heater.item = 1;
    heater.emission = emission(20.f, 10.f);
    heater.position = {10.5, 30.5, 10.5};
    const auto heaterId = registry.add(heater);

    thermal::Query query(world, registry);
    const std::array<double, 3> centre{10.5, 30.5, 10.5};
    const auto base = query.at(centre, 8.25);
    CHECK(near(base.temperatureC, base.ambientC + 20.0));
    EQUAL(base.detailedSources, size_t(1), "one source at the centre");
    const auto half = query.at({15.5, 30.5, 10.5}, 8.25);
    CHECK(near(half.temperatureC, half.ambientC + 10.0));
    const auto edge = query.at({20.5, 30.5, 10.5}, 8.25);
    CHECK(near(edge.temperatureC, edge.ambientC));
    EQUAL(edge.detailedSources, size_t(0), "outside the range");

    ThermalSource cooler;
    cooler.owner = ThermalSource::Owner::Item;
    cooler.item = 2;
    cooler.emission = emission(-10.f, 10.f);
    cooler.position = heater.position;
    const auto coolerId = registry.add(cooler);
    const auto overlap = query.at(centre, 8.25);
    CHECK(near(overlap.temperatureC, overlap.ambientC + 10.0));
    EQUAL(overlap.detailedSources, size_t(2), "overlapping sources add");

    registry.remove(heaterId);
    const auto cold = query.at(centre, 8.25);
    CHECK(near(cold.temperatureC, cold.ambientC - 10.0));
    registry.remove(coolerId);
    EQUAL(query.at(centre, 8.25).detailedSources, size_t(0), "all removed");
}

TEST(thermal_barrier_and_opening) {
    TrackerFixture f;
    f.load({0, 0, 0});
    std::vector<EditRequest> wall;
    for (int y = 0; y < 16; ++y)
        for (int z = 0; z < 16; ++z)
            wall.push_back({TrackerFixture::at(6, y, z), EditMode::Place, 1});
    f.editor.apply(wall);

    ThermalSourceRegistry registry;
    ThermalSource source;
    source.owner = ThermalSource::Owner::Item;
    source.item = 1;
    source.emission = emission(20.f, 20.f);
    source.position = {2.5, TrackerFixture::BASE_Y + 8.5, 8.5};
    registry.add(source);
    thermal::Query query(f.world, registry, thermal::voxelBarrier(f.chunks));

    const auto front = query.at({5.5, TrackerFixture::BASE_Y + 8.5, 8.5}, 8.25);
    const auto behind =
        query.at({10.5, TrackerFixture::BASE_Y + 8.5, 8.5}, 8.25);
    CHECK(front.detailedC > 10.f);
    CHECK(behind.detailedC < .01f);

    // Remove one cell from the wall to make a doorway on the sampled ray.
    EQUAL(int(f.editor.setBlock(TrackerFixture::at(6, 8, 8), 0)),
          int(EditStatus::Applied), "doorway");
    const auto open = query.at({10.5, TrackerFixture::BASE_Y + 8.5, 8.5}, 8.25);
    CHECK(open.detailedC > 10.f);
}

TEST(thermal_regional_dated_handover) {
    const auto world = thermalWorld();
    ThermalSourceRegistry registry;
    ThermalSource local;
    local.owner = ThermalSource::Owner::Item;
    local.item = 1;
    local.emission = emission(12.f, 8.f);
    local.position = {20.5, 30.5, 20.5};
    const auto id = registry.add(local);
    const auto key = registry.find(id)->key;

    thermal::Query query(world, registry);
    const std::array<double, 3> target{24.5, 30.5, 20.5};
    thermal::RegionalHeat regional;
    regional.key = key;
    regional.position = local.position;
    regional.deltaC = 12.f;
    regional.radius = 8.f;
    regional.falloff = 1.f;
    regional.startDay = 5;
    regional.endDay = 15;
    query.addRegional(regional);

    const auto detailed = query.at(target, 10.0);
    CHECK(near(detailed.detailedC, 6.0));
    EQUAL(detailed.regionalC, 0.f, "a local source owns the handover");
    EQUAL(detailed.regionalSources, size_t(0), "regional skipped");
    query.removeRegional(key);

    registry.remove(id);
    query.addRegional(regional);
    const auto coarse = query.at(target, 10.0);
    EQUAL(coarse.detailedC, 0.f, "detailed source removed");
    CHECK(near(coarse.regionalC, 6.0));
    CHECK(near(coarse.temperatureC, coarse.ambientC + 6.0));
    CHECK(near(coarse.temperatureC, detailed.temperatureC));
    EQUAL(query.at(target, 4.0).regionalSources, size_t(0),
          "dated entry is inactive before its start");
    EQUAL(query.at(target, 16.0).regionalSources, size_t(0),
          "dated entry is inactive after its end");
}

TEST(thermal_x_wrap) {
    const auto world = thermalWorld(2, 1);
    ThermalSourceRegistry registry;
    ThermalSource source;
    source.owner = ThermalSource::Owner::Item;
    source.item = 1;
    source.emission = emission(10.f, 20.f);
    source.position = {1.5, 20.5, 100.5};
    registry.add(source);
    thermal::Query query(world, registry);
    const auto wrapped = query.at({254.5, 20.5, 100.5}, 8.25);
    const double expected = 10.0 * (1.0 - 3.0 / 20.0);
    CHECK(near(wrapped.detailedC, expected, .01));
    const auto far = query.at({100.5, 20.5, 100.5}, 8.25);
    EQUAL(far.detailedSources, size_t(0), "distant X does not contribute");
}

TEST(thermal_tracker_block_lifecycle) {
    TrackerFixture f;
    f.load({0, 0, 0});
    f.place(8, 8, 8, 8);
    const auto tile = TrackerFixture::at(8, 8, 8);
    const auto* heater = f.registry.blockThermal(tile);
    CHECK(heater != nullptr);
    EQUAL(int(heater->block), 8, "heater block");
    CHECK(heater->emission.deltaC > 0);

    f.place(8, 8, 8, 9);
    const auto* cooler = f.registry.blockThermal(tile);
    CHECK(cooler != nullptr);
    EQUAL(int(cooler->block), 9, "replaced by the cooler");
    CHECK(cooler->emission.deltaC < 0);

    f.place(8, 8, 8, 0);
    CHECK(f.registry.blockThermal(tile) == nullptr);
    EQUAL(f.registry.size(), size_t(0), "no residual block source");

    f.place(9, 8, 8, 8);
    EQUAL(f.registry.size(), size_t(1), "a second tile registers");
    f.chunks.unregister(TrackerFixture::coord({0, 0, 0}));
    EQUAL(f.registry.size(), size_t(0), "unloading the chunk removes it");
}

TEST(thermal_carried_source_save_roundtrip) {
    ThermalSourceRegistry registry;
    ThermalSource source;
    source.owner = ThermalSource::Owner::Item;
    source.item = 1;
    source.emission = emission(20.f, 6.f);
    source.position = {12.5, 30.5, 12.5};
    const auto id = registry.add(source);
    registry.place(id, {13.5, 30.5, 12.5});

    std::vector<thermal_sources_codec::Record> records;
    registry.forEachItem(
        [&](ThermalSourceRegistry::Id sourceId, const ThermalSource& value) {
            records.push_back(
                {{value.position[0], value.position[1], value.position[2]},
                 "items.heater",
                 value.key,
                 sourceId == id});
        });
    const auto root =
        std::filesystem::path(TEST_SCRATCH_DIR) / "thermal_sources";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const SaveSlot slot{root, {}};
    save_catalog::writeThermalSources(slot, records);
    const auto file = save_catalog::readThermalSources(slot);
    EQUAL(file.records.size(), size_t(1), "one saved item");
    CHECK(file.records[0] == records[0]);
    CHECK(file.records[0].carried);

    // Reloading into a fresh registry leaves exactly one source.
    ThermalSourceRegistry loaded;
    ThermalSource restored;
    restored.owner = ThermalSource::Owner::Item;
    restored.item = 1;
    restored.emission = emission(20.f, 6.f);
    restored.position = file.records[0].position;
    restored.key = file.records[0].key;
    const auto loadedId = loaded.add(restored);
    EQUAL(loaded.size(), size_t(1), "exactly one restored item");
    CHECK(loaded.find(loadedId) != nullptr);
    registry.remove(id);
    EQUAL(registry.itemCount(), size_t(0), "removed source leaves no residue");

    // The missing-file path is empty, and malformed input is rejected.
    const auto emptyRoot = root / "empty";
    std::filesystem::create_directories(emptyRoot);
    CHECK(save_catalog::readThermalSources({emptyRoot, {}}).records.empty());
    std::filesystem::remove_all(root);
}
