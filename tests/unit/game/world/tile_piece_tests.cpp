#include <array>
#include <cmath>
#include <limits>
#include <span>

#include "test.hpp"
#include "world/tile_piece.hpp"

using namespace tile_geometry;

namespace {

void near(float actual, float expected) {
    CHECK(std::abs(actual - expected) < 5e-6f);
}

float polygonArea(std::span<const Vec3> vertices) {
    Vec3 area{};
    for (size_t i = 2; i < vertices.size(); ++i) {
        const auto a = vertices[i - 1] - vertices[0];
        const auto b = vertices[i] - vertices[0];
        area = area + Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                           a.x * b.y - a.y * b.x};
    }
    return .5f * std::sqrt(area.x * area.x + area.y * area.y + area.z * area.z);
}

float openingArea(const CellGeometry& cell, Face face) {
    float area = 0;
    for (const auto& polygon : cell.boundaryOpenings(face))
        area += polygonArea(polygon);
    return area;
}

float exteriorArea(const CellGeometry& cell) {
    float area = 0;
    for (const auto& polygon : cell.exteriorFaces()) {
        CHECK(polygon.vertices.size() >= 3);
        for (auto p : polygon.vertices) {
            CHECK(p.x >= -1e-6f && p.x <= 1 + 1e-6f);
            CHECK(p.y >= -1e-6f && p.y <= 1 + 1e-6f);
            CHECK(p.z >= -1e-6f && p.z <= 1 + 1e-6f);
            const auto n = polygon.plane.normal;
            near(n.x * p.x + n.y * p.y + n.z * p.z, polygon.plane.offset);
        }
        area += polygonArea(polygon.vertices);
    }
    return area;
}

}  // namespace

TEST(tile_piece_layers) {
    constexpr std::array layers{
        LayerThickness::Cm5,  LayerThickness::Cm10, LayerThickness::Cm25,
        LayerThickness::Cm50, LayerThickness::Cm75, LayerThickness::Cm100,
    };
    const CellGeometry empty;
    CHECK(!empty.contains({.5f, .5f, .5f}));
    near(openingArea(empty, Face::PosY), 1);
    const auto emptyGaps = empty.freeIntervals({.5f, .5f});
    CHECK(emptyGaps.size() == 1);
    near(emptyGaps.front().low, 0);
    near(emptyGaps.front().high, 1);
    for (auto thickness : layers) {
        const float metres = float(uint8_t(thickness)) / 100;
        auto layer = TilePiece::layer(7, 42, thickness);
        if (thickness == LayerThickness::Cm50)
            CHECK(layer.shape == TileShape::make(TileShapeProfile::Slab));
        CellGeometry cell;
        CHECK(cell.tryAdd(layer) == PiecePlacement::Added);
        CHECK(cell.contains({.3f, metres * .5f, .6f}));
        near(cell.supportHeight({.1f, .1f}, {.9f, .9f}).value(), metres);
        near(openingArea(cell, Face::PosX), 1 - metres);
        near(openingArea(cell, Face::NegY), 0);
        near(exteriorArea(cell), 2 + 4 * metres);
        const auto ray = cell.intersectRay({.3f, 2, .6f}, {0, -2, 0});
        CHECK(ray && ray->id == 7 && ray->blockId == 42);
        near(ray->hit.t, (2 - metres) / 2);
        CHECK((ray->hit.normal == Vec3{0, 1, 0}));
        const auto spaces = cell.freeIntervals({.3f, .6f});
        if (thickness == LayerThickness::Cm100) {
            CHECK(spaces.empty());
        } else {
            CHECK(spaces.size() == 1);
            near(spaces.front().low, metres);
            near(spaces.front().high, 1);
        }
    }

    CellGeometry bridge;
    CHECK(bridge.tryAdd(
              TilePiece::layer(11, 4, LayerThickness::Cm50, {0, .25f, 0})) ==
          PiecePlacement::Added);
    CHECK(bridge.tryAdd(TilePiece::layer(9, 2, LayerThickness::Cm5)) ==
          PiecePlacement::Added);
    CHECK(!bridge.contains({.5f, .2f, .5f}));
    CHECK(bridge.contains({.5f, .25f, .5f}));
    near(bridge.supportHeight({0, 0}, {1, 1}).value(), .75f);
    near(bridge.supportHeight({0, 0}, {1, 1}, .2f).value(), .05f);
    const auto gaps = bridge.freeIntervals({.5f, .5f});
    CHECK(gaps.size() == 2);
    near(gaps[0].low, .05f);
    near(gaps[0].high, .25f);
    near(gaps[1].low, .75f);
    near(gaps[1].high, 1);
    near(openingArea(bridge, Face::NegX), .45f);
    near(openingArea(bridge, Face::PosY), 1);
    const auto underside = bridge.intersectRay({.5f, .1f, .5f}, {0, 1, 0});
    CHECK(underside && underside->id == 11 && underside->blockId == 4);
    near(underside->hit.t, .15f);
    CHECK((underside->hit.normal == Vec3{0, -1, 0}));
}

