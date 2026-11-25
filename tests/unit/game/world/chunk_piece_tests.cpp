#include <array>

#include "test.hpp"
#include "world/chunk_mesher.hpp"
#include "world/structure_voxels.hpp"
#include "world_fixture.hpp"

namespace {

using tile_geometry::LayerThickness;
using tile_geometry::TilePiece;

const TileShape slab = TileShape::make(TileShapeProfile::Slab);
const CellPiece lower{{0, 1, slab}, {PieceOrigin::Terrain}};
const CellPiece upper{
    TilePiece::layer(17, 2, LayerThickness::Cm50, {0, .5f, 0}),
    {PieceOrigin::Player, 42}};

}  // namespace

TEST(chunk_piece_storage) {
    Chunk chunk;
    constexpr auto index = Chunk::cell(15, 8, 8);
    static_assert(sizeof(Tile) == 6);
    const auto empty = chunk.solidCellAt(index);
    CHECK(empty.base.tile.isAir() && empty.pieces().empty());
    const Tile stone{.block_id = 1, .lightLevel = {4, 3}, .cover = 2};
    chunk.setCell(index, stone);
    const auto cube = chunk.solidCellAt(index);
    CHECK(cube.pieces().size() == 1 &&
          cube.pieces()[0].geometry.shape.code() == 0);
    chunk.setCell(index, stone, slab);
    const auto ordinary = chunk.solidCellAt(index);
    CHECK(ordinary.pieces().size() == 1 &&
          ordinary.pieces()[0].geometry.shape == slab);
    EQUAL(chunk.pieceStorageBytes(), size_t{0}, "ordinary piece storage");

    const std::array inserted{upper, lower};  // input order is immaterial
    CHECK(chunk.setPieces(index, inserted) == CellPieceEdit::Applied);
    const auto mixed = chunk.solidCellAt(index);
    CHECK(chunk.hasCompoundAt(index) && chunk.compoundCount() == 1);
    EQUAL(mixed.pieces()[0].geometry.id, 0u, "base piece ID");
    EQUAL(mixed.pieces()[1].geometry.id, 17u, "upper piece ID");
    EQUAL(mixed.base.tile.cover, 2, "retained substrate cover");
    CHECK(mixed.pieces()[1].origin == upper.origin);

    auto conflict = lower;
    conflict.geometry.id = 2;
    CHECK(chunk.setPieces(index, std::array{lower, conflict}) ==
          CellPieceEdit::Overlap);
    CHECK(chunk.setPieces(index, std::array{lower, lower}) ==
          CellPieceEdit::Invalid);
    const std::array<CellPiece, Chunk::MaxPiecesPerCell + 1> tooMany{};
    CHECK(chunk.setPieces(index, tooMany) == CellPieceEdit::TooMany);
    const auto unchanged = chunk.solidCellAt(index);
    CHECK(unchanged.compound == mixed.compound);

    // Remove only the lower piece. A lone elevated slab needs the table.
    CHECK(chunk.setPieces(index, mixed.pieces().last(1)) ==
          CellPieceEdit::Applied);
    const auto raised = chunk.solidCellAt(index);
    CHECK(chunk.hasCompoundAt(index) && raised.pieces().size() == 1);
    CHECK(raised.pieces()[0].origin == upper.origin);
    EQUAL(raised.base.tile.block_id, 2, "surviving block");
    EQUAL(raised.base.tile.cover, 0, "removed substrate cover");
    EQUAL(raised.base.tile.lightLevel.sun_exposure, 4, "preserved sunlight");

    // Ground the surviving piece. Its geometry becomes an ordinary slab,
    // while its ID and provenance survive in sparse metadata.
    CellPiece grounded = raised.pieces()[0];
    grounded.geometry.offset = {};
    grounded.geometry.shape = {};
    grounded.geometry.ceiling = .5f;
    CHECK(chunk.setPieces(index, std::span(&grounded, 1)) ==
          CellPieceEdit::Applied);
    const auto simple = chunk.solidCellAt(index);
    CHECK(simple.base.shape == slab && !chunk.hasCompounds());
    CHECK(simple.pieces()[0].origin == upper.origin);
    EQUAL(simple.pieces()[0].geometry.id, 17u, "canonical piece ID");

    CHECK(chunk.setPieces(index, {}) == CellPieceEdit::Applied);
    EQUAL(chunk.pieceStorageBytes(), size_t{0}, "cleared last record");
    EQUAL(chunk.shapeCount(), size_t{0}, "cleared legacy shape");
    const auto cleared = chunk.solidCellAt(index);
    CHECK(cleared.base.tile.isAir() && cleared.pieces().empty());
    CHECK(mixed.pieces().size() == 2 && raised.pieces().size() == 1);

    CHECK(chunk.setPieces(index, inserted) == CellPieceEdit::Applied);
    CHECK(chunk.setPieces(index, mixed.pieces().first(1)) ==
          CellPieceEdit::Applied);
    EQUAL(chunk.pieceStorageBytes(), size_t{0},
          "original slab needs no side data");
    CHECK(chunk.shapeAt(index) == slab);
}

