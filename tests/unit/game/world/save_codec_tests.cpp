#include <filesystem>

#include "serialization/file_io.hpp"
#include "test.hpp"
#include "world/save/chunk_codec.hpp"
#include "world/save/region_file.hpp"
#include "world/save/save_catalog.hpp"
#include "world/save/terrain_edits_codec.hpp"
#include "world/save/trees_codec.hpp"
#include "world/save/world_map_codec.hpp"

namespace {
template <typename F>
bool throwsSerialization(F&& fn) {
    try {
        fn();
    } catch (const SerializationError&) {
        return true;
    }
    return false;
}
bool sameTiles(const Chunk& a, const Chunk& b) {
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int y = 0; y < Chunk::SIZE_Y; ++y)
            for (int x = 0; x < Chunk::SIZE_X; ++x) {
                const auto &l = a.tiles[z][y][x], &r = b.tiles[z][y][x];
                if (l.block_id != r.block_id || l.liquid != r.liquid ||
                    l.liquidLevel != r.liquidLevel || l.cover != r.cover ||
                    l.coverState != r.coverState)
                    return false;
            }
    return true;
}
std::filesystem::path scratch(const std::string& name) {
    auto dir = std::filesystem::path(TEST_SCRATCH_DIR) / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}
World smallWorld() {
    World world{};
    world.genSettings = WorldGenSettings::fromSeed(987654321ull);
    world.genSettings.width = world.width = 3;
    world.genSettings.height = world.height = 2;
    world.min_temp_c = world.genSettings.min_temp_c;
    world.max_temp_c = world.genSettings.max_temp_c;
    world.regions.resize(6);
    for (int i = 0; i < 6; ++i) {
        auto& r = world.regions[i];
        r.coord = {unsigned(i % 3), unsigned(i / 3)};
        r.height = 0.1f * i;
        r.land_height_m = 400 + i;
        r.land_height_sea_m = i - 3;
        r.temperature = 0.5f;
        r.temperature_c = 12.5f + i;
        r.humidity = 0.25f;
        r.water_distance = i * 2;
        r.energy = 0.75f;
        r.biome = BiomeType(i % 3);
        r.land = LandType(i % 2);
        r.vegetation = VegetationType(1);
        r.features = FeatureMask(1 << i);
        r.river = {RiverFlowDirection::SE, 0.4f};
        r.crust = CrustType::CONTINENTAL;
    }
    world.regions[4].entities.push_back({12, 3.5f, -1.f});
    RiverPolyline river;
    river.path = {{{1, 2}, 0.5f}, {{3, 4}, 0.75f}};
    RiverVertex v{};
    v.x = 1;
    v.flow = 2;
    river.mesh = {v};
    river.bounds = {1, 2, 3, 4};
    world.rivers.push_back(river);
    return world;
}
}  // namespace

TEST(chunk_codec_uniform) {
    Chunk chunk{};
    for (auto& plane : chunk.tiles)
        for (auto& row : plane)
            for (auto& tile : row) tile.block_id = 5;
    const auto encoded = chunk_codec::encode(chunk);
    CHECK(encoded.size() <= 8);
    Chunk decoded{};
    chunk_codec::decode(encoded, decoded);
    CHECK(sameTiles(chunk, decoded));
    EQUAL(chunk_codec::highestSolidY(decoded), 15, "solid top");
    EQUAL(chunk_codec::highestSolidY(Chunk{}), -1, "empty chunk");
}

