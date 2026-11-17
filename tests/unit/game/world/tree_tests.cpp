#include <chrono>
#include <climits>
#include <cmath>
#include <map>
#include <set>

#include "flora_assets.hpp"
#include "world/chunk_manager.hpp"
#include "world/save/chunk_codec.hpp"
#include "world/save/region_file.hpp"
#include "world/save/trees_codec.hpp"
#include "world/terrain_edit_tracker.hpp"
#include "world/terrain_edits.hpp"
#include "world/tree_foliage_builder.hpp"
#include "world/tree_generator.hpp"
#include "world/tree_rasterizer.hpp"
#include "world/tree_simulation.hpp"
#include "world/tree_skeleton.hpp"
#include "world/tree_wood_lod.hpp"
#include "world/world_editor.hpp"
#include "world_fixture.hpp"

// Tree skeletons and wood rasterization (docs/handoff/trees.md, step 2).

namespace {
const test::FloraAssets& content() {
    static test::FloraAssets assets;
    static const bool loaded = (test::loadAllContent(assets), true);
    (void)loaded;
    return assets;
}
const Tree& species(const std::string& name) {
    const auto& storage = content().getStorage<Tree>();
    return storage.items[storage.indexByName.at(name)];
}
TreeSkeletonPtr build(const std::string& name, uint32_t seed = 7,
                      float headroom = TreeMaxHeight,
                      std::vector<TreeCut> cuts = {}) {
    TreeSkeletonInput input;
    input.seed = seed;
    input.headroom = headroom;
    input.cuts = std::move(cuts);
    return buildTreeSkeleton(species(name), input);
}
constexpr TreeAnchor Origin{1000, 400, 2000};

size_t countWood(const TreeSkeleton& k, const TreeTileBox& box, float age) {
    std::vector<uint8_t> mask;
    markTreeWood(k, Origin, box, age, mask);
    size_t n = 0;
    for (const auto v : mask) n += v;
    return n;
}
// One horizontal layer of tiles around the trunk, `height` tiles above
// the anchor tile.
TreeTileBox layer(int height, int half) {
    return {Origin.x - half,     Origin.y + height,     Origin.z - half,
            Origin.x + half + 1, Origin.y + height + 1, Origin.z + half + 1};
}
// Highest live wood or foliage point (metres above the anchor point).
float highest(const TreeSkeleton& k) {
    float top = -INFINITY;
    for (const auto& s : k.segments)
        if (s.live()) top = std::max({top, s.a.y + s.r0, s.b.y + s.r1});
    for (const auto& f : k.foliage)
        if (!f.pruned) top = std::max(top, f.position.y);
    return top;
}
// The live trunk segment (order 0, not a sprout) spanning height y.
int32_t trunkAt(const TreeSkeleton& k, float y) {
    for (size_t i = 0; i < k.segments.size(); ++i) {
        const auto& s = k.segments[i];
        if (s.live() && s.order == 0 && !(s.flags & TreeSegment::Sprout) &&
            std::min(s.a.y, s.b.y) <= y && std::max(s.a.y, s.b.y) > y)
            return int32_t(i);
    }
    return -1;
}
const std::vector<std::string> Ordinary{"trees.spruce", "trees.mountain_pine",
                                        "trees.oak", "trees.palm",
                                        "trees.redwood"};
}  // namespace

TEST(tree_skeleton_deterministic) {
    for (const auto* name : {"trees.spruce", "trees.mountain_pine", "trees.oak",
                             "trees.palm", "trees.redwood", "trees.elder"}) {
        const auto a = build(name, 11), b = build(name, 11),
                   c = build(name, 12);
        CHECK(!a->segments.empty());
        CHECK(!a->foliage.empty());
        EQUAL(a->segments.size(), b->segments.size(), name);
        bool same = a->foliage.size() == b->foliage.size();
        for (size_t i = 0; same && i < a->segments.size(); ++i)
            same = a->segments[i].a == b->segments[i].a &&
                   a->segments[i].b == b->segments[i].b &&
                   a->segments[i].r0 == b->segments[i].r0 &&
                   a->segments[i].birthB == b->segments[i].birthB;
        CHECK(same);
        // Another seed gives another tree.
        CHECK(a->segments.size() != c->segments.size() ||
              !(a->segments.back().b == c->segments.back().b));
        // Parents come before their children (pruning relies on it).
        for (size_t i = 0; i < a->segments.size(); ++i)
            CHECK(a->segments[i].parent < int32_t(i));
    }
}

TEST(tree_sizes_follow_species) {
    for (const auto& name : Ordinary) {
        const auto& tree = species(name);
        for (uint32_t seed = 1; seed <= 6; ++seed) {
            const auto k = build(name, seed);
            CHECK(tree.size.height_m.contains(k->height));
            CHECK(tree.size.trunk_radius_m.contains(k->trunkRadius));
            // The crown fills, and stays within, the sampled height and
            // radius (up to the clumps' own size).
            const float top = highest(*k);
            // Tips reach the height; their wood radius may poke above.
            CHECK(top <= k->height + 1);
            CHECK(top >= k->height * 0.8f);
            const float reach = std::max({-k->boundsMin.x, k->boundsMax.x,
                                          -k->boundsMin.z, k->boundsMax.z});
            CHECK(reach <= tree.extent() + 4);
            CHECK(reach >= k->crownRadius * 0.6f);
        }
    }
}

TEST(tree_growth_only_adds_wood) {
    for (const auto& name : Ordinary) {
        const auto k = build(name, 3);
        const auto box = treeWoodBounds(*k, Origin);
        const float m = k->maturity;
        std::vector<uint8_t> previous;
        markTreeWood(*k, Origin, box, 0, previous);
        size_t last = 0;
        for (const float f : {0.02f, 0.1f, 0.25f, 0.5f, 0.75f, 1.f, 1.5f}) {
            std::vector<uint8_t> mask;
            markTreeWood(*k, Origin, box, m * f, mask);
            size_t count = 0;
            for (size_t i = 0; i < mask.size(); ++i) {
                CHECK(!previous[i] || mask[i]);  // never loses wood
                count += mask[i];
            }
            CHECK(count >= last);
            last = count;
            previous = std::move(mask);
        }
        // The delta helper lists exactly the added tiles.
        size_t added = 0;
        forEachTreeGrowth(*k, Origin, box, m * 0.25f, m * 0.5f,
                          [&](int, int, int) { ++added; });
        EQUAL(added,
              countWood(*k, box, m * 0.5f) - countWood(*k, box, m * 0.25f),
              name);
        // Nothing grows after maturity.
        EQUAL(countWood(*k, box, k->matureAge()),
              countWood(*k, box, k->matureAge() * 3), name);
    }
}

