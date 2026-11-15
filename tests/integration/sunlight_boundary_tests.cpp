#include "world_fixture.hpp"

namespace {
struct Tunnel : test::SunlightFixture {
    explicit Tunnel(unsigned workers = 1) : SunlightFixture(workers) {}

    void slab(ChunkCoord c) { test::fill(load(c), 1); }
    void solidPair(bool reverse = false, bool alongZ = false) {
        for (int side = 0; side < 2; ++side)
            for (unsigned y = 0; y < 2; ++y) {
                const unsigned c = reverse ? 1 - side : side;
                slab(alongZ ? ChunkCoord{0, y, c} : ChunkCoord{c, y, 0});
            }
    }
    void air(int x, int y, int z) { tile(x, y, z).block_id = 0; }
    void shaft(int x, int bottom, int z) {
        for (int y = bottom; y < 32; ++y) air(x, y, z);
    }
    void passage(bool reverse = false, bool alongZ = false) {
        const int source = reverse ? 17 : 14;
        if (alongZ) {
            shaft(8, 20, source);
            for (int z = reverse ? 0 : 14; z <= (reverse ? 17 : 31); ++z)
                air(8, 20, z);
        } else {
            shaft(source, 20, 8);
            for (int x = reverse ? 0 : 14; x <= (reverse ? 17 : 31); ++x)
                air(x, 20, 8);
        }
    }
    void expectPassage(bool reverse, bool alongZ) {
        const int source = reverse ? 17 : 14;
        for (int distance = 0; distance <= 17; ++distance) {
            const int position = source + (reverse ? -distance : distance);
            const int expected = std::max(0, 15 - distance);
            expectSun(alongZ ? 8 : position, 20, alongZ ? position : 8,
                      expected);
        }
    }
};

void horizontalSeam(bool reverse, bool alongZ) {
    Tunnel f;
    f.solidPair(reverse, alongZ);
    f.passage(reverse, alongZ);
    f.light();
    f.expectPassage(reverse, alongZ);
}
}  // namespace

TEST(sunlight_cross_pos_x) { horizontalSeam(false, false); }
TEST(sunlight_cross_neg_x) { horizontalSeam(true, false); }
TEST(sunlight_cross_pos_z) { horizontalSeam(false, true); }
TEST(sunlight_cross_neg_z) { horizontalSeam(true, true); }

TEST(sunlight_cross_pos_y) {
    Tunnel f;
    f.solidPair();
    f.shaft(14, 14, 8);
    for (int x = 14; x <= 17; ++x) f.air(x, 14, 8);
    for (int y = 14; y <= 20; ++y) f.air(17, y, 8);
    f.light();
    f.expectSun(17, 14, 8, 12);
    f.expectSun(17, 15, 8, 11);
    f.expectSun(17, 16, 8, 10);
    f.expectSun(17, 20, 8, 6);
}

TEST(sunlight_cross_neg_y) {
    Tunnel f;
    f.solidPair();
    f.shaft(14, 18, 8);
    for (int x = 14; x <= 17; ++x) f.air(x, 18, 8);
    for (int y = 10; y <= 18; ++y) f.air(17, y, 8);
    f.light();
    f.expectSun(17, 18, 8, 12);
    f.expectSun(17, 16, 8, 10);
    f.expectSun(17, 15, 8, 9);
    f.expectSun(17, 10, 8, 4);
}

TEST(sunlight_cross_three_axis_corner) {
    Tunnel f;
    for (unsigned z = 0; z < 2; ++z)
        for (unsigned y = 0; y < 2; ++y)
            for (unsigned x = 0; x < 2; ++x) f.slab({x, y, z});
    f.shaft(15, 18, 15);
    f.air(16, 18, 15);
    for (int y = 15; y <= 18; ++y) f.air(16, y, 16);
    f.light();
    f.expectSun(15, 18, 15, 15);
    f.expectSun(16, 18, 15, 14);
    f.expectSun(16, 18, 16, 13);
    f.expectSun(16, 15, 16, 10);
}

TEST(sunlight_cross_barrier_edit) {
    Tunnel f;
    f.solidPair();
    f.passage();
    f.light();
    f.expectSun(17, 20, 8, 12);
    f.tile(15, 20, 8).block_id = 1;
    f.sunlight.markColumnDirty(0, 0);
    f.light();
    f.expectSun(15, 20, 8, 14);  // lit wall receives, but cannot transmit
    f.expectSun(16, 20, 8, 0);
    f.expectSun(17, 20, 8, 0);
    f.air(15, 20, 8);
    f.sunlight.markColumnDirty(0, 0);
    f.light();
    f.expectSun(17, 20, 8, 12);
}

TEST(sunlight_cross_partial_opacity) {
    Tunnel f;
    f.solidPair();
    f.passage();
    f.tile(15, 20, 8).block_id = 2;  // opacity 3
    f.light();
    f.expectSun(15, 20, 8, 14);
    f.expectSun(16, 20, 8, 11);
    f.expectSun(17, 20, 8, 10);
}

TEST(sunlight_cross_water) {
    Tunnel f;
    f.solidPair();
    f.passage();
    auto& tile = f.tile(15, 20, 8);
    tile.liquid = LiquidType::Water;
    tile.liquidLevel = 255;
    tile.block_id = 2;  // combined opacity is max(3, 8), not 11
    f.light();
    f.expectSun(15, 20, 8, 14);
    f.expectSun(16, 20, 8, 6);
    f.expectSun(17, 20, 8, 5);
}

