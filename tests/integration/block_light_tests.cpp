#include <algorithm>
#include <array>
#include <cmath>

#include "world/block_light_propagator.hpp"
#include "world/light_sources.hpp"
#include "world/world_editor.hpp"
#include "world_fixture.hpp"

namespace {
constexpr uint8_t Stone = 1;    // opacity 15
constexpr uint8_t Glass = 2;    // opacity 3
constexpr uint8_t Lamp = 4;     // white, radius 15, falloff 1
constexpr uint8_t RedLamp = 5;  // red, radius 15
constexpr uint8_t Beacon = 6;   // white, radius 64 (far light)
constexpr uint8_t Spot = 7;     // radius 15, 20-30 degree cone down

// Expected level of the default lamp at effective distance d.
int lamp(double d) { return int(std::lround(15 * (1 - d / 15))); }

struct LightFixture : test::SunlightFixture {
    LightSourceRegistry registry;
    // The propagator listens to the editor: declared after it.
    WorldEditor editor{chunks, &sunlight, assets.getStorage<Block>()};
    BlockLightPropagator blockLight{chunks, assets.getStorage<Block>(), pool,
                                    &registry};

    LightFixture() { blockLight.watch(editor); }
    ~LightFixture() { pool.wait(); }

    // Loads an air chunk; its light inputs are rescanned after the fill.
    void air(ChunkCoord local) {
        load(local);
        blockLight.markChunkChanged(coord(local));
    }
    void relight() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            chunks.update();
            sunlight.update();
            blockLight.update();
            wait();
            CHECK(std::chrono::steady_clock::now() < deadline);
        } while (!sunlight.isIdle() || !blockLight.isIdle() ||
                 chunks.hasGenerationWork());
    }
    static TilePos at(int x, int y, int z) { return {x, y + BASE_Y, z}; }
    void place(int x, int y, int z, uint8_t block) {
        EQUAL(int(editor.setBlock(at(x, y, z), block)),
              int(EditStatus::Applied), "edit applied");
    }
    int light(int x, int y, int z) {
        return tile(x, y, z).lightLevel.artificial_light;
    }
    void expect(int x, int y, int z, int expected) {
        EQUAL(light(x, y, z), expected,
              "block light at (" + std::to_string(x) + "," + std::to_string(y) +
                  "," + std::to_string(z) + ")");
    }
};
}  // namespace

TEST(block_light_round_falloff) {
    LightFixture f;
    f.air({0, 0, 0});
    f.place(8, 8, 8, Lamp);
    f.relight();
    f.expect(8, 8, 8, 0);  // the lamp itself is opaque
    f.expect(9, 8, 8, lamp(1));
    f.expect(13, 8, 8, lamp(5));
    f.expect(8, 3, 8, lamp(5));
    // Round, not a diamond: (3, 4, 0) is 5 blocks away like (5, 0, 0).
    f.expect(11, 12, 8, lamp(5));
    f.expect(12, 12, 12, lamp(std::sqrt(48.0)));
    CHECK(!f.blockLight.chroma(LightFixture::coord({0, 0, 0})));
}

TEST(block_light_wall_shadow) {
    LightFixture f;
    f.air({0, 0, 0});
    // A full wall at x = 6 inside the only loaded chunk: neighbors are not
    // loaded, so light cannot go around it.
    std::vector<EditRequest> wall;
    for (int y = 0; y < 16; ++y)
        for (int z = 0; z < 16; ++z)
            wall.push_back(
                {LightFixture::at(6, y, z), EditMode::Replace, Stone});
    f.editor.apply(wall);
    f.place(3, 8, 8, Lamp);
    f.relight();
    f.expect(4, 8, 8, lamp(1));
    f.expect(5, 8, 8, lamp(2));
    for (int x = 7; x < 16; ++x) f.expect(x, 8, 8, 0);
}

TEST(block_light_soft_shadow_and_translucency) {
    LightFixture f;
    f.air({0, 0, 0});
    // A small pillar casts a soft shadow: the cell behind it is lit only
    // around it, so it is dimmer than at the same distance in open air.
    f.place(10, 8, 8, Stone);
    f.place(8, 8, 8, Lamp);
    f.relight();
    const int shadowed = f.light(12, 8, 8);
    CHECK(shadowed > 0);
    CHECK(shadowed < lamp(4));
    // Translucent glass costs its opacity: d = 2 + (cost 4 - 2) = 4 rather
    // than 2 in open air, at least through the glass itself.
    f.place(8, 8, 10, Glass);
    f.place(8, 9, 10, Stone);
    f.place(8, 7, 10, Stone);
    f.place(7, 8, 10, Stone);
    f.place(9, 8, 10, Stone);
    f.relight();
    CHECK(f.light(8, 8, 11) < lamp(3));
    f.expect(8, 8, 10, lamp(2));  // glass itself receives light
}

