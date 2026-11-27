#include <cmath>
#include <map>
#include <optional>
#include <utility>

#include "test.hpp"
#include "world/reservoirs.hpp"
#include "world/surface_ice.hpp"
#include "world/world.hpp"

namespace {

struct FakeTerrain final : surface_ice::Terrain {
    std::map<std::pair<int, int>, surface_ice::Column> columns;
    int max = 420;

    std::optional<surface_ice::Column> column(int x, int z) const override {
        const auto it = columns.find({x, z});
        return it == columns.end() ? std::nullopt : std::optional(it->second);
    }
    std::optional<tile_geometry::CellGeometry> cell(
        TilePos pos) const override {
        if (pos.y < 0 || pos.y >= max) return std::nullopt;
        return tile_geometry::CellGeometry{};
    }
    bool inBounds(int x, int z) const override {
        return x >= 0 && z >= 0 && x < 32 && z < 32;
    }
    int maxY() const override { return max; }
};

WaterSurface surface(uint32_t body, int top, uint8_t fill = 255,
                     uint32_t reach = 0) {
    return WaterSurface{top, body, reach, {}, body == 0 && reach == 0, fill};
}

void freezeStep(Reservoirs& water, surface_ice::Simulation& simulation,
                FakeTerrain& terrain, double days, float temperature) {
    simulation.update(
        water, terrain,
        [temperature](int, int, double, double) { return temperature; }, 10.,
        days, 8, 8, 8);
}

}  // namespace

TEST(surface_ice_stage_progression_and_balance) {
    constexpr uint32_t body = 42;
    Reservoirs water(nullptr);
    water.define(body, StorageCurve::constantArea(1, 400, 410));
    const auto capacity = water.volume(body);
    FakeTerrain terrain;
    terrain.columns[{1, 1}] = {400, surface(body, 410)};
    surface_ice::Simulation simulation;

    freezeStep(water, simulation, terrain, .1, -10.f);
    const auto* early = water.surfaceIceAt(1, 1);
    CHECK((!early || early->frozen == 0));
    freezeStep(water, simulation, terrain, .1, -10.f);
    const auto* state = water.surfaceIceAt(1, 1);
    CHECK(state);
    EQUAL(state->stage, uint8_t(1), "the first lid is the 5 cm stage");
    CHECK(std::abs(state->thickness - .05f) < 1e-5f);
    CHECK(std::abs(state->waterline - 410.f) < 1e-5f);
    EQUAL(state->frozen, surface_ice::waterUnitsForMetres(.05),
          "ice is one 5 cm water-equivalent layer");
    EQUAL(water.volume(body), capacity - state->frozen,
          "the body is debited exactly once");
    EQUAL(water.liquidTotal() + water.frozenTotal(), capacity,
          "freeze conserves water equivalent");

    // Prolonged cold grows one stage at a time; the sub-stage remainder is
    // retained rather than reset.
    freezeStep(water, simulation, terrain, .1, -10.f);
    freezeStep(water, simulation, terrain, .1, -10.f);
    const auto* grown = water.surfaceIceAt(1, 1);
    CHECK(grown);
    CHECK(grown->thickness >= .10f - 1e-5f);
    EQUAL(water.liquidTotal() + water.frozenTotal(), capacity,
          "later stages keep the balance sheet closed");
}

TEST(surface_ice_shallow_bank_freezes_through_first) {
    Reservoirs water(nullptr);
    const TilePos shallowCell{2, 400, 2};
    const TilePos deepCell{3, 400, 2};
    EQUAL(water.creditLocal(shallowCell, 5, ReservoirExchange::Input), 5,
          "a two-centimetre bank");
    EQUAL(water.creditLocal(deepCell, 255, ReservoirExchange::Input), 255,
          "a one-metre pool");
    FakeTerrain terrain;
    terrain.columns[{2, 2}] = {399, surface(0, 401, 5)};
    terrain.columns[{3, 2}] = {399, surface(0, 401, 255)};
    surface_ice::Simulation simulation;

    freezeStep(water, simulation, terrain, .1, -10.f);
    const auto* shallow = water.surfaceIceAt(2, 2);
    CHECK(shallow);
    CHECK(shallow->through);
    CHECK(shallow->thickness < .05f);
    {
        const auto* deepEarly = water.surfaceIceAt(3, 2);
        CHECK((!deepEarly || deepEarly->frozen == 0));
    }

    // The deep pool eventually forms the 5 cm lid while the bank stays
    // frozen through at its own depth.
    freezeStep(water, simulation, terrain, .1, -10.f);
    freezeStep(water, simulation, terrain, .1, -10.f);
    const auto* deep = water.surfaceIceAt(3, 2);
    CHECK(deep);
    CHECK(!deep->through);
    CHECK(deep->thickness >= .05f - 1e-5f);

    // A frozen-through bank has no liquid surface left to sample. Its saved
    // column state still thaws back into the original cell.
    surface_ice::Simulation thawing;
    thawing.update(
        water, terrain, [](int, int, double, double) { return 10.f; }, 2., 2.,
        8, 8, 8);
    const auto* thawed = water.surfaceIceAt(2, 2);
    CHECK(!thawed || thawed->frozen == 0);
    CHECK(water.snapshot()->cells.fill(shallowCell) > 0);
}