TEST(tile_piece_free_volume) {
    const auto volumesNear = [](double actual, double expected,
                                double tolerance) {
        CHECK(std::abs(actual - expected) <= tolerance);
    };
    const CellGeometry empty;
    volumesNear(empty.freeVolumeBelow(.25f), .25, 1e-9);
    CHECK(!empty.isFull());
    CHECK(!empty.lowestUpwardSupport());

    CellGeometry cube;
    CHECK(cube.tryAdd(TilePiece{1, 2, {}, {}, PieceFill::BelowProfile, 1}) ==
          PiecePlacement::Added);
    CHECK(cube.isFull());
    volumesNear(cube.freeVolumeBelow(1), 0, 1e-9);
    near(cube.lowestUpwardSupport().value(), 1);

    CellGeometry layer;
    CHECK(layer.tryAdd(TilePiece::layer(1, 2, LayerThickness::Cm5)) ==
          PiecePlacement::Added);
    volumesNear(layer.freeVolumeBelow(.05f), 0, 1e-7);
    volumesNear(layer.freeVolumeBelow(.1f), .05, 1e-7);
    near(layer.lowestUpwardSupport().value(), .05f);
    near(layer.lowestOpening(Face::NegX).value(), .05f);

    CellGeometry ramp;
    CHECK(ramp.tryAdd(TilePiece{
              1,
              2,
              TileShape::make(TileShapeProfile::Ramp, TileFacing::PosZ),
              {},
              PieceFill::BelowProfile,
              1}) == PiecePlacement::Added);
    // A 45-degree ramp one metre wide over one metre of run: the 5 cm level
    // clips a 5 cm long triangular pocket.
    volumesNear(ramp.freeVolumeBelow(.05f), .00125, 1e-7);
    volumesNear(ramp.freeVolumeBelow(1.f), .5, 1e-7);
    near(ramp.lowestUpwardSupport().value(), 0);
    near(ramp.lowestOpening(Face::NegZ).value(), 0);

    CellGeometry bridge;
    CHECK(bridge.tryAdd(
              TilePiece::layer(1, 2, LayerThickness::Cm50, {0, .25f, 0})) ==
          PiecePlacement::Added);
    volumesNear(bridge.freeVolumeBelow(.5f), .25, 1e-7);
    near(bridge.lowestUpwardSupport().value(), .75f);
    near(bridge.lowestOpening(Face::NegX).value(), 0);

    // The ramp infill used by the builder fills the inverse wedge above the
    // rock profile up to its cap. Below 5 cm the cell is solid; above it the
    // free space is the same shape a rising landing level would leave.
    const auto rampShape =
        TileShape::make(TileShapeProfile::Ramp, TileFacing::PosZ);
    CellGeometry infill;
    CHECK(infill.tryAdd(
              TilePiece{1, 2, rampShape, {}, PieceFill::BelowProfile, 1}) ==
          PiecePlacement::Added);
    CHECK(infill.tryAdd(
              TilePiece{2, 2, rampShape, {}, PieceFill::AboveProfile, .05f}) ==
          PiecePlacement::Added);
    volumesNear(infill.freeVolumeBelow(.05f), 0, 1e-7);
    volumesNear(infill.freeVolumeBelow(.1f), .00375, 1e-7);
}

