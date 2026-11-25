#include "world/grounded_probe.hpp"
#include "world/world_editor.hpp"
#include "world_fixture.hpp"

namespace {

using Profile = TileShapeProfile;

struct ShapeFixture : test::WorldFixture {
    void set(int x, int y, int z, TileShape shape = {}, uint8_t block = 1) {
        x = int(chunks.wrap().wrapTile(x));
        auto* chunk = chunks.getChunk(
            {unsigned(x / 16), unsigned(y / 16), unsigned(z / 16)});
        CHECK(chunk);
        Tile tile{};
        tile.block_id = block;
        chunk->setCell(Chunk::cell(x % 16, y % 16, z % 16), tile, shape);
    }

    GroundedProbe spawn(float x, float y, float z) {
        const auto probe = GroundedProbe::spawn(chunks, {x, y, z});
        CHECK(probe);
        return *probe;
    }
};

void walk(ShapeFixture& f, GroundedProbe& probe, Vec2 velocity, int frames) {
    for (int i = 0; i < frames; ++i) probe.update(f.chunks, velocity, .05f);
}

void near(float actual, float expected) {
    EQUAL(std::abs(actual - expected) < .001f, true,
          "expected " + std::to_string(expected) + "; got " +
              std::to_string(actual));
}

}  // namespace

TEST(ray_shape_volume) {
    ShapeFixture f;
    f.load({0, 0, 0});
    f.load({1, 0, 0});
    const auto wall = TileShape::make(Profile::Wall10);
    f.set(15, 8, 8, wall);
    f.set(17, 8, 8);

    auto hit = castTileRay(f.chunks, 14.5f, 8.5f, 8.5f, 1, 0, 0);
    CHECK(hit);
    EQUAL(hit->tx, 17, "the open part of a wall cell passes the ray");
    near(hit->distance, 2.5f);

    hit = castTileRay(f.chunks, 14.5f, 8.5f, 8.95f, 1, 0, 0);
    CHECK(hit);
    EQUAL(hit->tx, 15, "ray through the occupied wall strip");
    CHECK(hit->shape == wall);
    CHECK((hit->normal == Vec3{-1, 0, 0}));
    CHECK((hit->placementOffset == TilePos{-1, 0, 0}));

    hit = castTileRay(f.chunks, 15.5f, 8.5f, 8.25f, 0, 0, 1);
    CHECK(hit);
    near(hit->distance, .65f);
    near(hit->position.z, 8.9f);
    CHECK((hit->placementOffset == TilePos{0, 0, -1}));
    CHECK(!castTileRay(f.chunks, 15.5f, 8.5f, 8.25f, 0, 0, 1, .6f));

    hit = castTileRay(f.chunks, 15.5f, 8.5f, 8.95f, 0, 1, 0);
    CHECK(hit);
    CHECK(hit->normal == Vec3{});
    CHECK(hit->placementOffset == TilePos{});
    near(hit->distance, 0);

    f.tile(16, 8, 8).liquid = LiquidType::Water;
    hit = castTileRay(f.chunks, 14.5f, 8.5f, 8.5f, 1, 0, 0);
    CHECK(hit && hit->tile.isLiquid());
    hit = castTileRay(f.chunks, 14.5f, 8.5f, 8.5f, 1, 0, 0, 4, false);
    CHECK(hit && hit->tx == 17);

    f.set(10, 8, 11);
    f.set(11, 8, 11);
    const float diagonal = std::sqrt(.5f);
    const auto castEdge = [&] {
        return castTileRay(f.chunks, 10.5f, 8.5f, 10.5f, diagonal, 0, diagonal);
    };
    hit = castEdge();
    CHECK(hit && hit->tx == 10 && hit->tz == 11);
    CHECK((hit->placementOffset == TilePos{0, 0, -1}));
    f.set(10, 8, 11, wall);
    hit = castEdge();
    CHECK(hit && hit->tx == 11 && hit->tz == 11);
    CHECK((hit->placementOffset == TilePos{-1, 0, 0}));
}

TEST(ray_shape_placement) {
    ShapeFixture f;
    f.load({0, 0, 0});
    f.set(4, 5, 7, TileShape::make(Profile::Ramp));
    f.set(6, 5, 7, TileShape::make(Profile::Slab));
    const auto hit = castTileRay(f.chunks, 4.5f, 7.f, 7.25f, 0, -1, 0);
    CHECK(hit);
    near(hit->distance, 1.75f);
    near(hit->position.y, 5.25f);
    CHECK(hit->normal.y > 0 && hit->normal.z < 0);
    CHECK((hit->placementOffset == TilePos{0, 1, 0}));
    WorldEditor editor{f.chunks, &f.sunlight, f.assets.getStorage<Block>()};
    const auto [dx, dy, dz] = hit->placementOffset;
    const auto placed = TileShape::make(Profile::Wall25, TileFacing::NegX);
    EQUAL(int(editor.placeBlock({hit->tx + dx, hit->ty + dy, hit->tz + dz}, 2,
                                placed)),
          int(EditStatus::Applied), "the slope places above its owning tile");
    CHECK(f.chunks.cellAt(4, 6, 7)->shape == placed);

    auto slab = castTileRay(f.chunks, 6.5f, 7.f, 7.25f, 0, -1, 0);
    CHECK(slab);
    CHECK((slab->placementOffset == TilePos{0, 1, 0}));
    near(slab->position.y, 5.5f);
    slab = castTileRay(f.chunks, 6.5f, 3.f, 7.25f, 0, 1, 0);
    CHECK(slab);
    CHECK((slab->placementOffset == TilePos{0, -1, 0}));
}

