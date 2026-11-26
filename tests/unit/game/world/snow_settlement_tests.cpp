#include <cmath>
#include <map>
#include <optional>
#include <tuple>

#include "test.hpp"
#include "world/precipitation.hpp"
#include "world/snow_cover.hpp"
#include "world/snow_settlement.hpp"
#include "world/tile_piece.hpp"
#include "world/world_wrap.hpp"

namespace {

using tile_geometry::CellGeometry;
using tile_geometry::PieceFill;
using tile_geometry::PiecePlacement;
using tile_geometry::TilePiece;

struct PosLess {
    bool operator()(const TilePos& a, const TilePos& b) const {
        return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
    }
};

struct Fixture : snow::SnowTerrain {
    std::map<TilePos, CellGeometry, PosLess> cells;
    const Chunk* grassChunk = nullptr;
    int periodX = 0;
    int x0 = 0, x1 = 1;
    int z0 = 0, z1 = 1;
    int lowY = 0, highY = 20;

    std::optional<CellGeometry> cell(TilePos pos) const override {
        pos.x = wrapX(pos.x);
        if (pos.y < lowY || pos.y >= highY || pos.z < z0 || pos.z >= z1)
            return std::nullopt;
        if (grassChunk && pos == TilePos{0, 2, 0})
            return grassChunk->solidCellAt(Chunk::cell(0, 2, 0)).geometry();
        const auto found = cells.find(pos);
        return found == cells.end()
                   ? std::optional<CellGeometry>(CellGeometry{})
                   : found->second;
    }
    int wrapX(int x) const override {
        return periodX > 0 ? int(WorldWrap{periodX}.wrapTile(x)) : x;
    }
    bool inBounds(int x, int z) const override {
        return x >= x0 && x < x1 && z >= z0 && z < z1;
    }
    int minY() const override { return lowY; }
    int maxY() const override { return highY; }
};

CellGeometry cube() {
    CellGeometry cell;
    CHECK(cell.tryAdd(TilePiece{1, 1, {}, {}, PieceFill::BelowProfile, 1}) ==
          PiecePlacement::Added);
    return cell;
}

void weather(snow::SnowSettlement& settlement, const Fixture& terrain, int x,
             int z, double hours, float rate, float temperature) {
    if (hours <= 0) return;
    settlement.beginWeather(0);
    const double target = settlement.weatherDay(x, z) + hours / 24;
    precipitation::reconcile(
        settlement, terrain, x, z, target, false,
        [=](int64_t) {
            world_weather::Sample sample;
            sample.precipitationRate = rate;
            sample.temperatureC = temperature;
            return sample;
        },
        [](double) { return std::optional(false); });
}

CellGeometry ramp(TileFacing facing = TileFacing::PosZ) {
    CellGeometry cell;
    CHECK(cell.tryAdd(TilePiece{1,
                                1,
                                TileShape::make(TileShapeProfile::Ramp, facing),
                                {},
                                PieceFill::BelowProfile,
                                1}) == PiecePlacement::Added);
    return cell;
}

CellGeometry rampProfile(TileShapeProfile profile,
                         TileFacing facing = TileFacing::PosZ) {
    CellGeometry cell;
    CHECK(cell.tryAdd(TilePiece{1,
                                1,
                                TileShape::make(profile, facing),
                                {},
                                PieceFill::BelowProfile,
                                1}) == PiecePlacement::Added);
    return cell;
}

void solid(Fixture& fixture, int x, int y, int z) {
    fixture.cells[TilePos{x, y, z}] = cube();
}

void rampCell(Fixture& fixture, int x, int y, int z, TileFacing facing) {
    fixture.cells[TilePos{x, y, z}] = ramp(facing);
}

void conserved(const snow::SnowSettlement& settlement) {
    EQUAL(settlement.storedVolume() + settlement.pendingVolume() +
              settlement.sinkVolume(),
          settlement.depositedVolume(), "volume ledger");
}

void nearLevel(const snow::SnowSettlement& settlement, int x, int z,
               double expected) {
    const auto level = settlement.levelAt(x, z);
    CHECK(level.has_value());
    CHECK(std::abs(*level - expected) < 1e-4);
}

}  // namespace

// A 45-degree ramp at the foot of a closed landing. Supplying 0.05125 m3
// puts a horizontal 5 cm snowline on the landing and clips a 5 cm triangular
// pocket (0.00125 m3) into the toe of the ramp.
TEST(snow_settlement_ramp_wedge) {
    Fixture fixture;
    fixture.x0 = 0;
    fixture.x1 = 1;
    fixture.z0 = -1;
    fixture.z1 = 1;
    solid(fixture, 0, 9, -1);
    solid(fixture, 0, 9, 0);
    rampCell(fixture, 0, 10, 0, TileFacing::PosZ);

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(0, -1, 12, 51250) ==
          snow::DepositOutcome::Settled);

    EQUAL(settlement.volumeAt(0, -1), snow::Volume{50000}, "landing volume");
    EQUAL(settlement.volumeAt(0, 0), snow::Volume{1250}, "ramp toe wedge");
    nearLevel(settlement, 0, -1, 10.05);
    nearLevel(settlement, 0, 0, 10.05);
    conserved(settlement);
}

