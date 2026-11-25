#include <cmath>

#include "world/tile_shape.hpp"
#include "world_fixture.hpp"

using namespace tile_geometry;

namespace {

TileShape shape(int profile, unsigned turns = 0) {
    return *TileShape::fromCode(uint8_t(profile * 4 + turns));
}

void near(float actual, float expected, const std::string& context) {
    test::equal(std::abs(actual - expected) < 1e-6f, true,
                context + " expected " + std::to_string(expected) + ", got " +
                    std::to_string(actual),
                __FILE__, __LINE__);
}

float height(TileShape code, Vec3 point) {
    return supportHeight(code, point.x, point.z).value();
}

float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

}  // namespace

TEST(tile_shape_profiles) {
    for (unsigned code = 96; code < 256; ++code)
        CHECK(!TileShape::fromCode(uint8_t(code)));
    for (int id = 0; id < 24; ++id) {
        for (unsigned turn = 0; turn < 4; ++turn) {
            const auto code = shape(id, turn);
            EQUAL(int(code.code()), id * 4 + (id <= 1 ? 0 : int(turn)),
                  "stable profile code");
            CHECK(code.rotated(1).rotated(1).rotated(1).rotated(1) == code);
            const auto geometry = surface(code);
            CHECK(geometry.count >= 4);
            for (unsigned f = 0; f < geometry.count; ++f) {
                const auto& face = geometry.faces[f];
                near(dot(face.plane.normal, face.plane.normal), 1,
                     "unit outward normal");
                for (unsigned i = 0; i < face.count; ++i) {
                    const Vec3 p = face.vertices[i];
                    CHECK(p.x >= 0 && p.x <= 1 && p.y >= 0 && p.y <= 1 &&
                          p.z >= 0 && p.z <= 1);
                    near(dot(face.plane.normal, p), face.plane.offset,
                         "vertex on face plane");
                    CHECK(contains(code, p));
                }
            }
        }
    }
    near(height(shape(0), {.5f, 0, .5f}), 1, "cube height");
    near(height(shape(1), {.5f, 0, .5f}), .5f, "slab height");
    CHECK(!boundaryFace(shape(1), Face::PosY));
    CHECK(boundaryFace(shape(1), Face::PosX)->count == 4);
    CHECK(boundaryFace(shape(2), Face::PosX)->count == 3);
    CHECK(!boundaryFace(shape(2), Face::NegZ));
    CHECK(!boundaryFace(shape(6), Face::NegZ));
    CHECK(boundaryFace(shape(6), Face::PosZ)->count == 4);
    CHECK(boundaryFace(shape(6, 1), Face::PosX)->count == 4);
}

TEST(tile_shape_seams) {
    constexpr int straight[] = {2, 10, 11, 3, 4, 5};
    constexpr int segments[] = {0, 0, 1, 0, 1, 2};
    constexpr int runs[] = {1, 2, 2, 3, 3, 3};
    for (unsigned turn = 0; turn < 4; ++turn) {
        const auto facing = TileFacing(turn);
        const auto rotate = [=](Vec3 p) { return rotatePoint(p, facing); };
        for (int i : {1, 3, 4}) {
            for (float x : {0.f, .5f, 1.f})
                EQUAL(height(shape(straight[i], turn), rotate({x, 0, 1})),
                      height(shape(straight[i + 1], turn), rotate({x, 0, 0})),
                      "straight segments share exact endpoints");
        }

        for (int interval = 0; interval < 6; ++interval) {
            const int profile = straight[interval];
            const float low = float(segments[interval]);
            const float run = float(runs[interval]);
            for (bool inner : {false, true}) {
                const auto corner = shape((inner ? 18 : 12) + interval, turn);
                const float edge = inner ? 0 : 1;
                for (float along : {0.f, .5f, 1.f}) {
                    const auto alongZ = shape(profile, turn);
                    const auto alongX = shape(profile, (turn + 1) % 4);
                    const auto edgeX = rotate({edge, 0, along});
                    const auto edgeZ = rotate({along, 0, edge});
                    // Translate each canonical edge into its neighboring
                    // cell before rotating. Endpoints keep the same order.
                    EQUAL(height(corner, edgeX),
                          height(alongZ, rotate({1 - edge, 0, along})),
                          "shared X edge");
                    EQUAL(height(corner, edgeZ),
                          height(alongX, rotate({along, 0, 1 - edge})),
                          "shared Z edge");
                    near(height(corner, edgeX), (low + along) / run,
                         "ordered X edge endpoint or midpoint");
                    near(height(corner, edgeZ), (low + along) / run,
                         "ordered Z edge endpoint or midpoint");
                }

                for (auto p : {Vec3{.75f, 0, .25f}, Vec3{.25f, 0, .75f},
                               Vec3{.5f, 0, .5f}}) {
                    const float rise =
                        inner ? std::max(p.x, p.z) : std::min(p.x, p.z);
                    const auto local = rotate(p);
                    const float expected = (low + rise) / run;
                    near(height(corner, local), expected, "corner top height");
                    const auto hit =
                        intersectRay(corner, {local.x, 2, local.z}, {0, -1, 0});
                    CHECK(hit);
                    near(hit->t, 2 - expected, "corner facet ray");
                    CHECK(hit->normal.y > 0);
                    const auto geometry = surface(corner);
                    const unsigned facet = p.x >= p.z ? 2 : 1;
                    near(dot(hit->normal, geometry.faces[geometry.count - facet]
                                              .plane.normal),
                         1, "ray and surface facet normals agree");
                }
            }
        }
    }
}

