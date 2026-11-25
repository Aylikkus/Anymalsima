#include <climits>
#include <set>

#include "flora_assets.hpp"
#include "world/day_night.hpp"
#include "world/flora_generator.hpp"
#include "world/flora_mesh_builder.hpp"
#include "world/flora_simulation.hpp"
#include "world/save/flora_codec.hpp"
#include "world/save/region_file.hpp"
#include "world/save/world_meta.hpp"
#include "world/sky_height_map.hpp"
#include "world/world_clock.hpp"
#include "world/world_editor.hpp"
#include "world_fixture.hpp"

// Plants (docs/handoff/flora.md, step 5): deterministic per-chunk
// generation, persistence and edit rules.
namespace {
World meadowWorld(int width = 6, int height = 5) {
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
            r = test::regionAt(SEA_LEVEL_Y + 24);
            r.coord = {unsigned(x), unsigned(z)};
            r.land = LandType::PLAINS;
            r.biome = BiomeType::TEMPERATE;
            r.vegetation = VegetationType::GRASS;
            r.humidity = .65f;
            r.temperature_c = 16;
        }
    return world;
}

struct Generated {
    ChunkCoord coord;
    FloraLayer layer;
};

// Plants of every slab of chunk column (cx, cz).
std::vector<Generated> plantsOf(const World& world, const IAssets& assets,
                                const FloraSpeciesTable& table, unsigned cx,
                                unsigned cz, Vec2u captureRegion,
                                uint32_t seed = 99) {
    const auto own = TerrainNeighborhood::capture(
        world, {cx / CHUNKS_PER_REGION, cz / CHUNKS_PER_REGION}, assets);
    const auto columns = TerrainSampler(own).columns(cx, cz);
    const auto terrain =
        TerrainNeighborhood::capture(world, captureRegion, assets);
    std::vector<Generated> out;
    for (unsigned y = 20; y < 40; ++y) {
        auto layer =
            generateFlora({&table, &terrain, &columns, {cx, y, cz}, seed});
        if (!layer.empty()) out.push_back({{cx, y, cz}, std::move(layer)});
    }
    return out;
}

struct Placed {
    FloraVolume volume;
    const FloraSpeciesRule* rule;
};

bool overlaps(const Placed& a, const Placed& b) {
    const double gap = std::max(a.rule->spacing, b.rule->spacing);
    return a.volume.minX - gap < b.volume.minX + b.volume.sizeX + gap &&
           b.volume.minX - gap < a.volume.minX + a.volume.sizeX + gap &&
           a.volume.minZ - gap < b.volume.minZ + b.volume.sizeZ + gap &&
           b.volume.minZ - gap < a.volume.minZ + a.volume.sizeZ + gap;
}
}  // namespace

TEST(flora_codec_roundtrip) {
    FloraLayer layer;
    for (int i = 0; i < 40; ++i)
        layer.instances.push_back({uint8_t(1 + i % 3), uint8_t(i % 2),
                                   uint16_t(i * 977), uint8_t(i * 5),
                                   uint8_t(255 - i), uint8_t(i % 16),
                                   uint8_t(i * 7), -i * 13, i * 3});
    layer.resolvedDay = 42;
    // v3 header: version, resolvedDay, tree stamp (day, version), count.
    constexpr size_t Header = 1 + 4 + 4 + 2 + 2;
    const TreeStamp stamp{77, 3};
    const auto bytes = flora_codec::encode(layer, stamp);
    EQUAL(bytes.size(), size_t(Header + 40 * 16), "16 bytes per plant");
    TreeStamp decoded;
    CHECK(flora_codec::decode(bytes, &decoded) == layer);
    CHECK(decoded == stamp);
    flora_codec::decode(flora_codec::encode({}), &decoded);
    CHECK(!decoded.placed());  // no trees placed by default
    CHECK(flora_codec::decode(flora_codec::encode({})).empty());
    const auto rejects = [](Bytes bad, const char* context) {
        test::expectThrows([&] { flora_codec::decode(bad); }, context);
    };
    auto version = bytes;
    version[0] = std::byte{9};
    rejects(version, "version");
    auto truncated = bytes;
    truncated.resize(truncated.size() - 1);
    rejects(truncated, "truncated");
    auto trailing = bytes;
    trailing.push_back(std::byte{0});
    rejects(trailing, "trailing bytes");
    auto species = bytes;
    species[Header] = std::byte{0};
    rejects(species, "no species");
    auto anchor = bytes;
    anchor[Header + 6] = std::byte{16};
    rejects(anchor, "anchor outside the chunk");
    // Version 1 (no day stamps) still decodes.
    BinaryWriter v1;
    v1.write(uint8_t(1));
    v1.write(uint16_t(1));
    for (const uint8_t b : {3, 1, 7, 0, 32, 48, 5, 9}) v1.write(b);
    v1.write(int32_t(-20));
    const auto old = flora_codec::decode(v1.take());
    EQUAL(old.instances.size(), size_t(1), "v1 plant");
    EQUAL(int(old.instances[0].species), 3, "v1 species");
    EQUAL(old.instances[0].birthDay, -20, "v1 birth day");
    EQUAL(old.instances[0].harvestDay, 0, "v1 has no harvest");
    EQUAL(old.resolvedDay, 0, "v1 has no day stamp");
    // Version 2 (no tree stamp): chunks saved before trees get them once.
    BinaryWriter v2;
    v2.write(uint8_t(2));
    v2.write(int32_t(15));
    v2.write(uint16_t(0));
    TreeStamp none{5, 5};
    CHECK(flora_codec::decode(v2.take(), &none).resolvedDay == 15);
    CHECK(!none.placed());

    // Region files keep plants per chunk in FLOR.
    RegionSnapshot snapshot;
    snapshot.chunks.emplace_back(70, std::make_shared<const Chunk>());
    snapshot.flora.emplace_back(70, std::make_shared<const FloraLayer>(layer));
    snapshot.chunks.emplace_back(71, std::make_shared<const Chunk>());
    snapshot.flora.emplace_back(71, nullptr);  // emptied: saved as empty
    snapshot.trees.emplace_back(71, stamp);
    const auto data = region_file::decode(
        region_file::encode(region_file::merge(RegionData{}, snapshot)));
    EQUAL(data.flora.size(), size_t(2), "flora entries");
    CHECK(flora_codec::decode(*data.flora.at(70)) == layer);
    CHECK(flora_codec::decode(*data.flora.at(71), &decoded).empty());
    CHECK(decoded == stamp);  // stamps travel with empty layers too
    CHECK(data.extra.find("FLOR") == nullptr);
}