TEST(chunk_codec_shapes) {
    Chunk chunk;
    const Tile soil{.block_id = 5, .lightLevel = {}};
    for (auto& plane : chunk.tiles)
        for (auto& row : plane)
            for (auto& tile : row) tile = soil;
    const auto ramp = TileShape::make(TileShapeProfile::Ramp, TileFacing::NegZ);
    const auto half =
        TileShape::make(TileShapeProfile::OuterHalfHigh, TileFacing::NegX);
    const auto third =
        TileShape::make(TileShapeProfile::InnerThirdLow, TileFacing::PosX);
    const auto diagonal =
        TileShape::make(TileShapeProfile::DiagonalRamp, TileFacing::NegX);
    const auto rootCell = Chunk::cell(0, 0, 0);
    chunk.setCell(rootCell, soil, ramp);
    Chunk decoded;
    const auto uniform = chunk_codec::encode(chunk);
    chunk_codec::decode(uniform, decoded);
    CHECK(decoded.shapeCount() == 1 && decoded.shapeAt(rootCell) == ramp);
    CHECK(sameTiles(chunk, decoded));

    chunk.recordRoot(rootCell, 41, soil);
    chunk.setCell(rootCell, Tile{.block_id = 7, .lightLevel = {}});
    chunk.setCell(1, soil, half);
    chunk.setCell(4095, soil, third);
    chunk.setCell(2, soil, diagonal);
    chunk_codec::decode(chunk_codec::encode(chunk), decoded);
    CHECK(decoded.rootAt(rootCell) && decoded.rootAt(rootCell)->shape == ramp);
    CHECK(decoded.shapeAt(1) == half && decoded.shapeAt(4095) == third);
    CHECK(decoded.shapeAt(2) == diagonal);
    GrownBlockSet grown;
    grown.set(7);
    decoded.restoreRootSubstrate(grown);
    CHECK(!decoded.rootAt(rootCell));
    CHECK(decoded.cellAt(0, 0, 0).tile.block_id == 5);
    CHECK(decoded.shapeAt(rootCell) == ramp);

    // A v5 uniform palette with a v5 root tail (no substrate shape byte).
    BinaryWriter old;
    old.write(uint8_t(5));
    old.write(uint16_t(1));
    for (const uint8_t byte : {7, 0, 0, 0, 0}) old.write(byte);
    old.write(uint8_t(0));
    old.write(uint16_t(1));
    old.write(rootCell);
    old.write(uint64_t(41));
    for (const uint8_t byte : {5, 0, 0}) old.write(byte);
    old.write(uint16_t(0));
    chunk_codec::decode(old.bytes(), decoded);
    CHECK(!decoded.hasShapes());
    CHECK(decoded.rootAt(rootCell) &&
          decoded.rootAt(rootCell)->shape == TileShape{});
    decoded.restoreRootSubstrate(grown);
    CHECK(decoded.cellAt(0, 0, 0).tile.block_id == 5 && !decoded.hasShapes());

    const auto malformed =
        [&](uint8_t material, uint16_t count,
            std::initializer_list<std::pair<uint16_t, uint8_t>> values) {
            BinaryWriter entries, payload;
            entries.write(count);
            for (const auto& [index, code] : values) {
                entries.write(index);
                entries.write(code);
            }
            payload.write(chunk_codec::Version);
            payload.write(uint16_t(0x4001));
            payload.write(material);
            payload.write(uint32_t(0));
            payload.writeBlock(entries.bytes());
            CHECK(throwsSerialization(
                [&] { chunk_codec::decode(payload.bytes(), decoded); }));
            CHECK(decoded.cellAt(0, 0, 0).tile.block_id == 5 &&
                  !decoded.hasShapes());
        };
    malformed(5, 0, {});
    malformed(5, 2, {{0, ramp.code()}});
    malformed(5, 2, {{1, ramp.code()}, {0, half.code()}});
    malformed(5, 2, {{1, ramp.code()}, {1, half.code()}});
    malformed(5, 1, {{4096, ramp.code()}});
    malformed(0, 1, {{0, ramp.code()}});
    malformed(5, 1, {{0, 0}});
    malformed(5, 1, {{0, 5}});    // slab with noncanonical facing
    malformed(5, 1, {{0, 100}});  // reserved profile
    CHECK(throwsSerialization(
        [&] { chunk_codec::decode(ByteView(uniform).first(8), decoded); }));
}

TEST(chunk_codec_mixed_strips_light) {
    Chunk chunk{};
    for (int z = 0; z < 16; ++z)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                auto& tile = chunk.tiles[z][y][x];
                tile.block_id = y < 8 ? 3 : 0;
                if (y >= 8 && y < 10) {
                    tile.liquid = LiquidType::Water;
                    tile.liquidLevel = uint8_t(x * 16);
                }
                tile.lightLevel.sun_exposure = 15;
            }
    const auto encoded = chunk_codec::encode(chunk);
    CHECK(encoded.size() < 1024);
    Chunk decoded{};
    chunk_codec::decode(encoded, decoded);
    CHECK(sameTiles(chunk, decoded));
    EQUAL(int(decoded.tiles[0][15][0].lightLevel.sun_exposure), 0,
          "light is derived and not stored");
    EQUAL(chunk_codec::highestSolidY(decoded), 7, "mixed top");
}