TEST(block_light_chunk_seam) {
    LightFixture f;
    f.air({0, 0, 0});
    f.air({1, 0, 0});
    f.place(14, 8, 8, Lamp);
    f.relight();
    f.expect(15, 8, 8, lamp(1));
    f.expect(16, 8, 8, lamp(2));
    f.expect(17, 8, 8, lamp(3));
    f.expect(11, 8, 8, lamp(3));
    f.expect(20, 12, 8, lamp(std::sqrt(52.0)));
    // Moving the lamp to the other chunk relights both.
    f.place(14, 8, 8, 0);
    f.place(18, 8, 8, Lamp);
    f.relight();
    f.expect(15, 8, 8, lamp(3));
    f.expect(21, 8, 8, lamp(3));
}

TEST(block_light_late_neighbor) {
    LightFixture f;
    f.air({0, 0, 0});
    f.place(14, 8, 8, Lamp);
    f.relight();
    // The arriving neighbor gets the lamp's light and opens a path.
    f.air({1, 0, 0});
    f.relight();
    f.expect(17, 8, 8, lamp(3));
}

TEST(block_light_removal_clears) {
    LightFixture f;
    f.air({0, 0, 0});
    f.air({1, 0, 0});
    f.place(14, 8, 8, Lamp);
    f.relight();
    CHECK(f.light(18, 8, 8) > 0);
    f.place(14, 8, 8, 0);
    f.relight();
    for (int x = 0; x < 32; ++x) f.expect(x, 8, 8, 0);
}

TEST(block_light_color) {
    LightFixture f;
    f.air({0, 0, 0});
    f.air({1, 0, 0});
    f.place(1, 8, 8, RedLamp);
    f.relight();
    const auto* chroma = f.blockLight.chroma(LightFixture::coord({0, 0, 0}));
    CHECK(chroma != nullptr);
    const int cell = (8 * 16 + 8) * 16 + 6;
    const auto red = BlockLightPropagator::encodeChroma({1, 0, 0});
    EQUAL(int((*chroma)[cell * 2]), int(red[0]), "red co");
    EQUAL(int((*chroma)[cell * 2 + 1]), int(red[1]), "red cg");
    // Light beyond the red lamp's reach stays white: no buffer at all.
    CHECK(!f.blockLight.chroma(LightFixture::coord({1, 0, 0})));
    // Overlapping white light blends toward white.
    f.place(10, 8, 8, Lamp);
    f.relight();
    chroma = f.blockLight.chroma(LightFixture::coord({0, 0, 0}));
    CHECK(chroma != nullptr);
    const int between = (8 * 16 + 8) * 16 + 7;
    const int co = (*chroma)[between * 2];
    CHECK(co > 128 && co < int(red[0]));
}

TEST(block_light_spotlight) {
    LightFixture f;
    f.air({0, 0, 0});
    f.place(8, 14, 8, Spot);
    f.relight();
    CHECK(f.light(8, 10, 8) > 0);  // straight below
    f.expect(8, 15, 8, 0);         // behind the spotlight
    f.expect(12, 14, 8, 0);        // to the side
}

TEST(block_light_far_registry) {
    LightFixture f;
    f.air({0, 0, 0});
    f.place(8, 8, 8, Beacon);
    f.place(2, 8, 8, Lamp);
    f.relight();
    const auto pos = LightFixture::at(8, 8, 8);
    CHECK(f.registry.blockLight(pos) != nullptr);
    // Small lamps are indexed too (distant glow and glare), but they are
    // never far lights.
    CHECK(f.registry.blockLight(LightFixture::at(2, 8, 8)) != nullptr);
    CHECK(!f.registry.blockLight(LightFixture::at(2, 8, 8))->emission.isFar());
    CHECK(f.registry.dirty);
    f.expect(8, 9, 8, 15);
    // The near field fades out toward the hand-over to the per-pixel term.
    f.expect(9, 8, 8, 15);
    // Light adds up: the beacon (d = 7) plus the small lamp (d = 9.2).
    f.expect(8, 8, 15,
             std::min(15, int(std::lround(15 * ((1 - 7.0 / 64) +
                                                (1 - std::sqrt(85.0) / 15))))));
    // It survives the chunk unloading, and goes with the block.
    f.chunks.unregister(LightFixture::coord({0, 0, 0}));
    f.relight();
    CHECK(f.registry.blockLight(pos) != nullptr);
}