TEST(tile_piece_complementary_profiles) {
    // Exercise low/high and fractional junctions as well as the full ramps.
    for (unsigned profile = 2; profile <= 24; ++profile) {
        if (profile >= 6 && profile <= 9) continue;
        for (unsigned turn = 0; turn < 4; ++turn) {
            const auto shape =
                TileShape::make(TileShapeProfile(profile), TileFacing(turn));
            const auto sample = rotatePoint({.2f, 0, .6f}, shape.facing());
            const auto height =
                supportHeight(shape, sample.x, sample.z).value();
            const TilePiece lower{8, 12, shape};
            const TilePiece upper{3, 32, shape, {}, PieceFill::AboveProfile};
            CellGeometry base;
            CHECK(base.tryAdd(lower) == PiecePlacement::Added);

            const auto legacy = surface(shape);
            float legacyArea = 0;
            for (unsigned i = 0; i < legacy.count; ++i) {
                const auto& face = legacy.faces[i];
                legacyArea +=
                    polygonArea(std::span(face.vertices).first(face.count));
            }
            near(exteriorArea(base), legacyArea);
            CHECK(base.contains({sample.x, .5f * height, sample.z}));
            CHECK(!base.contains({sample.x, .5f * (1 + height), sample.z}));
            auto ray = base.intersectRay({sample.x, 2, sample.z}, {0, -1, 0});
            CHECK(ray && ray->id == lower.id);
            near(ray->hit.t, 2 - height);
            const Vec2 low{sample.x - .05f, sample.z - .05f};
            const Vec2 high{sample.x + .05f, sample.z + .05f};
            near(base.supportHeight(low, high).value(),
                 supportHeight(shape, low, high).value());
            near(base.freeIntervals({sample.x, sample.z}).at(0).low, height);

            CellGeometry cell;
            CHECK(cell.tryAdd(upper) == PiecePlacement::Added);
            ray = cell.intersectRay({sample.x, -1, sample.z}, {0, 1, 0});
            CHECK(ray && ray->id == upper.id && ray->hit.normal.y < 0);
            near(ray->hit.t, 1 + height);
            near(cell.freeIntervals({sample.x, sample.z}).at(0).high, height);
            CHECK(cell.tryAdd(lower) == PiecePlacement::Added);
            CHECK(base.tryAdd(upper) == PiecePlacement::Added);
            near(exteriorArea(cell), 6);
            near(exteriorArea(base), 6);
            CHECK(cell.freeIntervals({sample.x, sample.z}).empty());
            near(cell.supportHeight(low, high).value(), 1);
            CHECK(!cell.supportHeight(low, high, .99f));
            for (unsigned face = 0; face < 6; ++face)
                near(openingArea(cell, Face(face)), 0);
            const auto top =
                cell.intersectRay({sample.x, 2, sample.z}, {0, -1, 0});
            const auto bottom =
                cell.intersectRay({sample.x, -1, sample.z}, {0, 1, 0});
            CHECK(top && top->id == upper.id && top->blockId == upper.blockId);
            CHECK(bottom && bottom->id == lower.id &&
                  bottom->blockId == lower.blockId);
            const auto seam =
                cell.intersectRay({sample.x, height, sample.z}, {0, 1, 0});
            CHECK(seam && seam->hit.t == 0 && seam->hit.normal == Vec3{});
        }
    }

    const auto ramp = TileShape::make(TileShapeProfile::Ramp);
    CellGeometry toe;
    CHECK(toe.tryAdd({1, 12, ramp}) == PiecePlacement::Added);
    CHECK(toe.tryAdd({2, 32, ramp, {}, PieceFill::AboveProfile, .05f}) ==
          PiecePlacement::Added);
    const auto lower = toe.intersectRay({.5f, 2, .025f}, {0, -1, 0});
    const auto upper = toe.intersectRay({.5f, 2, .25f}, {0, -1, 0});
    CHECK(lower && lower->blockId == 32);
    CHECK(upper && upper->blockId == 12);
    near(lower->hit.t, 1.95f);
    near(upper->hit.t, 1.75f);
    near(toe.supportHeight({.25f, 0}, {.75f, .05f}).value(), .05f);
    near(toe.freeIntervals({.5f, .025f}).at(0).low, .05f);
    near(openingArea(toe, Face::NegZ), .95f);
}

TEST(tile_piece_wall_and_layer) {
    for (unsigned turn = 0; turn < 4; ++turn) {
        const auto facing = TileFacing(turn);
        const auto rotate = [=](Vec3 p) { return rotatePoint(p, facing); };
        const auto point = [&](Vec3 p) {
            const auto rotated = rotate(p);
            return Vec2{rotated.x, rotated.z};
        };
        const auto offset = rotate({0, .25f, -.1f}) - rotate({});
        CellGeometry cell;
        CHECK(cell.tryAdd(
                  {9, 17, TileShape::make(TileShapeProfile::Wall10, facing)}) ==
              PiecePlacement::Added);
        CHECK(cell.tryAdd(TilePiece::layer(3, 27, LayerThickness::Cm25,
                                           offset)) == PiecePlacement::Added);
        CHECK(cell.contains(rotate({.5f, .3f, .5f})));
        CHECK(!cell.contains(rotate({.5f, .6f, .5f})));
        CHECK(cell.freeIntervals(point({.5f, 0, .95f})).empty());
        const auto gaps = cell.freeIntervals(point({.5f, 0, .5f}));
        CHECK(gaps.size() == 2);
        near(gaps[0].high, .25f);
        near(gaps[1].low, .5f);
        near(openingArea(cell, Face::PosY), .9f);
        near(openingArea(cell, Face::NegY), .9f);
        // The layer covers the middle 25 cm of the wall's inward face.
        const auto inward = rotate({0, 0, -1}) - rotate({});
        float inwardArea = 0;
        for (const auto& face : cell.exteriorFaces())
            if (face.id == 9 && face.plane.normal == inward)
                inwardArea += polygonArea(face.vertices);
        near(inwardArea, .75f);

        const auto a = point({.4f, 0, .4f}), b = point({.6f, 0, .6f});
        near(cell.supportHeight({std::min(a.x, b.x), std::min(a.y, b.y)},
                                {std::max(a.x, b.x), std::max(a.y, b.y)})
                 .value(),
             .5f);
        const auto ray =
            cell.intersectRay(rotate({.5f, .75f, .5f}), inward * -1);
        CHECK(ray && ray->id == 9 && ray->blockId == 17);
        near(ray->hit.t, .4f);
        CHECK(ray->hit.normal == inward);
        const auto lowerHit =
            cell.intersectRay(rotate({.5f, 0, .5f}), {0, 1, 0});
        CHECK(lowerHit && lowerHit->id == 3 && lowerHit->blockId == 27);
        near(lowerHit->hit.t, .25f);
        const auto beyond =
            cell.intersectRay(rotate({.5f, 0, .5f}), {0, 1, 0}, 0, .2f);
        CHECK(!beyond);
    }
}