TEST(chunk_codec_wide_palette) {
    Chunk chunk{};
    int i = 0;
    for (auto& plane : chunk.tiles)
        for (auto& row : plane)
            for (auto& tile : row) {
                tile.block_id = uint8_t(i % 256);
                tile.liquidLevel = uint8_t((i / 256) % 3);
                ++i;
            }
    Chunk decoded{};
    chunk_codec::decode(chunk_codec::encode(chunk), decoded);
    CHECK(sameTiles(chunk, decoded));
}

TEST(chunk_codec_rejects_corrupt) {
    Chunk chunk{};
    chunk.tiles[1][2][3].block_id = 9;
    auto encoded = chunk_codec::encode(chunk);
    Chunk out{};
    auto badVersion = encoded;
    badVersion[0] = std::byte{77};
    CHECK(throwsSerialization([&] { chunk_codec::decode(badVersion, out); }));
    auto truncated = encoded;
    truncated.resize(truncated.size() - 3);
    CHECK(throwsSerialization([&] { chunk_codec::decode(truncated, out); }));
    auto badLiquid = encoded;
    badLiquid[4] = std::byte{9};  // first palette entry's liquid byte
    CHECK(throwsSerialization([&] { chunk_codec::decode(badLiquid, out); }));

    BinaryWriter raw;
    raw.write(chunk_codec::Version);
    raw.write(uint16_t{2});
    for (auto entry : {0, 1}) {
        raw.write(uint8_t(entry));
        for (int field = 0; field < 4; ++field) raw.write(uint8_t{0});
    }
    raw.write(uint8_t{0});  // uncompressed indices
    raw.writeBlock(Bytes(Chunk::CellCount + 1));
    CHECK(throwsSerialization([&] { chunk_codec::decode(raw.bytes(), out); }));
}

TEST(region_file_roundtrip) {
    Chunk ground{};
    ground.tiles[0][4][0].block_id = 2;
    RegionSnapshot snapshot;
    const ChunkCoord coord{8 + 3, 26, 16 + 5};  // region (1, 2)
    const auto index = RegionData::localIndex(coord);
    CHECK(RegionData::worldCoord({1, 2}, index) == coord);
    snapshot.chunks.emplace_back(index, std::make_shared<const Chunk>(ground));
    auto data = region_file::merge(RegionData{}, snapshot);
    EQUAL(data.tops[RegionData::column(index)], 26 * 16 + 4, "column top");
    data.extra.put("FUTR", 4, Bytes(10, std::byte{7}));

    const auto decoded = region_file::decode(region_file::encode(data));
    EQUAL(decoded.chunks.size(), size_t(1), "chunk count");
    Chunk out{};
    chunk_codec::decode(*decoded.chunks.at(index), out);
    CHECK(sameTiles(ground, out));
    CHECK(decoded.tops == data.tops);
    CHECK(decoded.extra.find("FUTR") != nullptr);
    CHECK(decoded.extra.find("CHNK") == nullptr);

    // Merging keeps untouched chunks' original blobs.
    RegionSnapshot second;
    second.chunks.emplace_back(uint16_t(0), std::make_shared<const Chunk>());
    const auto merged = region_file::merge(decoded, second);
    EQUAL(merged.chunks.size(), size_t(2), "merged count");
    CHECK(merged.chunks.at(index) == decoded.chunks.at(index));
}

TEST(world_seed_derivation) {
    const auto a = WorldGenSettings::fromSeed(42);
    const auto b = WorldGenSettings::fromSeed(42);
    const auto c = WorldGenSettings::fromSeed(43);
    EQUAL(a.height_seed, b.height_seed, "repeatable");
    EQUAL(a.feature_seed, b.feature_seed, "repeatable");
    CHECK(a.height_seed != c.height_seed);
    CHECK(a.height_seed != a.humidity_seed);
}