TEST(snow_settlement_ramp_wedge_incremental) {
    Fixture fixture;
    fixture.z0 = -1;
    fixture.z1 = 1;
    solid(fixture, 0, 9, -1);
    solid(fixture, 0, 9, 0);
    rampCell(fixture, 0, 10, 0, TileFacing::PosZ);

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(0, -1, 12, 25625) ==
          snow::DepositOutcome::Settled);
    EQUAL(settlement.volumeAt(0, -1) + settlement.volumeAt(0, 0),
          snow::Volume{25625}, "half total");
    CHECK(settlement.volumeAt(0, -1) > 25000);
    CHECK(settlement.volumeAt(0, 0) > 0);
    const auto halfLevel = settlement.levelAt(0, -1);
    CHECK(halfLevel.has_value());
    CHECK(*halfLevel > 10.025 && *halfLevel < 10.026);

    CHECK(settlement.deposit(0, -1, 12, 25625) ==
          snow::DepositOutcome::Settled);
    EQUAL(settlement.volumeAt(0, -1), snow::Volume{50000}, "full landing");
    EQUAL(settlement.volumeAt(0, 0), snow::Volume{1250}, "full wedge");
    nearLevel(settlement, 0, -1, 10.05);
    conserved(settlement);
}

// The ramp's lower edge is open over a floor one metre below. Loose snowfall
// leaves the 45-degree face and comes to rest on that floor.
TEST(snow_settlement_draining_ramp) {
    Fixture fixture;
    fixture.z0 = -1;
    fixture.z1 = 1;
    solid(fixture, 0, 9, 0);
    solid(fixture, 0, 8, -1);
    rampCell(fixture, 0, 10, 0, TileFacing::PosZ);

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(0, 0, 12, 100000) ==
          snow::DepositOutcome::Settled);

    EQUAL(settlement.volumeAt(0, 0), snow::Volume{0}, "bare ramp");
    EQUAL(settlement.volumeAt(0, -1), snow::Volume{100000}, "lower floor");
    nearLevel(settlement, 0, -1, 9.1);
    conserved(settlement);
}

// A wall at the toe closes the exit. The same ramp now holds its snow.
TEST(snow_settlement_blocked_toe) {
    Fixture fixture;
    fixture.z0 = -1;
    fixture.z1 = 1;
    solid(fixture, 0, 9, 0);
    solid(fixture, 0, 8, -1);
    solid(fixture, 0, 10, -1);
    rampCell(fixture, 0, 10, 0, TileFacing::PosZ);

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(0, 0, 12, 100000) ==
          snow::DepositOutcome::Settled);

    EQUAL(settlement.volumeAt(0, 0), snow::Volume{100000}, "blocked ramp");
    EQUAL(settlement.volumeAt(0, -1), snow::Volume{0}, "behind the wall");
    conserved(settlement);
}

// Two equal drop outlets at the same sill split the excess and the remainder
// deterministically.
TEST(snow_settlement_equal_outlets) {
    auto build = [] {
        Fixture fixture;
        fixture.z0 = -2;
        fixture.z1 = 2;
        solid(fixture, 0, 9, 0);
        solid(fixture, 0, 8, -1);
        solid(fixture, 0, 8, 1);
        return fixture;
    };
    auto firstFixture = build();
    snow::SnowSettlement first(firstFixture);
    CHECK(first.deposit(0, 0, 12, 1001) == snow::DepositOutcome::Settled);
    EQUAL(first.volumeAt(0, 0), snow::Volume{0}, "ridge drains");
    const auto positive = first.volumeAt(0, 1);
    const auto negative = first.volumeAt(0, -1);
    CHECK((positive == 501 && negative == 500) ||
          (positive == 500 && negative == 501));
    conserved(first);

    auto secondFixture = build();
    snow::SnowSettlement second(secondFixture);
    CHECK(second.deposit(0, 0, 12, 1001) == snow::DepositOutcome::Settled);
    EQUAL(second.volumeAt(0, 1), positive, "deterministic outlet order");
    EQUAL(second.volumeAt(0, -1), negative, "deterministic outlet order");
}