TEST(flora_growth_stages) {
    auto annual = Json::parse(R"({
        "name": "flora.annual", "kind": "flower",
        "placement": { "climate": [ {} ] },
        "growth": { "stages": { "seedling": 2, "young": 3, "mature": 5,
                                "flowering": 4, "withering": 2 } },
        "parts": [ { "type": "leaf_card", "material": "m" } ] })")
                      .get<Flora>();
    const auto at = [&](const Flora& f, double age) {
        return flora_growth::at(f, 7, age);
    };
    CHECK(at(annual, 0).stage == FloraStage::Seedling);
    CHECK(at(annual, 2.5).stage == FloraStage::Young);
    EQUAL(at(annual, 2.5).progress, 0.5f / 3.f, "progress in stage");
    CHECK(at(annual, 11).stage == FloraStage::Flowering);
    CHECK(at(annual, 15).stage == FloraStage::Withering &&
          at(annual, 15).alive);
    CHECK(!at(annual, 17).alive);
    auto perennial = annual;
    perennial.growth.perennial = true;
    perennial.growth.lifespan_days = ValueRange(100);
    // Grows up once, then cycles mature (5) and flowering (4).
    CHECK(at(perennial, 6).stage == FloraStage::Mature);
    CHECK(at(perennial, 11).stage == FloraStage::Flowering);
    CHECK(at(perennial, 15).stage == FloraStage::Mature);
    CHECK(at(perennial, 101).stage == FloraStage::Withering);
    CHECK(at(perennial, 101).alive && !at(perennial, 103).alive);
}

TEST(flora_generation_borders) {
    test::FloraAssets assets;
    test::loadAllContent(assets);
    const auto world = meadowWorld();
    const FloraSpeciesTable table(assets);
    CHECK(!table.empty());
    CHECK(table.reach() > table.verdict());
    // An 4x4 block of chunk columns across the region border at chunk 24.
    std::vector<Placed> large;
    std::map<uint8_t, int> counts;
    std::vector<std::pair<int64_t, int64_t>> small;
    for (unsigned cz = 22; cz < 26; ++cz)
        for (unsigned cx = 22; cx < 26; ++cx) {
            const auto mine =
                plantsOf(world, assets, table, cx, cz, {cx / 8, cz / 8});
            // Job order and the captured neighbourhood do not matter.
            const auto again = plantsOf(world, assets, table, cx, cz, {3, 3});
            CHECK(mine.size() == again.size());
            for (size_t i = 0; i < mine.size(); ++i)
                CHECK(mine[i].layer == again[i].layer);
            for (const auto& g : mine)
                for (const auto& p : g.layer.instances) {
                    ++counts[p.species];
                    const auto* rule = table.find(p.species);
                    CHECK(rule != nullptr);
                    const auto volume = floraVolume(p, g.coord, *rule);
                    if (rule->large)
                        large.push_back({volume, rule});
                    else
                        small.push_back({volume.minX, volume.minZ});
                }
        }
    CHECK(large.size() >= 2);
    CHECK(small.size() > 1000);  // lush meadow: ~30 tufts per 100 m²
    // Large plants stand on level ground with nothing rising beside them,
    // so their canopy never grows into a terrace wall.
    const auto data = TerrainNeighborhood::capture(world, {2, 2}, assets);
    const TerrainSampler sampler(data);
    for (const auto& b : large)
        for (int64_t dz = -1; dz <= b.volume.sizeZ; ++dz)
            for (int64_t dx = -1; dx <= b.volume.sizeX; ++dx) {
                const int y =
                    sampler.sample(b.volume.minX + dx, b.volume.minZ + dz)
                        .surface_y;
                const bool under = dx >= 0 && dz >= 0 && dx < b.volume.sizeX &&
                                   dz < b.volume.sizeZ;
                if (under)
                    EQUAL(y, b.volume.anchorY, "level footprint");
                else
                    CHECK(y <= b.volume.anchorY);
            }  // lush meadow: ~30 tufts per 100 m²
    // No two large plants overlap (with clearance), even across chunks.
    for (size_t i = 0; i < large.size(); ++i)
        for (size_t j = i + 1; j < large.size(); ++j)
            CHECK(!overlaps(large[i], large[j]));
    // Small plants never stand inside a large footprint.
    for (const auto& [x, z] : small)
        for (const auto& b : large)
            CHECK(!(x >= b.volume.minX && x < b.volume.minX + b.volume.sizeX &&
                    z >= b.volume.minZ && z < b.volume.minZ + b.volume.sizeZ));
    // Several species grow here (grass, ferns or flowers, bushes).
    CHECK(counts.size() >= 3);
    // A different world seed gives different plants.
    const auto other = plantsOf(world, assets, table, 22, 22, {2, 2}, 100);
    const auto base = plantsOf(world, assets, table, 22, 22, {2, 2});
    CHECK(other.empty() || base.empty() || !(other[0].layer == base[0].layer));
}

