#include "world/world_editor.hpp"
#include "world_fixture.hpp"

namespace {
struct EditorFixture : test::SunlightFixture {
    WorldEditor editor{chunks, &sunlight, assets.getStorage<Block>()};
    static TilePos at(int x, int y, int z) { return {x, y + BASE_Y, z}; }
};
void expectStatus(EditStatus actual, EditStatus expected,
                  const std::string& context) {
    EQUAL(int(actual), int(expected), context);
}
}  // namespace

TEST(editor_place_remove) {
    EditorFixture f;
    f.load({0, 0, 0});
    const auto coord = test::SunlightFixture::coord({0, 0, 0});
    std::vector<BlockChange> changes;
    f.editor.blockChanged.connect(
        [&](const BlockChange& c) { changes.push_back(c); });
    int lightEvents = 0;
    f.chunks.lightChanged.connect([&](const ChunkCoord&) { ++lightEvents; });

    f.tile(5, 5, 5).liquid = LiquidType::Water;
    expectStatus(f.editor.placeBlock(EditorFixture::at(5, 5, 5), 2),
                 EditStatus::Applied, "place into water");
    EQUAL(int(f.tile(5, 5, 5).block_id), 2, "placed block");
    CHECK(!f.tile(5, 5, 5).isLiquid());
    CHECK(f.chunks.isPersistDirty(coord));
    CHECK(lightEvents > 0);
    EQUAL(changes.size(), size_t(1), "one change");
    CHECK(changes[0].before.isLiquid());
    EQUAL(int(changes[0].after.block_id), 2, "change records new block");

    expectStatus(f.editor.placeBlock(EditorFixture::at(5, 5, 5), 1),
                 EditStatus::Occupied, "place into solid");
    expectStatus(f.editor.removeBlock(EditorFixture::at(5, 5, 5)),
                 EditStatus::Applied, "remove");
    CHECK(f.tile(5, 5, 5).isAir());
    expectStatus(f.editor.removeBlock(EditorFixture::at(5, 5, 5)),
                 EditStatus::Empty, "remove air");
    expectStatus(f.editor.setBlock(EditorFixture::at(5, 5, 5), 0),
                 EditStatus::Unchanged, "replace air with air");
    expectStatus(f.editor.placeBlock(EditorFixture::at(5, 5, 5), 200),
                 EditStatus::InvalidBlock, "undefined block");
    expectStatus(f.editor.placeBlock(EditorFixture::at(40, 5, 5), 1),
                 EditStatus::NotLoaded, "unloaded chunk");
    expectStatus(f.editor.placeBlock({-1, 0, 0}, 1), EditStatus::NotLoaded,
                 "negative coordinate");
    EQUAL(changes.size(), size_t(2), "only applied edits are reported");
}

TEST(editor_raises_column_top) {
    EditorFixture f;
    f.load({0, 0, 0});
    f.load({0, 1, 0});
    const int before = f.chunks.getColumnTopY(0, 0);
    const auto pos = EditorFixture::at(3, 30, 3);
    f.editor.placeBlock(pos, 1);
    CHECK(f.chunks.getColumnTopY(0, 0) >= pos.y);
    CHECK(f.chunks.getColumnTopY(0, 0) >= before);
}

TEST(editor_bulk_statuses) {
    EditorFixture f;
    f.load({0, 0, 0});
    const std::vector<EditRequest> requests{
        {EditorFixture::at(1, 1, 1), EditMode::Place, 1},
        {EditorFixture::at(1, 1, 1), EditMode::Place, 1},
        {EditorFixture::at(2, 1, 1), EditMode::Remove, 0},
        {EditorFixture::at(1, 1, 1), EditMode::Replace, 2},
    };
    int modified = 0;
    f.chunks.lightChanged.connect([&](const ChunkCoord&) { ++modified; });
    const auto result = f.editor.apply(requests);
    EQUAL(result.size(), requests.size(), "one status per request");
    expectStatus(result[0], EditStatus::Applied, "first place");
    expectStatus(result[1], EditStatus::Occupied, "sees earlier edit");
    expectStatus(result[2], EditStatus::Empty, "remove air");
    expectStatus(result[3], EditStatus::Applied, "replace");
    EQUAL(int(f.tile(1, 1, 1).block_id), 2, "final block");
    // Bookkeeping is coalesced: one modification per touched chunk.
    EQUAL(modified, 1, "one light notification for the batch");
}