// A single diagonal facet sheds into two lower floors at its low corner. The
// split uses the same deterministic equal-outlet rule.
TEST(snow_settlement_diagonal_ramp_split) {
    Fixture fixture;
    fixture.x0 = -1;
    fixture.x1 = 1;
    fixture.z0 = -1;
    fixture.z1 = 1;
    solid(fixture, 0, 9, 0);
    solid(fixture, -1, 8, 0);
    solid(fixture, 0, 8, -1);
    fixture.cells[TilePos{0, 10, 0}] =
        rampProfile(TileShapeProfile::DiagonalRamp);

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(0, 0, 12, 1001) == snow::DepositOutcome::Settled);

    const auto negativeX = settlement.volumeAt(-1, 0);
    const auto negativeZ = settlement.volumeAt(0, -1);
    CHECK((negativeX == 501 && negativeZ == 500) ||
          (negativeX == 500 && negativeZ == 501));
    EQUAL(settlement.volumeAt(0, 0), snow::Volume{0}, "diagonal sheds");
    conserved(settlement);
}

// Two ramps meet at a common low line. Their shared level fills both toes.
TEST(snow_settlement_opposed_slopes) {
    Fixture fixture;
    fixture.z0 = -1;
    fixture.z1 = 1;
    solid(fixture, 0, 9, -1);
    solid(fixture, 0, 9, 0);
    rampCell(fixture, 0, 10, -1, TileFacing::NegZ);
    rampCell(fixture, 0, 10, 0, TileFacing::PosZ);

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(0, 0, 12, 2000) == snow::DepositOutcome::Settled);

    const auto left = settlement.volumeAt(0, -1);
    const auto right = settlement.volumeAt(0, 0);
    EQUAL(left + right, snow::Volume{2000}, "trough total");
    CHECK(std::abs(left - right) <= 1);
    nearLevel(settlement, 0, -1, 10 + std::sqrt(.002));
    nearLevel(settlement, 0, 0, 10 + std::sqrt(.002));
    conserved(settlement);
}

// The upper ledge drains to the lower one. Once the lower fills to the
// ledge base, further snow is shared by both.
TEST(snow_settlement_stacked_ledges) {
    Fixture fixture;
    fixture.z0 = -1;
    fixture.z1 = 1;
    solid(fixture, 0, 9, 0);
    solid(fixture, 0, 8, -1);

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(0, 0, 12, 1000000) ==
          snow::DepositOutcome::Settled);
    EQUAL(settlement.volumeAt(0, 0), snow::Volume{0}, "upper ledge dry");
    EQUAL(settlement.volumeAt(0, -1), snow::Volume{1000000}, "lower filled");
    nearLevel(settlement, 0, -1, 10);

    CHECK(settlement.deposit(0, 0, 12, 1000000) ==
          snow::DepositOutcome::Settled);
    EQUAL(settlement.volumeAt(0, 0), snow::Volume{500000}, "upper shares");
    EQUAL(settlement.volumeAt(0, -1), snow::Volume{1500000}, "lower shares");
    nearLevel(settlement, 0, 0, 10.5);
    nearLevel(settlement, 0, -1, 10.5);
    conserved(settlement);
}

// The X seam joins two footprints. A ridge on one side drains to the floor
// on the other side.
TEST(snow_settlement_cross_region) {
    Fixture fixture;
    fixture.periodX = 2;
    fixture.x0 = 0;
    fixture.x1 = 2;
    solid(fixture, 1, 9, 0);
    solid(fixture, 0, 8, 0);

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(1, 0, 12, 100000) ==
          snow::DepositOutcome::Settled);

    EQUAL(settlement.volumeAt(1, 0), snow::Volume{0}, "ridge drains");
    EQUAL(settlement.volumeAt(0, 0), snow::Volume{100000}, "across the seam");
    nearLevel(settlement, 0, 0, 9.1);
    conserved(settlement);
}

// Bounded work leaves a deposit in transit. Persisting and restoring the
// state must not lose the volume, and a later retry must apply it once.
TEST(snow_settlement_bounded_pending) {
    Fixture fixture;
    fixture.x0 = 0;
    fixture.x1 = 100;
    fixture.z0 = 0;
    fixture.z1 = 1;
    for (int x = 0; x < 100; ++x) solid(fixture, x, 9, 0);

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(50, 0, 12, 1000, 10) ==
          snow::DepositOutcome::Pending);
    EQUAL(settlement.storedVolume(), snow::Volume{0}, "nothing committed");
    EQUAL(settlement.pendingVolume(), snow::Volume{1000}, "in transit");
    EQUAL(settlement.depositedVolume(), snow::Volume{1000}, "accepted");
    conserved(settlement);

    const auto encoded = settlement.encode();
    snow::SnowSettlement restored(fixture);
    CHECK(restored.restore(encoded));
    EQUAL(restored.pendingVolume(), snow::Volume{1000}, "restored transit");
    EQUAL(restored.storedVolume(), snow::Volume{0}, "restored storage");

    // A retry that is still too small keeps the unit in transit.
    EQUAL(restored.settle(10), size_t{0}, "no premature commit");
    EQUAL(restored.pendingVolume(), snow::Volume{1000}, "still in transit");
    conserved(restored);

    EQUAL(restored.settle(100000), size_t{1}, "retry applies");
    EQUAL(restored.storedVolume(), snow::Volume{1000}, "retry commits");
    EQUAL(restored.pendingVolume(), snow::Volume{0}, "transit emptied");
    nearLevel(restored, 50, 0, 10.00001);
    conserved(restored);
}

