#include <climits>

#include "flora_assets.hpp"
#include "serialization/binary_stream.hpp"
#include "world/ground_cover_climate.hpp"
#include "world/light_volume.hpp"
#include "world/save/chunk_codec.hpp"
#include "world/terrain_lod.hpp"
#include "world/terrain_sampler.hpp"
#include "world/world_editor.hpp"
#include "world_fixture.hpp"

// Ground cover (docs/handoff/flora.md, step 2): per-tile overlay data that
// never changes blocks, solidity or edit detection.
namespace {
World plainsWorld(int width = 5, int height = 5) {
    World world{};
    world.width = width;
    world.height = height;
    world.genSettings.width = width;
    world.genSettings.height = height;
    world.genSettings.height_seed = 1234;
    world.genSettings.feature_seed = 5678;
    world.regions.resize(width * height);
    for (int z = 0; z < height; ++z)
        for (int x = 0; x < width; ++x) {
            auto& r = world.regions[z * width + x];
            r = test::regionAt(SEA_LEVEL_Y + 30);
            r.coord = {unsigned(x), unsigned(z)};
            r.land = LandType::PLAINS;
            r.vegetation = VegetationType::GRASS;
            r.humidity = .6f;
            r.temperature_c = 15;
        }
    return world;
}
const std::vector<RegionLayer> Rocks = {
    {RegionLayerType::Rock, 1, 1000, 0, 0.f, 1}};

// Generates every chunk of column (cx, cz) that contains a surface tile.
struct Column {
    ChunkColumnInputs inputs;
    std::vector<std::pair<int, Chunk>> slabs;  // chunk y, tiles
    const Tile& tile(int x, int worldY, int z) const {
        for (const auto& [y, chunk] : slabs)
            if (worldY / Chunk::SIZE_Y == y)
                return chunk.tiles[z][worldY % Chunk::SIZE_Y][x];
        throw std::runtime_error("tile outside generated slabs");
    }
};
Column generate(const World& world, const IAssets& assets, unsigned cx,
                unsigned cz) {
    const Vec2u rc{cx / CHUNKS_PER_REGION, cz / CHUNKS_PER_REGION};
    const auto data = TerrainNeighborhood::capture(world, rc, assets);
    Column out;
    out.inputs = TerrainSampler(data).columns(cx, cz);
    int low = INT_MAX, high = INT_MIN;
    for (const auto& c : out.inputs.columns) {
        low = std::min(low, c.surface_y);
        high = std::max(high, c.surface_y);
    }
    const auto& region = world.regions[rc.y * world.width + rc.x];
    LocationGenerator generator;
    for (int y = low / Chunk::SIZE_Y - 1; y <= high / Chunk::SIZE_Y + 1; ++y) {
        Chunk chunk{};
        generator.generateChunk(chunk, {cx, unsigned(y), cz}, {}, region, Rocks,
                                {}, &out.inputs);
        out.slabs.emplace_back(y, chunk);
    }
    return out;
}
}  // namespace

