#include <set>
#include <tuple>

#include "test.hpp"
#include "test_assets.hpp"
#include "world/location_manager.hpp"
#include "world/save/chunk_codec.hpp"
#include "world/terrain_edit_tracker.hpp"

namespace {
using Tile3 = std::tuple<int, int, int>;

World editWorld() {
    World world{};
    world.width = 2;
    world.height = 2;
    world.genSettings.height_seed = 1234;
    world.genSettings.feature_seed = 5678;
    world.regions.resize(4);
    for (int z = 0; z < 2; ++z)
        for (int x = 0; x < 2; ++x) {
            auto& r = world.regions[z * 2 + x];
            r = test::regionAt(430);
            r.coord = {unsigned(x), unsigned(z)};
            r.land = LandType::PLAINS;
            r.vegetation = VegetationType::GRASS;
            r.humidity = .5;
            r.temperature_c = 15;
        }
    return world;
}

// A 2x2-region world (region border at tile x = 128) with production chunk
// streaming, editor and tracker; no meshing or lighting.
struct EditFixture {
    test::Assets assets;
    World world = editWorld();
    LocationGenerator generator;
    ThreadPool pool{2};
    ChunkManager chunks{generator, pool, nullptr, world, &assets};
    LocationManager locations{chunks, world};
    TerrainEdits edits;
    WorldEditor editor{chunks, nullptr, assets.getStorage<Block>()};
    // Disconnects from the editor when destroyed: declared after it.
    TerrainEditTracker tracker{chunks, edits};

    EditFixture() {
        locations.loadRegion({0, 0}, 1);
        tracker.watch(editor);
    }
    ~EditFixture() { pool.wait(); }

    int surface(int x, int z) { return chunks.generatedSurfaceY(x, z); }
    // Loads every slab of chunk columns [x0, x1] x [z0, z1] covering
    // tile heights [low, high].
    void load(unsigned x0, unsigned x1, unsigned z0, unsigned z1, int low,
              int high) {
        std::vector<ChunkCoord> wanted;
        for (unsigned z = z0; z <= z1; ++z)
            for (unsigned x = x0; x <= x1; ++x)
                for (int y = low / 16; y <= high / 16; ++y)
                    wanted.push_back({x, unsigned(y), z});
        for (int round = 0; round < 200; ++round) {
            bool ready = true;
            for (const auto& c : wanted) ready &= chunks.request(c) != nullptr;
            if (ready) return;
            chunks.update();
            CHECK(pool.wait_for(std::chrono::seconds(3)));
        }
        CHECK(false);
    }
    std::set<Tile3> structureTiles() const {
        std::set<Tile3> result;
        for (const auto& [key, edit] : *edits.snapshot())
            for (const auto& [slabY, blob] : edit->structure) {
                Chunk chunk;
                chunk_codec::decode(*blob, chunk);
                for (int z = 0; z < 16; ++z)
                    for (int y = 0; y < 16; ++y)
                        for (int x = 0; x < 16; ++x)
                            if (chunk.tiles[z][y][x].isSolid())
                                result.insert({int(key >> 32) * 16 + x,
                                               slabY * 16 + y,
                                               int(uint32_t(key)) * 16 + z});
            }
        return result;
    }
};
}  // namespace

TEST(edit_dig_pit_lowers_ground) {
    EditFixture f;
    const int s = f.surface(37, 37);
    f.load(2, 2, 2, 2, s - 24, s + 8);
    std::vector<EditRequest> dig;
    for (int z = 36; z <= 38; ++z)
        for (int x = 36; x <= 38; ++x)
            for (int y = f.surface(x, z); y > f.surface(x, z) - 5; --y)
                dig.push_back({{x, y, z}, EditMode::Remove});
    f.editor.apply(dig);
    f.tracker.update();
    const auto edit = f.edits.find(editKey(2, 2));
    CHECK(edit != nullptr);
    for (int z = 32; z < 48; ++z)
        for (int x = 32; x < 48; ++x) {
            const bool pit = x >= 36 && x <= 38 && z >= 36 && z <= 38;
            EQUAL(int(edit->groundY[ColumnEdit::index(x - 32, z - 32)]),
                  f.surface(x, z) - (pit ? 5 : 0), "ground height");
        }
    CHECK(edit->structure.empty());
    // Refilling restores generated terrain: the column needs no entry.
    for (auto& request : dig) {
        request.mode = EditMode::Place;
        request.block = 1;
    }
    f.editor.apply(dig);
    f.tracker.update();
    CHECK(!f.edits.contains(editKey(2, 2)));
}