// Z and the world top are closed. Snow on an edge column stays put.
TEST(snow_settlement_closed_map_edge) {
    Fixture fixture;
    fixture.z0 = 0;
    fixture.z1 = 1;
    solid(fixture, 0, 9, 0);

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(0, 0, 12, 1000) == snow::DepositOutcome::Settled);

    EQUAL(settlement.volumeAt(0, 0), snow::Volume{1000}, "closed edge");
    EQUAL(settlement.sinkVolume(), snow::Volume{0}, "no leak");
    nearLevel(settlement, 0, 0, 10.001);
    conserved(settlement);
}

// A column can only hold the free space below the closed world top. The rest
// is accounted for rather than silently clamped.
TEST(snow_settlement_top_sink) {
    Fixture fixture;
    fixture.z0 = 0;
    fixture.z1 = 1;
    fixture.highY = 12;
    solid(fixture, 0, 9, 0);

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(0, 0, 12, 3000000) ==
          snow::DepositOutcome::Settled);

    EQUAL(settlement.volumeAt(0, 0), snow::Volume{2000000}, "top capacity");
    EQUAL(settlement.sinkVolume(), snow::Volume{1000000}, "top sink");
    nearLevel(settlement, 0, 0, 12);
    conserved(settlement);
}

// A gentler (half-height) ramp starts with a shallow pocket at its toe. The
// landing level still caps the pocket at 5 cm.
TEST(snow_settlement_gentle_slope_pocket) {
    Fixture fixture;
    fixture.z0 = -1;
    fixture.z1 = 1;
    solid(fixture, 0, 9, -1);
    solid(fixture, 0, 9, 0);
    CellGeometry gentle;
    CHECK(gentle.tryAdd(TilePiece{
              1,
              1,
              TileShape::make(TileShapeProfile::RampHalfLow, TileFacing::PosZ),
              {},
              PieceFill::BelowProfile,
              1}) == PiecePlacement::Added);
    fixture.cells[TilePos{0, 10, 0}] = gentle;

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(0, -1, 12, 52500) ==
          snow::DepositOutcome::Settled);

    EQUAL(settlement.volumeAt(0, -1), snow::Volume{50000}, "gentle landing");
    EQUAL(settlement.volumeAt(0, 0), snow::Volume{2500}, "shallow pocket");
    nearLevel(settlement, 0, -1, 10.05);
    conserved(settlement);
}

// The rendered/queryable projection comes from the same column volumes. The
// landing becomes a 5 cm layer, the ramp toe a clipped wedge, and a shallower
// deposit becomes a visible dusting skin.
TEST(snow_cover_projects_settlement) {
    Fixture fixture;
    fixture.z0 = 0;
    fixture.z1 = 2;
    solid(fixture, 0, 9, 0);
    solid(fixture, 0, 9, 1);
    rampCell(fixture, 0, 10, 1, TileFacing::PosZ);

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(0, 0, 12, 51250) == snow::DepositOutcome::Settled);
    snow::SnowCover cover;
    cover.rebuild(settlement, fixture, 42);

    const auto landing = cover.cellAt({0, 10, 0});
    CHECK(landing);
    CHECK(landing->contains({.5f, .02f, .5f}));
    const auto landingTop = landing->supportHeight({.2f, .2f}, {.8f, .8f});
    CHECK(landingTop.has_value());
    nearLevel(settlement, 0, 0, 10.05);
    CHECK(std::abs(*landingTop - .05f) < 1e-4f);

    const auto toe = cover.cellAt({0, 10, 1});
    CHECK(toe);
    CHECK(toe->contains({.5f, .025f, .025f}));
    CHECK(!toe->contains({.5f, .03f, .2f}));
    CHECK(landing->contains({.5f, .01f, .5f}));
    CHECK(!cover.cellAt({0, 9, 0}));  // support stays a chunk solid
    cover.rebuild(settlement, fixture, 42);
    CHECK(cover.changedChunks().empty());
    CHECK(cover.cellAt({0, 10, 0}) == landing);

    Fixture dustFixture;
    dustFixture.z0 = 0;
    dustFixture.z1 = 1;
    solid(dustFixture, 0, 9, 0);
    snow::SnowSettlement dust(dustFixture);
    CHECK(dust.deposit(0, 0, 12, 1000) == snow::DepositOutcome::Settled);
    snow::SnowCover dustCover;
    dustCover.rebuild(dust, dustFixture, 42);
    const auto skin = dustCover.cellAt({0, 10, 0});
    CHECK(skin);
    const auto skinTop = skin->supportHeight({.2f, .2f}, {.8f, .8f});
    CHECK(skinTop.has_value());
    CHECK(*skinTop >= snow::SnowCover::MinimumVisibleDepth - 1e-5f);
    CHECK(*skinTop <= snow::SnowCover::DustingDepth + 1e-5f);
}