TEST(flora_generation_wraps_seam) {
    test::FloraAssets assets;
    test::loadAllContent(assets);
    const auto world = meadowWorld(16, 2);  // W = 2048 tiles, 128 chunks
    const FloraSpeciesTable table(assets);
    // Chunk 0 decided from the east side of the seam is the same.
    const auto west = plantsOf(world, assets, table, 0, 5, {0, 0});
    const auto east = plantsOf(world, assets, table, 0, 5, {15, 0});
    CHECK(!west.empty());
    CHECK(west.size() == east.size());
    for (size_t i = 0; i < west.size(); ++i)
        CHECK(west[i].layer == east[i].layer);
    // Large plants on both sides of the seam do not overlap.
    std::vector<Placed> large;
    for (unsigned cx : {126u, 127u, 0u, 1u})
        for (unsigned cz = 3; cz < 8; ++cz)
            for (const auto& g :
                 plantsOf(world, assets, table, cx, cz, {cx / 8, 0}))
                for (const auto& p : g.layer.instances)
                    if (const auto* r = table.find(p.species); r->large) {
                        auto v = floraVolume(p, g.coord, *r);
                        if (v.minX >= 1024) v.minX -= 2048;  // one frame
                        large.push_back({v, r});
                    }
    for (size_t i = 0; i < large.size(); ++i)
        for (size_t j = i + 1; j < large.size(); ++j)
            CHECK(!overlaps(large[i], large[j]));
}

namespace {
struct FloraStream {
    test::FloraAssets assets;
    World world = meadowWorld();
    LocationGenerator generator;
    ThreadPool pool{2};
    ChunkMesher mesher{assets};
    std::unique_ptr<ChunkManager> chunks;
    TerrainEdits skyEdits;
    std::unique_ptr<SkyHeightMap> skyMap;

    void prepareSky(const std::vector<ChunkCoord>& columns) {
        skyMap = std::make_unique<SkyHeightMap>(
            *chunks, skyEdits, assets.getStorage<Block>(), pool);
        for (const auto& c : columns) skyMap->request(c.x, c.z);
        do {
            chunks->update();
            skyMap->update();
            CHECK(pool.wait_for(std::chrono::seconds(10)));
        } while (!skyMap->isIdle());
    }
    // extraFlora: species added after the shipped ones.
    explicit FloraStream(const Json& extraFlora = Json::array()) {
        test::loadAllContent(assets);
        if (!extraFlora.empty()) assets.load<Flora>(extraFlora);
        chunks = std::make_unique<ChunkManager>(generator, pool, &mesher, world,
                                                &assets);
    }
    ~FloraStream() { pool.wait(); }
    // The slab of (cx, cz) with the most plants.
    ChunkCoord load(unsigned cx, unsigned cz, ChunkBlob stored = nullptr,
                    ChunkBlob storedFlora = nullptr, int onlyY = -1) {
        const auto& region = world.regions[(cz / 8) * world.width + cx / 8];
        ChunkCoord best{cx, 0, cz};
        size_t most = 0;
        // Like LocationManager, register the column's lowest slab too:
        // ChunkManager::generatedColumns finds the region through it.
        if (!chunks->isRegistered({cx, 0, cz}))
            chunks->registerChunk({cx, 0, cz}, {(cx / 8) * 8, 0, (cz / 8) * 8},
                                  &region);
        for (unsigned y = 20; y < 40; ++y) {
            if (onlyY >= 0 && int(y) != onlyY) continue;
            const ChunkCoord c{cx, y, cz};
            chunks->registerChunk(c, {(cx / 8) * 8, 0, (cz / 8) * 8}, &region,
                                  stored, storedFlora);
            chunks->request(c);
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            chunks->update();
            CHECK(pool.wait_for(std::chrono::seconds(3)));
            CHECK(std::chrono::steady_clock::now() < deadline);
        } while (chunks->hasGenerationWork());
        for (unsigned y = 20; y < 40; ++y) {
            const auto layer = chunks->flora({cx, y, cz});
            if (layer && layer->instances.size() > most) {
                most = layer->instances.size();
                best = {cx, y, cz};
            }
        }
        return best;
    }
};
}  // namespace

TEST(flora_streaming_persists) {
    FloraStream f;
    const auto coord = f.load(17, 18);
    const auto generated = f.chunks->flora(coord);
    CHECK(generated && !generated->empty());
    CHECK(f.chunks->isPersistDirty(coord));
    // Saving captures the plants beside the tiles.
    const auto snapshot = f.chunks->snapshotRegion({2, 2});
    const auto index = RegionData::localIndex(coord);
    const auto data = region_file::decode(
        region_file::encode(region_file::merge(RegionData{}, snapshot)));
    CHECK(data.flora.contains(index));
    // Edited plants are what reloads, not regenerated ones.
    auto edited = std::make_shared<FloraLayer>(*generated);
    edited->instances.resize(1);
    f.chunks->setFlora(coord, edited);
    const auto saved = f.chunks->snapshotRegion({2, 2});
    const auto edits = region_file::merge(data, saved);
    f.chunks->unregister(coord);
    f.load(17, 18, edits.chunks.at(index), edits.flora.at(index), int(coord.y));
    CHECK(f.chunks->flora(coord) && *f.chunks->flora(coord) == *edited);
    CHECK(!f.chunks->isPersistDirty(coord));
    // A saved chunk without plants (older saves) regrows them and saves.
    f.chunks->unregister(coord);
    f.load(17, 18, edits.chunks.at(index), nullptr, int(coord.y));
    CHECK(f.chunks->flora(coord) && *f.chunks->flora(coord) == *generated);
    CHECK(f.chunks->isPersistDirty(coord));
}

