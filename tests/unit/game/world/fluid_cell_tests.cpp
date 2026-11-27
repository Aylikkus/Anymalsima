#include <cmath>

#include "test.hpp"
#include "world/fluid_cell.hpp"

namespace {
using tile_geometry::CellGeometry;
using tile_geometry::LayerThickness;
using tile_geometry::TilePiece;

CellGeometry slab() {
    CellGeometry geometry;
    CHECK(geometry.tryAdd(TilePiece::layer(1, 1, LayerThickness::Cm50)) ==
          tile_geometry::PiecePlacement::Added);
    return geometry;
}

CellGeometry elevatedLid() {
    CellGeometry geometry;
    CHECK(geometry.tryAdd(
              TilePiece::layer(1, 1, LayerThickness::Cm25, {0, .75f, 0})) ==
          tile_geometry::PiecePlacement::Added);
    return geometry;
}

CellGeometry sealedMiddle() {
    CellGeometry geometry;
    CHECK(geometry.tryAdd(
              TilePiece::layer(1, 1, LayerThickness::Cm25, {0, .5f, 0})) ==
          tile_geometry::PiecePlacement::Added);
    return geometry;
}

CellGeometry closedCube() {
    CellGeometry geometry;
    CHECK(geometry.tryAdd(TilePiece::layer(1, 1, LayerThickness::Cm100)) ==
          tile_geometry::PiecePlacement::Added);
    return geometry;
}

CellGeometry wall() {
    CellGeometry geometry;
    TilePiece piece;
    piece.id = 1;
    piece.blockId = 1;
    piece.shape = TileShape::make(TileShapeProfile::Wall25);
    CHECK(geometry.tryAdd(piece) == tile_geometry::PiecePlacement::Added);
    return geometry;
}

bool near(float a, float b, float epsilon = 0.02f) {
    return std::abs(a - b) <= epsilon;
}
}  // namespace

TEST(fluid_profile_bottom_slab_capacity_and_openings) {
    const auto profile = fluid::Profile::make(slab());
    CHECK(!profile.uniform);
    CHECK(near(profile.capacity, .5f));
    CHECK(near(profile.ceiling, 1.f));
    // Water below the slab has no space in this cell.
    EQUAL(profile.volumeAt(.25f), 0.f, "space below the slab is solid");
    CHECK(near(profile.volumeAt(.75f), .25f));
    CHECK(near(profile.levelFor(profile.capacity * .5f), .75f));
    // The floor is closed; the sides open above the slab and the top is open.
    CHECK(profile.closed(Face::NegY));
    CHECK(!profile.closed(Face::PosY));
    CHECK(near(profile.openingLowest[uint8_t(Face::PosX)], .5f));
    EQUAL(profile.unitsAt(1.f), 128, "the free half is the full unit capacity");
    CHECK(near(profile.levelForUnits(32), .625f));
}

TEST(fluid_profile_elevated_lid_seals_the_upper_pocket) {
    const auto profile = fluid::Profile::make(elevatedLid());
    // Liquid rests below the lid and cannot use the sealed space above it.
    CHECK(near(profile.capacity, .75f));
    CHECK(near(profile.ceiling, .75f));
    CHECK(near(profile.volumeAt(1.f), .75f));
    CHECK(profile.hasOpening(Face::NegY));
    CHECK(!profile.closed(Face::PosX));
}

TEST(fluid_profile_middle_slab_distinguishes_disconnected_pockets) {
    const auto profile = fluid::Profile::make(sealedMiddle());
    // Only the lower run is reachable from the cell floor. The upper run is
    // a sealed pocket and does not contribute to resting capacity.
    CHECK(near(profile.capacity, .5f));
    CHECK(near(profile.ceiling, .5f));
    CHECK(near(profile.volumeAt(.9f), .5f));
}

TEST(fluid_profile_closed_face_and_side_gap) {
    const auto closed = fluid::Profile::make(closedCube());
    EQUAL(closed.capacity, 0.f, "a closed cube has no water capacity");
    for (int i = 0; i < 6; ++i) CHECK(closed.closed(Face(i)));

    const auto gap = fluid::Profile::make(wall());
    // The canonical wall covers the +Z edge, leaving the other faces open.
    CHECK(gap.closed(Face::PosZ));
    CHECK(!gap.closed(Face::NegZ));
    CHECK(gap.openingArea[uint8_t(Face::NegZ)] > .9f);
    CHECK(gap.openingArea[uint8_t(Face::PosX)] > .1f);

    const auto open = fluid::Profile::open();
    CHECK(!fluid::sharedOpening(gap, open, Face::PosZ));
    CHECK(fluid::sharedOpening(gap, open, Face::NegZ));
    CHECK(fluid::sharedOpening(open, gap, Face::PosZ));
}

TEST(fluid_profile_ramp_shoreline_is_submerged_up_to_the_head) {
    CellGeometry geometry;
    TilePiece piece;
    piece.id = 1;
    piece.blockId = 1;
    piece.shape = TileShape::make(TileShapeProfile::Ramp);
    CHECK(geometry.tryAdd(piece) == tile_geometry::PiecePlacement::Added);
    const auto profile = fluid::Profile::make(geometry);
    // A canonical ramp rises from 0 at -Z to 1 at +Z. Free volume below a
    // level h is the triangular prism h^2 / 2.
    CHECK(near(profile.capacity, .5f));
    CHECK(near(profile.volumeAt(.5f), .125f, .01f));
    CHECK(near(profile.volumeAt(.75f), .28125f, .01f));
    CHECK(near(profile.volumeAt(1.f), .5f, .01f));
    CHECK(near(profile.openingLowest[uint8_t(Face::NegZ)], 0.f, .01f));
    // The uphill face is covered to the full height.
    CHECK(profile.closed(Face::PosZ));
}

TEST(fluid_profile_open_cell_is_uniform) {
    const auto open = fluid::Profile::open();
    CHECK(open.uniform);
    EQUAL(open.capacity, 1.f, "an open cell holds one block");
    CHECK(near(open.volumeAt(.4f), .4f));
    CHECK(near(open.levelFor(.4f), .4f));
    EQUAL(open.unitsAt(1.f), 255, "an open cell tops out at 255 units");
    CHECK(near(open.levelForUnits(64), 64.f / 255.f));
    CHECK(near(open.levelForUnits(128), 128.f / 255.f));
}