TEST(snow_settlement_serialization) {
    Fixture fixture;
    fixture.x0 = 0;
    fixture.x1 = 1;
    fixture.z0 = -1;
    fixture.z1 = 1;
    solid(fixture, 0, 9, -1);
    solid(fixture, 0, 9, 0);
    rampCell(fixture, 0, 10, 0, TileFacing::PosZ);

    snow::SnowSettlement settlement(fixture);
    CHECK(settlement.deposit(0, -1, 12, 51250) ==
          snow::DepositOutcome::Settled);
    conserved(settlement);

    const auto encoded = settlement.encode();
    snow::SnowSettlement restored(fixture);
    CHECK(restored.restore(encoded));
    EQUAL(restored.volumeAt(0, -1), snow::Volume{50000}, "restored landing");
    EQUAL(restored.volumeAt(0, 0), snow::Volume{1250}, "restored wedge");
    EQUAL(restored.pendingVolume(), snow::Volume{0}, "restored pending");
    EQUAL(restored.depositedVolume(), snow::Volume{51250}, "restored input");
    nearLevel(restored, 0, -1, 10.05);
    conserved(restored);

    // A truncated or tagged-corrupt payload is rejected without mutating the
    // current state.
    const auto before = restored.encode();
    auto truncated = encoded;
    truncated.resize(encoded.size() - 1);
    CHECK(!restored.restore(truncated));
    CHECK(restored.encode() == before);
}

TEST(weather_exposure_closed_roofs_and_cave) {
    Fixture terrain;
    terrain.x1 = 3;
    for (int x = 0; x < 3; ++x) solid(terrain, x, 2, 0);
    solid(terrain, 0, 7, 0);
    // An optically clear material is still a solid weather barrier.
    CHECK((terrain.cells[{1, 7, 0}].tryAdd(TilePiece::layer(
               1, 2, tile_geometry::LayerThickness::Cm5, {0, .5f, 0})) ==
           PiecePlacement::Added));
    solid(terrain, 2, 16, 0);
    solid(terrain, 2, 17, 0);

    EQUAL(*precipitation::firstSurface(terrain, .5, .5)->height, 8.,
          "solid roof");
    CHECK(std::abs(*precipitation::firstSurface(terrain, 1.5, .5)->height -
                   7.55) < 1e-5);
    EQUAL(*precipitation::firstSurface(terrain, 2.5, .5)->height, 18.,
          "cave roof even with a tall air gap");
}

TEST(weather_exposure_roof_edge_above_ramp_and_wrap) {
    Fixture terrain;
    terrain.x1 = 2;
    terrain.periodX = 2;
    solid(terrain, 1, 2, 0);
    rampCell(terrain, 1, 3, 0, TileFacing::PosZ);
    CHECK((terrain.cells[{1, 6, 0}].tryAdd(
               {1,
                2,
                TileShape::make(TileShapeProfile::Wall50),
                {},
                PieceFill::BelowProfile,
                1}) == PiecePlacement::Added));
    const auto hits = precipitation::footprint(terrain, -1, 0);
    int roof = 0, ramp = 0;
    for (const auto& hit : hits) {
        CHECK(hit && hit->height);
        roof += hit->cellY == 6;
        ramp += hit->cellY == 3;
    }
    EQUAL(roof, 2, "two rays intercepted by the roof");
    EQUAL(ramp, 2, "two rays reach the slope through the opening");
    const auto east = precipitation::footprint(terrain, 1, 0);
    for (size_t i = 0; i < hits.size(); ++i)
        EQUAL(*hits[i]->height, *east[i]->height, "wrapped rays agree");

    snow::SnowSettlement snow(terrain);
    weather(snow, terrain, 1, 0, 1, 4, -5);
    EQUAL(snow.columnAt(1, 0, 3)->volume, 10000, "half reaches the ramp");
    EQUAL(snow.columnAt(1, 0, 6)->volume, 10000, "half stops on the roof");
    conserved(snow);
}