TEST(cover_codec_versions) {
    Chunk chunk{};
    chunk.tiles[0][3][0].block_id = 2;
    chunk.tiles[0][3][0].setCover(4, 11);
    chunk.tiles[1][3][0].block_id = 2;
    chunk.tiles[1][3][0].setCover(4, 15);
    chunk.tiles[1][3][0].coverState |= 0x30;  // reserved high nibble kept
    Chunk decoded{};
    chunk_codec::decode(chunk_codec::encode(chunk), decoded);
    EQUAL(int(decoded.tiles[0][3][0].cover), 4, "cover id");
    EQUAL(int(decoded.tiles[0][3][0].coverDensity()), 11, "density");
    EQUAL(int(decoded.tiles[1][3][0].coverState), 0x3F, "whole state byte");
    CHECK(decoded.tiles[1][3][0].isSolid());

    // A version 1 blob (three-byte palette) decodes with no cover.
    BinaryWriter v1;
    v1.write(uint8_t(1));
    v1.write(uint16_t(2));
    for (const uint8_t block : {0, 7}) {
        v1.write(block);
        v1.write(uint8_t(0));
        v1.write(uint8_t(0));
    }
    Bytes indices(Chunk::SIZE_X * Chunk::SIZE_Y * Chunk::SIZE_Z);
    indices[5] = std::byte{1};
    v1.write(uint8_t(0));  // stored without compression
    v1.writeBlock(indices);
    Chunk old{};
    old.tiles[0][0][0].setCover(9, 9);  // stale contents are replaced
    chunk_codec::decode(v1.take(), old);
    EQUAL(int(old.tiles[0][0][5].block_id), 7, "v1 block");
    for (const auto& plane : old.tiles)
        for (const auto& row : plane)
            for (const auto& tile : row)
                CHECK(tile.cover == 0 && tile.coverState == 0);

    // A state without a cover id is corrupt.
    Chunk uniform{};
    auto bad = chunk_codec::encode(uniform);
    EQUAL(bad.size(), size_t(3 + 5), "uniform v2 chunk");
    bad[7] = std::byte{3};  // coverState of the only palette entry
    bool threw = false;
    try {
        chunk_codec::decode(bad, uniform);
    } catch (const SerializationError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(cover_generation_surface_only) {
    test::FloraAssets assets;
    test::loadMeadow(assets);
    const auto world = plainsWorld();
    const auto& blocks = assets.getStorage<Block>();
    const uint8_t topsoil = blocks.get(std::string("blocks.loam_topsoil")).id;
    int covered = 0, low = 15, high = 0;
    for (const auto [cx, cz] : {std::pair{16u, 16u}, std::pair{23u, 17u}}) {
        const auto column = generate(world, assets, cx, cz);
        const auto again = generate(world, assets, cx, cz);
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int x = 0; x < Chunk::SIZE_X; ++x) {
                const auto& input =
                    column.inputs.columns[z * Chunk::SIZE_X + x];
                for (const auto& [cy, chunk] : column.slabs)
                    for (int y = 0; y < Chunk::SIZE_Y; ++y) {
                        const int worldY = cy * Chunk::SIZE_Y + y;
                        const auto& tile = chunk.tiles[z][y][x];
                        EQUAL(int(tile.cover),
                              int(again.tile(x, worldY, z).cover),
                              "deterministic cover");
                        EQUAL(int(tile.coverState),
                              int(again.tile(x, worldY, z).coverState),
                              "deterministic density");
                        if (worldY != input.surface_y) {
                            EQUAL(int(tile.cover), 0, "cover below/above");
                            continue;
                        }
                        EQUAL(int(tile.cover), int(input.soil.cover),
                              "surface cover follows the column input");
                        if (!tile.cover) continue;
                        EQUAL(int(tile.block_id), int(topsoil),
                              "cover grows on topsoil");
                        ++covered;
                        low = std::min<int>(low, tile.coverDensity());
                        high = std::max<int>(high, tile.coverDensity());
                    }
            }
    }
    CHECK(covered > 200);
    CHECK(low < high);

    // Ground cover data never changes blocks (so never solidity, sunlight
    // or ChunkManager::isEdited).
    test::FloraAssets bare;
    bare.load<Material>(Json::array({{{"name", "materials.loam"},
                                      {"texture_albedo", "a"},
                                      {"texture_normal", "b"}}}));
    bare.load<Block>(Json::array({{{"name", "blocks.loam"},
                                   {"material", "materials.loam"},
                                   {"render_category", "Terrain Soil"}},
                                  {{"name", "blocks.loam_topsoil"},
                                   {"material", "materials.loam"},
                                   {"render_category", "Terrain Soil"}}}));
    bare.load<Soil>(
        Json::array({Json{{"name", "soils.loam"},
                          {"block_name", "blocks.loam"},
                          {"topsoil_block_name", "blocks.loam_topsoil"},
                          {"depth_m", {3, 3}},
                          {"properties",
                           {{"porosity", .4},
                            {"permeability", .5},
                            {"water_retention", .6},
                            {"fertility", .7},
                            {"organic_content", .02}}},
                          {"placement", Json::array({Json::object()})}}}));
    const auto with = generate(world, assets, 16, 16);
    const auto without = generate(world, bare, 16, 16);
    CHECK(with.slabs.size() == without.slabs.size());
    for (size_t i = 0; i < with.slabs.size(); ++i)
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int y = 0; y < Chunk::SIZE_Y; ++y)
                for (int x = 0; x < Chunk::SIZE_X; ++x) {
                    const auto& a = with.slabs[i].second.tiles[z][y][x];
                    const auto& b = without.slabs[i].second.tiles[z][y][x];
                    EQUAL(int(a.block_id), int(b.block_id), "same blocks");
                    CHECK(b.cover == 0);
                }

    // Underwater surfaces never get cover.
    auto ocean = plainsWorld();
    for (auto& r : ocean.regions) r.land_height_m = SEA_LEVEL_Y - 20;
    for (const auto& c : generate(ocean, assets, 16, 16).inputs.columns)
        CHECK(c.surface_y >= SEA_LEVEL_Y || c.soil.cover == 0);
}

