#include "world_fixture.hpp"

TEST(chunk_lookup_boundaries) {
    test::WorldFixture f;
    auto& a = f.load({0, 0, 0});
    auto& x = f.load({1, 0, 0});
    auto& y = f.load({0, 1, 0});
    auto& z = f.load({0, 0, 1});
    CHECK(f.chunks.tileAt(15, 15, 15) == &a.tiles[15][15][15]);
    CHECK(f.chunks.tileAt(16, 7, 9) == &x.tiles[9][7][0]);
    CHECK(f.chunks.tileAt(7, 16, 9) == &y.tiles[9][0][7]);
    CHECK(f.chunks.tileAt(7, 9, 16) == &z.tiles[0][9][7]);
}

TEST(chunk_missing) {
    test::WorldFixture f;
    CHECK(f.chunks.request({100, 0, 0}) == nullptr);
    CHECK(f.chunks.tileAt(0, 0, 0) == nullptr);
    CHECK(f.chunks.tileAt(0, -1, 0) == nullptr);
    f.registerSlab({0, 0, 0});
    CHECK(!f.chunks.isReady({0, 0, 0}));
    CHECK(f.chunks.tileAt(0, 0, 0) == nullptr);
}

TEST(chunk_unregister_lifetime) {
    test::WorldFixture f;
    f.load({0, 0, 0});
    f.tile(3, 4, 5).block_id = 2;
    auto owned = f.chunks.getChunkShared({0, 0, 0});
    f.chunks.unregister({0, 0, 0});
    CHECK(!f.chunks.isRegistered({0, 0, 0}));
    CHECK(f.chunks.tileAt(3, 4, 5) == nullptr);
    EQUAL(int(owned->tiles[5][4][3].block_id), 2,
          "shared chunk survives manager removal");
}

TEST(chunk_top_bound) {
    test::WorldFixture f;
    f.registerSlab({0, 0, 0});
    // Underwater regions reserve room for a warped beach crossing into them.
    EQUAL(f.chunks.getColumnTopY(0, 0), TerrainTuning::surfaceCeiling(31),
          "initial surface");
    f.chunks.raiseColumnTopY(0, 0, 450);
    f.chunks.raiseColumnTopY(0, 0, 20);
    EQUAL(f.chunks.getColumnTopY(0, 0), 450, "monotonic column bound");
}

TEST(ray_first_hit) {
    test::WorldFixture f;
    f.load({0, 0, 0});
    f.tile(4, 8, 8).block_id = 1;
    f.tile(7, 8, 8).block_id = 2;
    auto hit = castTileRay(f.chunks, 1.5f, 8.5f, 8.5f, 1, 0, 0);
    CHECK(hit.has_value());
    EQUAL(hit->tx, 4, "nearest ray hit x");
    EQUAL(hit->ty, 8, "nearest ray hit y");
    EQUAL(hit->tz, 8, "nearest ray hit z");
    EQUAL(int(hit->tile.block_id), 1, "nearest block");
}

TEST(ray_distance_limit) {
    test::WorldFixture f;
    f.load({0, 0, 0});
    f.tile(4, 8, 8).block_id = 1;
    CHECK(!castTileRay(f.chunks, 1.5f, 8.5f, 8.5f, 1, 0, 0, 2));
    CHECK(castTileRay(f.chunks, 1.5f, 8.5f, 8.5f, 1, 0, 0, 3));
}

TEST(ray_liquid_hit) {
    test::WorldFixture f;
    f.load({0, 0, 0});
    f.tile(4, 8, 8).liquid = LiquidType::Water;
    auto hit = castTileRay(f.chunks, 4.5f, 8.5f, 8.5f, 0, 1, 0);
    CHECK(hit.has_value());
    CHECK(hit->tile.isLiquid());
    EQUAL(hit->tx, 4, "ray starts inside water");
}

TEST(ray_chunk_seam) {
    test::WorldFixture f;
    f.load({0, 0, 0});
    f.load({1, 0, 0});
    f.tile(17, 8, 8).block_id = 1;
    auto hit = castTileRay(f.chunks, 14.5f, 8.5f, 8.5f, 1, 0, 0);
    CHECK(hit.has_value());
    EQUAL(hit->tx, 17, "ray across x chunk boundary");
}

TEST(ray_negative_direction) {
    test::WorldFixture f;
    f.load({0, 0, 0});
    f.tile(5, 8, 8).block_id = 1;
    auto hit = castTileRay(f.chunks, 10.5f, 8.5f, 8.5f, -1, 0, 0, 6);
    CHECK(hit.has_value());
    EQUAL(hit->tx, 5, "negative x ray");
}