TEST(chunk_piece_border_snapshot) {
    constexpr auto index = Chunk::cell(0, 8, 8);
    Chunk live;
    live.setCell(index, Tile{.block_id = 1, .lightLevel = {}}, slab);
    auto baseline = std::make_shared<structure_voxels::Slab>();
    baseline->block = 1;
    baseline->bits.set(index);
    baseline->setShape(index, slab);
    const ChunkMeshSnapshot before{
        std::make_shared<const Chunk>(live), baseline, {}};
    const auto generated = before.solidCellAt(index);
    CHECK(generated.pieces()[0].origin.kind == PieceOrigin::Structure);
    const std::array pieces{generated.pieces()[0], upper};
    CHECK(live.setPieces(index, pieces) == CellPieceEdit::Applied);
    const ChunkMeshSnapshot captured{
        std::make_shared<const Chunk>(live), baseline, {}};
    const ChunkMesher::Neighbour neighbor(captured, 0, 8, 8);
    CHECK(live.setPieces(index, std::span(&lower, 1)) ==
          CellPieceEdit::Applied);
    CHECK(live.setPieces(Chunk::cell(15, 8, 8), pieces) ==
          CellPieceEdit::Applied);

    LocationGenerator generator;
    generator.generateChunk(live, {0, 8, 0}, {}, test::regionAt(31), {}, {});
    CHECK(!live.hasShapes() && live.pieceStorageBytes() == 0);
    const auto border = neighbor.solidCell();
    CHECK(border && border->base.shape == slab);
    CHECK(border->pieces().size() == 2);
    CHECK(border->pieces()[0].origin.kind == PieceOrigin::Structure);
    CHECK(border->pieces()[1].origin == upper.origin);
    EQUAL(border->pieces()[1].geometry.blockId, 2, "captured border material");
}

TEST(chunk_piece_mesh_revision) {
    test::WorldFixture f;
    f.world.width = 1;
    f.world.height = 1;
    const ChunkCoord center{0, 4, 4};
    const ChunkCoord west{7, 4, 4};
    const ChunkCoord around[] = {center,    west,      {1, 4, 4}, {0, 5, 4},
                                 {0, 3, 4}, {0, 4, 5}, {0, 4, 3}};
    for (const auto& coord : around) test::fill(f.load(coord), 1);
    for (int i = 0; i < 3; ++i) {
        f.chunks.update();
        f.wait();
    }

    const TilePos border{-1, 72, 72};
    const auto cube = f.chunks.solidCellAt(border);
    CHECK(cube && cube->cell.pieces().size() == 1);
    const auto canonical = f.chunks.solidCellAt({127, 72, 72});
    CHECK(canonical && canonical->revision == cube->revision);
    auto unknown = upper;
    unknown.geometry.blockId = 255;
    CHECK(f.chunks.setCellPieces(border, cube->revision,
                                 std::span(&unknown, 1)) ==
          CellPieceEdit::Invalid);
    // Its legacy projection is a cube, but the elevated slab opens the side.
    CHECK(
        f.chunks.setCellPieces(border, cube->revision, std::span(&upper, 1)) ==
        CellPieceEdit::Applied);
    const auto raised = f.chunks.solidCellAt(border);
    CHECK(raised && raised->cell.compound);

    // Hold the sole worker while the first job captures its inputs.
    std::promise<void> release;
    f.pool.detach_task([gate = release.get_future()] { gate.wait(); },
                       BS::pr::highest);
    f.chunks.update();
    CHECK(f.chunks.meshInFlight(center));
    const std::array pieces{lower, upper};
    CHECK(f.chunks.setCellPieces(border, raised->revision, pieces) ==
          CellPieceEdit::Applied);
    CHECK(f.chunks.setCellPieces(border, raised->revision, {}) ==
          CellPieceEdit::Stale);
    release.set_value();
    f.wait();
    f.chunks.update();
    // Collection rejects the captured revision and submits the replacement.
    CHECK(f.chunks.meshInFlight(center));
    f.wait();
    f.chunks.update();
    CHECK(!f.chunks.meshInFlight(center));
    const auto mixed = f.chunks.solidCellAt(border);
    CHECK(mixed && mixed->cell.pieces().size() == 2);

    f.chunks.unregister(west);
    CHECK(!f.chunks.solidCellAt(border));
    f.load(west);
    CHECK(f.chunks.setCellPieces(border, mixed->revision, {}) ==
          CellPieceEdit::Stale);
    CHECK(raised->cell.pieces().size() == 1 &&
          mixed->cell.pieces().size() == 2);
}