TEST(flora_edits) {
    FloraStream f;
    const auto& table = f.chunks->floraSpecies();
    WorldEditor editor(*f.chunks, nullptr, f.assets.getStorage<Block>());
    // Find a chunk with a blocking bush and one with a small plant.
    std::optional<std::pair<ChunkCoord, FloraInstance>> bush, small;
    for (unsigned cx = 16; cx < 22 && !(bush && small); ++cx)
        for (unsigned cz = 16; cz < 22 && !(bush && small); ++cz) {
            const auto c = f.load(cx, cz);
            if (const auto layer = f.chunks->flora(c))
                for (const auto& p : layer->instances) {
                    const auto* r = table.find(p.species);
                    // Interior plants only: neighbours are not loaded.
                    const auto v = floraVolume(p, c, *r);
                    const bool inside = v.minX >= c.x * 16 &&
                                        v.minX + v.sizeX <= c.x * 16 + 16 &&
                                        v.minZ >= c.z * 16 &&
                                        v.minZ + v.sizeZ <= c.z * 16 + 16 &&
                                        p.y + 1 < 16;
                    if (!inside) continue;
                    if (r->blocksPlacement && !bush) bush = {{c, p}};
                    if (!r->large && !small) small = {{c, p}};
                }
        }
    CHECK(bush && small);
    const auto count = [&](ChunkCoord c) {
        const auto layer = f.chunks->flora(c);
        return layer ? layer->instances.size() : size_t(0);
    };
    const auto& [bc, bp] = *bush;
    const auto bv = floraVolume(bp, bc, *table.find(bp.species));
    const TilePos inBush{int(bv.minX), bv.anchorY + 1, int(bv.minZ)};
    EQUAL(int(editor.placeBlock(inBush, 1)), int(EditStatus::Blocked),
          "bush footprint blocks placement");
    // Digging the ground under the bush removes it. The layer keeps its
    // resolved day: a reset day makes FloraSimulation re-sample the chunk
    // after a year (regression).
    {
        auto stamped = std::make_shared<FloraLayer>(*f.chunks->flora(bc));
        stamped->resolvedDay = 500;
        f.chunks->setFlora(bc, std::move(stamped));
    }
    const auto before = count(bc);
    EQUAL(int(editor.removeBlock({int(bv.minX), bv.anchorY, int(bv.minZ)})),
          int(EditStatus::Applied), "dig under the bush");
    EQUAL(count(bc), before - 1, "bush removed with its ground");
    EQUAL(f.chunks->flora(bc)->resolvedDay, 500,
          "edits keep the layer's resolved day");
    CHECK(f.chunks->isPersistDirty(bc));
    // A block placed on a small plant removes it.
    const auto& [sc, sp] = *small;
    const auto sv = floraVolume(sp, sc, *table.find(sp.species));
    const auto smallBefore = count(sc);
    int removed = 0;
    for (const auto& p : f.chunks->flora(sc)->instances)
        if (const auto v = floraVolume(p, sc, *table.find(p.species));
            v.contains(sv.minX, sv.anchorY + 1, sv.minZ))
            ++removed;
    EQUAL(
        int(editor.placeBlock({int(sv.minX), sv.anchorY + 1, int(sv.minZ)}, 1)),
        int(EditStatus::Applied), "place on a small plant");
    EQUAL(count(sc), smallBefore - removed, "small plants in the tile removed");
    CHECK(removed >= 1);
}

TEST(flora_meshes) {
    test::FloraAssets assets;
    test::loadAllContent(assets);
    const FloraMeshBuilder builder(assets);
    const auto& species = assets.getStorage<Flora>();
    const auto& materials = assets.getStorage<FoliageMaterial>();
    const auto id = [&](const char* name) {
        return species.get(std::string(name)).id;
    };
    // Every shipped species, stage and variant builds a valid mesh.
    for (const auto& f : species.items)
        for (int stage = 0; stage < FloraStageCount; ++stage)
            for (int variant = 0; variant < FloraMeshBuilder::Variants;
                 ++variant) {
                const auto mesh =
                    builder.build(f.id, variant, FloraStage(stage));
                CHECK(mesh.indices.size() % 3 == 0);
                for (const auto i : mesh.indices)
                    CHECK(i < mesh.vertices.size());
                for (const auto& v : mesh.vertices) {
                    CHECK(v.height >= 0 && v.height <= 1);
                    CHECK(v.layer < float(materials.items.size()));
                    CHECK(std::abs(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz -
                                   1) < 1e-3f);
                    // Low clump cards may dip into the ground (it hides
                    // the seam), but only a little.
                    CHECK(v.y >= -0.35f && v.y <= mesh.height + 1e-4f);
                }
            }
    // Deterministic per variant; variants differ.
    const auto bush = id("flora.round_bush");
    const auto a = builder.build(bush, 2, FloraStage::Mature);
    const auto b = builder.build(bush, 2, FloraStage::Mature);
    const auto c = builder.build(bush, 3, FloraStage::Mature);
    CHECK(a.vertices.size() == b.vertices.size());
    CHECK(std::equal(a.indices.begin(), a.indices.end(), b.indices.begin()));
    CHECK(a.vertices.size() != c.vertices.size() || a.height != c.height);
    // Every bush variant keeps its canopy over its 2x2 footprint (plus the
    // 0.3 m overhang) at the largest instance scale.
    for (int variant = 0; variant < FloraMeshBuilder::Variants; ++variant)
        for (int stage = 0; stage < FloraStageCount; ++stage)
            for (const auto& v :
                 builder.build(bush, variant, FloraStage(stage)).vertices) {
                CHECK(std::abs(v.x) * 1.15f <= 1.3f + 1e-3f);
                CHECK(std::abs(v.z) * 1.15f <= 1.3f + 1e-3f);
            }
    // A bush is a rounded volume about its recipe's size, shaded with
    // spherical normals.
    CHECK(a.height > 0.8f && a.height < 2.6f);
    CHECK(a.radius > 0.5f && a.radius < 2.f);
    int shaped = 0;
    for (const auto& v : a.vertices) shaped += v.normalMode > 0.5f;
    CHECK(shaped > int(a.vertices.size()) * 9 / 10);
    // Berries only while fruiting; seedlings are small.
    const float berry =
        float(materials.get(std::string("foliage.red_berry")).id - 1);
    const auto hasLayer = [](const FloraMesh& m, float layer) {
        return std::any_of(m.vertices.begin(), m.vertices.end(),
                           [&](const auto& v) { return v.layer == layer; });
    };
    CHECK(!hasLayer(a, berry));
    CHECK(hasLayer(builder.build(bush, 2, FloraStage::Fruiting), berry));
    CHECK(builder.build(bush, 2, FloraStage::Seedling).height <
          a.height * 0.5f);
    // A flowering flower has a stem (untextured), petals and its centre.
    const auto flower =
        builder.build(id("flora.meadow_flower"), 0, FloraStage::Flowering);
    CHECK(hasLayer(flower, -1));
    CHECK(hasLayer(
        flower,
        float(materials.get(std::string("foliage.meadow_petal")).id - 1)));
    CHECK(hasLayer(
        flower,
        float(materials.get(std::string("foliage.flower_center")).id - 1)));
    CHECK(!hasLayer(
        builder.build(id("flora.meadow_flower"), 0, FloraStage::Young),
        float(materials.get(std::string("foliage.meadow_petal")).id - 1)));
    // Grass tufts have no parts while "flowering" (a stage they lack).
    CHECK(builder.build(id("flora.grass_tuft"), 0, FloraStage::Flowering)
              .empty());
    CHECK(builder.build(0, 0, FloraStage::Mature).empty());
}