TEST(camera_direction) {
    float x, y, z;
    cameraDirection(0, 0, x, y, z);
    EQUAL(x, 0.0f, "forward x");
    EQUAL(y, 0.0f, "forward y");
    EQUAL(z, 1.0f, "forward z");
    cameraDirection(0.7f, 0.4f, x, y, z);
    CHECK(std::abs(x * x + y * y + z * z - 1) < 1e-6f);
    CHECK(y > 0);
}

TEST(sunlight_open_column) {
    test::SunlightFixture f;
    f.load({0, 0, 0});
    f.load({0, 1, 0});
    f.light();
    for (int y = 0; y < 32; ++y) f.expectSun(8, y, 8, 15);
}

TEST(sunlight_opaque_roof) {
    test::SunlightFixture f;
    // Deliberately retain obsolete full light to test replacement with zeros.
    f.load({0, 0, 0}, 15);
    f.load({0, 1, 0}, 15);
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) f.tile(x, 24, z).block_id = 1;
    f.light();
    f.expectSun(8, 25, 8, 15);
    f.expectSun(8, 24, 8, 15);  // lit surface receives incoming sunlight
    f.expectSun(8, 23, 8, 0);
    f.expectSun(8, 0, 8, 0);
}

TEST(sunlight_partial_opacity) {
    test::SunlightFixture f;
    f.load({0, 0, 0});
    f.load({0, 1, 0});
    // Entire planes prevent lateral light from bypassing the attenuator.
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
            f.tile(x, 25, z).block_id = 2;
            f.tile(x, 24, z).block_id = 2;
        }
    f.light();
    f.expectSun(8, 25, 8, 15);
    f.expectSun(8, 24, 8, 12);
    f.expectSun(8, 23, 8, 9);
    f.expectSun(8, 0, 8, 9);
}

TEST(sunlight_water) {
    test::SunlightFixture f;
    f.load({0, 0, 0});
    f.load({0, 1, 0});
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
            f.tile(x, 25, z).liquid = LiquidType::Water;
            f.tile(x, 25, z).liquidLevel = 255;
            f.tile(x, 24, z).liquid = LiquidType::Water;
            f.tile(x, 24, z).liquidLevel = 255;
        }
    f.light();
    f.expectSun(8, 25, 8, 15);
    f.expectSun(8, 24, 8, 7);
    f.expectSun(8, 23, 8, 0);
}

TEST(sunlight_slab_seam) {
    test::SunlightFixture f;
    f.load({0, 0, 0});
    f.load({0, 1, 0});
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) f.tile(x, 16, z).block_id = 2;
    f.light();
    f.expectSun(8, 16, 8, 15);
    f.expectSun(8, 15, 8, 12);
    f.expectSun(8, 0, 8, 12);
}

TEST(sunlight_late_slab) {
    test::SunlightFixture f;
    f.registerSlab({0, 0, 0});
    f.load({0, 1, 0});
    f.light();
    CHECK(!f.chunks.isReady(f.coord({0, 0, 0})));
    f.load({0, 0, 0});
    f.light();
    f.expectSun(8, 15, 8, 15);
    f.expectSun(8, 0, 8, 15);
}

TEST(sunlight_horizontal) {
    test::SunlightFixture f;
    f.load({0, 0, 0});
    f.load({0, 1, 0});
    // A short tunnel opens onto a sunlit shaft at x=7. Its roof blocks
    // vertical sunlight. Light must enter from the side at (8,20,8).
    for (int z = 0; z < 16; ++z)
        for (int x = 8; x < 16; ++x) f.tile(x, 21, z).block_id = 1;
    f.light();
    f.expectSun(7, 20, 8, 15);
    const int exposure = f.tile(8, 20, 8).lightLevel.sun_exposure;
    if (exposure == 0)
        throw std::runtime_error(
            "sunlight at tunnel entrance (8,20,8): expected > 0, actual 0");
}

TEST(sunlight_dirty_column) {
    test::SunlightFixture f;
    f.load({0, 0, 0});
    f.load({0, 1, 0});
    f.light();
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) f.tile(x, 24, z).block_id = 1;
    f.sunlight.markColumnDirty(0, 0);
    f.light();
    f.expectSun(8, 23, 8, 0);
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) f.tile(x, 24, z).block_id = 0;
    f.sunlight.markColumnDirty(0, 0);
    f.light();
    f.expectSun(8, 23, 8, 15);
    f.expectSun(8, 0, 8, 15);
}

TEST(sunlight_stale_result) {
    test::SunlightFixture f;
    f.load({0, 0, 0});
    f.load({0, 1, 0});
    f.sunlight.update();
    f.wait();  // old result queued, deliberately not applied
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) f.tile(x, 24, z).block_id = 1;
    f.sunlight.markColumnDirty(0, 0);
    f.light();
    f.expectSun(8, 23, 8, 0);
    f.expectSun(8, 24, 8, 15);
}