TEST(weather_snow_hatch_timing) {
    Fixture terrain;
    solid(terrain, 0, 2, 0);
    snow::SnowSettlement snow(terrain);
    weather(snow, terrain, 0, 0, .5, 4, -5);
    solid(terrain, 0, 7, 0);
    weather(snow, terrain, 0, 0, .5, 4, -5);
    EQUAL(snow.columnAt(0, 0, 3)->volume, 10000, "half on the open floor");
    EQUAL(snow.columnAt(0, 0, 8)->volume, 10000, "half on the shut hatch");

    // Open halfway through a separate event. A bare hatch is opened after
    // measuring its precipitation, so the floor ledger excludes roof fall.
    snow.clear();
    weather(snow, terrain, 0, 0, .5, 4, -5);
    terrain.cells.erase({0, 7, 0});
    weather(snow, terrain, 0, 0, .5, 4, -5);
    EQUAL(snow.columnAt(0, 0, 3)->volume, 10000, "half reaches opened hatch");
    snow.supportChanged({0, 7, 0});
    EQUAL(snow.columnAt(0, 0, 3)->volume, 20000, "roof snow falls separately");
    EQUAL(snow.columns().size(), size_t(1), "no floating snow");
    conserved(snow);
}

TEST(weather_snow_and_melt_interval_accounting) {
    Fixture terrain;
    solid(terrain, 0, 2, 0);
    snow::SnowSettlement whole(terrain), parts(terrain);
    weather(whole, terrain, 0, 0, 0, 4, -5);
    EQUAL(whole.revision(), uint64_t(0), "pause leaves the ledger alone");
    weather(whole, terrain, 0, 0, 1, 6, 3);
    EQUAL(whole.depositedVolume(), 0, "rain adds zero snow");

    weather(whole, terrain, 0, 0, 1, 3.142f, -5);
    for (int i = 0; i < 10; ++i) weather(parts, terrain, 0, 0, .1, 3.142f, -5);
    CHECK(std::abs(whole.storedVolume() - parts.storedVolume()) <= 6);
    const auto before = whole.storedVolume();
    weather(whole, terrain, 0, 0, 1, 0, 1.256f);
    for (int i = 0; i < 10; ++i) weather(parts, terrain, 0, 0, .1, 0, 1.256f);
    CHECK(std::abs(whole.storedVolume() - parts.storedVolume()) <= 12);
    EQUAL(double(before - whole.storedVolume()) / snow::SnowPerWater,
          whole.meltwaterCm3(), "bulk snow converts to water equivalent");

    snow::SnowSettlement restored(terrain);
    CHECK(restored.restore(whole.encode()));
    EQUAL(restored.storedVolume(), whole.storedVolume(),
          "bulk volume persists");
    EQUAL(restored.meltwaterCm3(), whole.meltwaterCm3(), "meltwater persists");
    CHECK(restored.meltwater() == whole.meltwater());
}

TEST(weather_melt_uncovers_vegetation) {
    Fixture terrain;
    Chunk chunk;
    terrain.grassChunk = &chunk;
    auto& grass = chunk.tiles[0][2][0];
    grass.block_id = 1;
    grass.cover = 1;
    const auto original = grass;
    snow::SnowSettlement snow(terrain);
    weather(snow, terrain, 0, 0, 1, 10, -5);
    snow::SnowCover cover;
    cover.rebuild(snow, terrain, 42);
    CHECK(cover.cellAt({0, 3, 0}));

    solid(terrain, 0, 7, 0);
    weather(snow, terrain, 0, 0, 4, 0, 5);
    EQUAL(snow.storedVolume(), 50000, "sheltered snow survives outdoor melt");
    terrain.cells.erase({0, 7, 0});
    weather(snow, terrain, 0, 0, 4, 0, 5);
    cover.rebuild(snow, terrain, 42);
    CHECK(!cover.cellAt({0, 3, 0}));
    EQUAL(snow.meltwaterCm3(), 10000., "50 mm of snow yields 10 mm SWE");
    CHECK(grass.block_id == original.block_id &&
          grass.cover == original.cover &&
          grass.coverState == original.coverState);
}

namespace {
constexpr double SixHours = 6.0 / 24;

double replay(snow::SnowSettlement& snow, const Fixture& terrain, int x, int z,
              double day, bool ready = true) {
    return precipitation::reconcile(
        snow, terrain, x, z, day, false,
        [](int64_t) { return world_weather::Sample{}; },
        [=](double) -> std::optional<bool> {
            if (!ready) return std::nullopt;
            return false;
        });
}
}  // namespace

