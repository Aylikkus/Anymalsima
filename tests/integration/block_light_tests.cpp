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
    // Near lamps never enter the far-light registry.
    CHECK(!f.registry.blockLight(LightFixture::at(2, 8, 8)));
    CHECK(f.registry.dirty);
    // The near field fades out toward the hand-over to the per-pixel term.
    f.expect(9, 8, 8, 15);
    f.expect(8, 8, 15, int(std::lround(15 * (1 - 7.0 / 64))));
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
    registry.setBlockLight({20, 0, 0}, 1, near);   // ignored: not far
    registry.setBlockLight({4090, 0, 0}, 1, far);  // across the seam
    registry.setBlockLight({2000, 0, 0}, 1, far);  // out of range
    EQUAL(registry.size(), size_t(3), "near lights are not stored");
    const auto views = visibleFarLights(registry, {0, 0, 0}, 300, 4096);
    EQUAL(views.size(), size_t(2), "visible");
    // Wrapped copy at x = -5.5 is nearest; both reach the eye.
    CHECK(std::abs(views[0].position[0] - -5.5) < 1e-9);
    CHECK(std::abs(views[1].position[0] - 10.5) < 1e-9);
}