TEST(cover_wraps_seam) {
    test::FloraAssets assets;
    test::loadMeadow(assets);
    const auto world = plainsWorld(16, 2);
    const int W = world.wrap().tiles;
    EQUAL(W, 2048, "world tiles");
    const auto east = TerrainNeighborhood::capture(world, {15, 0}, assets);
    const auto west = TerrainNeighborhood::capture(world, {0, 0}, assets);
    for (unsigned cz : {0u, 3u, 9u}) {
        // Chunk column W/16 is column 0 seen from the east side.
        const auto a = TerrainSampler(east).columns(W / Chunk::SIZE_X, cz);
        const auto b = TerrainSampler(west).columns(0, cz);
        for (size_t i = 0; i < a.columns.size(); ++i) {
            EQUAL(a.columns[i].surface_y, b.columns[i].surface_y, "surface");
            EQUAL(int(a.columns[i].soil.cover), int(b.columns[i].soil.cover),
                  "cover id across the seam");
            EQUAL(int(a.columns[i].soil.coverDensity),
                  int(b.columns[i].soil.coverDensity),
                  "cover density across the seam");
        }
    }
}

TEST(cover_mesh_faces) {
    test::FloraAssets assets;
    test::loadMeadow(assets);
    ChunkMesher mesher(assets);
    Chunk chunk{};
    // Four solid layers; the top layer is covered on x < 8 only.
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < Chunk::SIZE_X; ++x) {
                auto& tile = chunk.tiles[z][y][x];
                tile.block_id = y == 3 ? 2 : 1;
                if (y == 3 && x < 8) tile.setCover(1, 12);
            }
    static const Tile air{};
    CoverClimate climate;
    climate.humidity = {1, 1, 1, 1};
    climate.temperature_c = {30, 30, 30, 30};
    const auto mesh = mesher.build(
        chunk, {0, 0, 0}, [](int, int, int) { return &air; }, climate);
    const auto& cover = assets.getStorage<GroundCover>().get(1);
    const float fringe = ChunkMesher::packFringe(cover.faces.side_fringe_m);
    EQUAL(fringe, 20.f + 1000.f * 40.f, "packed fringe range");
    int coveredTop = 0, bareTop = 0, coveredSide = 0, bottom = 0;
    for (size_t q = 0; q < mesh.vertices.size(); q += 4) {
        const auto& v = mesh.vertices[q];
        float minY = v.y, minX = v.x;
        for (size_t k = 1; k < 4; ++k) {
            minY = std::min(minY, mesh.vertices[q + k].y);
            minX = std::min(minX, mesh.vertices[q + k].x);
        }
        for (size_t k = 1; k < 4; ++k) {
            // A quad has one cover state on all four corners.
            EQUAL(mesh.vertices[q + k].u, v.u, "quad cover layer");
            EQUAL(mesh.vertices[q + k].v, v.v, "quad fringe");
        }
        const bool covered = v.u > -0.5f;
        if (covered) {
            EQUAL(v.u, float(test::FloraAssets::CoverLayer), "overlay layer");
            // Humid and warm: the humid_warm corner of the default gradient.
            EQUAL(v.tint, cover.tint_gradient.humid_warm.packed(), "tint");
        } else {
            EQUAL(v.tint, 0xFFFFFFFFu, "neutral tint");
        }
        if (v.ny > 0.5f) {
            covered ? ++coveredTop : ++bareTop;
            if (covered) {
                EQUAL(v.v, 0.f, "no fringe on top faces");
                CHECK(minX < 8);
            }
        } else if (v.ny < -0.5f) {
            ++bottom;
            CHECK(!covered);
        } else if (covered) {
            ++coveredSide;
            EQUAL(v.v, fringe, "side fringe");
            CHECK(minY > 2.5f);  // only the covered top layer's sides
        }
    }
    // Different covers never merge: the top splits into two quads.
    EQUAL(coveredTop, 1, "one covered top quad");
    EQUAL(bareTop, 1, "one bare top quad");
    CHECK(bottom >= 1);
    // Covered side faces: -X wall, and the x < 8 halves of the Z walls.
    EQUAL(coveredSide, 3, "covered side strips");
}