TEST(editor_shape_changes) {
    EditorFixture f;
    const auto pos = EditorFixture::at(5, 5, 5);
    auto& chunk = f.load({0, 0, 0});
    const auto cell = Chunk::cell(5, 5, 5);
    const auto ramp = TileShape::make(TileShapeProfile::Ramp);
    const auto wall = TileShape::make(TileShapeProfile::Wall25);
    std::vector<BlockChange> changes;
    f.editor.blockChanged.connect(
        [&](const BlockChange& c) { changes.push_back(c); });
    expectStatus(f.editor.placeBlock(pos, 1), EditStatus::Applied, "cube");
    const Chunk snapshot = chunk;
    const EditRequest batch[] = {
        {.pos = pos, .mode = EditMode::Replace, .block = 1, .shape = ramp},
        {.pos = pos, .mode = EditMode::Replace, .block = 1, .shape = ramp},
        {.pos = pos,
         .mode = EditMode::Replace,
         .block = 1,
         .shape = ramp.rotated(1)},
    };
    const auto status = f.editor.apply(batch);
    CHECK(status[0] == EditStatus::Applied &&
          status[1] == EditStatus::Unchanged &&
          status[2] == EditStatus::Applied);
    CHECK(changes.size() == 3);
    CHECK(changes[1].beforeShape == TileShape{} &&
          changes[1].afterShape == ramp);
    CHECK(changes[2].beforeShape == ramp &&
          changes[2].afterShape == ramp.rotated(1));
    CHECK(changes[2].before.block_id == 1 && changes[2].after.block_id == 1);
    CHECK(!snapshot.hasShapes() && chunk.shapeAt(cell) == ramp.rotated(1));
    expectStatus(f.editor.removeBlock(pos), EditStatus::Applied,
                 "shaped removal");
    CHECK(!chunk.hasShapes() && changes.back().afterShape == TileShape{});

    Tile& localWater = f.tile(5, 5, 5);
    localWater.liquid = LiquidType::Water;
    localWater.waterOrigin = WaterOrigin::Local;
    localWater.liquidLevel = 91;
    expectStatus(f.editor.placeBlock(pos, 1, wall), EditStatus::Blocked,
                 "local water ledger");
    CHECK(localWater.liquidLevel == 91 && !chunk.hasShapes());
    localWater.waterOrigin = WaterOrigin::Reservoir;
    expectStatus(f.editor.placeBlock(pos, 1, wall), EditStatus::Applied,
                 "partial block displaces the whole projected cell");
    CHECK(!localWater.isLiquid() && chunk.shapeAt(cell) == wall);
    expectStatus(f.editor.setBlock(pos, 1), EditStatus::Applied,
                 "legacy API sets a cube");
    CHECK(!chunk.hasShapes());

    const auto natural = EditorFixture::at(6, 5, 5);
    const auto rootCell = Chunk::cell(6, 5, 5);
    // Roots can claim only unedited natural substrate.
    chunk.setCell(rootCell, Tile{.block_id = 1, .lightLevel = {}}, ramp);
    expectStatus(f.editor.apply({.pos = natural,
                                 .mode = EditMode::Replace,
                                 .block = 2,
                                 .treeGrowth = true,
                                 .rootOwner = 17}),
                 EditStatus::Applied, "growth displaces the substrate");
    CHECK(chunk.rootAt(rootCell) && chunk.rootAt(rootCell)->shape == ramp);
    CHECK(!chunk.hasShapes() && changes.back().beforeShape == ramp);
}

TEST(editor_relights_column) {
    EditorFixture f;
    f.load({0, 0, 0});
    f.load({0, 1, 0});
    f.light();
    f.expectSun(8, 4, 8, 15);
    // A roof cuts direct sunlight below it; removing it restores the light.
    std::vector<EditRequest> roof;
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            roof.push_back({EditorFixture::at(x, 20, z), EditMode::Place, 1});
    f.editor.apply(roof);
    f.light();
    f.expectSun(8, 4, 8, 0);
    for (auto& request : roof) request.mode = EditMode::Remove;
    f.editor.apply(roof);
    f.light();
    f.expectSun(8, 4, 8, 15);
}

TEST(ray_reports_face) {
    test::WorldFixture f;
    auto& chunk = f.load({0, 0, 0});
    chunk.tiles[5][5][5].block_id = 1;
    // Down onto the top face, skipping water above it.
    chunk.tiles[5][7][5].liquid = LiquidType::Water;
    auto hit = castTileRay(f.chunks, 5.5f, 9.5f, 5.5f, 0, -1, 0, 16, false);
    CHECK(hit.has_value());
    EQUAL(hit->ty, 5, "solid hit below water");
    EQUAL(hit->placementOffset.y, 1, "entered through the top face");
    EQUAL(hit->placementOffset.x + hit->placementOffset.z, 0, "single axis normal");
    hit = castTileRay(f.chunks, 1.5f, 5.5f, 5.5f, 1, 0, 0, 16);
    CHECK(hit.has_value());
    EQUAL(hit->placementOffset.x, -1, "entered through the -x face");
}

TEST(editor_relight_keeps_indirect) {
    EditorFixture f;
    f.load({0, 0, 0});
    f.load({0, 1, 0});
    // A roof over x < 8 leaves the cell below lit only by bounced light.
    std::vector<EditRequest> roof;
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 8; ++x)
            roof.push_back({EditorFixture::at(x, 20, z), EditMode::Place, 1});
    f.editor.apply(roof);
    f.light();
    const int lit = f.tile(2, 10, 8).lightLevel.sun_exposure;
    CHECK(lit > 0);
    // An unrelated edit in the open part of the column recomputes direct
    // light for both slabs. Until indirect light finishes, the shadowed cell
    // must keep its previous light instead of a direct-only zero.
    f.editor.placeBlock(EditorFixture::at(14, 25, 14), 1);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        f.chunks.update();
        f.sunlight.update();
        CHECK(f.tile(2, 10, 8).lightLevel.sun_exposure > 0);
        f.wait();
        CHECK(std::chrono::steady_clock::now() < deadline);
    } while (!f.sunlight.isIdle());
    EQUAL(int(f.tile(2, 10, 8).lightLevel.sun_exposure), lit,
          "bounced light restored");
}