TEST(tree_size_classes_in_blocks) {
    // Thin trees never form blocks.
    for (uint32_t seed = 1; seed <= 4; ++seed) {
        const auto k = build("trees.mountain_pine", seed);
        CHECK(treeWoodBounds(*k, Origin).empty());
    }
    // Trunk cross-sections at 4 tiles above the ground match the sampled
    // trunk radius (after taper), class by class.
    for (const auto& name :
         {"trees.spruce", "trees.palm", "trees.oak", "trees.redwood"}) {
        for (uint32_t seed = 1; seed <= 3; ++seed) {
            const auto k = build(name, seed);
            // Tile row 4 above the anchor tile has its centres at 3.5 m.
            const int32_t trunk = trunkAt(*k, 3.5f);
            CHECK(trunk >= 0);
            const auto& s = k->segments[size_t(trunk)];
            const float t = (3.5f - s.a.y) / (s.b.y - s.a.y);
            const float r = k->fullRadius(s, t, 0);
            const auto box = layer(4, int(r) + 3);
            const size_t tiles = countWood(*k, box, k->matureAge());
            const float equivalent = std::sqrt(float(tiles) / 3.14159f);
            // Near the block threshold the thicker wood just below may
            // reach this row; only compare clearly thick trunks.
            if (r < 0.65f) continue;
            CHECK(tiles >= 1);
            CHECK(equivalent >= r - 0.6f);
            CHECK(equivalent <= r * 1.15f + 0.8f);
        }
    }
    const auto spruce = build("trees.spruce", 1);
    CHECK(treeSizeClass(spruce->trunkRadius) == TreeSizeClass::Small);
    const auto oak = build("trees.oak", 1);
    CHECK(treeSizeClass(oak->trunkRadius) == TreeSizeClass::Medium);
    const auto redwood = build("trees.redwood", 1);
    CHECK(treeSizeClass(redwood->trunkRadius) == TreeSizeClass::Large);
    // A massive trunk spans several chunks: at 10 m the bole is far wider
    // than a chunk (16 tiles).
    const auto elder = build("trees.elder", 1);
    CHECK(treeSizeClass(elder->trunkRadius) == TreeSizeClass::Massive);
    const int half = int(elder->trunkRadius * 1.6f) + 2;
    const size_t tiles = countWood(*elder, layer(10, half), elder->matureAge());
    CHECK(std::sqrt(float(tiles) / 3.14159f) >= 16);
    // Giant limbs are walkable wood: some wood well outside the bole.
    const auto bounds = treeWoodBounds(*elder, Origin);
    CHECK(bounds.sizeX() > int(elder->trunkRadius * 3));
}

TEST(tree_height_cap) {
    for (const auto* name :
         {"trees.spruce", "trees.oak", "trees.palm", "trees.redwood"}) {
        const auto open = build(name, 5);
        for (const float headroom : {4.f, 9.f, 20.f}) {
            const auto k = build(name, 5, headroom);
            // Nothing rises above the ceiling, wood or foliage.
            CHECK(highest(*k) <= headroom + 0.01f);
            CHECK(k->boundsMax.y <= headroom + k->foliage.front().size);
            if (headroom < open->height) {
                // Tiles: wood stays below the ceiling tile.
                const auto box = treeWoodBounds(*k, Origin);
                std::vector<uint8_t> mask;
                markTreeWood(*k, Origin, box, k->matureAge(), mask);
                for (int y = box.y0; y < box.y1; ++y)
                    for (int z = box.z0; z < box.z1; ++z)
                        for (int x = box.x0; x < box.x1; ++x)
                            if (mask[box.index(x, y, z)])
                                CHECK(y < Origin.y + 1 + int(headroom));
                // Capped trees spread instead.
                CHECK(k->crownRadius >= open->crownRadius);
            }
        }
    }
    // A massive tree under a low sky still fits.
    const auto elder = build("trees.elder", 2, 60);
    CHECK(highest(*elder) <= 60.01f);
    // No room at all: no tree.
    CHECK(build("trees.oak", 1, 0.5f)->segments.empty());
}

TEST(tree_ground_pruning) {
    // A steep slope rising toward +x: branches ending inside it are dropped
    // (with everything they carry), the trunk stays.
    TreeSkeletonInput input;
    input.seed = 4;
    input.ground = [](float dx, float) { return dx * 6; };
    const auto k = buildTreeSkeleton(species("trees.oak"), input);
    const auto free = build("trees.oak", 4);
    size_t live = 0;
    for (const auto& s : k->segments) {
        if (!s.live()) continue;
        ++live;
        if (s.order > 0) CHECK(s.b.y >= s.b.x * 6 - 0.31f);
        if (s.parent >= 0) CHECK(k->segments[size_t(s.parent)].live());
    }
    size_t freeLive = 0;
    for (const auto& s : free->segments) freeLive += s.live();
    CHECK(live < freeLive);
    CHECK(trunkAt(*k, 1) >= 0);
    for (const auto& f : k->foliage)
        if (!f.pruned) CHECK(f.position.y >= f.position.x * 6 + 0.19f);
}

TEST(tree_cuts_and_regrowth) {
    const auto oak = build("trees.oak", 9);
    const float m = oak->maturity;
    // A cut through the bole at 2.5 m, made at maturity.
    const int32_t at = trunkAt(*oak, 2.5f);
    CHECK(at >= 0);
    const auto& s = oak->segments[size_t(at)];
    const float t = (2.5f - s.a.y) / (s.b.y - s.a.y);
    const TreeCut cut{at, t, m};
    const auto cutOak = build("trees.oak", 9, TreeMaxHeight, {cut});
    CHECK(!cutOak->dead);
    // Right after the cut nothing stands above it but the stump.
    const auto box = treeWoodBounds(*oak, Origin);
    std::vector<uint8_t> mask;
    markTreeWood(*cutOak, Origin, box, m + 1, mask);
    for (int y = Origin.y + 5; y < box.y1; ++y)
        for (int z = box.z0; z < box.z1; ++z)
            for (int x = box.x0; x < box.x1; ++x)
                CHECK(!mask[box.index(x, y, z)]);
    // The removed wood is the pruned selection of cut 1.
    std::vector<uint8_t> removed;
    markTreeWood(*cutOak, Origin, box, m + 1, removed, {1});
    size_t removedCount = 0;
    for (const auto v : removed) removedCount += v;
    CHECK(removedCount > 20);
    // The stump resprouts (oak) and grows a new crown from the cut.
    bool sprouts = false;
    for (const auto& seg : cutOak->segments)
        sprouts = sprouts || (seg.live() && (seg.flags & TreeSegment::Sprout));
    CHECK(sprouts);
    const float later = m + species("trees.oak").cut.resprout_days.max + m * 2;
    CHECK(countWood(*cutOak, layer(6, 12), later) > 0);
    CHECK(countWood(*cutOak, layer(6, 12), m + 1) == 0);
    // Cut foliage is gone; the sprout carries new foliage.
    size_t liveFoliage = 0;
    for (const auto& f : cutOak->foliage) liveFoliage += !f.pruned;
    CHECK(liveFoliage > 0);
    // Cuts are deterministic too.
    const auto again = build("trees.oak", 9, TreeMaxHeight, {cut});
    EQUAL(again->segments.size(), cutOak->segments.size(), "same cut tree");

    // At the root: oaks resprout from the stump, spruces die.
    const int32_t oakRoot = trunkAt(*oak, 0.5f);
    const auto& rs = oak->segments[size_t(oakRoot)];
    const auto stumpOak =
        build("trees.oak", 9, TreeMaxHeight,
              {{oakRoot, (0.5f - rs.a.y) / (rs.b.y - rs.a.y), m}});
    CHECK(!stumpOak->dead);
    const auto spruce = build("trees.spruce", 9);
    const int32_t spruceRoot = trunkAt(*spruce, 0.5f);
    const auto& ss = spruce->segments[size_t(spruceRoot)];
    const auto stumpSpruce = build(
        "trees.spruce", 9, TreeMaxHeight,
        {{spruceRoot, (0.5f - ss.a.y) / (ss.b.y - ss.a.y), spruce->maturity}});
    CHECK(stumpSpruce->dead);
    for (const auto& seg : stumpSpruce->segments)
        CHECK(!(seg.live() && (seg.flags & TreeSegment::Sprout)));
    // A spruce cut higher up regrows from the cut.
    const int32_t high = trunkAt(*spruce, 6);
    const auto& hs = spruce->segments[size_t(high)];
    const auto topped =
        build("trees.spruce", 9, TreeMaxHeight,
              {{high, (6 - hs.a.y) / (hs.b.y - hs.a.y), spruce->maturity}});
    CHECK(!topped->dead);
    bool regrows = false;
    for (const auto& seg : topped->segments)
        regrows = regrows || (seg.live() && (seg.flags & TreeSegment::Sprout));
    CHECK(regrows);
    // Regrowth respects the headroom too.
    const auto low = build("trees.spruce", 9, 12);
    const int32_t lowCut = trunkAt(*low, 6);
    const auto& ls = low->segments[size_t(lowCut)];
    const auto lowTopped =
        build("trees.spruce", 9, 12,
              {{lowCut, (6 - ls.a.y) / (ls.b.y - ls.a.y), low->maturity}});
    CHECK(highest(*lowTopped) <= 12.01f);
}