TEST(world_map_roundtrip) {
    const auto world = smallWorld();
    const auto decoded =
        world_map_codec::decode(world_map_codec::encode(world));
    EQUAL(decoded.width, 3, "width");
    EQUAL(decoded.height, 2, "height");
    EQUAL(decoded.genSettings.seed, world.genSettings.seed, "seed");
    EQUAL(decoded.genSettings.feature_seed, world.genSettings.feature_seed,
          "feature seed");
    for (size_t i = 0; i < world.regions.size(); ++i) {
        const auto &a = world.regions[i], &b = decoded.regions[i];
        CHECK(a.coord == b.coord);
        EQUAL(a.height, b.height, "height");
        EQUAL(a.land_height_m, b.land_height_m, "land height");
        EQUAL(a.land_height_sea_m, b.land_height_sea_m, "sea height");
        EQUAL(a.temperature_c, b.temperature_c, "temperature");
        EQUAL(a.water_distance, b.water_distance, "water distance");
        CHECK(a.biome == b.biome && a.land == b.land &&
              a.vegetation == b.vegetation && a.features == b.features &&
              a.crust == b.crust && a.river.flow == b.river.flow);
        EQUAL(a.entities.size(), b.entities.size(), "entities");
    }
    EQUAL(decoded.regions[4].entities[0].size, 12, "entity size");
    EQUAL(decoded.rivers.size(), size_t(1), "rivers");
    EQUAL(decoded.rivers[0].path[1].second, 0.75f, "river width");
    EQUAL(decoded.rivers[0].mesh[0].flow, 2.f, "river mesh");
    CHECK(decoded.rivers[0].bounds == world.rivers[0].bounds);

    auto bytes = world_map_codec::encode(world);
    bytes.resize(bytes.size() / 2);
    CHECK(throwsSerialization([&] { world_map_codec::decode(bytes); }));
}

TEST(save_generator_version_gate) {
    const auto root = scratch("generator_version");
    auto world = smallWorld();
    world.generatorVersion = WorldMeta::NaturalShapeVersion - 1;
    auto legacy = save_catalog::create(world, {0, 0}, root);
    legacy.meta.generatorVersion = world.generatorVersion;
    save_catalog::writeMeta(legacy);
    const auto old = save_catalog::readWorldMap(legacy);
    EQUAL(old->generatorVersion, WorldMeta::NaturalShapeVersion - 1,
          "legacy generator stays selected");
    world.generatorVersion = WorldMeta::GeneratorVersion;
    auto current = save_catalog::create(world, {1, 1}, root);
    current.meta.generatorVersion = world.generatorVersion;
    save_catalog::writeMeta(current);
    const auto loaded = save_catalog::readWorldMap(current);
    EQUAL(loaded->generatorVersion, WorldMeta::GeneratorVersion,
          "current generator selects natural shapes");
    std::filesystem::remove_all(root);
}

TEST(save_catalog_slots) {
    const auto root = scratch("catalog");
    const auto world = smallWorld();
    const auto first = save_catalog::create(world, {1, 1}, root);
    const auto second = save_catalog::create(world, {2, 0}, root);
    CHECK(first.directory != second.directory);  // same seed and second
    CHECK(first.directory.filename().string().starts_with("987654321_"));
    CHECK(std::filesystem::is_directory(first.directory / "regions"));

    auto meta = second.meta;
    meta.lastPlayed = "2999-01-01T00:00:00";
    meta.camera = WorldMeta::CameraState{{10, 20, 30}, 1, -1, 25};
    meta.extra["futureKey"] = "kept";
    save_catalog::writeMeta({second.directory, meta});
    std::filesystem::create_directories(root / "broken");
    FileIO::writeJson((root / "broken" / "world.json").string(),
                      Json{{"formatVersion", 99}});

    const auto slots = save_catalog::list(root);
    EQUAL(slots.size(), size_t(2), "broken folder skipped");
    CHECK(slots[0].directory == second.directory);  // most recently played
    EQUAL(slots[0].meta.seed, uint64_t(987654321ull), "seed");
    CHECK(slots[0].meta.camera.has_value());
    EQUAL(slots[0].meta.camera->position.y, 20.f, "camera");
    EQUAL(slots[0].meta.camera->distance, 25.f, "camera distance");
    EQUAL(slots[0].meta.extra.value("futureKey", ""), std::string("kept"),
          "unknown keys preserved");
    EQUAL(slots[1].meta.startRegion.x, 1u, "start region");

    const auto loaded = save_catalog::readWorldMap(slots[1]);
    EQUAL(loaded->regions.size(), size_t(6), "world map regions");
    std::filesystem::remove_all(root);
}