TEST(flora_world_clock) {
    WorldClock clock;
    EQUAL(clock.day(), int64_t(0), "starts at day 0");
    const auto midnight = skyAtTime(clock.timeOfDay()).sun;
    CHECK(midnight[1] < -0.9f);
    clock.advance(WorldClock::DayLengthSeconds / 4);
    const auto sunrise = skyAtTime(clock.timeOfDay()).sun;
    CHECK(std::abs(sunrise[1]) < 1e-6f);
    clock.advance(WorldClock::DayLengthSeconds / 4);
    EQUAL(clock.timeOfDay(), 0.5, "noon after half a day");
    const auto noon = skyAtTime(clock.timeOfDay()).sun;
    CHECK(noon[1] > 0.9f);
    CHECK(midnight[3] < sunrise[3] && sunrise[3] < noon[3]);
    const auto seededSky = skyAtTime(0.0, 17);
    CHECK(seededSky.seed == skyAtTime(0.5, 17).seed);
    CHECK(seededSky.seed != skyAtTime(0.0, 17 + (uint64_t{1} << 63)).seed);
    CHECK(seededSky.sun == midnight);
    clock.setSpeed(60);
    clock.advance(WorldClock::DayLengthSeconds * 2);
    EQUAL(clock.day(), int64_t(120), "debug speed");
    clock.advance(-5);
    EQUAL(clock.day(), int64_t(120), "time never runs backwards");
    clock.set(-3);
    EQUAL(clock.days(), 0.0, "clamped at 0");
    // world.json keeps the clock; older files start at day 0.
    WorldMeta meta;
    meta.seed = 1;
    meta.width = meta.height = 1;
    meta.worldDays = 12.25;
    WorldMeta loaded;
    loaded.deserialize(meta.serialize());
    EQUAL(loaded.worldDays, 12.25, "saved game time");
    clock.set(loaded.worldDays);
    CHECK(skyAtTime(clock.timeOfDay()).sun == sunrise);
    auto old = meta.serialize();
    old.erase("worldDays");
    loaded.deserialize(old);
    EQUAL(loaded.worldDays, 0.0, "older saves");
    CHECK(!loaded.extra.contains("worldDays"));
}

TEST(flora_life_cycle) {
    test::FloraAssets assets;
    test::loadAllContent(assets);
    const auto& species = assets.getStorage<Flora>();
    const auto& tuft = species.get(std::string("flora.grass_tuft"));
    const auto& bush = species.get(std::string("flora.round_bush"));
    const auto& flower = species.get(std::string("flora.meadow_flower"));
    FloraLayer layer;
    layer.instances.push_back({tuft.id, 0, 777, 40, 40, 3, 0, -5, 0});
    layer.instances.push_back({bush.id, 0, 4242, 80, 80, 3, 0, -10, 0});
    layer.instances.push_back({tuft.id, 0, 123, 90, 90, 3, 0, 50, 0});
    // Before anyone dies nothing changes; a seed waits for its day.
    auto same = layer;
    CHECK(!advanceFlora(same, species, 1));
    CHECK(!flora_growth::at(tuft, 123, 1 - 50).alive);
    // Plants past their death age are gone: no rebirth in place.
    auto later = layer;
    const double death = -5 + flora_growth::deathAge(tuft, 777);
    CHECK(advanceFlora(later, species, death + 1));
    for (const auto& p : later.instances) CHECK(p.seed != 777);
    // The bush (born -10, living 400+ days) outlives the tuft.
    CHECK(std::any_of(later.instances.begin(), later.instances.end(),
                      [&](const auto& p) { return p.species == bush.id; }));
    auto empty = layer;
    advanceFlora(empty, species, 100000);
    CHECK(empty.instances.empty());
    // A harvested bush regrows after regrow_after_harvest_days.
    auto picked = layer;
    picked.instances[1].flags = FloraInstance::Harvested;
    picked.instances[1].harvestDay = 0;
    CHECK(!advanceFlora(picked, species, 5));
    CHECK(picked.instances[1].flags & FloraInstance::Harvested);
    CHECK(advanceFlora(picked, species, 25));
    CHECK(!(picked.instances[1].flags & FloraInstance::Harvested));
    // Seeds drop at the end of the seed stage of each cycle.
    const auto days = flora_growth::durations(bush, 4242);
    EQUAL(flora_growth::seedDrop(bush, 4242),
          days[int(FloraStage::Mature)] + days[int(FloraStage::Flowering)] +
              days[int(FloraStage::Fruiting)],
          "bush: end of fruiting");
    const auto flowerDays = flora_growth::durations(flower, 9);
    double toFlowerEnd = 0;
    for (int st = 0; st <= int(FloraStage::Flowering); ++st)
        toFlowerEnd += flowerDays[st];
    EQUAL(flora_growth::seedDrop(flower, 9), toFlowerEnd,
          "annual flower: end of flowering");
    // Every shipped species leaves enough seedlings to sustain itself where
    // nothing crowds it, but not absurdly many (crowding holds the rest).
    for (const auto& f : species.items)
        for (uint16_t seed : {1, 999, 40000}) {
            const double r0 = flora_growth::replacementRatio(f, seed);
            CHECK(r0 >= 1.5 && r0 <= 40);
        }
}