TEST(surface_ice_thaw_restores_liquid_and_substage_progress) {
    constexpr uint32_t body = 7;
    Reservoirs water(nullptr);
    water.define(body, StorageCurve::constantArea(1, 400, 410));
    const auto capacity = water.volume(body);
    FakeTerrain terrain;
    terrain.columns[{1, 1}] = {400, surface(body, 410)};
    surface_ice::Simulation simulation;

    freezeStep(water, simulation, terrain, .1, -10.f);
    const auto* partial = water.surfaceIceAt(1, 1);
    CHECK(partial);
    const auto progress = partial->progress;
    CHECK(progress > 0);

    // Warming reverses the same sub-stage progress before any thaw.
    freezeStep(water, simulation, terrain, .02, 10.f);
    partial = water.surfaceIceAt(1, 1);
    CHECK(partial);
    CHECK(partial->progress < progress);
    EQUAL(partial->frozen, 0, "warming removes no ice before the threshold");

    freezeStep(water, simulation, terrain, .1, -10.f);
    freezeStep(water, simulation, terrain, .1, -10.f);
    const auto* frozen = water.surfaceIceAt(1, 1);
    CHECK(frozen);
    CHECK(frozen->frozen > 0);
    const auto before = water.volume(body);

    Reservoirs reloaded(nullptr);
    reloaded.define(body, StorageCurve::constantArea(1, 400, 410));
    reloaded.restore(water.encode());
    const auto* saved = reloaded.surfaceIceAt(1, 1);
    CHECK(saved);
    EQUAL(saved->frozen, frozen->frozen, "frozen volume survives reload");
    EQUAL(saved->stage, frozen->stage, "stage survives reload");
    EQUAL(saved->progress, frozen->progress, "sub-stage progress survives");
    EQUAL(reloaded.volume(body), water.volume(body),
          "the liquid debit survives reload");

    // A sustained warm spell thaws the lid and credits the body back.
    reloaded.restore(water.encode());
    surface_ice::Simulation thawing;
    FakeTerrain reloadedTerrain;
    reloadedTerrain.columns[{1, 1}] = {400, surface(body, 410)};
    thawing.update(
        reloaded, reloadedTerrain,
        [](int, int, double, double) { return 10.f; }, 11., 2., 8, 8, 8);
    const auto* melted = reloaded.surfaceIceAt(1, 1);
    CHECK(!melted || melted->frozen == 0);
    EQUAL(reloaded.volume(body), capacity,
          "thaw returns the water to the body");
    EQUAL(reloaded.liquidTotal() + reloaded.frozenTotal(), capacity,
          "thaw keeps the balance sheet closed");
    (void)before;
}

TEST(surface_ice_flowing_reach_forms_ice_slower) {
    Reservoirs still(nullptr);
    Reservoirs flowing(nullptr);
    FakeTerrain stillTerrain, flowingTerrain;
    stillTerrain.columns[{1, 1}] = {399, surface(0, 401, 255)};
    flowingTerrain.columns[{1, 1}] = {399, surface(0, 401, 255, 5)};
    surface_ice::Simulation stillSimulation, flowingSimulation;

    freezeStep(still, stillSimulation, stillTerrain, 1. / 24., -10.f);
    freezeStep(flowing, flowingSimulation, flowingTerrain, 1. / 24., -10.f);
    const auto* stillState = still.surfaceIceAt(1, 1);
    const auto* flowingState = flowing.surfaceIceAt(1, 1);
    CHECK(stillState && flowingState);
    CHECK(flowingState->progress < stillState->progress);
    const double ratio =
        double(flowingState->progress) / double(stillState->progress);
    CHECK(std::abs(ratio - surface_ice::FlowFreezeFactor) < .02);
}