TEST(tree_skeleton_cache) {
    TreeSkeletonCache cache(2);
    int builds = 0;
    const auto make = [&] {
        ++builds;
        return build("trees.spruce", 1);
    };
    const auto a = cache.get(1, make);
    const auto b = cache.get(1, make);
    CHECK(a.get() == b.get());
    EQUAL(builds, 1, "cached");
    cache.get(2, make);
    cache.get(3, make);  // evicts key 1 (least recently used)
    EQUAL(cache.size(), size_t(2), "capacity");
    cache.get(1, make);
    EQUAL(builds, 4, "rebuilt after eviction");
}

// Not a correctness test: prints build and rasterization costs per species
// (run `world_logic_tests tree_costs`).
TEST(tree_costs) {
    using Clock = std::chrono::steady_clock;
    for (const auto* name : {"trees.spruce", "trees.mountain_pine", "trees.oak",
                             "trees.palm", "trees.redwood", "trees.elder"}) {
        const auto t0 = Clock::now();
        const auto k = build(name, 21);
        const auto t1 = Clock::now();
        // One chunk at the trunk, 2 chunks up.
        const TreeTileBox chunk{Origin.x - 8, Origin.y + 1,  Origin.z - 8,
                                Origin.x + 8, Origin.y + 17, Origin.z + 8};
        const size_t wood = countWood(*k, chunk, k->matureAge());
        const auto t2 = Clock::now();
        std::printf(
            "%s: %zu segments, %zu foliage, build %.2f ms, chunk raster %.2f "
            "ms (%zu wood)\n",
            name, k->segments.size(), k->foliage.size(),
            std::chrono::duration<double, std::milli>(t1 - t0).count(),
            std::chrono::duration<double, std::milli>(t2 - t1).count(), wood);
    }
}

// ---------------------------------------------------------------------------
// Generation (step 3): placement, chunk wood, saves and edit detection.

namespace {
// Humid temperate forest on plains: oaks, with redwood groves.
World forestWorld(int width = 6, int height = 5) {
    World world{};
    world.width = width;
    world.height = height;
    world.genSettings.width = width;
    world.genSettings.height = height;
    world.genSettings.height_seed = 4321;
    world.genSettings.feature_seed = 8765;
    world.regions.resize(size_t(width) * height);
    for (int z = 0; z < height; ++z)
        for (int x = 0; x < width; ++x) {
            auto& r = world.regions[size_t(z) * width + x];
            r = test::regionAt(SEA_LEVEL_Y + 24);
            r.coord = {unsigned(x), unsigned(z)};
            r.land = LandType::PLAINS;
            r.biome = BiomeType::TEMPERATE;
            r.vegetation = VegetationType::FOREST;
            r.humidity = .75f;
            r.temperature_c = 14;
        }
    return world;
}

struct TreeStream {
    test::FloraAssets assets;
    World world;
    LocationGenerator generator;
    ThreadPool pool{2};
    ChunkMesher mesher{assets};
    std::unique_ptr<ChunkManager> chunks;
    static constexpr unsigned Low = 20, High = 40;  // slabs loaded

    explicit TreeStream(World w = forestWorld(),
                        const Json& extraTrees = Json::array())
        : world(std::move(w)) {
        test::loadAllContent(assets);
        if (!extraTrees.empty()) assets.load<Tree>(extraTrees);
        chunks = std::make_unique<ChunkManager>(generator, pool, &mesher, world,
                                                &assets);
    }
    ~TreeStream() { pool.wait(); }
    // Loads slabs Low..High of column (cx, cz); stored: saved payloads by
    // slab (with plants and tree stamps when given).
    void load(unsigned cx, unsigned cz,
              const std::map<unsigned, ChunkBlob>& stored = {},
              const std::map<unsigned, ChunkBlob>& storedFlora = {}) {
        const auto& region = world.regions[(cz / 8) * world.width + cx / 8];
        const ChunkCoord origin{(cx / 8) * 8, 0, (cz / 8) * 8};
        if (!chunks->isRegistered({cx, 0, cz}))
            chunks->registerChunk({cx, 0, cz}, origin, &region);
        for (unsigned y = Low; y < High; ++y) {
            const auto s = stored.find(y);
            const auto f = storedFlora.find(y);
            chunks->registerChunk({cx, y, cz}, origin, &region,
                                  s == stored.end() ? nullptr : s->second,
                                  f == storedFlora.end() ? nullptr : f->second);
            chunks->request({cx, y, cz});
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(20);
        do {
            chunks->update();
            CHECK(pool.wait_for(std::chrono::seconds(5)));
            CHECK(std::chrono::steady_clock::now() < deadline);
        } while (chunks->hasGenerationWork());
    }
    size_t wood(unsigned cx, unsigned cz) {
        size_t n = 0;
        for (unsigned y = Low; y < High; ++y)
            if (const auto* c = chunks->getChunk({cx, y, cz}))
                for (int z = 0; z < 16; ++z)
                    for (int ty = 0; ty < 16; ++ty)
                        for (int x = 0; x < 16; ++x)
                            n += chunks->grownBlocks().test(
                                c->tiles[z][ty][x].block_id);
        return n;
    }
};

bool sameTiles(const Chunk& a, const Chunk& b) {
    for (int z = 0; z < 16; ++z)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                if (a.tiles[z][y][x].block_id != b.tiles[z][y][x].block_id)
                    return false;
    return true;
}

TreeSet regionSet(const World& world, const TreeSpeciesTable& table,
                  Vec2u region, uint32_t seed = 31) {
    const auto terrain = TerrainNeighborhood::capture(world, region, content());
    return generateRegionTrees({&table, &terrain, region, seed, nullptr});
}
}  // namespace

TEST(tree_region_sets) {
    const auto world = forestWorld();
    const TreeSpeciesTable table(content());
    CHECK(!table.empty());
    CHECK(table.maxExtent() <= TreeMaxReach);
    const auto a = regionSet(world, table, {2, 2});
    const auto b = regionSet(world, table, {3, 2});
    CHECK(a.size() > 10);
    // Deterministic.
    const auto again = regionSet(world, table, {2, 2});
    EQUAL(again.size(), a.size(), "same trees");
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i]->key == again[i]->key);
        CHECK(a[i]->anchor == again[i]->anchor);
        CHECK(a[i]->skeleton->segments.size() ==
              again[i]->skeleton->segments.size());
    }
    // Anchored in their region, on the ground, never inside it.
    const auto terrain = TerrainNeighborhood::capture(world, {2, 2}, content());
    const TerrainSampler sampler(terrain);
    bool redwood = false, oak = false;
    for (const auto& t : a) {
        CHECK(t->anchor.x >= 256 && t->anchor.x < 384);
        CHECK(t->anchor.z >= 256 && t->anchor.z < 384);
        CHECK(t->anchor.y <=
              sampler.sample(t->anchor.x, t->anchor.z).surface_y);
        CHECK(t->anchor.y >= SEA_LEVEL_Y);
        CHECK(t->birthDay <= 0);
        const auto& name = table.find(t->species)->species.name;
        redwood = redwood || name == "trees.redwood";
        oak = oak || name == "trees.oak";
    }
    CHECK(oak && redwood);
    // Spacing holds across the region border, decided independently.
    TreeSet both = a;
    both.insert(both.end(), b.begin(), b.end());
    for (size_t i = 0; i < both.size(); ++i)
        for (size_t j = i + 1; j < both.size(); ++j) {
            const auto& p = *both[i];
            const auto& q = *both[j];
            const double gap =
                std::max(table.find(p.species)->species.spacing_m.min,
                         table.find(q.species)->species.spacing_m.min);
            const double dx = p.anchor.x - q.anchor.x,
                         dz = p.anchor.z - q.anchor.z;
            CHECK(dx * dx + dz * dz >= gap * gap - 1e-6);
        }
}