TEST(region_index_covers_world_height) {
    // u16 local indices must address every slab of the tallest column.
    static_assert(RegionData::ChunkCount <= 65536);
    const Vec2u region{3, 5};
    const ChunkCoord top{region.x * CHUNKS_PER_REGION + 7, Y_CHUNKS - 1,
                         region.y * CHUNKS_PER_REGION + 7};
    const auto index = RegionData::localIndex(top);
    CHECK(index == RegionData::ChunkCount - 1);
    CHECK(RegionData::worldCoord(region, index) == top);
}

TEST(structure_save_migrates_trees) {
    using namespace structure_voxels;
    const SaveSlot slot{scratch("structures"), {}};
    CHECK(save_catalog::readStructures(slot).trees.empty());
    // TREE v2 used a removed flag and omitted the cut plane.
    BinaryWriter old;
    old.write(uint32_t(1));
    old.write(uint64_t(11));
    old.write(uint8_t(0));
    old.write(uint32_t(9));
    for (const int coord : {20, 430, 19, 0}) old.write(int32_t(coord));
    old.write(100.f);
    old.write(uint8_t(1));
    old.write(uint16_t(1));
    old.write(int32_t(2));
    old.write(.5f);
    old.write(10.f);
    SectionContainer legacy;
    legacy.put("TREE", 2, old.take());
    legacy.put("KEEP", 5, Bytes{std::byte{9}});
    FileIO::writeBytesAtomic(slot.directory / "trees.dat", legacy.encode());

    auto file = save_catalog::readStructures(slot);
    CHECK(file.migratedTrees);
    CHECK(file.trees.at(11).removed);
    EQUAL(file.trees.at(11).cuts.front().segment, 2, "v2 cut");
    file.voxels["castle"][20] = {OverrideMode::Patch,
                                 2,
                                 20,
                                 24,
                                 true,
                                 {{{20, 431, 24}, 0}, {{21, 432, 24}, 1}}};
    file.voxels["castle"][21] = {OverrideMode::Removed, 3, 0, 0, false, {}};
    const auto wall =
        TileShape::make(TileShapeProfile::Wall10, TileFacing::PosX);
    file.voxels["settlement"][22] = {OverrideMode::Replace,      1, 15, 2, true,
                                     {{{-1, 500, 2}, {1, wall}}}};
    save_catalog::writeStructures(slot, file.trees, file.voxels, file.extra);
    const auto saved = save_catalog::readStructures(slot);
    CHECK(!saved.migratedTrees);
    CHECK(saved.trees == file.trees);
    CHECK(saved.voxels == file.voxels);
    CHECK(saved.extra.find("KEEP") && saved.extra.find("KEEP")->version == 5);
    CHECK(std::filesystem::exists(slot.directory / "trees.dat"));
    Column column(15, 0);
    for (const auto& kind : applyOverrides({}, saved.voxels))
        kind.addTo(column, WorldWrap{256});
    const auto planted = column.rasterize(500 / Side);
    CHECK(planted && planted->tileAt(cellIndex(15, 500 % Side, 2)) == 1);
    CHECK(planted->shapeAt(cellIndex(15, 500 % Side, 2)) == wall);
    // A later save may deliberately remove the final tree record. The old
    // copy must never resurrect it.
    save_catalog::writeStructures(slot, {}, saved.voxels, saved.extra);
    CHECK(save_catalog::readStructures(slot).trees.empty());
    auto invalid = saved.voxels;
    invalid.at("castle").at(20).voxels[{0, WORLD_HEIGHT_Y, 0}] = 1;
    CHECK(throwsSerialization([&] { structures_codec::encode({}, invalid); }));
    SectionContainer unsupported;
    unsupported.put("VOXL", 99, {});
    FileIO::writeBytesAtomic(slot.directory / "structures.dat",
                             unsupported.encode());
    CHECK(throwsSerialization([&] { save_catalog::readStructures(slot); }));
    std::filesystem::remove_all(slot.directory);
}