TEST(surface_ice_projects_a_fractional_lid_and_distant_top) {
    Reservoirs water(nullptr);
    const TilePos cell{4, 400, 4};
    EQUAL(water.creditLocal(cell, 128, ReservoirExchange::Input), 128,
          "a half-full local cell");
    FakeTerrain terrain;
    terrain.columns[{4, 4}] = {399, surface(0, 401, 128)};
    surface_ice::Simulation simulation;
    freezeStep(water, simulation, terrain, .2, -10.f);
    const auto* state = water.surfaceIceAt(4, 4);
    CHECK(state);
    CHECK((std::abs(state->waterline - (400.f + 128.f / 255.f)) < 1e-5f));
    CHECK(state->frozen > 0);

    surface_ice::IceCover cover;
    cover.rebuild(water, terrain, 7);
    const auto top = cover.tops().find({4, 4});
    CHECK(top != cover.tops().end());
    CHECK((std::abs(double(top->second) / 1'000'000. - (400. + 128. / 255.)) <
           1e-5));

    const ChunkCoord coord{0, 25, 0};
    const auto slab = cover.environment()->find(coord);
    CHECK(slab != cover.environment()->end());
    const auto index = Chunk::cell(4, 400 % Chunk::SIZE_Y, 4);
    const auto geometry = slab->second->find(index);
    CHECK(geometry != slab->second->end());
    CHECK(!geometry->second->empty());
    const auto height = geometry->second->supportHeight({.2f, .2f}, {.8f, .8f});
    CHECK(height);
    CHECK((std::abs(*height - (128.f / 255.f)) < .01f));
}

TEST(surface_ice_regional_features_supply_deep_and_sea_ice) {
    World world{};
    world.width = 1;
    world.height = 3;
    world.regions.resize(3);
    for (int i = 0; i < 3; ++i) {
        world.regions[i].coord = {0, unsigned(i)};
        world.regions[i].land = LandType::WATER;
        world.regions[i].temperature_c = -20.f;
    }
    world.regions[0].addFeature(FeatureMask::ICE_SHEET);
    world.regions[1].addFeature(FeatureMask::SEA_ICE);
    surface_ice::Regional regional(world);
    CHECK(regional.update(0.));
    CHECK(regional.targetAt(64, 64) > 15.f);
    CHECK(
        (regional.targetAt(64, 192) > .5f && regional.targetAt(64, 192) < 2.f));
    // The boundary between the sheet and the one-metre sea ice is blended
    // rather than a hard step.
    const auto boundary = regional.targetAt(64, 128);
    CHECK((boundary > 1.f && boundary < 16.f));
}

TEST(surface_ice_feature_cover_projects_without_a_ledger_record) {
    Reservoirs water(nullptr);
    FakeTerrain terrain;
    terrain.columns[{1, 1}] = {399, surface(0, 401, 255)};
    surface_ice::IceCover cover;
    const std::set<std::pair<unsigned, unsigned>> dirty{{0, 0}};
    const std::function<float(int, int)> target = [](int, int) { return 1.f; };
    cover.rebuild(water, terrain, 7, &dirty, &target);
    CHECK(!cover.environment()->empty());
    const auto top = cover.tops().find({1, 1});
    CHECK(top != cover.tops().end());
    CHECK(!water.snapshot()->surfaceIceAt(1, 1));

    // A picked surface cell removes the whole regional column and the cut
    // survives a rebuild instead of being filled again.
    CHECK(cover.cellAt({1, 400, 1}) != EnvironmentCell{});
    CHECK(cover.cut({1, 400, 1}));
    CHECK(cover.cellAt({1, 400, 1}) == EnvironmentCell{});
    CHECK(cover.cutColumns().contains({1, 1}));
    cover.rebuild(water, terrain, 7, &dirty, &target);
    CHECK(cover.cellAt({1, 400, 1}) == EnvironmentCell{});
}