TEST(tree_column_wraps_seam) {
    // One spruce anchored 2 tiles before the seam of a 512-tile world: the
    // first column sees it as the copy at x = -2.
    auto tree = std::make_shared<TreeInstance>();
    tree->key = 1;
    tree->species = species("trees.spruce").id;
    tree->anchor = {510, 400, 100};
    tree->skeleton = build("trees.spruce", 3);
    tree->wood = treeWoodBounds(*tree->skeleton, tree->anchor);
    tree->bounds = tree->wood;
    tree->bounds.x0 -= 4;
    tree->bounds.x1 += 4;
    tree->discRadius = 1.5;
    const TreeSet set{tree};
    const auto first = makeTreeColumn(0, 6, {&set}, 512);
    const auto last = makeTreeColumn(31, 6, {&set}, 512);
    EQUAL(first.trees.size(), size_t(1), "seen across the seam");
    EQUAL(last.trees.size(), size_t(1), "seen in its own column");
    EQUAL(first.trees[0].anchor.x, -2, "nearest copy");
    EQUAL(last.trees[0].anchor.x, 510, "canonical copy");
    // Wood tops are per column: only the trunk's column has block wood.
    CHECK(last.woodTop > 400);
    // Wood placed on both sides of the seam joins up: the trunk column is
    // in the last chunk (x 510), the chunk at x 0 sees the same tree.
    Chunk a, b;
    const ChunkCoord ca{31, 25, 6}, cb{0, 25, 6};
    const TreeSpeciesTable table(content());
    placeTreeWood(a, ca, last, table, 1e6);
    placeTreeWood(b, cb, first, table, 1e6);
    size_t trunk = 0;
    for (int z = 0; z < 16; ++z)
        for (int y = 0; y < 16; ++y) trunk += a.tiles[z][y][14].block_id != 0;
    CHECK(trunk > 0);
}

TEST(tree_chunks_order_independent) {
    // Columns on both sides of a region border, loaded in both orders.
    TreeStream ab, ba;
    ab.load(15, 18);
    ab.load(16, 18);
    ba.load(16, 18);
    ba.load(15, 18);
    for (const unsigned cx : {15u, 16u})
        for (unsigned y = TreeStream::Low; y < TreeStream::High; ++y) {
            const auto* p = ab.chunks->getChunk({cx, y, 18});
            const auto* q = ba.chunks->getChunk({cx, y, 18});
            CHECK(p && q);
            CHECK(sameTiles(*p, *q));
        }
}

TEST(tree_chunks_air_and_tops) {
    TreeStream f;
    size_t total = 0;
    for (unsigned cx = 16; cx < 20; ++cx)
        for (unsigned cz = 16; cz < 20; ++cz) {
            f.load(cx, cz);
            const auto column = f.chunks->treeColumn(cx, cz);
            CHECK(column != nullptr);
            const size_t wood = f.wood(cx, cz);
            total += wood;
            int highest = INT_MIN;
            for (unsigned y = TreeStream::Low; y < TreeStream::High; ++y) {
                const auto* c = f.chunks->getChunk({cx, y, cz});
                CHECK(c != nullptr);
                CHECK(f.chunks->treeStamp({cx, y, cz}).placed());
                for (int z = 0; z < 16; ++z)
                    for (int ty = 0; ty < 16; ++ty)
                        for (int x = 0; x < 16; ++x)
                            if (f.chunks->grownBlocks().test(
                                    c->tiles[z][ty][x].block_id))
                                highest = std::max(highest, int(y) * 16 + ty);
            }
            if (wood) {
                // Sunlight sees the wood: the column top covers it, and so
                // does the tree column's bound.
                CHECK(column->woodTop >= highest);
                CHECK(f.chunks->getColumnTopY(cx, cz) >= highest);
            }
        }
    CHECK(total > 100);
}

