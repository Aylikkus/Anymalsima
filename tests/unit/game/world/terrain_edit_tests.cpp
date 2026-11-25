#include <set>
#include <tuple>

#include "test.hpp"
#include "test_assets.hpp"
#include "world/location_manager.hpp"
#include "world/save/chunk_codec.hpp"
#include "world/save/structures_codec.hpp"
#include "world/save/terrain_edits_codec.hpp"
#include "world/sky_height_map.hpp"
#include "world/surface_sampler.hpp"
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

TEST(edit_ground_shape_rebuild) {
    EditFixture f;
    const TilePos pos{40, f.surface(40, 40), 40};
    const ChunkCoord slab{2, unsigned(pos.y / 16), 2};
    const auto key = editKey(2, 2);
    const int index = ColumnEdit::index(8, 8);
    f.load(2, 2, 2, 2, pos.y - 16, pos.y + 16);
    const auto original = *f.chunks.cellAt(pos.x, pos.y, pos.z);
    const auto ramp =
        TileShape::make(TileShapeProfile::InnerHalfHigh, TileFacing::NegZ);
    const auto block = original.tile.block_id;
    CHECK(f.editor.setBlock(pos, block, ramp) == EditStatus::Applied);
    CHECK(f.chunks.isEdited(slab));
    f.tracker.update();
    CHECK(f.edits.find(key) && f.edits.find(key)->groundShape[index] == ramp);
    CHECK(f.editor.setBlock(pos, block, ramp.rotated(1)) ==
          EditStatus::Applied);
    f.tracker.update();
    const auto saved = *f.chunks.getChunk(slab);
    f.chunks.unregister(slab);
    f.tracker.markDirty(2, 2);
    f.tracker.update();
    const auto file = terrain_edits_codec::decode(
        terrain_edits_codec::encode(*f.edits.snapshot()));
    CHECK(file.columns.at(key)->groundShape[index] == ramp.rotated(1));
    SurfaceSampler distant(f.world, f.assets, nullptr,
                           std::make_shared<const EditMap>(file.columns));
    CHECK(distant.column(pos.x, pos.z).groundShape == ramp.rotated(1));

    f.chunks.registerChunk(
        slab, {}, &f.world.regions.front(),
        std::make_shared<const Bytes>(chunk_codec::encode(saved)));
    f.load(2, 2, 2, 2, pos.y, pos.y);
    CHECK(f.chunks.isEdited(slab));
    const uint8_t substitute = block == 1 ? 2 : 1;
    CHECK(f.editor.setBlock(pos, substitute) == EditStatus::Applied);
    CHECK(f.chunks.isEdited(slab));
    f.tracker.update();
    CHECK(f.edits.find(key) &&
          f.edits.find(key)->groundBlock[index] == substitute);
    CHECK(f.editor.setBlock(pos, block) == EditStatus::Applied);
    CHECK(!f.chunks.isEdited(slab));
    f.tracker.update();
    CHECK(!f.edits.contains(key));
}

namespace {
class CastleSource final : public structure_voxels::Source {
   public:
    explicit CastleSource(bool tower = false, TileShape roofShape = {})
        : tower(tower), roofShape(roofShape) {
        key = 701;
        revision = tower ? 2 : 1;
        bounds = {40, 478, 40, 44, tower ? 517 : 481, 41};
    }
    std::optional<structure_voxels::Slab> rasterize(int level, int sx, int sy,
                                                    int sz) const override {
        using namespace structure_voxels;
        const int span = spanOf(level), scale = 1 << level;
        const TileBox slab{sx * span,       sy * span,       sz * span,
                           (sx + 1) * span, (sy + 1) * span, (sz + 1) * span};
        Slab out;
        out.block = 1;
        std::array<int, Cells> counts{};
        const auto add = [&](int x, int y, TileShape shape = TileShape{}) {
            if (slab.intersect({x, y, 40, x + 1, y + 1, 41}).empty()) return;
            const size_t i =
                cellIndex((x - slab.x0) / scale, (y - slab.y0) / scale,
                          (40 - slab.z0) / scale);
            if (++counts[i] * 4 >= scale * scale * scale) out.bits.set(i);
            if (!level) out.setShape(i, shape);
        };
        add(40, 478);
        add(41, 480);
        if (tower)
            add(40, 516);
        else
            add(40, 480);
        add(42, 480, roofShape);
        add(43, 480);
        return out.bits.any() ? std::optional{std::move(out)} : std::nullopt;
    }

