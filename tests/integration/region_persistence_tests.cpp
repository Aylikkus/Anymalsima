#include <chrono>
#include <filesystem>
#include <fstream>

#include "render/ui_context.hpp"
#include "test.hpp"
#include "test_assets.hpp"
#include "world/location_manager.hpp"
#include "world/save/region_store.hpp"

namespace {

World flatWorld() {
    World world{};
    world.width = world.height = 3;
    world.genSettings = WorldGenSettings::fromSeed(2024);
    world.regions.resize(9);
    for (unsigned z = 0; z < 3; ++z)
        for (unsigned x = 0; x < 3; ++x) {
            auto& r = world.regions[z * 3 + x];
            r = test::regionAt(420);
            r.coord = {x, z};
            r.land = LandType::PLAINS;
            r.vegetation = VegetationType::GRASS;
            r.humidity = .5;
            r.temperature_c = 15;
        }
    return world;
}

std::filesystem::path scratch(const std::string& name) {
    auto dir = std::filesystem::path(TEST_SCRATCH_DIR) / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// One play session: a ChunkManager + store + LocationManager over a world dir.
struct Session {
    Session(const World& world, const std::filesystem::path& dir)
        : chunks(generator, pool, nullptr, world, &assets),
          store(dir, pool),
          locations(chunks, world, &store) {}
    ~Session() { pool.wait(); }

    template <typename Done>
    void pumpUntil(Done done) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done()) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            locations.update();
            store.update();
            chunks.update();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    Chunk& ready(ChunkCoord coord) {
        pumpUntil([&] { return chunks.isRegistered(coord); });
        pumpUntil([&] { return chunks.request(coord) != nullptr; });
        return *chunks.getChunk(coord);
    }

    test::Assets assets;
    LocationGenerator generator;
    ThreadPool pool{2};
    ChunkManager chunks;
    RegionStore store;
    LocationManager locations;
};

constexpr ChunkCoord Surface{3, 26, 4};  // region (0,0), holds y = 420
constexpr ChunkCoord Deep{5, 3, 1};
constexpr ChunkCoord Sky{2, 40, 6};

bool sameTiles(const Chunk& a, const Chunk& b) {
    for (int z = 0; z < 16; ++z)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                const auto &l = a.tiles[z][y][x], &r = b.tiles[z][y][x];
                if (l.block_id != r.block_id || l.liquid != r.liquid ||
                    l.liquidLevel != r.liquidLevel)
                    return false;
            }
    return true;
}

}  // namespace

TEST(region_save_and_reload) {
    const auto world = flatWorld();
    const auto dir = scratch("reload");
    Chunk surface, deep;
    {
        Session s(world, dir);
        s.locations.loadRegion({0, 0}, 0);
        surface = s.ready(Surface);
        deep = s.ready(Deep);
        s.ready(Sky);
        CHECK(s.chunks.isPersistDirty(Surface));  // freshly generated
        s.locations.unloadDistant({2, 2}, 0);
        CHECK(!s.chunks.isRegistered(Surface));
        s.store.flush();
        EQUAL(s.store.failedWrites(), size_t(0), "write failures");
    }
    CHECK(std::filesystem::is_regular_file(dir / "regions" / "r.0.0.dat"));
    CHECK(!std::filesystem::exists(dir / "regions" / "r.0.0.dat.tmp"));
    {
        Session s(world, dir);
        s.locations.loadRegion({0, 0}, 0);
        CHECK(sameTiles(s.ready(Surface), surface));
        CHECK(sameTiles(s.ready(Deep), deep));
        // Loaded from disk: unchanged chunks are not rewritten.
        CHECK(!s.chunks.isPersistDirty(Surface));
        // Chunks never materialised still come from the generator.
        s.ready({0, 30, 0});
        CHECK(s.chunks.isPersistDirty({0, 30, 0}));
    }
}

TEST(region_edit_persists) {
    const auto world = flatWorld();
    const auto dir = scratch("edit");
    {
        Session s(world, dir);
        s.locations.loadRegion({0, 0}, 0);
        s.ready(Surface);
        s.locations.unloadDistant({2, 2}, 0);
        s.store.flush();
    }
    {
        Session s(world, dir);
        s.locations.loadRegion({0, 0}, 0);
        s.ready(Surface);
        CHECK(s.chunks.withChunkLocked(Surface, [](Chunk& c) {
            c.tiles[1][15][2].block_id = 77;  // y = 431, above the surface
        }));
        s.chunks.markModified(Surface);
        CHECK(s.chunks.isPersistDirty(Surface));
        s.locations.saveAll(true);
        s.store.flush();
    }
    {
        Session s(world, dir);
        s.locations.loadRegion({0, 0}, 0);
        EQUAL(int(s.ready(Surface).tiles[1][15][2].block_id), 77,
              "edited tile");
        // The saved column ceiling covers the edit for sunlight.
        CHECK(s.chunks.getColumnTopY(Surface.x, Surface.z) >= 431);
    }
}

TEST(region_reload_while_writing) {
    const auto world = flatWorld();
    const auto dir = scratch("ordering");
    Session s(world, dir);
    s.locations.loadRegion({0, 0}, 0);
    auto& first = s.ready(Surface);
    s.chunks.withChunkLocked(Surface,
                             [](Chunk& c) { c.tiles[0][15][0].block_id = 55; });
    s.chunks.markModified(Surface);
    (void)first;
    // Unload and immediately load again: the reload must observe the write.
    s.locations.unloadDistant({2, 2}, 0);
    s.locations.loadRegion({0, 0}, 0);
    EQUAL(int(s.ready(Surface).tiles[0][15][0].block_id), 55,
          "edit survives an in-flight write");
    CHECK(!s.chunks.isPersistDirty(Surface));
}

TEST(region_corrupt_file_regenerates) {
    const auto world = flatWorld();
    const auto dir = scratch("corrupt");
    std::filesystem::create_directories(dir / "regions");
    {
        std::ofstream bad(dir / "regions" / "r.0.0.dat", std::ios::binary);
        bad << "ANYM definitely not a region";
    }
    Session s(world, dir);
    s.locations.loadRegion({0, 0}, 0);
    s.ready(Surface);
    CHECK(s.chunks.isPersistDirty(Surface));
    CHECK(std::filesystem::exists(dir / "regions" / "r.0.0.dat.corrupt"));
}

TEST(region_unload_while_opening) {
    const auto world = flatWorld();
    const auto dir = scratch("cancel");
    Session s(world, dir);
    s.locations.loadRegion({0, 0}, 1);
    s.locations.unloadDistant({2, 2}, 0);
    CHECK(!s.locations.hasPendingLoads());
    s.store.flush();
    CHECK(s.store.idle());
    for (unsigned z = 0; z < 2; ++z)
        for (unsigned x = 0; x < 2; ++x)
            CHECK(!s.chunks.isRegistered({x * 8, 0, z * 8}));
}