TEST(tree_saved_chunks_not_edits) {
    TreeStream f;
    // A column holding wood (most of a forest's columns do).
    unsigned cx = 16, cz = 16;
    for (bool found = false; !found;) {
        f.load(cx, cz);
        found = f.wood(cx, cz) > 0;
        if (!found && ++cx == 22) {
            cx = 16;
            CHECK(++cz < 22);
        }
    }
    const auto snapshot = f.chunks->snapshotRegion({2, 2}, true);
    const auto data = region_file::decode(
        region_file::encode(region_file::merge(RegionData{}, snapshot)));
    std::map<unsigned, ChunkBlob> stored, plants;
    for (unsigned y = TreeStream::Low; y < TreeStream::High; ++y) {
        const auto index = RegionData::localIndex({cx, y, cz});
        stored[y] = data.chunks.at(index);
        plants[y] = data.flora.at(index);
    }
    // Reloaded with their stamps: identical, not edits, not re-saved.
    {
        TreeStream g;
        g.load(cx, cz, stored, plants);
        for (unsigned y = TreeStream::Low; y < TreeStream::High; ++y) {
            CHECK(!g.chunks->isEdited({cx, y, cz}));
            CHECK(!g.chunks->isPersistDirty({cx, y, cz}));
            CHECK(sameTiles(*g.chunks->getChunk({cx, y, cz}),
                            *f.chunks->getChunk({cx, y, cz})));
        }
        // Distant terrain sees no structures in a forest.
        TerrainEdits edits;
        TerrainEditTracker tracker(*g.chunks, edits, true);
        tracker.markDirty(cx, cz);
        tracker.update();
        EQUAL(edits.size(), size_t(0), "trees are not edits");
    }
    // Saved before trees existed (no stamp, no wood): whole trees once.
    {
        std::map<unsigned, ChunkBlob> bare;
        for (unsigned y = TreeStream::Low; y < TreeStream::High; ++y) {
            Chunk chunk = *f.chunks->getChunk({cx, y, cz});
            for (int z = 0; z < 16; ++z)
                for (int ty = 0; ty < 16; ++ty)
                    for (int x = 0; x < 16; ++x)
                        if (f.chunks->grownBlocks().test(
                                chunk.tiles[z][ty][x].block_id))
                            chunk.tiles[z][ty][x] = {};
            bare[y] = std::make_shared<const Bytes>(chunk_codec::encode(chunk));
        }
        TreeStream g;
        g.load(cx, cz, bare);
        for (unsigned y = TreeStream::Low; y < TreeStream::High; ++y) {
            CHECK(g.chunks->treeStamp({cx, y, cz}).placed());
            CHECK(sameTiles(*g.chunks->getChunk({cx, y, cz}),
                            *f.chunks->getChunk({cx, y, cz})));
        }
        CHECK(g.wood(cx, cz) == f.wood(cx, cz));
    }
}

TEST(tree_flora_avoids_trunks) {
    TreeStream f;
    size_t plants = 0;
    for (unsigned cx = 16; cx < 19; ++cx)
        for (unsigned cz = 16; cz < 19; ++cz) {
            f.load(cx, cz);
            const auto column = f.chunks->treeColumn(cx, cz);
            for (unsigned y = TreeStream::Low; y < TreeStream::High; ++y) {
                const auto layer = f.chunks->flora({cx, y, cz});
                if (!layer) continue;
                for (const auto& p : layer->instances) {
                    ++plants;
                    const double x = cx * 16.0 + p.localX();
                    const double z = cz * 16.0 + p.localZ();
                    for (const auto& disc : column->discs)
                        CHECK(!disc.contains(x, z));
                }
            }
        }
    CHECK(plants > 0);
}

TEST(tree_landmarks) {
    // A denser copy of the elder, so a small map holds several.
    auto giant = Json::parse(R"({
        "name": "trees.test_giant", "form": "giant",
        "size": { "height_m": [100, 120], "trunk_radius_m": [24, 40],
                  "crown_radius_m": [90, 120] },
        "growth": { "maturity_days": 80000, "lifespan_days": 3000000 },
        "form_params": { "limbs": [5, 7], "root_flare": 0.5,
                         "branch_orders": 2 },
        "wood": { "block": "blocks.elder_wood", "bark": "foliage.bark_elder" },
        "foliage": { "type": "leaf_clump", "material": "foliage.elder_leaf",
                     "clump_size_m": 5, "density": 0.4 },
        "placement": {
            "climate": [ { "allowed_land": ["Plains"] } ],
            "density_per_100m2": 0.05,
            "spacing_m": 600, "persistent_sites": true, "priority": 100 } })");
    test::FloraAssets assets;
    test::loadAllContent(assets);
    assets.load<Tree>(Json::array({giant}));
    const TreeSpeciesTable table(assets);
    const auto world = forestWorld(16, 8);
    const auto sites = placeTreeLandmarks(table, world, 5);
    CHECK(sites.size() >= 3);
    const auto again = placeTreeLandmarks(table, world, 5);
    EQUAL(again.size(), sites.size(), "deterministic");
    for (size_t i = 0; i < sites.size(); ++i)
        for (size_t j = i + 1; j < sites.size(); ++j) {
            const double dx =
                double(WorldWrap::delta(sites[i].x, sites[j].x, 16 * 128));
            const double dz = double(sites[i].z - sites[j].z);
            CHECK(dx * dx + dz * dz >= 600.0 * 600.0);
        }
    // The region holding one builds it: a massive trunk spanning chunks,
    // and no ordinary tree inside its trunk.
    const auto& site = sites.front();
    const Vec2u region{unsigned(site.x / 128), unsigned(site.z / 128)};
    const auto terrain = TerrainNeighborhood::capture(world, region, assets);
    const auto set = generateRegionTrees({&table, &terrain, region, 5, &sites});
    const TreeInstance* landmark = nullptr;
    for (const auto& t : set)
        if (table.find(t->species)->landmark) landmark = t.get();
    CHECK(landmark != nullptr);
    CHECK(landmark->wood.sizeX() > 48);
    CHECK(landmark->birthDay < -80000);
    for (const auto& t : set) {
        if (t.get() == landmark) continue;
        const double dx = t->anchor.x - landmark->anchor.x;
        const double dz = t->anchor.z - landmark->anchor.z;
        CHECK(std::sqrt(dx * dx + dz * dz) > landmark->discRadius);
    }
}

// ---------------------------------------------------------------------------
// Crowns (step 4): shared shapes and foliage meshes.

TEST(tree_rotation_exact) {
    // A quarter turn of a shape rasterizes as the same tiles turned, so a
    // shared crown mesh drawn with that yaw matches the wood.
    const auto& oak = species("trees.oak");
    TreeSkeletonInput input;
    input.seed = 17;
    const auto base = buildTreeSkeleton(oak, input);
    const TreeAnchor origin{0, 0, 0};
    std::vector<uint8_t> mask;
    const auto box = treeWoodBounds(*base, origin);
    markTreeWood(*base, origin, box, base->matureAge(), mask);
    std::set<std::tuple<int, int, int>> plain;
    for (int y = box.y0; y < box.y1; ++y)
        for (int z = box.z0; z < box.z1; ++z)
            for (int x = box.x0; x < box.x1; ++x)
                if (mask[box.index(x, y, z)]) plain.insert({x, y, z});
    CHECK(plain.size() > 50);
    for (int k = 1; k < 4; ++k) {
        input.rotation = k;
        const auto turned = buildTreeSkeleton(oak, input);
        const auto tbox = treeWoodBounds(*turned, origin);
        markTreeWood(*turned, origin, tbox, turned->matureAge(), mask);
        std::set<std::tuple<int, int, int>> got;
        for (int y = tbox.y0; y < tbox.y1; ++y)
            for (int z = tbox.z0; z < tbox.z1; ++z)
                for (int x = tbox.x0; x < tbox.x1; ++x)
                    if (mask[tbox.index(x, y, z)]) got.insert({x, y, z});
        // x' = c x + s z, z' = -s x + c z (the foliage shader's yaw).
        const int c = k == 2 ? -1 : 0, s = k == 1 ? 1 : k == 3 ? -1 : 0;
        std::set<std::tuple<int, int, int>> expected;
        for (const auto& [x, y, z] : plain)
            expected.insert({c * x + s * z, y, -s * x + c * z});
        CHECK(got == expected);
    }
    // Generated ordinary trees mostly share shapes; landmarks never do.
    const auto world = forestWorld();
    const TreeSpeciesTable table(content());
    const auto set = regionSet(world, table, {2, 2});
    size_t shared = 0;
    for (const auto& t : set) {
        if (t->variant >= 0) {
            ++shared;
            CHECK(t->skeleton ==
                  table.variant(t->species, t->variant, t->rotation));
        }
    }
    CHECK(shared * 10 >= set.size() * 8);
}