TEST(terrain_edits_roundtrip) {
    auto edit = std::make_shared<ColumnEdit>();
    for (int i = 0; i < ColumnEdit::Tiles; ++i) {
        edit->groundY[i] = int16_t(400 - i % 7);
        edit->groundBlock[i] = uint8_t(1 + i % 3);
        edit->waterY[i] = i % 5 ? -1 : int16_t(385);
    }
    Chunk lower, upper;
    lower.tiles[3][15][4].block_id = 2;
    upper.tiles[3][0][4].block_id = 2;  // one pillar crossing the slab seam
    upper.tiles[3][9][5].block_id = 1;
    edit->structure = {
        {26, std::make_shared<const Bytes>(chunk_codec::encode(upper))},
        {25, std::make_shared<const Bytes>(chunk_codec::encode(lower))}};
    edit->removed = {{26, {1, 4095}}, {27, {2}}};
    edit->finish();
    EQUAL(edit->structureTopY(), 26 * 16 + 9, "structure top");
    CHECK(edit->structure.front().first == 25);  // sorted
    EditMap columns{{editKey(40, 7), edit}, {editKey(41, 7), edit}};
    SectionContainer extra;
    extra.put("FUTR", 3, Bytes{std::byte{7}});

    const auto file = terrain_edits_codec::decode(
        terrain_edits_codec::encode(columns, extra));
    EQUAL(file.columns.size(), size_t(2), "columns");
    const auto& back = *file.columns.at(editKey(41, 7));
    CHECK(back.groundY == edit->groundY);
    CHECK(back.groundBlock == edit->groundBlock);
    CHECK(back.waterY == edit->waterY);
    CHECK(back.removed == edit->removed);
    EQUAL(back.structureTopY(), edit->structureTopY(), "decoded top");
    Chunk decoded;
    chunk_codec::decode(**back.slab(26), decoded);
    CHECK(sameTiles(decoded, upper));
    CHECK(back.slab(24) == nullptr);
    CHECK(file.extra.find("FUTR") != nullptr);  // unknown section kept

    BinaryWriter old;
    old.write(uint32_t(1));
    old.write(uint32_t(41));
    old.write(uint32_t(7));
    for (const auto y : edit->groundY) old.write(y);
    for (const auto block : edit->groundBlock) old.write(block);
    for (const auto y : edit->waterY) old.write(y);
    old.write(uint16_t(1));
    old.write(uint16_t(26));
    old.writeBlock(chunk_codec::encode(upper));
    SectionContainer legacy;
    legacy.put("EDIT", 1, old.take());
    const auto oldFile = terrain_edits_codec::decode(legacy.encode());
    const auto& oldColumn = *oldFile.columns.at(editKey(41, 7));
    CHECK(oldColumn.removed.empty());
    chunk_codec::decode(**oldColumn.slab(26), decoded);
    CHECK(sameTiles(decoded, upper));
}

TEST(terrain_edits_rejects_corrupt) {
    auto edit = std::make_shared<ColumnEdit>();
    edit->groundY.fill(10);
    Chunk chunk;
    chunk.tiles[0][0][0].block_id = 1;
    edit->structure = {
        {30, std::make_shared<const Bytes>(chunk_codec::encode(chunk))}};
    edit->finish();
    const auto bytes = terrain_edits_codec::encode({{editKey(1, 2), edit}});
    CHECK(throwsSerialization([&] {
        terrain_edits_codec::decode(ByteView(bytes).first(bytes.size() - 9));
    }));
    // Out-of-range ground height inside an otherwise valid container.
    BinaryWriter out;
    out.write(uint32_t(1));
    out.write(uint32_t(0));
    out.write(uint32_t(0));
    for (int i = 0; i < ColumnEdit::Tiles; ++i) out.write(int16_t(30000));
    SectionContainer container;
    container.put("EDIT", 1, out.take());
    const auto invalid = container.encode();
    CHECK(throwsSerialization([&] { terrain_edits_codec::decode(invalid); }));
    // A missing file is an empty index.
    const auto root = scratch("edits");
    std::filesystem::create_directories(root);
    CHECK(save_catalog::readTerrainEdits({root, {}}).columns.empty());
    save_catalog::writeTerrainEdits({root, {}}, {{editKey(1, 2), edit}});
    EQUAL(save_catalog::readTerrainEdits({root, {}}).columns.size(), size_t(1),
          "written file");
    std::filesystem::remove_all(root);
}