TEST(ray_shape_junction_facets) {
    ShapeFixture f;
    f.load({0, 0, 0});
    for (const auto profile :
         {Profile::OuterHalfHigh, Profile::InnerThirdMiddle})
        for (unsigned facing = 0; facing < 4; ++facing) {
            const auto direction = TileFacing(facing);
            const auto shape = TileShape::make(profile, direction);
            f.set(4, 5, 7, shape);
            const auto a =
                tile_geometry::rotatePoint({.25f, 0, .75f}, direction);
            const auto b =
                tile_geometry::rotatePoint({.75f, 0, .25f}, direction);
            const auto pick = [&](Vec3 p) {
                return castTileRay(f.chunks, 4 + p.x, 7.f, 7 + p.z, 0, -1, 0);
            };
            const auto hitA = pick(a), hitB = pick(b);
            CHECK(hitA && hitB);
            CHECK(std::abs(hitA->normal.x) != std::abs(hitB->normal.x));
            for (const auto* hit : {&*hitA, &*hitB}) {
                CHECK((hit->placementOffset == TilePos{0, 1, 0}));
                CHECK(hit->shape == shape);
            }
            const auto diagonal = pick({.5f, 0, .5f});
            CHECK(diagonal);
            CHECK(diagonal->normal == hitA->normal ||
                  diagonal->normal == hitB->normal);
            CHECK(pick({.5f, 0, .5f})->normal == diagonal->normal);
        }
}