TEST(tree_foliage_meshes) {
    const auto& materials = content().getStorage<FoliageMaterial>();
    for (const auto* name : {"trees.spruce", "trees.mountain_pine", "trees.oak",
                             "trees.palm", "trees.redwood", "trees.elder"}) {
        const auto& tree = species(name);
        const auto k = build(name, 5);
        const float mature = k->matureAge();
        const auto near = tree_foliage::build(
            *k, tree, materials, mature, tree_foliage::Detail::Near, false);
        const auto far = tree_foliage::build(*k, tree, materials, mature,
                                             tree_foliage::Detail::Far, false);
        const auto again = tree_foliage::build(
            *k, tree, materials, mature, tree_foliage::Detail::Near, false);
        CHECK(!near.empty());
        CHECK(!far.empty());
        CHECK(far.vertices.size() < near.vertices.size());
        // Distant: few large clumps and the wood as bark prisms.
        const auto distant = tree_foliage::build(
            *k, tree, materials, mature, tree_foliage::Detail::Distant, false);
        CHECK(!distant.empty());
        CHECK(distant.vertices.size() <= 65535);
        size_t distantLeaves = 0, nearLeaves = 0;
        for (const auto& v : distant.vertices) distantLeaves += v.layer >= 0;
        for (const auto& v : near.vertices) nearLeaves += v.layer >= 0;
        CHECK(distantLeaves < nearLeaves);
        CHECK(near.vertices.size() <= 65535);
        CHECK(near.indices.size() % 3 == 0);
        for (const auto i : near.indices) CHECK(i < near.vertices.size());
        EQUAL(again.vertices.size(), near.vertices.size(), name);
        // Leaves are textured, bark untextured; the wind weight is 0..1.
        bool leaves = false;
        for (const auto& v : near.vertices) {
            leaves = leaves || v.layer >= 0;
            CHECK(v.height >= 0 && v.height <= 1);
        }
        CHECK(leaves);
        // Within the tree's bounds (plus a clump).
        CHECK(near.height <= k->boundsMax.y + 4);
        // A sapling has a smaller crown; a dead tree has no leaves.
        const auto young = tree_foliage::build(
            *k, tree, materials, tree_foliage::bucketAge(3, mature),
            tree_foliage::Detail::Near, false);
        CHECK(young.vertices.size() < near.vertices.size());
        const auto dead = tree_foliage::build(*k, tree, materials, mature,
                                              tree_foliage::Detail::Near, true);
        for (const auto& v : dead.vertices) CHECK(v.layer < 0);
    }
    // Buckets: 32 growth steps, then one mature mesh.
    EQUAL(tree_foliage::bucket(0, 100), 0, "first bucket");
    EQUAL(tree_foliage::bucket(50, 100), 16, "middle bucket");
    EQUAL(tree_foliage::bucket(100, 100), tree_foliage::Buckets, "mature");
    EQUAL(tree_foliage::bucket(1e9f, 100), tree_foliage::Buckets, "old");
    CHECK(tree_foliage::bucketAge(tree_foliage::Buckets, 100) == 100);
}

// ---------------------------------------------------------------------------
// Growth, overrides and cutting (steps 5 and 6).

TEST(trees_codec_roundtrip) {
    TreeOverrideMap map;
    TreeRecord cut;
    cut.key = 1234;
    cut.cuts = {{12, 0.25f, 400}, {40, 0.5f, 900}};
    map[cut.key] = cut;
    TreeRecord planted;
    planted.anchor = {500, 410, 77};
    planted.key = plantedTreeKey(planted.anchor);
    planted.species = 3;
    planted.seed = 99;
    planted.birthDay = 12;
    planted.headroom = 5;
    map[planted.key] = planted;
    CHECK(planted.planted() && !cut.planted());
    const auto bytes = trees_codec::encode(map);
    const auto file = trees_codec::decode(bytes);
    CHECK(file.trees == map);
    CHECK(trees_codec::decode(trees_codec::encode({})).trees.empty());
    // Malformed sections are refused.
    const auto rejects = [](Bytes data, const char* what) {
        SectionContainer container;
        container.put("TREE", 1, std::move(data));
        test::expectThrows([&] { trees_codec::decode(container.encode()); },
                           what);
    };
    {
        BinaryWriter w;
        w.write(uint32_t(1));
        rejects(w.take(), "truncated record");
    }
    {
        BinaryWriter w;
        w.write(uint32_t(1));
        w.write(uint64_t(5));
        w.write(uint8_t(0));
        w.write(uint32_t(0));
        for (const int v : {1, 2, 3, 0}) w.write(int32_t(v));
        w.write(1.f);
        w.write(uint16_t(1));
        w.write(int32_t(3));
        w.write(2.f);  // t outside [0, 1]
        w.write(1.f);
        rejects(w.take(), "bad cut");
    }
    {
        SectionContainer container;
        container.put("TREE", 7, Bytes{});
        test::expectThrows([&] { trees_codec::decode(container.encode()); },
                           "version");
    }
}

TEST(tree_cuts_incremental) {
    // A tree cut twice while the game runs equals one rebuilt with both cuts
    // (what a region job does from trees.dat), turned or not.
    const auto& oak = species("trees.oak");
    for (const int rotation : {0, 1}) {
        TreeSkeletonInput input;
        input.seed = 23;
        input.rotation = rotation;
        const auto base = buildTreeSkeleton(oak, input);
        const int32_t a = trunkAt(*base, 2.5f);
        CHECK(a >= 0);
        const auto& s = base->segments[size_t(a)];
        std::vector<TreeCut> cuts{
            {a, (2.5f - s.a.y) / (s.b.y - s.a.y), base->maturity}};
        const auto once = applyTreeCuts(*base, oak, cuts);
        // Cut the sprout later.
        int32_t sprout = -1;
        for (size_t i = 0; i < once->segments.size(); ++i)
            if (once->segments[i].live() &&
                (once->segments[i].flags & TreeSegment::Sprout))
                sprout = int32_t(i);
        CHECK(sprout >= 0);
        cuts.push_back({sprout, 0.5f, base->maturity * 3});
        const auto twice = applyTreeCuts(*once, oak, cuts, 1);
        const auto all = applyTreeCuts(*base, oak, cuts);
        input.cuts = cuts;
        const auto built = buildTreeSkeleton(oak, input);
        for (const auto* k : {all.get(), built.get()}) {
            EQUAL(k->segments.size(), twice->segments.size(), "segments");
            for (size_t i = 0; i < k->segments.size(); ++i) {
                CHECK(k->segments[i].a == twice->segments[i].a);
                CHECK(k->segments[i].b == twice->segments[i].b);
                CHECK(k->segments[i].flags == twice->segments[i].flags);
            }
        }
    }
}

