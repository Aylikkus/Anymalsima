#include <filesystem>

#include "serialization/file_io.hpp"
#include "test.hpp"
#include "world/save/chunk_codec.hpp"
#include "world/save/region_file.hpp"
#include "world/save/save_catalog.hpp"
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
                    l.liquidLevel != r.liquidLevel)
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