TEST(probe_ramp_runs) {
    constexpr Profile Profiles[][3]{
        {Profile::Ramp},
        {Profile::RampHalfLow, Profile::RampHalfHigh},
        {Profile::RampThirdLow, Profile::RampThirdMiddle,
         Profile::RampThirdHigh},
    };
    constexpr Vec2 Directions[]{{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
    for (int run = 1; run <= 3; ++run)
        for (unsigned facing = 0; facing < 4; ++facing) {
            ShapeFixture f;
            f.load({0, 0, 0});
            const auto d = Directions[facing];
            const int dx = int(d.x), dz = int(d.y);
            f.set(7 - dx, 4, 7 - dz);
            for (int piece = 0; piece < run; ++piece)
                f.set(7 + dx * piece, 5, 7 + dz * piece,
                      TileShape::make(Profiles[run - 1][piece],
                                      TileFacing(facing)));
            f.set(7 + dx * run, 5, 7 + dz * run);
            auto probe = f.spawn(7.5f - dx, 5, 7.5f - dz);
            for (float sign : {1.f, -1.f}) {
                for (int frame = 0; frame < 10 * (run + 1); ++frame) {
                    const float before = probe.feet().y;
                    walk(f, probe, {2 * sign * d.x, 2 * sign * d.y}, 1);
                    CHECK(probe.grounded());
                    CHECK(std::abs(probe.feet().y - before) < .101f);
                }
                near(probe.feet().y, sign > 0 ? 6.f : 5.f);
                const int piece = sign > 0 ? run : -1;
                near(probe.feet().x, 7.5f + dx * piece);
                near(probe.feet().z, 7.5f + dz * piece);
            }
        }
}

TEST(probe_corner_approaches) {
    for (const bool inner : {false, true})
        for (unsigned facing = 0; facing < 4; ++facing) {
            ShapeFixture f;
            f.load({0, 0, 0});
            const auto rotation = TileFacing(facing);
            const auto point = [&](float x, float z) {
                return tile_geometry::rotatePoint({x, 0, z}, rotation);
            };
            const auto piece = [&](float x, float z, Profile profile,
                                   unsigned extraTurn) {
                const Vec3 p = point(x, z);
                f.set(int(7 + p.x), 5, int(7 + p.z),
                      TileShape::make(profile, rotation).rotated(extraTurn));
            };
            piece(.5f, .5f, inner ? Profile::InnerCorner : Profile::OuterCorner,
                  0);
            const float approach = inner ? -.5f : 1.5f;
            const float inside = inner ? .65f : .35f;
            const auto a = point(approach, inside);
            const auto b = point(inside, approach);
            const auto turn = point(inside, inside);
            piece(approach, .5f, Profile::Ramp, 0);
            piece(.5f, approach, Profile::Ramp, 1);
            auto probe = f.spawn(7 + a.x, 6.5f, 7 + a.z);
            const auto moveTo = [&](Vec3 p) {
                const auto from = probe.feet();
                walk(f, probe, {7 + p.x - from.x, 7 + p.z - from.z}, 20);
                CHECK(probe.grounded());
                near(probe.feet().x, 7 + p.x);
                near(probe.feet().z, 7 + p.z);
            };
            moveTo(turn);
            moveTo(b);
            moveTo(turn);
            moveTo(a);
            near(probe.feet().y, 5 + inside + GroundedProbe::Radius);
        }
}

TEST(probe_obstacles_and_gap) {
    ShapeFixture f;
    f.load({0, 0, 0});
    for (int x = 1; x < 14; ++x)
        for (int z = 3; z <= 8; ++z) f.set(x, 4, z);
    f.set(6, 5, 4, TileShape::make(Profile::Wall10, TileFacing::PosX));
    auto probe = f.spawn(4.5f, 6, 4.5f);
    probe.update(f.chunks, {35, 0}, .25f);
    CHECK(probe.feet().x > 6.5f && probe.feet().x <= 6.651f);
    near(probe.feet().y, 5);

    f.set(5, 6, 5);  // One meter of clearance.
    probe = f.spawn(4.5f, 6, 5.5f);
    walk(f, probe, {4, 0}, 10);
    CHECK(probe.feet().x <= 4.751f);
    f.set(5, 6, 5, {}, 0);
    f.set(5, 7, 5);  // Two meters of clearance.
    walk(f, probe, {4, 0}, 10);
    CHECK(probe.feet().x > 6.5f);

    f.set(6, 5, 7, TileShape::make(Profile::RampThirdLow, TileFacing::PosX));
    f.set(7, 5, 7, TileShape::make(Profile::RampThirdMiddle, TileFacing::PosX));
    const auto high = TileShape::make(Profile::RampThirdHigh, TileFacing::PosX);
    f.set(8, 5, 7, high);
    f.set(9, 5, 7);
    probe = f.spawn(9.5f, 6, 7.5f);
    f.set(7, 5, 7, {}, 0);
    walk(f, probe, {-2, 0}, 23);
    CHECK(probe.feet().x < 7.5f);  // Back off the high piece.
    walk(f, probe, {}, 12);
    near(probe.feet().y, 5);
    CHECK(probe.grounded());
    CHECK(f.chunks.cellAt(8, 5, 7)->shape == high);
    walk(f, probe, {4, 0}, 10);
    CHECK(probe.feet().x <= 7.751f);  // The high piece is a side wall here.

    probe = f.spawn(4.5f, 6, 7.5f);
    f.set(5, 4, 7, {}, 0);  // No support under the hole.
    walk(f, probe, {2, 0}, 10);
    CHECK(!probe.grounded());
    CHECK(probe.feet().y < 5);

    f.load({0, Y_CHUNKS - 1, 0});
    f.set(5, WORLD_HEIGHT_Y - 1, 5);
    probe = f.spawn(5.5f, WORLD_HEIGHT_Y, 5.5f);
    walk(f, probe, {}, 1);
    CHECK(probe.grounded());
    near(probe.feet().y, WORLD_HEIGHT_Y);
}

TEST(probe_and_ray_chunk_seams) {
    for (const bool wrapped : {false, true}) {
        ShapeFixture f;
        const int seam = wrapped ? 128 : 16;
        const unsigned left = unsigned(seam / 16 - 1);
        const unsigned right = wrapped ? 0 : left + 1;
        if (wrapped) {
            f.world.width = 1;
            f.world.height = 1;
        }
        f.load({left, 0, 0});
        f.load({right, 0, 0});
        f.set(seam - 2, 4, 7);
        f.set(seam - 1, 5, 7,
              TileShape::make(Profile::RampHalfLow, TileFacing::PosX));
        f.set(seam, 5, 7,
              TileShape::make(Profile::RampHalfHigh, TileFacing::PosX));
        f.set(seam + 1, 5, 7);

        const auto hit = castTileRay(f.chunks, seam - .5f, 5.9f, 7.5f, 1, 0, 0);
        CHECK(hit);
        EQUAL(hit->tx, wrapped ? 0 : seam, "hit uses canonical X");
        near(hit->position.x, seam + .8f);
        CHECK((hit->placementOffset == TilePos{0, 1, 0}));

        auto probe = f.spawn(seam - 1.5f, 5, 7.5f);
        walk(f, probe, {2, 0}, 30);
        near(probe.feet().x, (wrapped ? 0 : seam) + 1.5f);
        near(probe.feet().y, 6);
        walk(f, probe, {-2, 0}, 30);
        near(probe.feet().x, seam - 1.5f);
        near(probe.feet().y, 5);
    }
}
