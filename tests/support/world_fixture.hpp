#pragma once

#include "test.hpp"

// Production ChunkManager uses the pool alias declared by UIContext.
#include "render/ui_context.hpp"
#include "test_assets.hpp"
#include "world/sunlight_propagator.hpp"
#include "world/tile_raycast.hpp"

namespace test {
struct WorldFixture {
    explicit WorldFixture(unsigned workers = 1) : pool(workers) {}
    Assets assets;
    World world{};
    Region region = regionAt(31);
    LocationGenerator generator;
    ThreadPool pool;
    ChunkMesher mesher{assets};
    ChunkManager chunks{generator, pool, &mesher, world, &assets};
    SunLightPropagator sunlight{chunks, assets.getStorage<Block>(), pool};

    // Wait before owners captured by workers are destroyed, even on failure.
    ~WorldFixture() { pool.wait(); }

    void wait() { CHECK(pool.wait_for(std::chrono::seconds(3))); }
    void registerSlab(ChunkCoord coord) {
        chunks.registerChunk(coord, {0, 0, 0}, &region);
    }
    Chunk& load(ChunkCoord coord, uint8_t sun = 0) {
        if (!chunks.isRegistered(coord)) registerSlab(coord);
        chunks.request(coord);
        chunks.update();
        wait();
        chunks.update();
        auto* chunk = chunks.request(coord);
        CHECK(chunk != nullptr);
        fill(*chunk, 0, sun);
        return *chunk;
    }
    void light() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            chunks.update();
            sunlight.update();
            wait();
            CHECK(std::chrono::steady_clock::now() < deadline);
        } while (!sunlight.isIdle() || chunks.hasGenerationWork());
    }
    Tile& tile(int x, int y, int z) {
        auto* tile = chunks.tileAt(x, y, z);
        CHECK(tile != nullptr);
        return *const_cast<Tile*>(tile);
    }
    void expectSun(int x, int y, int z, int expected) {
        EQUAL(int(tile(x, y, z).lightLevel.sun_exposure), expected,
              "sunlight at (" + std::to_string(x) + "," + std::to_string(y) +
                  "," + std::to_string(z) + ")");
    }
};

// Local Y=0 maps to sea level. Keep synthetic air/roof fixtures above the
// generated ocean bound; their local dimensions and expected levels are fixed.
struct SunlightFixture : WorldFixture {
    static constexpr int BASE_Y = SEA_LEVEL_Y;
    explicit SunlightFixture(unsigned workers = 1) : WorldFixture(workers) {
        region = regionAt(BASE_Y + 31);
    }
    static ChunkCoord coord(ChunkCoord local) {
        local.y += BASE_Y / Chunk::SIZE_Y;
        return local;
    }
    void registerSlab(ChunkCoord local) {
        WorldFixture::registerSlab(coord(local));
    }
    Chunk& load(ChunkCoord local, uint8_t sun = 0) {
        return WorldFixture::load(coord(local), sun);
    }
    Tile& tile(int x, int y, int z) {
        return WorldFixture::tile(x, y + BASE_Y, z);
    }
    void expectSun(int x, int y, int z, int expected) {
        WorldFixture::expectSun(x, y + BASE_Y, z, expected);
    }
};
}  // namespace test