TEST(flora_catchup_uses_direct_sky) {
    FloraStream f;
    const auto coord = f.load(18, 18);
    f.prepareSky({coord});
    const auto original = f.chunks->flora(coord);
    CHECK(original && !original->empty());
    FloraSimulation sim(*f.chunks, f.assets, f.world, *f.skyMap);
    const double today = FloraSimulation::CatchUpDays + 1;
    sim.update(today);
    const auto open = f.chunks->flora(coord);
    CHECK(open && open->resolvedDay == int(today));

    // Place an overhead roof in the heightmap beyond the loaded slabs.
    // The old near-field light in the plant's chunk is unchanged.
    constexpr int Roof = 800;
    CHECK(!f.chunks->isReady({coord.x, Roof / Chunk::SIZE_Y, coord.z}));
    sky::Overrides overrides;
    const auto key = SkyHeightMap::columnKey(coord.x, coord.z);
    for (int i = 0; i < sky::Tile::Size; ++i) overrides[key][i].opaque = Roof;
    f.skyMap->setOverrides(std::move(overrides));
    f.chunks->setFlora(coord, original);
    // The slow sweep reaches the chunk again, using the same elapsed days.
    sim.update(today);
    CHECK(!f.chunks->flora(coord));
}

TEST(flora_simulation_spread_harvest_regrow) {
    FloraStream f;
    WorldEditor editor(*f.chunks, nullptr, f.assets.getStorage<Block>());
    // Load a 5x5 block of chunk columns (their plants resolve on load);
    // populations are measured in the inner 3x3, away from the unloaded
    // edge where seeds are lost (the game loads all around the camera).
    std::vector<ChunkCoord> surface, all;
    for (unsigned cz = 16; cz < 21; ++cz)
        for (unsigned cx = 16; cx < 21; ++cx) {
            const auto c = f.load(cx, cz);
            all.push_back(c);
            if (cx > 16 && cx < 20 && cz > 16 && cz < 20) surface.push_back(c);
        }
    const auto inner = [](const ChunkCoord& c) {
        return c.x > 16 && c.x < 20 && c.z > 16 && c.z < 20;
    };
    f.prepareSky(all);
    FloraSimulation sim(*f.chunks, f.assets, f.world, *f.skyMap);
    sim.watch(editor);
    const auto count = [&] {
        size_t n = 0;
        f.chunks->forEachReady([&](const ChunkCoord& c, Chunk&) {
            if (const auto l = f.chunks->flora(c)) n += l->instances.size();
        });
        return n;
    };
    const auto settle = [&](double day) {
        for (int i = 0; i < 200; ++i) sim.update(day);
    };
    settle(0.5);
    const auto start = count();
    const auto center = f.chunks->flora(surface[4]);
    CHECK(center != nullptr);
    // Resolving again on the same day changes nothing.
    settle(0.9);
    CHECK(f.chunks->flora(surface[4]) == center);
    // Three in-game years in 5-day steps: plants die and seeds replace
    // them. Crowding holds every species near its generated population:
    // it neither collapses into hollows nor overgrows.
    const auto& table = f.chunks->floraSpecies();
    const auto perSpecies = [&] {
        std::map<uint8_t, size_t> n;
        f.chunks->forEachReady([&](const ChunkCoord& c, Chunk&) {
            if (!inner(c)) return;
            if (const auto l = f.chunks->flora(c))
                for (const auto& p : l->instances) ++n[p.species];
        });
        return n;
    };
    const auto initial = perSpecies();
    std::map<uint8_t, size_t> highest = initial, lowest = initial;
    std::map<uint8_t, double> lastYear;  // summed over the third year
    int lastYearSamples = 0;
    double day = 1;
    for (; day <= 1100; day += 5) {
        settle(day);
        const auto now = perSpecies();
        for (const auto& [sp, n0] : initial) {
            const size_t n = now.count(sp) ? now.at(sp) : 0;
            highest[sp] = std::max(highest[sp], n);
            lowest[sp] = std::min(lowest[sp], n);
            if (day > 730) lastYear[sp] += double(n);
        }
        if (day > 730) ++lastYearSamples;
    }
    for (const auto& [sp, n0] : initial) {
        if (n0 < 10) continue;  // too few for statistics
        // Annuals rise and fall with their generations: judge the average.
        const double mean = lastYear[sp] / lastYearSamples;
        CHECK(mean > 0.5 * n0 && mean < 1.8 * n0);
        CHECK(highest[sp] < n0 * 2.2);  // no overgrowth
        CHECK(lowest[sp] > 0);          // never dies out
    }
    // No two plants of a species share a spot or a seed.
    f.chunks->forEachReady([&](const ChunkCoord& c, Chunk&) {
        const auto l = f.chunks->flora(c);
        if (!l) return;
        EQUAL(l->resolvedDay, int32_t(day - 5), "resolved to today");
        std::set<std::tuple<int, int, int, int>> spots;
        for (const auto& p : l->instances)
            CHECK(spots.insert({p.species, p.x, p.z, p.y}).second);
    });
    // Idempotent: the same day again plants nothing new.
    const auto settled = perSpecies();
    settle(day - 5 + 0.5);
    CHECK(perSpecies() == settled);
    day -= 5;

    // A long absence (over a year) re-samples plants at equilibrium: about
    // the generated population, with today's ages.
    day += 500;
    settle(day);
    const auto resampled = perSpecies();
    for (const auto& [sp, n0] : initial) {
        if (n0 < 10) continue;
        // A fresh sample of the same density: within sampling noise (40%,
        // or three standard deviations of a Poisson count when small).
        const double n = double(resampled.count(sp) ? resampled.at(sp) : 0);
        const double tolerance =
            std::max(0.4 * double(n0), 3 * std::sqrt(double(n0)));
        CHECK(std::abs(n - double(n0)) <= tolerance);
    }
    f.chunks->forEachReady([&](const ChunkCoord& c, Chunk&) {
        const auto l = f.chunks->flora(c);
        if (!l) return;
        EQUAL(l->resolvedDay, int32_t(day), "re-sampled today");
        for (const auto& p : l->instances) CHECK(p.birthDay <= day + 10);
    });
    (void)table;
    (void)center;
    (void)start;

    // Harvest: find a fruiting bush (scan days for one), pick its berries.
    const auto& species = f.assets.getStorage<Flora>();
    std::optional<TilePos> bushTile;
    const double searchEnd = day + 500;
    while (day < searchEnd && !bushTile) {
        day += 3;
        settle(day);
        f.chunks->forEachReady([&](const ChunkCoord& c, Chunk&) {
            const auto l = f.chunks->flora(c);
            if (!l || bushTile) return;
            for (const auto& p : l->instances) {
                const auto& sp = species.get(p.species);
                if (sp.forage.empty() || !sp.growth.regrow_after_harvest_days)
                    continue;
                if (flora_growth::at(sp, p.seed, day - p.birthDay).stage !=
                    FloraStage::Fruiting)
                    continue;
                const auto v = floraVolume(p, c, *table.find(p.species));
                bushTile = TilePos{int(v.minX), v.anchorY, int(v.minZ)};
                return;
            }
        });
    }
    CHECK(bushTile.has_value());
    const auto yields = sim.harvest(*bushTile, day);
    CHECK(!yields.empty());
    EQUAL(yields[0].item, std::string("items.red_berry"), "forage item");
    CHECK(yields[0].count >= 2 && yields[0].count <= 6);
    CHECK(sim.harvest(*bushTile, day).empty());  // already picked
    bool marked = false;
    for (const auto& info : sim.plantsAt(*bushTile, day))
        marked = marked || (info.plant.flags & FloraInstance::Harvested);
    CHECK(marked);

    // Ground cover regrows on exposed topsoil: smother some grass with a
    // block, take the block away, and wait.
    std::optional<TilePos> grass;
    for (const auto& c : surface) {
        f.chunks->withChunkLocked(c, [&](Chunk& chunk) {
            for (int z = 2; z < 14 && !grass; ++z)
                for (int x = 2; x < 14 && !grass; ++x)
                    for (int y = 0; y + 1 < 16 && !grass; ++y)
                        if (chunk.tiles[z][y][x].coverDensity() >= 4 &&
                            chunk.tiles[z][y + 1][x].isAir())
                            grass =
                                TilePos{int(c.x * 16 + x), int(c.y * 16 + y),
                                        int(c.z * 16 + z)};
        });
        if (grass) break;
    }
    CHECK(grass.has_value());
    const auto above = TilePos{grass->x, grass->y + 1, grass->z};
    const auto density =
        f.chunks->tileAt(grass->x, grass->y, grass->z)->coverDensity();
    CHECK(editor.placeBlock(above, 1) == EditStatus::Applied ||
          editor.placeBlock(above, 1) == EditStatus::Blocked);
    if (f.chunks->tileAt(above.x, above.y, above.z)->isSolid()) {
        CHECK(f.chunks->tileAt(grass->x, grass->y, grass->z)->cover == 0);
        editor.removeBlock(above);
        const_cast<Tile*>(f.chunks->tileAt(above.x, above.y, above.z))
            ->lightLevel.sun_exposure = 15;
        settle(day + 0.5);
        CHECK(f.chunks->tileAt(grass->x, grass->y, grass->z)->cover == 0);
        settle(day + 1.2);
        CHECK(f.chunks->tileAt(grass->x, grass->y, grass->z)->cover != 0);
        settle(day + 20);
        EQUAL(
            int(f.chunks->tileAt(grass->x, grass->y, grass->z)->coverDensity()),
            int(density), "grows back to the generated density");
        EQUAL(sim.coverChunks(), size_t(0), "nothing left to regrow");
    }
}

