#include <cmath>
#include <map>
#include <optional>
#include <tuple>

#include "test.hpp"
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
    int periodX = 0;
    int x0 = 0, x1 = 1;
    int z0 = 0, z1 = 1;
    int lowY = 0, highY = 20;

    std::optional<CellGeometry> cell(TilePos pos) const override {
        pos.x = wrapX(pos.x);
        if (pos.y < lowY || pos.y >= highY || pos.z < z0 || pos.z >= z1)
            return std::nullopt;
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