TEST(tile_shape_intersections) {
    for (unsigned turn = 0; turn < 4; ++turn) {
        const auto facing = TileFacing(turn);
        const auto wall = shape(6, turn);
        const auto open = rotatePoint({.5f, 2, .5f}, facing);
        CHECK(!supportHeight(wall, open.x, open.z));
        CHECK(!intersectRay(wall, open, {0, -1, 0}));
        const auto filled = rotatePoint({.5f, 2, .95f}, facing);
        const auto hit = intersectRay(wall, filled, {0, -1, 0});
        CHECK(hit);
        near(hit->t, 1, "wall top ray");
        CHECK(!intersectRay(wall, filled, {0, -1, 0}, 0, .5f));
        const auto inset = rotatePoint({.5f, .5f, 0}, facing);
        const auto direction =
            rotatePoint({0, 0, 1}, facing) - rotatePoint({}, facing);
        const auto side = intersectRay(wall, inset, direction);
        CHECK(side);
        near(side->t, .9f, "wall inset ray");
        near(dot(side->normal, direction), -1, "wall inset normal");
    }
    const auto inner = shape(18);
    const auto parts = convexParts(inner);
    CHECK(parts.count == 2);
    for (const auto& part : parts.parts)
        CHECK(part.count == part.exteriorCount + 1);
    const auto cross = intersectRay(inner, {-1, .25f, .5f}, {1, 0, 0});
    CHECK(cross);
    near(cross->t, 1, "cross-diagonal ray enters through outer side");
    CHECK((cross->normal == Vec3{-1, 0, 0}));
    const auto atBoundary =
        intersectRay(inner, {-1, .25f, .5f}, {1, 0, 0}, 1, 2);
    CHECK(atBoundary && atBoundary->normal == cross->normal);
    const auto aboveCrease = intersectRay(inner, {.1f, .7f, .5f}, {1, 0, 0});
    CHECK(aboveCrease);
    near(aboveCrease->t, .6f, "diagonal crossing in air hits the later facet");
    CHECK(aboveCrease->normal.x < 0 && aboveCrease->normal.y > 0);
    const auto inside = intersectRay(inner, {.25f, .1f, .5f}, {1, 0, 0});
    CHECK(inside && inside->t == 0 && inside->normal == Vec3{});
}

TEST(tile_shape_storage) {
    Chunk chunk;
    const Tile solid{.block_id = 1, .lightLevel = {}};
    const Tile water{.liquid = LiquidType::Water, .lightLevel = {}};
    const auto low = Chunk::cell(1, 2, 3);
    const auto high = Chunk::cell(15, 15, 15);
    const auto wall = shape(6, 2);
    const auto ramp = shape(2, 1);
    chunk.setCell(low, solid);
    chunk.setCell(high, water, ramp);
    CHECK(!chunk.hasShapes());
    chunk.setCell(high, solid, ramp);
    chunk.setCell(low, solid, wall);
    CHECK(chunk.hasShapes());
    CHECK(chunk.cellAt(1, 2, 3).shape == wall);
    CHECK(chunk.shapeAt(high) == ramp);
    CHECK(chunk.shapeAt(low + 1) == TileShape{});

    const Chunk snapshot = chunk;
    chunk.setCell(high, solid, wall);
    chunk.setCell(low, {}, ramp);
    CHECK(chunk.cellAt(1, 2, 3).tile.isAir());
    CHECK(chunk.shapeAt(low) == TileShape{});
    CHECK(chunk.shapeAt(high) == wall);
    CHECK(snapshot.cellAt(1, 2, 3).tile.isSolid());
    CHECK(snapshot.shapeAt(low) == wall && snapshot.shapeAt(high) == ramp);
    chunk.setCell(high, solid);
    CHECK(!chunk.hasShapes());
}

TEST(tile_shape_snapshots) {
    test::WorldFixture f;
    const ChunkCoord coord{1, 0, 0};
    f.load(coord);
    constexpr auto index = Chunk::cell(0, 2, 3);
    const Tile first{.block_id = 1, .lightLevel = {}};
    const Tile second{.block_id = 2, .lightLevel = {}};
    const auto ramp = shape(2, 1);
    const auto wall = shape(6, 2);
    const auto edit = [&](Tile tile, TileShape code) {
        CHECK(f.chunks.withChunkLocked(
            coord, [&](Chunk& chunk) { chunk.setCell(index, tile, code); }));
        f.chunks.markModified(coord);
    };
    edit(first, ramp);
    const auto region = f.chunks.snapshotRegion({0, 0});
    RegionSnapshot column;
    edit(first, ramp);
    f.chunks.snapshotColumn(1, 0, column);
    edit(first, wall);
    CHECK(f.chunks.isPersistDirty(coord));
    CHECK(f.chunks.cellAt(16, 2, 3)->shape == wall);
    edit(second, wall);
    const auto current = f.chunks.cellAt(16, 2, 3);
    CHECK(current && current->tile.block_id == 2 && current->shape == wall);
    CHECK(!f.chunks.cellAt(15, -1, 3));
    for (const auto* snapshot :
         {&region, static_cast<const RegionSnapshot*>(&column)}) {
        CHECK(snapshot->chunks.size() == 1);
        const auto saved = snapshot->chunks.front().second->cellAt(0, 2, 3);
        CHECK(saved.tile.block_id == 1 && saved.shape == ramp);
    }
}