TEST(block_light_far_registry_removed_with_block) {
    LightFixture f;
    f.air({0, 0, 0});
    f.place(8, 8, 8, Beacon);
    f.relight();
    f.place(8, 8, 8, 0);
    f.relight();
    CHECK(!f.registry.blockLight(LightFixture::at(8, 8, 8)));
}

TEST(block_light_entity_source) {
    LightFixture f;
    f.air({0, 0, 0});
    LightSource torch;
    torch.owner = LightSource::Owner::Entity;
    torch.position = {8.5, LightFixture::BASE_Y + 8.5, 8.5};
    const auto id = f.registry.add(torch);
    f.relight();
    f.expect(8, 8, 8, 15);  // an entity light does not occupy a tile
    f.expect(11, 8, 8, lamp(3));
    f.registry.place(id, {4.5, LightFixture::BASE_Y + 8.5, 8.5}, {0, -1, 0});
    f.relight();
    f.expect(4, 8, 8, 15);
    f.expect(8, 8, 8, lamp(4));
    f.expect(11, 8, 8, lamp(7));
    f.registry.remove(id);
    f.relight();
    f.expect(4, 8, 8, 0);
}

TEST(block_light_wraps_x) {
    // One region wide without a map: chunks wrap at x = 128.
    LightFixture f;
    f.world.width = 1;
    f.world.height = 1;
    f.air({7, 0, 0});
    f.air({0, 0, 0});
    f.place(126, 8, 8, Lamp);
    f.relight();
    f.expect(127, 8, 8, lamp(1));
    f.expect(0, 8, 8, lamp(2));
    f.expect(1, 8, 8, lamp(3));
}

TEST(far_light_selection) {
    LightSourceRegistry registry;
    LightEmission far;
    far.radius = 100;
    LightEmission near;
    registry.setBlockLight({10, 0, 0}, 1, far);
    registry.setBlockLight({20, 0, 0}, 1, near);    // indexed, not far
    registry.setBlockLight({4090, 0, 0}, 1, far);   // across the seam
    registry.setBlockLight({2000, 0, 0}, 1, far);   // out of range
    registry.setBlockLight({30, 0, 0}, 1, far, 0);  // walled in: skipped
    EQUAL(registry.size(), size_t(5), "every light is indexed");
    const auto views = visibleFarLights(registry, {0, 0, 0}, 300, 4096);
    EQUAL(views.size(), size_t(2), "visible");
    // Wrapped copy at x = -5.5 is nearest; both reach the eye.
    CHECK(std::abs(views[0].position[0] - -5.5) < 1e-9);
    CHECK(std::abs(views[1].position[0] - 10.5) < 1e-9);
}

TEST(block_light_additive) {
    LightFixture f;
    f.air({0, 0, 0});
    f.place(2, 8, 8, Lamp);
    f.place(14, 8, 8, Lamp);
    f.relight();
    // Both lamps reach (8, 14, 8) at sqrt(72): their light adds up, so the
    // voxel field matches the summed per-pixel far term at the hand-over.
    const double one = 1 - std::sqrt(72.0) / 15;
    f.expect(8, 14, 8, int(std::lround(15 * 2 * one)));
    // And saturates at 15.
    f.expect(8, 8, 8, 15);
}

TEST(block_light_spot_into_wall) {
    LightFixture f;
    f.air({0, 0, 0});
    // A spotlight pointing down at a block right below it lights nothing:
    // light may not leave through its back or side faces.
    f.place(8, 7, 8, Stone);
    f.place(8, 8, 8, Spot);
    f.relight();
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            if (f.tile(x, y, 8).isAir()) f.expect(x, y, 8, 0);
    // Facing open air it lights below.
    f.place(8, 7, 8, 0);
    f.relight();
    CHECK(f.light(8, 4, 8) > 0);
}