TEST(weather_history_shelter_travel_and_reload) {
    Fixture terrain;
    terrain.x1 = 12;
    terrain.periodX = 12;
    for (int x : {0, 3, 6, 9}) solid(terrain, x, 2, 0);
    for (int x : {3, 9}) solid(terrain, x, 7, 0);

    snow::SnowSettlement snow(terrain);
    snow.beginWeather(0);
    snow.setWeatherForce(0, 4);  // Snow
    for (int bin = 1; bin <= 12; ++bin)
        for (int x : {0, 3})
            replay(snow, terrain, x, 0, weather_history::intervalStart(bin));
    snow.setWeatherForce(SixHours, 1);  // Clear, +5 C
    for (int bin = 13; bin <= 24; ++bin)
        for (int x : {0, 3})
            replay(snow, terrain, x, 0, weather_history::intervalStart(bin));

    const double target = 2 * SixHours;
    EQUAL(snow.weatherDay(6, 0), 0., "unvisited begins at weather epoch");
    EQUAL(replay(snow, terrain, 6, 0, target, false), 0.,
          "unready water and geometry do not advance the cursor");
    const double halfway = 9. / 24;
    replay(snow, terrain, 6, 0, halfway);
    snow.checkpointWeather(target);

    snow::SnowSettlement loaded(terrain);
    CHECK(loaded.restore(snow.encode()));
    EQUAL(loaded.weatherHorizon(), target, "persisted world clock horizon");
    EQUAL(loaded.weatherDay(-6, 0), halfway, "canonical progress after reload");
    loaded.setWeatherForce(target, 0);  // In-game forcing resets on reload.
    replay(loaded, terrain, 6, 0, target);
    replay(loaded, terrain, 9, 0, target);
    for (int x : {0, 6})
        EQUAL(loaded.volumeAt(x, 0), 45000, "exposed pad keeps 45 mm");
    for (int x : {3, 9}) {
        CHECK(!loaded.columnAt(x, 0, 3));
        EQUAL(loaded.volumeAt(x, 0), 45000, "snow fell on the roof");
    }
    EQUAL(loaded.meltwaterCm3(), 60000., "roof and floor meltwater conserved");
    const auto saved = loaded.encode();
    for (int x : {0, 3, 6, 9}) replay(loaded, terrain, x, 0, target);
    CHECK(loaded.encode() == saved);
    EQUAL(loaded.depositedVolume(), 480000, "no second snowfall");
}

TEST(weather_history_dated_exposure_change) {
    Fixture terrain;
    solid(terrain, 0, 2, 0);
    snow::SnowSettlement snow(terrain);
    snow.beginWeather(0);
    snow.setWeatherForce(0, 4);
    constexpr double editDay = 15. / (60 * 24);
    replay(snow, terrain, 0, 0, editDay);
    solid(terrain, 0, 7, 0);
    snow::SnowSettlement loaded(terrain);
    CHECK(loaded.restore(snow.encode()));
    replay(loaded, terrain, 0, 0, 2 * editDay);
    EQUAL(loaded.columnAt(0, 0, 3)->volume, 5000, "snow before roof edit");
    EQUAL(loaded.columnAt(0, 0, 8)->volume, 5000, "snow after roof edit");

    loaded.setWeatherForce(2 * editDay, 1);
    replay(loaded, terrain, 0, 0, SixHours);
    EQUAL(loaded.volumeAt(0, 0), 5000, "closed roof shelters old floor snow");
}

TEST(weather_history_full_year_skip_visit_order) {
    World world;
    world.width = 4;
    world.height = 3;
    world.genSettings.seed = 2024;
    world.regions.resize(world.width * world.height);
    for (int z = 0; z < world.height; ++z)
        for (int x = 0; x < world.width; ++x) {
            auto& region = world.regions[z * world.width + x];
            region.coord = {unsigned(x), unsigned(z)};
            region.land_height_m = 64;
            region.temperature_c = 4;
        }

    Fixture terrain;
    terrain.x1 = world.width * WorldWrap::RegionSize;
    terrain.periodX = terrain.x1;
    terrain.highY = 100;
    constexpr int A = 1, B = 65;
    solid(terrain, A, 60, 0);
    solid(terrain, B, 60, 0);
    weather_history::Cache history;
    constexpr double year = world_calendar::DaysPerYear;
    const auto advance = [&](snow::SnowSettlement& snow, int x, double end) {
        return precipitation::reconcile(
            snow, terrain, x, 0, end, true,
            [&](int64_t bin) { return history.at(world, x, 0, bin).weather; },
            [](double) { return std::optional(false); });
    };
    const auto skip = [&](snow::SnowSettlement& snow, int x) {
        int batches = 0;
        while (snow.weatherDay(x, 0) < year) {
            const double before = snow.weatherDay(x, 0);
            const double after = advance(snow, x, year);
            CHECK(after > before && after - before <= 7.);
            CHECK(++batches <= 52);
        }
        EQUAL(batches, 52, "bounded full-year replay");
    };

    snow::SnowSettlement westFirst(terrain), eastFirst(terrain), daily(terrain);
    for (auto* snow : {&westFirst, &eastFirst, &daily}) snow->beginWeather(0);
    skip(westFirst, A);
    skip(westFirst, B);
    skip(eastFirst, B);
    skip(eastFirst, A);
    CHECK(westFirst.encode() == eastFirst.encode());
    for (int day = 1; day <= year; ++day)
        for (int x : {A, B}) EQUAL(advance(daily, x, day), day, "daily replay");
    CHECK(westFirst.encode() == daily.encode());
    CHECK(westFirst.meltwaterCm3() > 0 && westFirst.storedVolume() > 0);
    EQUAL(westFirst.storedVolume() + westFirst.pendingVolume() +
              westFirst.sinkVolume() +
              std::llround(westFirst.meltwaterCm3() * snow::SnowPerWater),
          westFirst.depositedVolume(), "full-year material balance");
}