   private:
    bool tower;
    TileShape roofShape;
};
}  // namespace

TEST(edit_generated_castle_damage) {
    using namespace structure_voxels;
    EditFixture f;
    const auto castle = std::make_shared<const CastleSource>();
    f.chunks.setGeneratedStructureKinds({Kind(4, {{castle, 40, 40, true}})});
    const uint64_t key = editKey(2, 2);
    const ChunkCoord roof{2, 30, 2};
    f.load(2, 2, 2, 2, f.surface(40, 40) - 16, 526);
    const auto reload = [&](const Chunk& saved) {
        f.chunks.registerChunk(
            roof, {}, &f.world.regions.front(),
            std::make_shared<const Bytes>(chunk_codec::encode(saved)));
        f.load(2, 2, 2, 2, 480, 480);
        f.tracker.update();
    };
    const Chunk original = *f.chunks.getChunk(roof);
    reload(original);
    CHECK(!f.chunks.isEdited(roof));
    f.tracker.markDirty(2, 2);
    f.tracker.update();
    CHECK(!f.edits.contains(key));

    CHECK(f.editor.removeBlock({40, 480, 40}) == EditStatus::Applied);
    CHECK(f.editor.removeBlock({42, 480, 40}) == EditStatus::Applied);
    CHECK(f.editor.placeBlock({42, 480, 40}, 2) == EditStatus::Applied);
    f.tracker.update();
    const auto edit = f.edits.find(key);
    CHECK(edit && edit->suppresses(8, 480, 8));
    CHECK(edit->suppresses(10, 480, 8));
    CHECK(!edit->suppresses(9, 480, 8));
    CHECK((f.structureTiles() == std::set<Tile3>{{42, 480, 40}}));

    const auto masked = f.edits.maskStructure(castle, f.chunks.wrap());
    const auto exact = masked->rasterize(0, 2, 30, 2);
    CHECK(exact && !exact->bits.test(cellIndex(8, 0, 8)));
    CHECK(exact->bits.test(cellIndex(9, 0, 8)));
    const auto coarse = masked->rasterize(1, 1, 15, 1);
    CHECK(!coarse);
    std::vector<int> tops;
    masked->skyTops(castle->bounds, tops);
    EQUAL(tops[0], 478, "removed roof exposes the floor below");

    const Chunk damaged = *f.chunks.getChunk(roof);
    f.chunks.unregister(roof);
    f.tracker.markDirty(2, 2);
    f.tracker.update();
    CHECK(f.edits.find(key)->suppresses(8, 480, 8));
    reload(damaged);
    CHECK(f.chunks.isEdited(roof));
    SkyHeightMap map(f.chunks, f.edits, f.assets.getStorage<Block>(), f.pool);
    map.request(2, 2);
    map.update();
    CHECK(f.pool.wait_for(std::chrono::seconds(3)));
    map.update();
    EQUAL(map.top(40, 40).value_or(-1), 478, "streamed sky follows damage");
    EQUAL(map.top(41, 40).value_or(-1), 480, "adjacent roof stays");
    CHECK(f.editor.placeBlock({40, 480, 40}, 1) == EditStatus::Applied);
    CHECK(f.editor.removeBlock({42, 480, 40}) == EditStatus::Applied);
    CHECK(f.editor.placeBlock({42, 480, 40}, 1) == EditStatus::Applied);
    f.tracker.update();
    CHECK(!f.edits.contains(key));
}

TEST(edit_generated_castle_revision) {
    using namespace structure_voxels;
    EditFixture f;
    const auto castle = std::make_shared<const CastleSource>();
    const auto tower = std::make_shared<const CastleSource>(true);
    f.chunks.setGeneratedStructureKinds({Kind(4, {{castle, 40, 40, true}})});
    f.load(2, 2, 2, 2, f.surface(40, 40) - 16, 526);
    const ChunkCoord roof{2, 30, 2}, upper{2, 32, 2};
    f.chunks.unregister(roof);
    f.chunks.unregister(upper);
    f.chunks.setGeneratedStructureKinds({Kind(4, {{tower, 40, 40, true}})});
    // With no stored chunk, this column now uses the settlement's revision.
    for (const auto coord : {roof, upper})
        f.chunks.registerChunk(coord, {}, &f.world.regions.front());
    f.load(2, 2, 2, 2, 480, 526);
    CHECK(f.chunks.tileAt(40, 480, 40)->isAir());
    EQUAL(f.chunks.tileAt(40, 516, 40)->block_id, 1, "new watchtower");
    f.tracker.markDirty(2, 2);
    f.tracker.update();
    CHECK(!f.edits.contains(editKey(2, 2)));
}