namespace {
// A rare perennial flower: about two plants per hectare, kept at its sites
// across epochs, reproducing through seeds.
Json moonflower(bool persistent = true, double density = 0.02,
                const char* name = "flora.test_moonflower") {
    auto j = Json::parse(R"({
        "kind": "flower",
        "footprint": {"radius_m": 0.15},
        "placement": {"climate": [{}], "max_slope": 0.8},
        "growth": {
            "stages": {"seedling": [3, 5], "young": [5, 10], "mature": [20, 30],
                       "flowering": [10, 20], "withering": [5, 8]},
            "perennial": true,
            "lifespan_days": [150, 250],
            "spread": {"radius_m": 4, "seeds": [2, 4], "germination": 0.7}
        },
        "parts": [
            {"type": "stem", "height_m": 0.3, "material": "foliage.small_leaf"},
            {"type": "petal_ring", "count": 6, "attach": "stem_tip",
             "material": "foliage.meadow_petal"}
        ]})");
    j["name"] = name;
    j["placement"]["density_per_100m2"] = density;
    j["placement"]["persistent_sites"] = persistent;
    return j;
}

// Plants of one species in chunk columns [x0, x1) x [z0, z1), generated
// with the flora seed salted by `salt` (cluster patches and persistent
// sites from the unsalted seed, as ChunkManager does).
std::vector<std::pair<double, double>> sites(const World& world,
                                             const IAssets& assets,
                                             const FloraSpeciesTable& table,
                                             uint8_t species, unsigned x0,
                                             unsigned x1, unsigned z0,
                                             unsigned z1, uint32_t salt) {
    std::vector<std::pair<double, double>> out;
    std::unordered_map<uint64_t, TerrainNeighborhood> captures;
    for (unsigned cz = z0; cz < z1; ++cz)
        for (unsigned cx = x0; cx < x1; ++cx) {
            const uint64_t key = uint64_t(cx / 8) << 32 | (cz / 8);
            auto it = captures.find(key);
            if (it == captures.end())
                it = captures
                         .emplace(key, TerrainNeighborhood::capture(
                                           world, {cx / 8, cz / 8}, assets))
                         .first;
            const auto columns = TerrainSampler(it->second).columns(cx, cz);
            int low = INT_MAX, high = INT_MIN;
            for (const auto& c : columns.columns) {
                low = std::min(low, c.surface_y);
                high = std::max(high, c.surface_y);
            }
            for (int y = low / 16; y <= high / 16; ++y) {
                const auto layer = generateFlora({&table,
                                                  &it->second,
                                                  &columns,
                                                  {cx, unsigned(y), cz},
                                                  99 ^ salt,
                                                  nullptr,
                                                  99});
                for (const auto& p : layer.instances)
                    if (p.species == species)
                        out.push_back(
                            {cx * 16 + p.localX(), cz * 16 + p.localZ()});
            }
        }
    return out;
}
}  // namespace