TEST(weather_history_snow_legacy_and_bad_versions) {
    Fixture terrain;
    snow::SnowSettlement snow(terrain);
    for (const unsigned version : {1, 2}) {
        BinaryWriter legacy;
        legacy.writeTag("SNOW");
        legacy.write<uint32_t>(version);
        legacy.write<uint64_t>(0);                    // supplied
        legacy.write<uint64_t>(0);                    // sink
        if (version == 2) legacy.write<int64_t>(0);   // melted
        legacy.write<uint32_t>(0);                    // resting columns
        legacy.write<uint32_t>(0);                    // pending transfers
        if (version == 2) legacy.write<uint32_t>(0);  // meltwater
        CHECK(snow.restore(legacy.take()));
        snow.beginWeather(37.5);
        EQUAL(snow.weatherDay(0, 0), 37.5, "legacy save starts at its date");
    }
    const auto before = snow.encode();
    auto bad = before;
    bad[44] = std::byte{0xff};  // history simulation version in empty v3
    CHECK(!snow.restore(bad));
    CHECK(snow.encode() == before);
}

TEST(weather_history_pending_transfer_survives_checkpoint) {
    Fixture terrain;
    solid(terrain, 0, 2, 0);
    snow::SnowSettlement snow(terrain);
    snow.beginWeather(0);
    snow.setWeatherForce(0, 4);
    replay(snow, terrain, 0, 0, SixHours);
    CHECK(snow.deposit(0, 0, 3, 2500, 0) == snow::DepositOutcome::Pending);
    snow.setWeatherForce(SixHours, 1);

    snow::SnowSettlement loaded(terrain);
    CHECK(loaded.restore(snow.encode()));
    EQUAL(loaded.weatherDay(0, 0), SixHours, "checkpoint retains cursor");
    EQUAL(loaded.pendingVolume(), 2500, "checkpoint retains queued transfer");
    EQUAL(loaded.settle(), size_t(1), "queued transfer commits once");
    replay(loaded, terrain, 0, 0, 2 * SixHours);
    EQUAL(loaded.volumeAt(0, 0), 47500, "old snow and transfer thaw together");
    EQUAL(loaded.depositedVolume(), 122500, "past snowfall is counted once");
    EQUAL(loaded.pendingVolume(), 0, "transit queue drained");
}

TEST(snow_cover_refreshes_only_changed_resident_columns) {
    struct CountingTerrain : Fixture {
        mutable size_t eastReads = 0;
        std::optional<CellGeometry> cell(TilePos pos) const override {
            if (pos.x >= 32) ++eastReads;
            return Fixture::cell(pos);
        }
    } terrain;
    terrain.x1 = 34;
    solid(terrain, 1, 2, 0);
    solid(terrain, 33, 2, 0);
    snow::SnowSettlement snow(terrain);
    for (int x : {1, 33})
        CHECK(snow.snowfall(x, 0, 3, 50000) == snow::DepositOutcome::Settled);

    snow::SnowCover cover;
    snow::ChunkColumns resident = {{0, 0}, {2, 0}};
    auto dirty = snow.takeProjectionChanges();
    CHECK(dirty == resident);
    cover.rebuild(snow, terrain, 42, &resident, &dirty);
    const auto initial = cover.environment();
    const ChunkCoord east{2, 0, 0};

    EQUAL(snow.melt({1, 0, 3}, 25000), 25000, "partly melt west pad");
    dirty = snow.takeProjectionChanges();
    EQUAL(dirty.size(), size_t(1), "one chunk column is dirty");
    terrain.eastReads = 0;
    cover.rebuild(snow, terrain, 42, &resident, &dirty);
    EQUAL(terrain.eastReads, size_t(0), "untouched geometry is not read");
    CHECK(cover.environment()->at(east) == initial->at(east));
    EQUAL(cover.changedChunks().size(), size_t(1), "only west slab published");
    CHECK(cover.cellAt({1, 3, 0}));

    resident.erase({0, 0});
    cover.rebuild(snow, terrain, 42, &resident, &dirty);
    CHECK(!cover.cellAt({1, 3, 0}));
    CHECK(cover.environment()->at(east) == initial->at(east));
    EQUAL(snow.volumeAt(1, 0), 25000, "unload preserves west material ledger");
    CHECK(initial->contains({0, 0, 0}));
}
