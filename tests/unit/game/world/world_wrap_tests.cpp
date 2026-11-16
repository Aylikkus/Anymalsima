#include <set>

#include "test.hpp"
#include "test_assets.hpp"
#include "world/terrain_lod.hpp"
#include "world/world_editor.hpp"
#include "world_fixture.hpp"

// The world wraps on X: x = W - 1 and x = 0 are adjacent columns.
namespace {
World wrappedWorld(int width, int depth, int height = 430) {
    World world{};
    world.width = width;
    world.height = depth;
    world.genSettings.width = width;
    world.genSettings.height = depth;
    world.genSettings.height_seed = 1234;
    world.genSettings.feature_seed = 5678;
    world.regions.resize(width * depth);
    for (int z = 0; z < depth; ++z)
        for (int x = 0; x < width; ++x) {
            auto& r = world.regions[z * width + x];
            r = test::regionAt(height);
            r.coord = {unsigned(x), unsigned(z)};
            r.land = LandType::HILLS;
            r.vegetation = VegetationType::GRASS;
        }
    return world;
}

// One region wide (8 chunk columns) without a map: chunks wrap at x = 128.
struct SeamFixture : test::SunlightFixture {
    SeamFixture() {
        world.width = 1;
        world.height = 1;
    }
    static constexpr int W = 128;
};
}  // namespace

TEST(wrap_helper_math) {
    const WorldWrap wrap = WorldWrap::fromRegions(2);  // 256 tiles, 16 chunks
    EQUAL(wrap.tiles, 256, "tiles");
    EQUAL(wrap.chunks(), 16, "chunks");
    EQUAL(wrap.wrapTile(-1), int64_t(255), "tile -1");
    EQUAL(wrap.wrapTile(256), int64_t(0), "tile W");
    EQUAL(wrap.wrapTile(255), int64_t(255), "tile W-1");
    EQUAL(wrap.wrapChunk(-1), int64_t(15), "chunk -1");
    EQUAL(wrap.wrapRegion(2), int64_t(0), "region wraps");
    EQUAL(wrap.deltaTile(255, 0), int64_t(-1), "nearest copy behind seam");
    EQUAL(wrap.deltaTile(0, 255), int64_t(1), "nearest copy past seam");
    EQUAL(wrap.deltaTile(128, 0), int64_t(-128), "half period");
    EQUAL(wrap.deltaChunk(15, 1), int64_t(-2), "chunk delta");
    CHECK(std::abs(wrap.imageX(250.5, 3) - -5.5) < 1e-9);
    CHECK(std::abs(wrap.wrapX(-0.25) - 255.75) < 1e-9);
    const WorldWrap off{};
    EQUAL(off.wrapTile(-5), int64_t(-5), "disabled wrap");
    EQUAL(off.deltaChunk(3, 10), int64_t(-7), "disabled delta");
}

TEST(wrap_noise_periodic) {
    constexpr int period = 2048;
    for (const int scale : {16, 21, 44, 64, 112, 173}) {
        for (const double x : {0.0, 3.5, 700.25, 2047.9}) {
            const double z = 91.5;
            const double here = SoilGenerator::field(x, z, scale, 7, period);
            CHECK(std::abs(here - SoilGenerator::field(x + period, z, scale, 7,
                                                       period)) < 1e-9);
            CHECK(std::abs(here - SoilGenerator::field(x - period, z, scale, 7,
                                                       period)) < 1e-9);
        }
        // Continuous across the seam.
        CHECK(
            std::abs(SoilGenerator::field(period - 1e-3, 5, scale, 7, period) -
                     SoilGenerator::field(0, 5, scale, 7, period)) < 1e-3);
    }
    // A scale dividing the period keeps the plain lattice, except in the
    // last cell, which blends into cell 0 across the seam.
    for (const double x : {0.0, 31.5, 1000.0, 1983.0})
        EQUAL(SoilGenerator::field(x, 3, 64, 9, period),
              SoilGenerator::field(x, 3, 64, 9), "unchanged inside period");
}

TEST(wrap_terrain_seam) {
    test::Assets assets;
    const auto world = wrappedWorld(16, 2);  // W = 2048
    const int W = world.wrap().tiles;
    EQUAL(W, 2048, "world tiles");
    const auto east = TerrainNeighborhood::capture(world, {15, 0}, assets);
    const auto west = TerrainNeighborhood::capture(world, {0, 0}, assets);
    const TerrainSampler eastSampler(east), westSampler(west);
    double interior = 0, seam = 0;
    for (int z = 8; z < 248; z += 8) {
        // Both sides of the seam agree on every column.
        EQUAL(eastSampler.surface(W, z), westSampler.surface(0, z),
              "same column from both neighborhoods");
        EQUAL(westSampler.surface(-1, z), eastSampler.surface(W - 1, z),
              "same column from both neighborhoods");
        seam = std::max(seam, std::abs(eastSampler.surface(W - 1, z) -
                                       westSampler.surface(0, z)));
        for (int x = 16; x < 112; ++x)
            interior =
                std::max(interior, std::abs(westSampler.surface(x, z) -
                                            westSampler.surface(x + 1, z)));
    }
    // The seam step looks like any other neighbouring column step.
    CHECK(seam <= interior + 1e-9);
    // Surface heights from the LOD sampler wrap the same way.
    SurfaceSampler surface(world, assets);
    for (int z : {10, 100, 200}) {
        EQUAL(surface.height(-1, z), surface.height(W - 1, z), "wrap -1");
        EQUAL(surface.height(W, z), surface.height(0, z), "wrap W");
    }
}