TEST(cover_volume_dilation) {
    Chunk chunk{};
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int x = 0; x < Chunk::SIZE_X; ++x)
            for (int y = 0; y <= 3; ++y) chunk.tiles[z][y][x].block_id = 1;
    chunk.tiles[5][3][5].setCover(1, 15);
    chunk.tiles[5][3][6].setCover(1, 0);
    chunk.tiles[5][3][7].block_id = 1;
    LightVolume::CoverCells cells{};
    LightVolume::encodeCover(chunk, cells);
    EQUAL(int(cells[LightVolume::coverCellIndex(5, 3, 5)]), 16, "raw: 1+15");
    EQUAL(int(cells[LightVolume::coverCellIndex(6, 3, 5)]), 1, "raw: 1+0");
    EQUAL(int(cells[LightVolume::coverCellIndex(7, 3, 5)]), 0, "raw: none");
    std::array<const LightVolume::CoverCells*, 27> neighborhood{};
    neighborhood[LightVolume::neighbor(0, 0, 0)] = &cells;
    LightVolume::CoverVolume volume{};
    LightVolume::buildCover(neighborhood, volume);
    const auto at = [&](int x, int y, int z) {
        return int(volume[LightVolume::coverTexelIndex(x + 1, y + 1, z + 1)]);
    };
    EQUAL(at(5, 3, 5), 255, "covered tile");
    EQUAL(at(5, 4, 5), 255, "air above the cover");
    EQUAL(at(5, 2, 5), 255, "soil under the cover");
    EQUAL(at(5, 5, 5), 0, "two tiles above");
    EQUAL(at(6, 3, 5), 0, "zero density stays zero");
    EQUAL(at(7, 3, 5), 0, "uncovered tile");
    // Missing neighbours repeat the edge.
    chunk.tiles[5][3][0].setCover(1, 15);
    LightVolume::encodeCover(chunk, cells);
    LightVolume::buildCover(neighborhood, volume);
    EQUAL(at(-1, 3, 5), 255, "border repeats the edge cell");
}