TEST(edit_structure_overhang) {
    EditFixture f;
    const int s = f.surface(40, 40);
    f.load(2, 2, 2, 2, s - 24, s + 40);
    std::vector<EditRequest> build;
    std::set<Tile3> placed;
    for (int y = s + 1; y <= s + 30; ++y) {
        build.push_back({{40, y, 40}, EditMode::Place, 2});
        placed.insert({40, y, 40});
    }
    for (int z = 38; z <= 42; ++z)
        for (int x = 38; x <= 42; ++x)
            if (x != 40 || z != 40) {
                build.push_back({{x, s + 30, z}, EditMode::Place, 1});
                placed.insert({x, s + 30, z});
            }
    f.editor.apply(build);
    f.tracker.update();
    const auto edit = f.edits.find(editKey(2, 2));
    CHECK(edit != nullptr);
    CHECK(f.structureTiles() == placed);
    EQUAL(edit->structureTopY(), s + 30, "structure top");
    // Ground under the overhang stays generated.
    EQUAL(int(edit->groundY[ColumnEdit::index(6, 6)]), f.surface(38, 38),
          "ground below overhang");
    for (auto& request : build) request.mode = EditMode::Remove;
    f.editor.apply(build);
    f.tracker.update();
    CHECK(!f.edits.contains(editKey(2, 2)));
}

TEST(edit_structure_crosses_region) {
    EditFixture f;
    int high = 0, low = WORLD_HEIGHT_Y;
    for (int x = 118; x <= 138; ++x) {
        high = std::max(high, f.surface(x, 40));
        low = std::min(low, f.surface(x, 40));
    }
    f.load(7, 8, 2, 2, low - 24, high + 24);
    // One batch: a wall across the region border with a one-sided overhang.
    std::vector<EditRequest> build;
    std::set<Tile3> placed;
    for (int x = 118; x <= 138; ++x) {
        for (int y = f.surface(x, 40) + 1; y <= high + 10; ++y) {
            build.push_back({{x, y, 40}, EditMode::Place, 1});
            placed.insert({x, y, 40});
        }
        build.push_back({{x, high + 10, 41}, EditMode::Place, 1});
        placed.insert({x, high + 10, 41});
    }
    f.editor.apply(build);
    f.tracker.update();
    CHECK(f.edits.contains(editKey(7, 2)));
    CHECK(f.edits.contains(editKey(8, 2)));
    CHECK(f.structureTiles() == placed);
    // Unloading one region keeps the whole structure in the index, and
    // refreshing an unregistered column does not drop it.
    f.locations.unloadDistant({0, 0}, 0);
    CHECK(!f.chunks.isRegistered({8, 0, 2}));
    f.tracker.markDirty(8, 2);
    f.tracker.update();
    CHECK(f.structureTiles() == placed);
}

TEST(edit_unloaded_slab_keeps_structure) {
    EditFixture f;
    const int s = f.surface(40, 40);
    f.load(2, 2, 2, 2, s - 24, s + 40);
    std::vector<EditRequest> build;
    std::set<Tile3> placed;
    for (int y = s + 1; y <= s + 34; ++y) {
        build.push_back({{40, y, 40}, EditMode::Place, 2});
        placed.insert({40, y, 40});
    }
    f.editor.apply(build);
    f.tracker.update();
    CHECK(f.structureTiles() == placed);
    // The top slab leaves memory; a later refresh keeps its stored voxels.
    const ChunkCoord top{2, unsigned((s + 34) / 16), 2};
    f.chunks.unregister(top);
    f.tracker.markDirty(2, 2);
    f.tracker.update();
    CHECK(f.structureTiles() == placed);
}

TEST(edit_detected_from_save) {
    EditFixture f;
    const int s = f.surface(40, 40);
    f.load(2, 2, 2, 2, s - 24, s + 8);
    const ChunkCoord coord{2, unsigned(s / 16), 2};
    const ChunkCoord origin{0, 0, 0};
    const Region* region = &f.world.regions[0];
    // An untouched saved chunk is not an edit.
    Chunk copy = *f.chunks.getChunk(coord);
    f.chunks.unregister(coord);
    f.chunks.registerChunk(
        coord, origin, region,
        std::make_shared<const Bytes>(chunk_codec::encode(copy)));
    f.load(2, 2, 2, 2, s, s);
    CHECK(!f.chunks.isEdited(coord));
    f.tracker.update();
    CHECK(!f.edits.contains(editKey(2, 2)));
    // A saved chunk with a dug tile (an edit from an earlier session).
    copy.tiles[8][s % 16][8].block_id = 0;
    f.chunks.unregister(coord);
    f.chunks.registerChunk(
        coord, origin, region,
        std::make_shared<const Bytes>(chunk_codec::encode(copy)));
    f.load(2, 2, 2, 2, s, s);
    CHECK(f.chunks.isEdited(coord));
    f.tracker.update();
    const auto edit = f.edits.find(editKey(2, 2));
    CHECK(edit != nullptr);
    EQUAL(int(edit->groundY[ColumnEdit::index(8, 8)]), s - 1,
          "detected dug tile");
}