namespace {
// The column of a generated tree of `name` with block wood, anchored inside
// its column, among the loaded columns 16..21.
std::optional<TreeColumn::Entry> findTree(TreeStream& f,
                                          const std::string& name, unsigned& cx,
                                          unsigned& cz) {
    for (cz = 16; cz < 22; ++cz)
        for (cx = 16; cx < 22; ++cx) {
            f.load(cx, cz);
            const auto column = f.chunks->treeColumn(cx, cz);
            for (const auto& e : column->trees) {
                const auto* rule =
                    f.chunks->treeSpecies().find(e.tree->species);
                if (rule->species.name != name || e.tree->wood.empty())
                    continue;
                if (unsigned(e.tree->anchor.x / 16) != cx ||
                    unsigned(e.tree->anchor.z / 16) != cz)
                    continue;
                if (e.tree->age(0) < e.tree->skeleton->matureAge()) continue;
                return e;
            }
        }
    return std::nullopt;
}
size_t woodNear(TreeStream& f, const TreeAnchor& a, int y0, int y1, int r,
                uint8_t block) {
    size_t n = 0;
    for (int y = y0; y < y1; ++y)
        for (int z = a.z - r; z <= a.z + r; ++z)
            for (int x = a.x - r; x <= a.x + r; ++x)
                if (const Tile* t = f.chunks->tileAt(x, y, z);
                    t && t->block_id == block)
                    ++n;
    return n;
}
// Runs the growth sweep until every loaded tree chunk has been visited.
void sweepAll(TreeSimulation& sim, double day) {
    for (int i = 0; i < 400; ++i) sim.update(day);
}
}  // namespace

TEST(tree_cut_and_regrowth) {
    TreeStream f;
    unsigned cx = 0, cz = 0;
    const auto found = findTree(f, "trees.oak", cx, cz);
    CHECK(found.has_value());
    // Load the columns around it so its crown is loaded too.
    for (unsigned z = cz - 1; z <= cz + 1; ++z)
        for (unsigned x = cx - 1; x <= cx + 1; ++x) f.load(x, z);
    WorldEditor editor(*f.chunks, nullptr, f.assets.getStorage<Block>());
    TreeSimulation sim(*f.chunks, editor);
    sim.update(0);
    const auto& tree = *found->tree;
    const auto a = found->anchor;
    const uint8_t wood = f.chunks->treeSpecies().find(tree.species)->woodBlock;
    const int cutY = a.y + 3;
    CHECK(woodNear(f, a, cutY + 2, cutY + 6, 4, wood) > 0);
    // Saw through the trunk at one height: remove every wood tile there.
    for (int z = a.z - 6; z <= a.z + 6; ++z)
        for (int x = a.x - 6; x <= a.x + 6; ++x)
            if (const Tile* t = f.chunks->tileAt(x, cutY, z);
                t && t->block_id == wood)
                editor.removeBlock({x, cutY, z});
    EQUAL(sim.cutCount(), size_t(1), "one cut");
    CHECK(sim.dirty());
    const auto record = f.chunks->treeOverrides()->find(tree.key);
    CHECK(record != f.chunks->treeOverrides()->end());
    EQUAL(record->second.cuts.size(), size_t(1), "recorded");
    // The trunk above the cut is gone (no physics: it disappears).
    EQUAL(woodNear(f, a, cutY + 1, cutY + 8, 3, wood), size_t(0),
          "nothing above the cut");
    // The tree regrows from the cut (oaks resprout) over the following
    // years, through the growth sweep.
    sweepAll(sim, 6000);
    CHECK(woodNear(f, a, cutY + 1, cutY + 8, 3, wood) > 0);

    // Saved chunks with the cut reload the same way, rebuilt from trees.dat.
    const auto snapshot = f.chunks->snapshotRegion({cx / 8, cz / 8}, true);
    const auto data = region_file::decode(
        region_file::encode(region_file::merge(RegionData{}, snapshot)));
    TreeStream g;
    g.chunks->setTreeOverrides(f.chunks->treeOverrides());
    g.chunks->setFloraDay(6000);
    std::map<unsigned, ChunkBlob> stored, plants;
    for (unsigned y = TreeStream::Low; y < TreeStream::High; ++y) {
        const auto index = RegionData::localIndex({cx, y, cz});
        if (data.chunks.contains(index)) stored[y] = data.chunks.at(index);
        if (data.flora.contains(index)) plants[y] = data.flora.at(index);
    }
    g.load(cx, cz, stored, plants);
    for (unsigned y = TreeStream::Low; y < TreeStream::High; ++y)
        CHECK(sameTiles(*g.chunks->getChunk({cx, y, cz}),
                        *f.chunks->getChunk({cx, y, cz})));
}

TEST(tree_reload_catches_up) {
    // A sapling planted on day 0 in a saved chunk: reloading on day 1500
    // grows its wood into the stored tiles, and a hole the player made in
    // older wood stays.
    TreeStream f;
    const unsigned cx = 17, cz = 17;
    f.load(cx, cz);
    WorldEditor editor(*f.chunks, nullptr, f.assets.getStorage<Block>());
    TreeSimulation sim(*f.chunks, editor);
    // An open ground tile near the column centre.
    const int x = int(cx) * 16 + 8, z = int(cz) * 16 + 8;
    int y = WORLD_HEIGHT_Y - 1;
    while (y > 0 &&
           !(f.chunks->tileAt(x, y, z) && f.chunks->tileAt(x, y, z)->isSolid()))
        --y;
    CHECK(sim.plant({x, y, z}, species("trees.spruce").id, 0));
    const auto snapshot = f.chunks->snapshotRegion({2, 2}, true);
    const auto data = region_file::decode(
        region_file::encode(region_file::merge(RegionData{}, snapshot)));
    std::map<unsigned, ChunkBlob> stored, plants;
    for (unsigned sy = TreeStream::Low; sy < TreeStream::High; ++sy) {
        const auto index = RegionData::localIndex({cx, sy, cz});
        stored[sy] = data.chunks.at(index);
        plants[sy] = data.flora.at(index);
    }
    TreeStream g;
    g.chunks->setTreeOverrides(f.chunks->treeOverrides());
    g.chunks->setFloraDay(1500);
    g.load(cx, cz, stored, plants);
    const uint8_t wood =
        g.chunks->treeSpecies().find(species("trees.spruce").id)->woodBlock;
    const TreeAnchor anchor{x, y, z};
    CHECK(woodNear(g, anchor, y + 1, y + 6, 1, wood) > 0);
    CHECK(g.chunks->treeStamp({cx, unsigned((y + 1) / 16), cz}).day == 1500);
}