namespace {
struct CoverEditFixture : test::SunlightFixture {
    WorldEditor editor{chunks, &sunlight, assets.getStorage<Block>()};
    static TilePos at(int x, int y, int z) { return {x, y + BASE_Y, z}; }
    void grass(int x, int y, int z) {
        tile(x, y, z).block_id = 1;
        tile(x, y, z).setCover(3, 15);
    }
};
}  // namespace

TEST(cover_cleared_by_edits) {
    CoverEditFixture f;
    f.load({0, 0, 0});
    f.load({0, 1, 0});
    const auto lower = test::SunlightFixture::coord({0, 0, 0});
    const auto upper = test::SunlightFixture::coord({0, 1, 0});
    std::vector<BlockChange> changes;
    f.editor.blockChanged.connect(
        [&](const BlockChange& c) { changes.push_back(c); });

    f.grass(5, 5, 5);
    f.grass(6, 5, 5);
    f.grass(7, 5, 5);
    // A cover never makes a tile solid or liquid.
    CHECK(f.tile(5, 5, 5).isSolid() && !f.tile(5, 5, 5).isLiquid());

    // Placing a block on grass smothers that grass.
    f.editor.placeBlock(CoverEditFixture::at(5, 6, 5), 2);
    EQUAL(int(f.tile(5, 5, 5).cover), 0, "smothered cover");
    EQUAL(int(f.tile(5, 5, 5).block_id), 1, "the grass tile itself stays");
    EQUAL(int(f.tile(6, 5, 5).cover), 3, "neighbour keeps its cover");
    EQUAL(changes.size(), size_t(1), "cover changes are not block changes");
    // Digging the grass tile removes its cover; air has none.
    f.editor.removeBlock(CoverEditFixture::at(6, 5, 5));
    EQUAL(int(f.tile(6, 5, 5).cover), 0, "dug cover");
    EQUAL(int(f.tile(6, 5, 5).coverState), 0, "dug state");
    // Replacing the block clears the old block's cover.
    f.editor.setBlock(CoverEditFixture::at(7, 5, 5), 2);
    EQUAL(int(f.tile(7, 5, 5).cover), 0, "replaced block");

    // Across a chunk border: grass on top of the lower chunk.
    f.chunks.snapshotRegion({0, 0});  // clear persistDirty flags
    CHECK(!f.chunks.isPersistDirty(lower));
    f.grass(3, 15, 3);
    f.editor.placeBlock(CoverEditFixture::at(3, 16, 3), 2);
    EQUAL(int(f.tile(3, 15, 3).cover), 0, "cover under a chunk border");
    CHECK(f.chunks.isPersistDirty(lower));
    CHECK(f.chunks.isPersistDirty(upper));
}

