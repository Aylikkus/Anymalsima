#include "world_fixture.hpp"

TEST(sunlight_generated_ocean) {
    test::WorldFixture f;
    f.region = test::regionAt(SEA_LEVEL_Y - 2);
    // Use actual generation results, including default Tile lighting. Both
    // slabs are generated before any sunlight work starts.
    f.registerSlab({0, 23, 0});
    f.registerSlab({0, 24, 0});
    f.chunks.request({0, 23, 0});
    f.chunks.request({0, 24, 0});
    f.wait();
    CHECK(f.chunks.request({0, 23, 0}) != nullptr);
    CHECK(f.chunks.request({0, 24, 0}) != nullptr);
    CHECK(f.tile(8, SEA_LEVEL_Y, 8).isLiquid());
    CHECK(f.tile(8, SEA_LEVEL_Y - 1, 8).isLiquid());
    f.light();
    f.expectSun(8, SEA_LEVEL_Y, 8, 15);
    f.expectSun(8, SEA_LEVEL_Y - 1, 8, 7);
    f.expectSun(8, SEA_LEVEL_Y - 2, 8, 0);
}