TEST(tree_planted_indoors) {
    // A ceiling 5 tiles above the ground: the sapling never grows into it.
    TreeStream f;
    const unsigned cx = 18, cz = 18;
    f.load(cx, cz);
    WorldEditor editor(*f.chunks, nullptr, f.assets.getStorage<Block>());
    TreeSimulation sim(*f.chunks, editor);
    const int x = int(cx) * 16 + 7, z = int(cz) * 16 + 7;
    int y = WORLD_HEIGHT_Y - 1;
    while (y > 0 &&
           !(f.chunks->tileAt(x, y, z) && f.chunks->tileAt(x, y, z)->isSolid()))
        --y;
    const int ceiling = y + 6;
    for (int dz = -4; dz <= 4; ++dz)
        for (int dx = -4; dx <= 4; ++dx)
            editor.placeBlock({x + dx, ceiling, z + dz}, 1);
    CHECK(sim.plant({x, y, z}, species("trees.oak").id, 0));
    const auto record =
        f.chunks->treeOverrides()->at(plantedTreeKey({x, y, z}));
    EQUAL(record.headroom, 5.f, "headroom under the ceiling");
    sweepAll(sim, 20000);
    const uint8_t wood =
        f.chunks->treeSpecies().find(species("trees.oak").id)->woodBlock;
    CHECK(woodNear(f, {x, y, z}, y + 1, ceiling, 3, wood) > 0);
    // Its own wood (generated trees of the forest may stand above).
    const auto column = f.chunks->treeColumn(cx, cz);
    const TreeInstance* planted = nullptr;
    for (const auto& e : column->trees)
        if (e.tree->key == plantedTreeKey({x, y, z})) planted = e.tree.get();
    CHECK(planted != nullptr);
    CHECK(planted->skeleton->boundsMax.y <= 6);  // clumps may pad a little
    CHECK(planted->wood.y1 <= ceiling);
    std::vector<uint8_t> mask;
    markTreeWood(*planted->skeleton, planted->anchor, planted->wood,
                 planted->age(20000), mask);
    size_t own = 0;
    for (const auto v : mask) own += v;
    CHECK(own > 0);
    CHECK(sim.describe({x, y + 1, z}, 20000).find("planted") !=
          std::string::npos);
}

// ---------------------------------------------------------------------------
// Far billboards.

TEST(tree_billboards) {
    // Placement only, yet the same trees as full generation: the hand-over
    // from distant meshes to billboards keeps every tree in place.
    const auto world = forestWorld();
    const TreeSpeciesTable table(content());
    const auto terrain = TerrainNeighborhood::capture(world, {2, 2}, content());
    const auto full = generateRegionTrees({&table, &terrain, {2, 2}, 31});
    const auto far = regionBillboards({&table, &terrain, {2, 2}, 31});
    std::set<std::tuple<float, float, float>> expected, got;
    for (const auto& t : full)
        if (!table.find(t->species)->landmark)
            expected.insert({t->anchor.x + 0.5f, float(t->anchor.y + 1),
                             t->anchor.z + 0.5f});
    for (const auto& b : far) got.insert({b.x, b.y, b.z});
    CHECK(!got.empty());
    CHECK(got == expected);
    // Sorted by hash: any prefix is an even thinning.
    for (size_t i = 1; i < far.size(); ++i)
        CHECK(far[i - 1].hash <= far[i].hash);
    for (const auto& b : far) {
        CHECK(b.height >= 1 && b.crown >= 1);
        CHECK(b.maturity > 0 && b.lifespan >= b.maturity);
    }
    // A billboard mesh for every species: crown cards textured, trunk not.
    const auto& materials = content().getStorage<FoliageMaterial>();
    for (const auto& rule : table.species()) {
        const auto shape = table.variant(rule.id, 0, 0);
        const auto mesh =
            tree_foliage::buildBillboard(*shape, rule.species, materials);
        CHECK(!mesh.empty());
        bool card = false, trunk = false;
        for (const auto& v : mesh.vertices) {
            card = card || v.layer >= 0;
            trunk = trunk || v.layer < 0;
            CHECK(v.x >= -1.01f && v.x <= 1.01f && v.y >= -0.6f && v.y <= 1.6f);
        }
        CHECK(card && trunk);
    }
}

// ---------------------------------------------------------------------------
// Distant voxel wood of landmarks (StructureLod through tree_wood_lod).

TEST(tree_wood_lod_slabs) {
    // A big trunk standing on the X seam: its slabs wrap to canonical keys.
    constexpr int Period = 16 * 128;
    auto instance = std::make_shared<TreeInstance>();
    instance->skeleton = build("trees.redwood");
    instance->anchor = {3, 300, 700};
    instance->wood = treeWoodBounds(*instance->skeleton, instance->anchor);
    CHECK(instance->wood.x0 < 0);
    const float age = instance->skeleton->matureAge();
    const TreeWoodSource source{instance, age, 9};
    std::vector<uint8_t> mask;
    markTreeWood(*instance->skeleton, instance->anchor, instance->wood, age,
                 mask);
    const auto& box = instance->wood;
    // Level 0: exactly the rasterizer's wood.
    const auto full = tree_wood_lod::rasterize(source, 0, Period);
    size_t wood = 0, got = 0;
    for (int y = box.y0; y < box.y1; ++y)
        for (int z = box.z0; z < box.z1; ++z)
            for (int x = box.x0; x < box.x1; ++x) {
                if (!mask[box.index(x, y, z)]) continue;
                ++wood;
                const int wx = int(WorldWrap::mod(x, Period));
                const auto it = full->find(ChunkCoord{
                    unsigned(wx / 16), unsigned(y / 16), unsigned(z / 16)});
                CHECK(it != full->end());
                if (it != full->end() &&
                    it->second.bits.test(
                        size_t(((z % 16) * 16 + y % 16) * 16 + wx % 16)))
                    ++got;
            }
    CHECK(wood > 1000);
    EQUAL(got, wood, "every wood tile in its slab");
    size_t bits = 0;
    for (const auto& [coord, slab] : *full) {
        CHECK(coord.x < unsigned(Period / 16));
        EQUAL(tree_wood_lod::levelOf(coord), 0, "level 0 key");
        EQUAL(int(slab.block), 9, "wood block");
        bits += slab.bits.count();
        CHECK(!slab.enclosed || slab.bits.all());
    }
    EQUAL(bits, wood, "no extra wood");
    // Level 2: cells of 4 tiles, wood when at least 16 of their 64 are.
    const auto coarse = tree_wood_lod::rasterize(source, 2, Period);
    CHECK(!coarse->empty());
    size_t cells = 0;
    for (const auto& [key, slab] : *coarse) {
        EQUAL(tree_wood_lod::levelOf(key), 2, "level 2 key");
        const auto base = tree_wood_lod::baseOf(key);
        CHECK(base.x < unsigned(Period / 64));
        for (size_t i = 0; i < slab.bits.size(); ++i) {
            if (!slab.bits.test(i)) continue;
            ++cells;
            const int cx = int(i % 16), cy = int(i / 16 % 16),
                      cz = int(i / 256);
            int count = 0;
            for (int dz = 0; dz < 4; ++dz)
                for (int dy = 0; dy < 4; ++dy)
                    for (int dx = 0; dx < 4; ++dx) {
                        // The canonical cell's copy around the tree.
                        int x = (int(base.x) * 16 + cx) * 4 + dx;
                        if (x >= box.x1) x -= Period;
                        const int y = (int(base.y) * 16 + cy) * 4 + dy,
                                  z = (int(base.z) * 16 + cz) * 4 + dz;
                        if (x >= box.x0 && x < box.x1 && y >= box.y0 &&
                            y < box.y1 && z >= box.z0 && z < box.z1)
                            count += mask[box.index(x, y, z)];
                    }
            CHECK(count >= 16);
        }
    }
    CHECK(cells > 0 && cells * 16 <= wood);
}