TEST(structure_revision_rebases_saved_and_ready_chunks) {
    using namespace structure_voxels;
    EditFixture f;
    const auto generated = TileShape::make(TileShapeProfile::OuterHalfLow);
    const auto castle = std::make_shared<const CastleSource>(false, generated);
    f.chunks.setGeneratedStructureKinds(
        {Kind(4, {{castle, 40, 40, true}}, "castle")});
    f.load(2, 2, 2, 2, f.surface(40, 40) - 16, 526);
    const ChunkCoord roof{2, 30, 2}, tower{2, 32, 2};
    // Keep a player replacement where the NPCs are about to remove the roof.
    const TilePos rotatedRoof{42, 480, 40};
    const auto shape = generated.rotated(1);
    CHECK(f.editor.setBlock(rotatedRoof, 1, shape) == EditStatus::Applied);
    f.tracker.update();
    CHECK(f.edits.find(editKey(2, 2))->suppresses(10, 480, 8));

    const auto before = region_file::decode(region_file::encode(
        region_file::merge({}, f.chunks.snapshotRegion({0, 0}))));
    const auto roofIndex = RegionData::localIndex(roof);
    const auto towerIndex = RegionData::localIndex(tower);
    CHECK(!before.structureBaselines.at(roofIndex)->empty());
    Chunk baseline;
    chunk_codec::decode(*before.structureBaselines.at(roofIndex), baseline);
    CHECK(baseline.cellAt(10, 0, 8).shape == generated);
    CHECK(before.structureBaselines.at(towerIndex)->empty());
    f.chunks.unregister(roof);

    // The tower slab is resident; the roof slab stays on disk while the
    // settlement changes both. Restore the override as a new session would.
    Overrides overrides;
    overrides["castle"][castle->key] = {
        OverrideMode::Patch,
        2,
        40,
        40,
        true,
        {{{40, 480, 40}, 0}, {{42, 480, 40}, 0}, {{40, 516, 40}, 1}}};
    f.chunks.setStructureOverrides(
        structures_codec::decode(structures_codec::encode({}, overrides))
            .voxels);
    for (int pass = 0; f.chunks.hasStructureWork(); ++pass) {
        CHECK(pass < 100);
        f.chunks.update(&f.editor);
        CHECK(f.pool.wait_for(std::chrono::seconds(3)));
    }
    EQUAL(f.chunks.tileAt(40, 516, 40)->block_id, 1, "tower applied live");
    CHECK(!f.chunks.isRegistered(roof));

    f.chunks.registerChunk(
        roof, {}, &f.world.regions.front(), before.chunks.at(roofIndex),
        before.flora.at(roofIndex), before.structureBaselines.at(roofIndex));
    f.load(2, 2, 2, 2, 480, 480);
    CHECK(f.chunks.tileAt(40, 480, 40)->isAir());
    const auto player = f.chunks.cellAt(42, 480, 40);
    CHECK(player && player->tile.block_id == 1 && player->shape == shape);
    CHECK(f.chunks.isPersistDirty(roof));
    f.tracker.update();
    const auto edit = f.edits.find(editKey(2, 2));
    CHECK(edit && edit->removed.empty());
    CHECK((f.structureTiles() == std::set<Tile3>{{42, 480, 40}}));
    Chunk distant;
    chunk_codec::decode(**edit->slab(30), distant);
    CHECK(distant.cellAt(10, 0, 8).shape == shape);

    f.chunks.registerChunk(
        tower, {}, &f.world.regions.front(), before.chunks.at(towerIndex),
        before.flora.at(towerIndex), before.structureBaselines.at(towerIndex));
    f.load(2, 2, 2, 2, 516, 516);
    EQUAL(f.chunks.tileAt(40, 516, 40)->block_id, 1, "tower applied at reload");
    CHECK(!f.chunks.isEdited(tower));
}