TEST(flora_rare_species) {
    // Only rare species, so generation over a wide area stays cheap.
    test::FloraAssets rareOnly;
    for (const auto* file : {"materials/rocks.json", "materials/soils.json",
                             "materials/vegetation.json"})
        rareOnly.loadContent<Material>(file);
    for (const auto* file : {"blocks/rocks.json", "blocks/soils.json"})
        rareOnly.loadContent<Block>(file);
    rareOnly.loadContent<Soil>("soils/soils.json");
    rareOnly.loadContent<GroundCover>("ground_covers/ground_covers.json");
    rareOnly.loadContent<FoliageMaterial>(
        "foliage_materials/foliage_materials.json");
    rareOnly.load<Flora>(Json::array(
        {moonflower(true), moonflower(false, 0.02, "flora.test_drifter")}));
    const FloraSpeciesTable table(rareOnly);
    const auto* rule = table.find(1);
    CHECK(rule && rule->persistentSites);
    // Cells as large as the density asks: ~70 m for 2 per hectare.
    CHECK(rule->cell >= 60 && rule->cell <= 80);
    const auto world = meadowWorld();
    // 40x24 chunks = 640x384 m = 24.6 ha: about 49 sites where the ground
    // allows (a pure function of the seed).
    const auto here = sites(world, rareOnly, table, 1, 4, 44, 8, 32, 0);
    CHECK(here.size() >= 15 && here.size() <= 80);
    // Persistent sites survive an epoch change; ordinary ones move.
    const auto salt = ChunkManager::floraEpochSalt(800);
    CHECK(salt != 0);
    CHECK(sites(world, rareOnly, table, 1, 4, 44, 8, 32, salt) == here);
    const auto drift = sites(world, rareOnly, table, 2, 4, 44, 8, 32, 0);
    CHECK(sites(world, rareOnly, table, 2, 4, 44, 8, 32, salt) != drift);
    // A lone plant can replace itself (crowding is measured over an area
    // holding about three plants at this density).
    CHECK(flora_growth::replacementRatio(rareOnly.getStorage<Flora>().get(1),
                                         5) > 2);
}

TEST(flora_rare_species_persists) {
    // A rare species in the streamed meadow: plant one by hand, then let
    // three years pass. Seeds keep it alive near where it was; crowding
    // keeps it rare.
    FloraStream f(Json::array({moonflower(true, 0.02)}));
    std::vector<ChunkCoord> all;
    for (unsigned cz = 16; cz < 21; ++cz)
        for (unsigned cx = 16; cx < 21; ++cx) all.push_back(f.load(cx, cz));
    f.prepareSky(all);
    FloraSimulation sim(*f.chunks, f.assets, f.world, *f.skyMap);
    const auto& species = f.assets.getStorage<Flora>();
    const auto rare = species.get(std::string("flora.test_moonflower")).id;
    // Plant one where a grass tuft grows (suitable ground), in the middle.
    const auto centre = f.chunks->flora(all[12]);
    CHECK(centre && !centre->instances.empty());
    auto layer = *centre;
    FloraInstance flower = layer.instances.front();
    flower.species = rare;
    flower.seed = 31337;
    flower.birthDay = -20;
    layer.instances.push_back(flower);
    f.chunks->setFlora(all[12], std::make_shared<const FloraLayer>(layer));
    const auto count = [&] {
        int n = 0;
        f.chunks->forEachReady([&](const ChunkCoord& c, Chunk&) {
            if (const auto l = f.chunks->flora(c))
                for (const auto& p : l->instances) n += p.species == rare;
        });
        return n;
    };
    const int start = count();
    CHECK(start >= 1);
    int lowest = start, highest = start;
    for (double day = 1; day <= 1100; day += 5) {
        for (int i = 0; i < 200; ++i) sim.update(day);
        lowest = std::min(lowest, count());
        highest = std::max(highest, count());
    }
    // Alive through several generations, and still rare: a few around the
    // original site at most.
    CHECK(lowest >= 1);
    CHECK(highest <= start + 8);
}