TEST(sunlight_cross_competing_sources) {
    Tunnel f;
    f.solidPair();
    f.passage();
    f.shaft(30, 20, 8);
    f.light();
    f.expectSun(22, 20, 8, 7);
    f.expectSun(29, 20, 8, 14);
    // Remove the first source without removing the second contribution.
    f.tile(14, 21, 8).block_id = 1;
    f.sunlight.markColumnDirty(0, 0);
    f.light();
    f.expectSun(22, 20, 8, 7);
    f.expectSun(16, 20, 8, 1);
    f.expectSun(15, 20, 8, 0);
}

TEST(sunlight_cross_late_neighbor) {
    Tunnel f;
    f.slab({0, 0, 0});
    f.slab({0, 1, 0});
    f.shaft(14, 20, 8);
    f.air(15, 20, 8);
    f.light();
    f.expectSun(15, 20, 8, 14);
    f.slab({1, 0, 0});
    f.slab({1, 1, 0});
    for (int x = 16; x < 32; ++x) f.air(x, 20, 8);
    f.light();
    f.expectPassage(false, false);
}

TEST(sunlight_cross_load_order_workers) {
    Tunnel first(1), second(2);
    first.solidPair();
    second.solidPair(true);
    first.passage();
    second.passage();
    first.light();
    second.light();
    first.expectPassage(false, false);
    second.expectPassage(false, false);
}

TEST(sunlight_cross_source_unload) {
    Tunnel f;
    f.solidPair();
    f.passage();
    f.light();
    f.expectSun(16, 20, 8, 13);
    f.chunks.unregister(f.coord({0, 0, 0}));
    f.chunks.unregister(f.coord({0, 1, 0}));
    f.light();
    f.expectSun(16, 20, 8, 0);
    f.slab({0, 0, 0});
    f.slab({0, 1, 0});
    f.shaft(14, 20, 8);
    f.air(15, 20, 8);
    f.light();
    f.expectSun(16, 20, 8, 13);
}

TEST(sunlight_cross_stale_neighbor_result) {
    Tunnel f;
    f.solidPair();
    f.passage();
    f.light();
    f.sunlight.markColumnDirty(0, 0);
    f.sunlight.update();
    f.wait();
    f.sunlight.update();  // completed direct result -> indirect snapshots
    f.wait();             // leave old results unconsumed
    f.chunks.unregister(f.coord({1, 0, 0}));
    f.chunks.unregister(f.coord({1, 1, 0}));
    f.slab({1, 0, 0});
    f.slab({1, 1, 0});  // same coordinates, new opaque chunk identities
    f.light();
    f.expectSun(16, 20, 8, 13);
    f.expectSun(17, 20, 8, 0);
}

TEST(sunlight_missing_upper_slab) {
    test::SunlightFixture f;
    f.registerSlab({0, 1, 0});
    f.load({0, 0, 0});
    // The required upper slab must be generated, not treated as empty sky.
    // Generated terrain at Y=415 blocks the entire lower slab.
    f.light();
    CHECK(f.chunks.isReady(f.coord({0, 1, 0})));
    f.expectSun(8, 15, 8, 0);
    f.expectSun(8, 31, 8, 15);
}

TEST(sunlight_unknown_upper_slab) {
    test::SunlightFixture f;
    f.load({0, 0, 0});
    f.light();
    // The unregistered slab above could contain a roof. Do not assume sky.
    f.expectSun(8, 15, 8, 0);
    f.load({0, 1, 0});
    f.light();
    f.expectSun(8, 15, 8, 15);
}

TEST(sunlight_ready_above_bound) {
    test::SunlightFixture f;
    f.load({0, 1, 0});
    f.load({0, 2, 0});
    f.light();
    f.expectSun(8, 47, 8, 15);
    f.expectSun(8, 32, 8, 15);
}

TEST(sunlight_raised_bound) {
    test::SunlightFixture f;
    f.load({0, 1, 0});
    f.load({0, 2, 0});
    f.light();
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) f.tile(x, 40, z).block_id = 1;
    f.chunks.raiseColumnTopY(0, 0, test::SunlightFixture::BASE_Y + 40);
    f.sunlight.markColumnDirty(0, 0);
    f.light();
    f.expectSun(8, 40, 8, 15);
    f.expectSun(8, 39, 8, 0);
    f.expectSun(8, 31, 8, 0);
}

TEST(sunlight_teardown_pending) {
    test::SunlightFixture f;
    f.load({0, 0, 0});
    f.load({0, 1, 0});
    auto solver = std::make_unique<SunLightPropagator>(
        f.chunks, f.assets.getStorage<Block>(), f.pool);
    solver->update();
    CHECK(!solver->isIdle());
    solver.reset();  // joins owned futures and disconnects both callbacks
    f.load({1, 1, 0});
    f.chunks.unregister(f.coord({1, 1, 0}));
    f.light();
    f.expectSun(8, 0, 8, 15);
}

TEST(chunk_negative_coordinates) {
    test::WorldFixture f;
    f.load({0, 0, 0});
    CHECK(f.chunks.tileAt(-1, 0, 0) == nullptr);
    CHECK(f.chunks.tileAt(0, -1, 0) == nullptr);
    CHECK(f.chunks.tileAt(0, 0, -1) == nullptr);
}