TEST(tile_piece_clipping_and_rejection) {
    CellGeometry cell;
    const auto ramp = TileShape::make(TileShapeProfile::DiagonalRamp);
    CHECK(cell.tryAdd({1, 0, ramp}) == PiecePlacement::Invalid);
    CHECK(cell.tryAdd({1, 1, ramp, {0, 0, 1}}) == PiecePlacement::Empty);
    CHECK(cell.tryAdd({1, 1, ramp, {0, -1, 0}}) == PiecePlacement::Empty);
    CHECK(cell.tryAdd(
              {1, 1, ramp, {0, std::numeric_limits<float>::infinity(), 0}}) ==
          PiecePlacement::Invalid);
    CHECK(cell.tryAdd({1, 1, ramp, {-.25f, -.25f, 0}}) ==
          PiecePlacement::Added);
    CHECK(cell.contains({0, .125f, .5f}));
    CHECK(!cell.contains({.8f, .125f, .5f}));
    CHECK(!cell.contains({-.1f, .125f, .5f}));
    CHECK(!cell.supportHeight({.75f, .5f}, {1, 1}));
    CHECK(!cell.supportHeight({.5f, .5f}, {.1f, 1}));
    CHECK(cell.freeIntervals({-.1f, 0}).empty());
    const auto side = cell.intersectRay({-1, .125f, .5f}, {1, 0, 0}, 1, 1);
    CHECK(side && side->id == 1);
    CHECK((side->hit.normal == Vec3{-1, 0, 0}));
    CHECK(cell.tryAdd({1, 1, ramp}) == PiecePlacement::DuplicateId);
    CHECK(cell.tryAdd({2, 33, ramp}) == PiecePlacement::Overlap);
    const auto first = cell.intersectRay({.5f, 2, .5f}, {0, -1, 0});
    CHECK(first && first->id == 1 && first->blockId == 1);
    near(first->hit.t, 1.625f);

    CellGeometry contact;
    const auto lower = TilePiece::layer(4, 2, LayerThickness::Cm50);
    auto upper = TilePiece::layer(2, 3, LayerThickness::Cm50, {0, .49f, 0});
    CHECK(contact.tryAdd(lower) == PiecePlacement::Added);
    CHECK(contact.tryAdd(upper) == PiecePlacement::Overlap);
    upper.offset.y = .5f;
    CHECK(contact.tryAdd(upper) == PiecePlacement::Added);
    CHECK(contact.freeIntervals({.5f, .5f}).empty());
    near(exteriorArea(contact), 6);
    const auto tie = contact.intersectRay({-1, .5f, .5f}, {1, 0, 0});
    CHECK(tie && tie->id == 2);

    CellGeometry edgeContact;
    const auto left =
        TilePiece::layer(1, 2, LayerThickness::Cm50, {-.5f, 0, 0});
    auto right = TilePiece::layer(2, 3, LayerThickness::Cm50, {.5f, .49f, 0});
    CHECK(edgeContact.tryAdd(left) == PiecePlacement::Added);
    CHECK(edgeContact.tryAdd(right) == PiecePlacement::Added);
    near(exteriorArea(edgeContact), 4.98f);  // 1 cm of face contact.
    right.offset.y = .5f;
    CellGeometry edgeOnly;
    CHECK(edgeOnly.tryAdd(right) == PiecePlacement::Added);
    CHECK(edgeOnly.tryAdd(left) == PiecePlacement::Added);
    near(exteriorArea(edgeOnly), 5);
    near(edgeOnly.supportHeight({0, 0}, {.5f, 1}).value(), .5f);
}