TEST(wrap_chunks_tiles_ray_editor) {
    SeamFixture f;
    WorldEditor editor{f.chunks, &f.sunlight, f.assets.getStorage<Block>()};
    test::fill(f.load({7, 0, 0}), 1);
    test::fill(f.load({0, 0, 0}), 0);
    const int local = 5, y = SeamFixture::BASE_Y + local;
    // Tiles past either X end resolve to the canonical column.
    CHECK(f.chunks.tileAt(-1, y, 3) == f.chunks.tileAt(127, y, 3));
    CHECK(f.chunks.tileAt(128, y, 3) == f.chunks.tileAt(0, y, 3));
    // A ray leaving x = 0 towards -X hits the solid column at x = 127.
    const auto hit = castTileRay(f.chunks, 0.5f, y + .5f, 3.5f, -1, 0, 0, 8);
    CHECK(hit.has_value());
    EQUAL(hit->tx, 127, "hit reported canonically");
    EQUAL(hit->nx, 1, "hit face");
    // Placing against that face from the other side lands in chunk 0.
    f.tile(127, local, 3).block_id = 0;
    std::vector<BlockChange> changes;
    editor.blockChanged.connect(
        [&](const BlockChange& c) { changes.push_back(c); });
    EQUAL(int(editor.placeBlock({-1, y, 3}, 2)), int(EditStatus::Applied),
          "place past the seam");
    EQUAL(int(f.tile(127, local, 3).block_id), 2, "placed at canonical tile");
    EQUAL(changes.size(), size_t(1), "one change");
    EQUAL(changes[0].pos.x, 127, "change reported canonically");
    EQUAL(int(editor.placeBlock({128, y, 3}, 2)), int(EditStatus::Applied),
          "place past the far end");
    EQUAL(int(f.tile(0, local, 3).block_id), 2, "placed at x = 0");
}

TEST(wrap_sunlight_crosses_seam) {
    // Same tunnel as sunlight_cross_pos_x, from chunk 7 into chunk 0.
    SeamFixture f;
    for (unsigned y = 0; y < 2; ++y) {
        test::fill(f.load({7, y, 0}), 1);
        test::fill(f.load({0, y, 0}), 1);
    }
    const int source = 7 * 16 + 14;
    for (int y = 20; y < 32; ++y) f.tile(source, y, 8).block_id = 0;
    for (int x = source; x <= source + 17; ++x) f.tile(x, 20, 8).block_id = 0;
    f.light();
    for (int distance = 0; distance <= 17; ++distance)
        f.expectSun(source + distance, 20, 8, std::max(0, 15 - distance));
}

TEST(wrap_lod_selection_across_seam) {
    constexpr int period = 8192, depth = 4096;
    constexpr float view = 2048;
    CHECK(TerrainLod::wrapsExactly(period, view));
    CHECK(!TerrainLod::wrapsExactly(6 * 128, view));
    const Vec3 eye{period - 10.f, 500, 2000};
    const auto layout = TerrainLod::select(
        period, depth, eye, 1080, [](const auto& p) { return p.spacing * .4f; },
        false, 2, 128, eye, view, period);
    CHECK(!layout.empty());
    std::set<TerrainLod::Patch> unique(layout.begin(), layout.end());
    EQUAL(unique.size(), layout.size(), "no patch twice");
    const auto owner = [&](int x, int z) {
        int found = -1, count = 0;
        for (size_t i = 0; i < layout.size(); ++i) {
            const auto& p = layout[i];
            if (x >= p.x && x < p.x + p.size() && z >= p.z &&
                z < p.z + p.size()) {
                found = int(i);
                ++count;
            }
        }
        CHECK(count <= 1);
        return found;
    };
    for (const auto& p : layout) {
        CHECK(p.x >= 0 && p.x + p.size() <= period);
        CHECK(p.z >= 0 && p.z < depth);
    }
    // Both sides of the seam are covered, finely next to the eye.
    for (const int x : {period - 1000, period - 1, 0, 1000})
        for (const int z : {1500, 2000, 2500}) CHECK(owner(x, z) >= 0);
    EQUAL(layout[owner(0, 2000)].spacing, 1, "exact next to the eye");
    // 2:1 balance holds between patches touching across the seam.
    for (int z = 0; z < depth; z += 32) {
        const int a = owner(period - 1, z), b = owner(0, z);
        if (a < 0 || b < 0) continue;
        const int sa = layout[a].spacing, sb = layout[b].spacing;
        CHECK(sa <= 2 * sb && sb <= 2 * sa);
    }
    // Without a period nothing past x = 0 is selected.
    const auto bounded = TerrainLod::select(
        period, depth, eye, 1080, [](const auto& p) { return p.spacing * .4f; },
        false, 2, 128, eye, view);
    for (const auto& p : bounded) CHECK(p.x + p.size() > eye.x - view);
}

TEST(wrap_lod_build_has_no_seam_walls) {
    test::Assets assets;
    const auto world = wrappedWorld(16, 4);
    SurfaceSampler sampler(world, assets);
    std::atomic_bool running{false};
    const TerrainLod::Patch patch{0, 128, 4};
    const auto lowestAtX0 = [&](bool wrapX) {
        const auto mesh = TerrainLod::build(patch, sampler, running, 8, wrapX);
        float low = 1e9f;
        for (const auto& v : mesh.vertices)
            if (v.x == 0) low = std::min(low, v.y);
        return low;
    };
    // Bounded: a wall down to the floor closes the world edge.
    EQUAL(lowestAtX0(false), 0.f, "edge wall");
    // Wrapped: only a skirt, like any inner edge.
    CHECK(lowestAtX0(true) > 300);
}