TEST(block_light_sealed_registry) {
    LightFixture f;
    f.air({0, 0, 0});
    f.place(8, 8, 8, Beacon);
    for (const auto& d :
         {std::array{1, 0, 0}, std::array{-1, 0, 0}, std::array{0, 1, 0},
          std::array{0, -1, 0}, std::array{0, 0, 1}, std::array{0, 0, -1}})
        f.place(8 + d[0], 8 + d[1], 8 + d[2], Stone);
    f.relight();
    const auto* beacon = f.registry.blockLight(LightFixture::at(8, 8, 8));
    CHECK(beacon != nullptr);
    EQUAL(int(beacon->openFaces), 0, "walled in");
    CHECK(beacon->sealed());
    CHECK(visibleFarLights(f.registry, {8, LightFixture::BASE_Y + 8.0, 8}, 4096,
                           0)
              .empty());
    f.place(8, 9, 8, 0);
    f.relight();
    beacon = f.registry.blockLight(LightFixture::at(8, 8, 8));
    EQUAL(int(beacon->openFaces), 1 << 2, "+y opened");
    CHECK(!beacon->sealed());
}

TEST(block_light_spot_sealed_by_front) {
    LightFixture f;
    f.air({0, 0, 0});
    // Open on every side but the one it faces: a spotlight is sealed.
    f.place(8, 7, 8, Stone);
    f.place(8, 8, 8, Spot);
    f.relight();
    const auto* spot = f.registry.blockLight(LightFixture::at(8, 8, 8));
    CHECK(spot != nullptr);
    CHECK(spot->sealed());
}

TEST(block_light_open_faces_neighbor_arrives) {
    LightFixture f;
    f.air({0, 0, 0});
    f.place(15, 8, 8, Lamp);
    f.relight();
    const auto pos = LightFixture::at(15, 8, 8);
    // +x is in a chunk that is not loaded yet: assumed open.
    EQUAL(int(f.registry.blockLight(pos)->openFaces), 0x3f, "all open");
    f.air({1, 0, 0});
    f.relight();
    f.place(16, 8, 8, Stone);
    f.relight();
    EQUAL(int(f.registry.blockLight(pos)->openFaces), 0x3e,
          "+x closed by the neighbor chunk");
}

TEST(light_registry_buckets) {
    LightSourceRegistry registry;
    LightEmission small;
    // More buckets than the query window, so the window path is taken.
    for (int i = 0; i < 200; ++i)
        registry.setBlockLight({i * 200, 10, 300}, 1, small);
    // Around x = 4000 with a 300-tile range: lights at 3800, 4000, 4200
    // (bucket granularity may add a few more; exact range is the caller's).
    std::vector<int> xs;
    registry.forEachNear(4000, 300, 300, 0,
                         [&](LightSourceRegistry::Id, const LightSource& s) {
                             xs.push_back(s.tile.x);
                         });
    for (int want : {3800, 4000, 4200})
        CHECK(std::find(xs.begin(), xs.end(), want) != xs.end());
    CHECK(xs.size() < 10);
    // Wrapping: from x = 50 in a 40000-wide world, x = 39800 is 250 away.
    xs.clear();
    registry.forEachNear(50, 300, 300, 40000,
                         [&](LightSourceRegistry::Id, const LightSource& s) {
                             xs.push_back(s.tile.x);
                         });
    CHECK(std::find(xs.begin(), xs.end(), 39800) != xs.end());
    const auto inChunk = registry.blockLightsIn({192, 0, 288}, {208, 16, 304});
    EQUAL(inChunk.size(), size_t(1), "one light in the chunk box");
    const auto version = registry.version();
    registry.removeBlockLight({200, 10, 300});
    CHECK(registry.version() != version);
}

TEST(block_light_far_spreads_directly) {
    LightFixture f;
    f.air({0, 0, 0});
    // A far light walled in on five faces, open toward +x.
    f.place(8, 8, 8, Beacon);
    for (const auto& d :
         {std::array{-1, 0, 0}, std::array{0, 1, 0}, std::array{0, -1, 0},
          std::array{0, 0, 1}, std::array{0, 0, -1}})
        f.place(8 + d[0], 8 + d[1], 8 + d[2], Stone);
    f.relight();
    // In front of the opening, also below it.
    CHECK(f.light(10, 8, 8) > 0);
    CHECK(f.light(11, 5, 8) > 0);
    // Under, above and behind the box the light would have to turn back.
    f.expect(8, 5, 8, 0);
    f.expect(8, 11, 8, 0);
    f.expect(5, 8, 8, 0);
    // A small lamp in the same box still fills around the corner.
    f.place(8, 8, 8, Lamp);
    f.relight();
    CHECK(f.light(8, 5, 8) > 0);
}