TEST(light_sources_roundtrip) {
    using light_sources_codec::Record;
    const std::vector<Record> lights{{{300, 900, 12}, 6}, {{5, 400, 70000}, 9}};
    SectionContainer extra;
    extra.put("FUTR", 3, Bytes{std::byte{1}, std::byte{2}});
    const auto file =
        light_sources_codec::decode(light_sources_codec::encode(lights, extra));
    EQUAL(file.lights.size(), size_t(2), "lights");
    // Sorted by x, then z, then y.
    CHECK(file.lights[0] == lights[1]);
    CHECK(file.lights[1] == lights[0]);
    CHECK(file.extra.find("FUTR") != nullptr);
    CHECK(file.extra.find("LITE") == nullptr);
    // A missing file is an empty index.
    const auto root = scratch("lights");
    std::filesystem::create_directories(root);
    CHECK(save_catalog::readLightSources({root, {}}).lights.empty());
    save_catalog::writeLightSources({root, {}}, lights);
    EQUAL(save_catalog::readLightSources({root, {}}).lights.size(), size_t(2),
          "written file");
    std::filesystem::remove_all(root);
}

TEST(light_sources_rejects_corrupt) {
    const auto bytes =
        light_sources_codec::encode({{{1, 2, 3}, 6}, {{4, 5, 6}, 6}});
    CHECK(throwsSerialization([&] {
        light_sources_codec::decode(ByteView(bytes).first(bytes.size() - 3));
    }));
    auto section = [](auto&& fill) {
        BinaryWriter out;
        fill(out);
        SectionContainer container;
        container.put("LITE", 2, out.take());
        return container.encode();
    };
    auto record = [](BinaryWriter& out, uint32_t x, uint16_t y, uint8_t b) {
        out.write(x);
        out.write(y);
        out.write(uint32_t(0));
        out.write(b);
        out.write(uint8_t(0x3f));
    };
    const auto duplicate = section([&](BinaryWriter& out) {
        out.write(uint32_t(2));
        record(out, 1, 1, 6);
        record(out, 1, 1, 6);
    });
    CHECK(throwsSerialization([&] { light_sources_codec::decode(duplicate); }));
    const auto high = section([&](BinaryWriter& out) {
        out.write(uint32_t(1));
        record(out, 1, uint16_t(WORLD_HEIGHT_Y), 6);
    });
    CHECK(throwsSerialization([&] { light_sources_codec::decode(high); }));
    const auto trailing = section([&](BinaryWriter& out) {
        out.write(uint32_t(1));
        record(out, 1, 1, 6);
        out.write(uint8_t(0));
    });
    CHECK(throwsSerialization([&] { light_sources_codec::decode(trailing); }));
    const auto noBlock = section([&](BinaryWriter& out) {
        out.write(uint32_t(1));
        record(out, 1, 1, 0);
    });
    CHECK(throwsSerialization([&] { light_sources_codec::decode(noBlock); }));
}

TEST(light_sources_versions) {
    using light_sources_codec::Record;
    // v2 keeps the open faces.
    const auto v2 = light_sources_codec::decode(
        light_sources_codec::encode({{{7, 8, 9}, 6, 0x05}}));
    EQUAL(int(v2.lights.at(0).openFaces), 5, "v2 faces");
    // v1 files (far lights only, no faces) read as all faces open.
    BinaryWriter out;
    out.write(uint32_t(1));
    out.write(uint32_t(7));
    out.write(uint16_t(8));
    out.write(uint32_t(9));
    out.write(uint8_t(6));
    SectionContainer container;
    container.put("LITE", 1, out.take());
    const auto v1 = light_sources_codec::decode(container.encode());
    EQUAL(int(v1.lights.at(0).openFaces), 0x3f, "v1 faces open");
    EQUAL(int(v1.lights.at(0).block), 6, "v1 block");
    // Invalid face bits are rejected.
    BinaryWriter bad;
    bad.write(uint32_t(1));
    bad.write(uint32_t(7));
    bad.write(uint16_t(8));
    bad.write(uint32_t(9));
    bad.write(uint8_t(6));
    bad.write(uint8_t(0x40));
    SectionContainer badContainer;
    badContainer.put("LITE", 2, bad.take());
    const auto encoded = badContainer.encode();
    CHECK(throwsSerialization([&] { light_sources_codec::decode(encoded); }));
}