TEST(cover_lod_matches_detail) {
    test::FloraAssets assets;
    test::loadMeadow(assets);
    assets.load<Rock>(Json::array(
        {{{"name", "rocks.test"},
          {"block_name", "blocks.loam"},
          {"category", "Igneous Intrusive"},
          {"body_size_m", {128, 128}},
          {"properties", {{"porosity", .1}, {"permeability", .1}}}}}));
    const auto world = plainsWorld();
    SurfaceSampler sampler(world, assets);
    const auto& cover = assets.getStorage<GroundCover>().get(1);
    std::atomic_bool cancelled{false};
    // A stepped patch has one cover texel per tile, equal to the detailed
    // chunk's cover, density and climate tint.
    const TerrainLod::Patch stepped{256, 256, 1};
    const auto mesh = TerrainLod::build(stepped, sampler, cancelled);
    EQUAL(mesh.coverSize, stepped.size(), "texel per tile");
    EQUAL(mesh.covers.size(), size_t(32 * 32 * 4), "cover bytes");
    int covered = 0;
    for (unsigned cz = 16; cz < 18; ++cz)
        for (unsigned cx = 16; cx < 18; ++cx) {
            const auto columns = TerrainSampler(TerrainNeighborhood::capture(
                                                    world, {2, 2}, assets))
                                     .columns(cx, cz);
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x) {
                    const auto& soil = columns.columns[z * 16 + x].soil;
                    const int tx = int(cx - 16) * 16 + x,
                              tz = int(cz - 16) * 16 + z;
                    const size_t i = (size_t(tz) * 32 + tx) * 4;
                    EQUAL(int(mesh.covers[i]),
                          soil.cover ? test::FloraAssets::CoverLayer + 1 : 0,
                          "overlay layer + 1");
                    if (!soil.cover) continue;
                    ++covered;
                    EQUAL(int(mesh.covers[i + 1]), soil.coverDensity * 17,
                          "density");
                    const auto [h, t] = ground_cover_climate::at(
                        world, 256 + tx + 0.5, 256 + tz + 0.5);
                    EQUAL(int(mesh.covers[i + 2] | mesh.covers[i + 3] << 8),
                          int(ground_cover_climate::packRgb565(
                              cover.tint_gradient.at(h, t))),
                          "tint");
                }
        }
    CHECK(covered > 500);
    // Stepped walls carry their column top for the fringe; tops do not.
    int walls = 0;
    for (const auto& v : mesh.vertices) {
        if (v.layer < -1.5) continue;
        if (std::abs(v.ny) < 0.01f) {
            ++walls;
            CHECK(v.u >= SEA_LEVEL_Y && v.u >= v.y - 0.001f);
        } else {
            EQUAL(v.u, -1.f, "no column top on top faces");
        }
    }
    CHECK(walls > 0);
    // Smooth patches use the material map's grid.
    const auto smooth =
        TerrainLod::build({256, 256, 4}, sampler, cancelled, 16);
    EQUAL(smooth.coverSize, 16, "smooth cover grid");
    EQUAL(smooth.covers.size(), size_t(16 * 16 * 4), "smooth bytes");

    // An edit that moves the ground removes the cover there.
    auto edit = std::make_shared<ColumnEdit>();
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
            const int i = ColumnEdit::index(x, z);
            edit->groundY[i] = int16_t(sampler.height(256 + x, 256 + z) -
                                       (x == 3 && z == 4 ? 2 : 0));
            edit->groundBlock[i] = 1;
            edit->waterY[i] = -1;
        }
    const auto before = sampler.cover(256 + 5, 256 + 4);
    auto edits = std::make_shared<EditMap>();
    (*edits)[editKey(16, 16)] = edit;
    sampler.setEdits(edits);
    EQUAL(int(sampler.cover(256 + 3, 256 + 4).layer), 0, "dug column");
    EQUAL(int(sampler.cover(256 + 5, 256 + 4).layer), int(before.layer),
          "unchanged column keeps its cover");
}

TEST(cover_tint_packing) {
    using ground_cover_climate::packRgb565;
    EQUAL(int(packRgb565({1, 1, 1})), 0xFFFF, "white");
    EQUAL(int(packRgb565({1, 0, 0})), 0xF800, "red in the high bits");
    EQUAL(int(packRgb565({0, 1, 0})), 0x07E0, "green");
    EQUAL(int(packRgb565({0, 0, 1})), 0x001F, "blue");
    // Climate is bilinear between region centres and wraps on X.
    auto world = plainsWorld(4, 2);
    world.regions[0].humidity = 0;
    world.regions[1].humidity = 1;
    const auto [centre, t0] = ground_cover_climate::at(world, 64, 64);
    const auto [between, t1] = ground_cover_climate::at(world, 128, 64);
    EQUAL(centre, 0.f, "region centre");
    EQUAL(between, 0.5f, "halfway to the next centre");
    const auto [wrapped, t2] = ground_cover_climate::at(world, 4 * 128, 64);
    const auto [seam, t3] = ground_cover_climate::at(world, 0, 64);
    EQUAL(wrapped, seam, "X wraps");
    (void)t0, (void)t1, (void)t2, (void)t3;
}
