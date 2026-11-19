#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <fstream>
#include <map>
#include <set>

#include "flora_assets.hpp"
#include "world/chunk_manager.hpp"
#include "world/save/chunk_codec.hpp"
#include "world/save/region_file.hpp"
#include "world/save/trees_codec.hpp"
#include "world/structure_voxels.hpp"
#include "world/sunlight_propagator.hpp"
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
// The shipped oak and palm are thin (mesh-only) trees. Tests of chopping,
// cuts and regrowth use "trees.test_oak": the shipped oak with a Medium,
// voxel trunk and its own living wood block.
void loadTreeContent(test::FloraAssets& assets) {
    test::loadAllContent(assets);
    std::ifstream blocks(std::string(SOIL_CONTENT_DIR) + "/blocks/trees.json");
    for (auto block : Json::parse(blocks))
        if (block.at("name") == "blocks.oak_wood") {
            block["name"] = "blocks.test_oak_wood";
            assets.load<Block>(Json::array({block}));
        }
    std::ifstream trees(std::string(SOIL_CONTENT_DIR) + "/trees/trees.json");
    for (auto tree : Json::parse(trees))
        if (tree.at("name") == "trees.oak") {
            tree["name"] = "trees.test_oak";
            tree["size"]["trunk_radius_m"] = Json::array({1.6, 2.6});
            tree["wood"]["block"] = "blocks.test_oak_wood";
            assets.load<Tree>(Json::array({tree}));
        }
}
const test::FloraAssets& content() {
    static test::FloraAssets assets;
    static const bool loaded = (loadTreeContent(assets), true);
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

TEST(tree_trunk_foliage_on_stem) {
    // Fronds and juvenile leaves growing on the trunk stay on it after the
    // crown is fitted to its size: young palms' fronds used to float off a
    // leaning stem.
    for (const auto* name :
         {"trees.palm", "trees.oak", "trees.spruce", "trees.mountain_pine"})
        for (uint32_t seed = 1; seed <= 12; ++seed) {
            const auto k = build(name, seed);
            for (const auto& f : k->foliage) {
                if (f.segment < 0) continue;
                const auto& s = k->segments[size_t(f.segment)];
                if (s.order > 0) continue;
                const TreeVec axis = s.b - s.a;
                const float t = std::clamp(
                    (f.position - s.a).dot(axis) / axis.dot(axis), 0.f, 1.f);
                const TreeVec d = f.position - lerp(s.a, s.b, t);
                CHECK(std::hypot(d.x, d.z) < 0.35f);
            }
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
    const auto oak = build("trees.test_oak", 1);
    CHECK(treeSizeClass(oak->trunkRadius) == TreeSizeClass::Medium);
    // The shipped oak and palm are thin: crowns and stems are meshes, with
    // no wood block anywhere, root flare included.
    for (const auto* name : {"trees.oak", "trees.palm"})
        for (uint32_t seed = 1; seed <= 6; ++seed) {
            const auto k = build(name, seed);
            CHECK(treeSizeClass(k->trunkRadius) == TreeSizeClass::Thin);
            CHECK(treeWoodBounds(*k, Origin).empty());
        }
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
    const auto oak = build("trees.test_oak", 9);
    const float m = oak->maturity;
    // A cut through the bole at 2.5 m, made at maturity.
    const int32_t at = trunkAt(*oak, 2.5f);
    CHECK(at >= 0);
    const auto& s = oak->segments[size_t(at)];
    const float t = (2.5f - s.a.y) / (s.b.y - s.a.y);
    const TreeCut cut{at, t, m};
    const auto cutOak = build("trees.test_oak", 9, TreeMaxHeight, {cut});
    CHECK(!cutOak->dead);
    // Right after the cut nothing stands above it but the stump: the cut
    // is flat (tile row 4 has its centres at 3.5 m, a metre above it).
    const auto box = treeWoodBounds(*oak, Origin);
    std::vector<uint8_t> mask;
    markTreeWood(*cutOak, Origin, box, m + 1, mask);
    for (int y = Origin.y + 4; y < box.y1; ++y)
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
    const float later =
        m + species("trees.test_oak").cut.resprout_days.max + m * 2;
    CHECK(countWood(*cutOak, layer(6, 12), later) > 0);
    CHECK(countWood(*cutOak, layer(6, 12), m + 1) == 0);
    // Cut foliage is gone; the sprout carries new foliage.
    size_t liveFoliage = 0;
    for (const auto& f : cutOak->foliage) liveFoliage += !f.pruned;
    CHECK(liveFoliage > 0);
    // Cuts are deterministic too.
    const auto again = build("trees.test_oak", 9, TreeMaxHeight, {cut});
    EQUAL(again->segments.size(), cutOak->segments.size(), "same cut tree");

    // At the root: oaks resprout from the stump, spruces die.
    const int32_t oakRoot = trunkAt(*oak, 0.5f);
    const auto& rs = oak->segments[size_t(oakRoot)];
    const auto stumpOak =
        build("trees.test_oak", 9, TreeMaxHeight,
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

TEST(tree_cut_flat_on_thick_trunks) {
    // A horizontal cut through a redwood bole leaves no wood above the cut
    // plane: the rounded ends of the trunk segments below it (radius of
    // metres) are clipped too, so no dome floats above the saw line. Cuts
    // at a segment joint take the trunk above as well.
    const auto redwood = build("trees.redwood", 3);
    const float m = redwood->maturity;
    for (const float height : {6.f, 11.f}) {
        int32_t at = trunkAt(*redwood, height);
        CHECK(at >= 0);
        for (const bool joint : {false, true}) {
            const auto& s = redwood->segments[size_t(at)];
            const float t = joint ? 1.f : (height - s.a.y) / (s.b.y - s.a.y);
            const float cutY = s.a.y + (s.b.y - s.a.y) * t;
            const auto cut =
                build("trees.redwood", 3, TreeMaxHeight, {{at, t, m}});
            const auto box = treeWoodBounds(*redwood, Origin);
            std::vector<uint8_t> mask;
            markTreeWood(*cut, Origin, box, m + 1, mask);
            size_t above = 0, below = 0;
            for (int y = box.y0; y < box.y1; ++y)
                for (int z = box.z0; z < box.z1; ++z)
                    for (int x = box.x0; x < box.x1; ++x) {
                        if (!mask[box.index(x, y, z)]) continue;
                        const float centre = float(y - Origin.y) - 0.5f;
                        // Lean tolerance: a metre past the cut height.
                        if (centre > cutY + 1.5f) ++above;
                        if (centre < cutY - 1) ++below;
                    }
            EQUAL(above, size_t(0), "no wood above the cut");
            CHECK(below > 0);
        }
    }
}

TEST(tree_regrowth_coherent) {
    // A trunk cut regrows as a young tree of the species standing on the
    // stump: attached, inside the species' crown, conifers conical, never
    // a bare spike or a star of long arms, and its wood connected.
    for (const auto* name : {"trees.spruce", "trees.test_oak", "trees.redwood"})
        for (const float fraction : {0.25f, 0.6f}) {
            const auto& tree = species(name);
            const auto k = build(name, 6);
            const float h = k->height * fraction;
            const int32_t at = trunkAt(*k, h);
            // The oak's bole ends at its crown base.
            if (at < 0 && tree.form == TreeForm::Decurrent) continue;
            CHECK(at >= 0);
            const auto& seg = k->segments[size_t(at)];
            const float t = (h - seg.a.y) / (seg.b.y - seg.a.y);
            const TreeVec cutPoint = lerp(seg.a, seg.b, t);
            const float cutAge = k->matureAge() * 1.2f;
            const auto cut = build(name, 6, TreeMaxHeight, {{at, t, cutAge}});
            CHECK(!cut->dead);
            const float start = cut->growthPhaseStart();
            CHECK(start >= cutAge);
            // 1. Attached at the cut, nothing below it.
            size_t roots = 0;
            for (const auto& s : cut->segments) {
                if (!s.live() || !(s.flags & TreeSegment::Sprout)) continue;
                CHECK(std::min(s.a.y, s.b.y) >= cutPoint.y - 0.5f);
                const bool root = !(cut->segments[size_t(s.parent)].flags &
                                    TreeSegment::Sprout);
                if (!root) continue;
                ++roots;
                CHECK((s.a - cutPoint).length() < 0.05f);
            }
            EQUAL(roots, size_t(1), name);
            // Visible regrowth at an age: height above the cut, and reach
            // around the root of its wood and live foliage.
            const auto measure = [&](float age, float& height, float& reach,
                                     float& topReach, float& lowReach) {
                height = reach = topReach = lowReach = 0;
                // Points with their own radius: wood tips (0) and foliage
                // clumps (half their size).
                std::vector<std::pair<TreeVec, float>> points;
                for (const auto& s : cut->segments) {
                    if (!s.live() || !(s.flags & TreeSegment::Sprout)) continue;
                    const float v = cut->visible(s, age);
                    if (v <= 0) continue;
                    points.push_back({lerp(s.a, s.b, v), 0.f});
                }
                for (const auto& f : cut->foliage)
                    if (!f.pruned && f.segment >= 0 &&
                        (cut->segments[size_t(f.segment)].flags &
                         TreeSegment::Sprout) &&
                        age >= f.birth && age < f.death)
                        points.push_back({f.position, f.size * 0.5f});
                for (const auto& [p, size] : points)
                    height = std::max(height, p.y - cutPoint.y);
                for (const auto& [p, size] : points) {
                    const float r =
                        std::hypot(p.x - cutPoint.x, p.z - cutPoint.z) + size;
                    reach = std::max(reach, r);
                    const float up = (p.y - cutPoint.y) / std::max(1.f, height);
                    if (up > 0.75f) topReach = std::max(topReach, r);
                    if (up < 0.5f) lowReach = std::max(lowReach, r);
                }
            };
            const float span = cut->matureAge() - start;
            for (const float f : {0.1f, 0.25f, 0.5f, 1.f}) {
                float height, reach, top, low;
                measure(start + span * f, height, reach, top, low);
                // 3. Shaped like a sapling of the species at the same stage
                // (young conifers are narrow; a bare regrown spike is far
                // narrower than they are).
                if (height > 3) {
                    const auto sapling = build(name, 6);
                    const float sa = sapling->matureAge() * f;
                    float sh = 0, sr = 0;
                    for (const auto& s : sapling->segments) {
                        if (!s.live()) continue;
                        const float v = sapling->visible(s, sa);
                        if (v <= 0) continue;
                        const auto p = lerp(s.a, s.b, v);
                        sh = std::max(sh, p.y);
                        sr = std::max(sr, std::hypot(p.x, p.z));
                    }
                    for (const auto& q : sapling->foliage)
                        if (!q.pruned && sa >= q.birth && sa < q.death) {
                            sh = std::max(sh, q.position.y);
                            sr = std::max(
                                sr, std::hypot(q.position.x, q.position.z) +
                                        q.size * 0.5f);
                        }
                    if (sh > 1) CHECK(reach / height >= 0.5f * (sr / sh));
                }
                // 4. Inside the species' crown (a capped tree spreads up to
                // 1.4 times, as regrowth under the old height is) and the
                // old height.
                CHECK(reach <= tree.size.crown_radius_m.max * 1.4f + 1);
                CHECK(cutPoint.y + height <= k->height + 1);
                // 5. Conifers regrow as cones.
                if (f == 1.f && tree.form == TreeForm::Excurrent)
                    CHECK(top < low);
            }
            // 6. The wood above the cut is one piece with the stump.
            const auto box = treeWoodBounds(*cut, Origin);
            std::vector<uint8_t> mask;
            markTreeWood(*cut, Origin, box, cut->matureAge(), mask);
            std::vector<uint8_t> seen(mask.size(), 0);
            std::vector<std::array<int, 3>> stack;
            for (int y = box.y0; y < box.y1; ++y)
                for (int z = box.z0; z < box.z1; ++z)
                    for (int x = box.x0; x < box.x1; ++x)
                        if (mask[box.index(x, y, z)] &&
                            float(y - Origin.y) - 0.5f < cutPoint.y) {
                            seen[box.index(x, y, z)] = 1;
                            stack.push_back({x, y, z});
                        }
            while (!stack.empty()) {
                const auto [x, y, z] = stack.back();
                stack.pop_back();
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx, ny = y + dy, nz = z + dz;
                            if (nx < box.x0 || ny < box.y0 || nz < box.z0 ||
                                nx >= box.x1 || ny >= box.y1 || nz >= box.z1)
                                continue;
                            const size_t i = box.index(nx, ny, nz);
                            if (!mask[i] || seen[i]) continue;
                            seen[i] = 1;
                            stack.push_back({nx, ny, nz});
                        }
            }
            size_t floating = 0;
            for (size_t i = 0; i < mask.size(); ++i)
                floating += mask[i] && !seen[i];
            EQUAL(floating, size_t(0), name);
        }
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
        loadTreeContent(assets);
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
        // Thin trees stand on their own column, never in the side of a
        // step under higher ground.
        if (table.find(t->species)->species.sizeClass() == TreeSizeClass::Thin)
            CHECK(t->anchor.y ==
                  sampler.sample(t->anchor.x, t->anchor.z).surface_y);
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

TEST(tree_waiting_chunk_does_not_stall_streaming) {
    // A chunk waiting for its region tree sets must never stop other
    // chunks from generating: the starvation guard used to pick that oldest
    // chunk again and again for the whole dispatch budget, every frame.
    TreeStream f;
    f.load(17, 17);               // builds the tree sets around region (2, 2)
    f.chunks->setMaxTreeJobs(0);  // region (5, 0) never gets its trees
    const auto request = [&](unsigned cx, unsigned cz) {
        const auto& region = f.world.regions[(cz / 8) * f.world.width + cx / 8];
        const ChunkCoord origin{(cx / 8) * 8, 0, (cz / 8) * 8};
        if (!f.chunks->isRegistered({cx, 0, cz}))
            f.chunks->registerChunk({cx, 0, cz}, origin, &region);
        for (unsigned y = TreeStream::Low; y < TreeStream::High; ++y) {
            f.chunks->registerChunk({cx, y, cz}, origin, &region);
            f.chunks->request({cx, y, cz});
        }
    };
    request(44, 2);  // oldest in the queue, blocked on trees
    std::vector<std::pair<unsigned, unsigned>> ready;
    for (unsigned cx = 16; cx < 24; ++cx)
        for (unsigned cz = 16; cz < 24; ++cz)
            if (cx != 17 || cz != 17) {
                request(cx, cz);
                ready.push_back({cx, cz});
            }
    const auto allReady = [&] {
        for (const auto& [cx, cz] : ready)
            for (unsigned y = TreeStream::Low; y < TreeStream::High; ++y)
                if (!f.chunks->getChunk({cx, y, cz})) return false;
        return true;
    };
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!allReady() && std::chrono::steady_clock::now() < deadline) {
        f.chunks->update();
        CHECK(f.pool.wait_for(std::chrono::seconds(5)));
    }
    CHECK(allReady());
    CHECK(f.chunks->getChunk({44, TreeStream::Low, 2}) == nullptr);
    CHECK(f.chunks->pendingTreeSets() > 0);
    // Released, the held region builds and its chunks follow.
    f.chunks->setMaxTreeJobs(2);
    f.load(44, 2);
    CHECK(f.chunks->getChunk({44, TreeStream::Low, 2}) != nullptr);
    CHECK(f.chunks->pendingTreeSets() == 0);
}

TEST(streaming_progresses_with_huge_queue) {
    // Fast travel queues thousands of chunks. Ordering them must never use
    // up the dispatch budget: every update makes progress, even with no
    // budget at all (the queue used to freeze for good past ~8k entries).
    TreeStream f;
    f.load(17, 17);
    f.chunks->setDispatchBudget(std::chrono::nanoseconds(0));
    const auto& region = f.world.regions[2 * f.world.width + 2];
    for (unsigned cz = 16; cz < 24; ++cz)
        for (unsigned cx = 16; cx < 24; ++cx) {
            const ChunkCoord origin{16, 0, 16};
            for (unsigned y = 0; y < unsigned(Y_CHUNKS); ++y) {
                if (!f.chunks->isRegistered({cx, y, cz}))
                    f.chunks->registerChunk({cx, y, cz}, origin, &region);
                f.chunks->request({cx, y, cz});
            }
        }
    size_t queued = f.chunks->pendingGeneration();
    CHECK(queued > 3000);
    for (int i = 0; i < 60; ++i) {
        CHECK(f.pool.wait_for(std::chrono::seconds(5)));
        f.chunks->update();
        const size_t now = f.chunks->pendingGeneration();
        CHECK(now < queued);
        queued = now;
    }
}

TEST(streaming_withdraws_stale_requests) {
    // Requests left behind the camera leave the queue; a sunlight
    // prerequisite of a chunk inside the window stays.
    TreeStream f;
    const auto& region = f.world.regions[2 * f.world.width + 2];
    const ChunkCoord origin{16, 0, 16};
    for (unsigned cx = 16; cx < 24; ++cx) {
        for (unsigned y = 20; y < 30; ++y)
            f.chunks->registerChunk({cx, y, 18}, origin, &region);
    }
    for (unsigned cx = 16; cx < 24; ++cx)
        for (unsigned y = 20; y < 30; ++y) f.chunks->request({cx, y, 18});
    // A prerequisite far above, for a chunk inside the window.
    f.chunks->registerChunk({23, 50, 18}, origin, &region);
    f.chunks->requestLighting({23, 50, 18}, {17, 25, 18});
    f.chunks->withdrawOutside({17, 25, 18}, 2);
    CHECK(f.chunks->isQueued({17, 25, 18}));
    CHECK(f.chunks->isQueued({19, 27, 18}));
    CHECK(!f.chunks->isQueued({20, 25, 18}));
    CHECK(!f.chunks->isQueued({17, 20, 18}));
    CHECK(f.chunks->isQueued({23, 50, 18}));
    // x 16..19 (registered from 16) by y 23..27, and the prerequisite.
    EQUAL(f.chunks->pendingGeneration(), size_t(4 * 5 + 1), "window only");
    // Withdrawn chunks can be requested again.
    f.chunks->request({20, 25, 18});
    CHECK(f.chunks->isQueued({20, 25, 18}));
}

TEST(streaming_new_chunks_sky_lit) {
    // Chunks become Ready long before their sunlight sweep when streaming
    // is busy: open air above the ground starts at full sky (it used to
    // show black), the ground itself stays dark.
    TreeStream f;  // no sunlight propagator: nothing sweeps
    f.load(18, 18);
    const int x = 18 * 16 + 3, z = 18 * 16 + 9;
    int ground = WORLD_HEIGHT_Y - 1;
    while (ground > 0 && !(f.chunks->tileAt(x, ground, z) &&
                           f.chunks->tileAt(x, ground, z)->isSolid()))
        --ground;
    CHECK(ground > 0);
    // Above any tree wood: the top loaded slab.
    const Tile* sky = f.chunks->tileAt(x, TreeStream::High * 16 - 1, z);
    CHECK(sky && sky->isAir());
    EQUAL(int(sky->lightLevel.sun_exposure), 15, "open sky");
    EQUAL(int(f.chunks->tileAt(x, ground, z)->lightLevel.sun_exposure), 0,
          "solid ground");
}

TEST(tree_sets_wanted_by_renderer) {
    // Crowns out to the distant mesh range need tree sets of regions no
    // chunk loads: they are built while wanted and released after.
    TreeStream f;
    const Vec2u far{5, 4};
    CHECK(!f.chunks->hasTreeSet(far));
    const auto revision = f.chunks->treeSetsRevision();
    f.chunks->wantTreeSets({far});
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!f.chunks->hasTreeSet(far) &&
           std::chrono::steady_clock::now() < deadline) {
        f.chunks->update();
        CHECK(f.pool.wait_for(std::chrono::seconds(5)));
    }
    CHECK(f.chunks->hasTreeSet(far));
    CHECK(f.chunks->treeSetsRevision() != revision);
    f.chunks->wantTreeSets({});
    CHECK(!f.chunks->hasTreeSet(far));
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
    const auto& oak = species("trees.test_oak");
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

TEST(tree_far_crowns_stay_attached) {
    // Coarser crowns keep fewer clumps and thin branches, but every clump
    // they keep stays on a drawn branch: oak canopies used to float over
    // bare limbs at medium distance.
    const auto& materials = content().getStorage<FoliageMaterial>();
    for (const auto* name : {"trees.oak", "trees.test_oak"})
        for (uint32_t seed = 1; seed <= 4; ++seed) {
            const auto& tree = species(name);
            const auto k = build(name, seed);
            float clump = 0;
            for (const auto& f : k->foliage) clump = std::max(clump, f.size);
            for (const auto detail :
                 {tree_foliage::Detail::Far, tree_foliage::Detail::Distant}) {
                const float grow =
                    detail == tree_foliage::Detail::Far ? 1.35f : 2.2f;
                const auto mesh = tree_foliage::build(
                    *k, tree, materials, k->matureAge(), detail, false);
                std::vector<TreeVec> bark;
                for (const auto& v : mesh.vertices)
                    if (v.layer < 0) bark.push_back({v.x, v.y, v.z});
                CHECK(!bark.empty());
                float worst = 0;
                for (const auto& v : mesh.vertices) {
                    if (v.layer < 0) continue;
                    float nearest = INFINITY;
                    for (const auto& b : bark)
                        nearest = std::min(
                            nearest, (TreeVec{v.x, v.y, v.z} - b).length());
                    worst = std::max(worst, nearest);
                }
                // A card corner lies within its clump of the branch tip.
                CHECK(worst <= clump * grow + 1.5f);
            }
        }
}

TEST(tree_crown_mesh_never_ahead_of_wood) {
    // Crown meshes are built for quantized ages, wood for the exact day.
    // The mesh must never be ahead: it would leave a branch's base to
    // blocks that do not exist yet and draw its thin rest floating above
    // them (seen on fresh regrowth from a cut redwood).
    const auto check = [](const TreeSkeleton& k, float from, float to) {
        const float start = k.growthPhaseStart(), mature = k.matureAge();
        for (int i = 0; i <= 400; ++i) {
            const float age = from + (to - from) * float(i) / 400.f;
            const float meshAge = tree_foliage::bucketAge(
                tree_foliage::bucket(age, mature, start), mature, start);
            // (Before a regrowth starts its mesh is built at the start,
            // where the regrowth is still unborn.)
            CHECK(meshAge <= std::max(age, start) + 1e-4f * mature);
            for (const auto& s : k.segments) {
                if (!s.live()) continue;
                const auto span = tree_foliage::tubeSpan(k, s, meshAge);
                if (!span) continue;
                // Never past the tip, and starting where the blocks end.
                CHECK(span->second <= k.visible(s, age) + 1e-4f);
                if (span->first > 0)
                    CHECK(k.radius(s, span->first, age) >=
                          TreeVoxelRadius - 1e-3f);
            }
        }
    };
    for (const auto* name :
         {"trees.spruce", "trees.test_oak", "trees.redwood"}) {
        const auto k = build(name, 4);
        // (A sapling's first mesh is a quarter step ahead: it has no
        // blocks yet, and the first bucket's start would be empty.)
        check(*k, k->matureAge() / tree_foliage::Buckets * 0.25f,
              k->matureAge());
        // Regrowth from a trunk cut of a mature tree.
        const int32_t at = trunkAt(*k, 3.5f);
        CHECK(at >= 0);
        const float age = k->matureAge() * 1.5f;
        const auto cut = build(name, 4, TreeMaxHeight, {{at, 0.5f, age}});
        // The regrowth starts after the resprout delay, not when its last
        // twig is born (that made old trees show a grown sapling at once).
        const float start = cut->growthPhaseStart();
        CHECK(start > age);
        CHECK(start <= age + species(name).cut.resprout_days.max + 1);
        // From the cut on: before and during the regrowth.
        check(*cut, age, cut->matureAge());
    }
}

TEST(tree_foliage_meshes) {
    const auto& materials = content().getStorage<FoliageMaterial>();
    // A crown as its parts (massive crowns need several 16-bit meshes):
    // every part is a valid mesh; vertices are compared over the whole.
    struct Whole {
        std::vector<FoliageVertex> vertices;
        float height = 0;
        size_t parts = 0;
        bool empty() const { return vertices.empty(); }
    };
    const auto whole = [&](const TreeSkeleton& k, const Tree& tree, float age,
                           tree_foliage::Detail detail, bool bare) {
        Whole out;
        for (const auto& part :
             tree_foliage::buildParts(k, tree, materials, age, detail, bare)) {
            CHECK(!part.empty());
            CHECK(part.vertices.size() <= 65535);
            CHECK(part.indices.size() % 3 == 0);
            for (const auto i : part.indices) CHECK(i < part.vertices.size());
            out.vertices.insert(out.vertices.end(), part.vertices.begin(),
                                part.vertices.end());
            out.height = std::max(out.height, part.height);
            ++out.parts;
        }
        return out;
    };
    for (const auto* name : {"trees.spruce", "trees.mountain_pine", "trees.oak",
                             "trees.palm", "trees.redwood", "trees.elder"}) {
        const auto& tree = species(name);
        const auto k = build(name, 5);
        const float mature = k->matureAge();
        const auto near =
            whole(*k, tree, mature, tree_foliage::Detail::Near, false);
        const auto far =
            whole(*k, tree, mature, tree_foliage::Detail::Far, false);
        const auto again =
            whole(*k, tree, mature, tree_foliage::Detail::Near, false);
        CHECK(!near.empty());
        CHECK(!far.empty());
        CHECK(far.vertices.size() < near.vertices.size());
        // Distant: few large clumps and the wood as bark prisms.
        const auto distant =
            whole(*k, tree, mature, tree_foliage::Detail::Distant, false);
        CHECK(!distant.empty());
        size_t distantLeaves = 0, nearLeaves = 0;
        for (const auto& v : distant.vertices) distantLeaves += v.layer >= 0;
        for (const auto& v : near.vertices) nearLeaves += v.layer >= 0;
        CHECK(distantLeaves < nearLeaves);
        EQUAL(again.vertices.size(), near.vertices.size(), name);
        EQUAL(again.parts, near.parts, name);
        // build() is the first part.
        EQUAL(tree_foliage::build(*k, tree, materials, mature,
                                  tree_foliage::Detail::Near, false)
                  .vertices.size(),
              near.parts == 1 ? near.vertices.size()
                              : size_t(tree_foliage::buildParts(
                                           *k, tree, materials, mature,
                                           tree_foliage::Detail::Near, false)
                                           .front()
                                           .vertices.size()),
              "build is the first part");
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
        const auto young = whole(*k, tree, tree_foliage::bucketAge(3, mature),
                                 tree_foliage::Detail::Near, false);
        CHECK(young.vertices.size() < near.vertices.size());
        const auto dead =
            whole(*k, tree, mature, tree_foliage::Detail::Near, true);
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
    cut.cuts = {{12, 0.25f, 400}, {40, 0.5f, 900, {0.6f, 0, -0.8f}}};
    map[cut.key] = cut;
    TreeRecord planted;
    planted.anchor = {500, 410, 77};
    planted.key = plantedTreeKey(planted.anchor);
    planted.species = 3;
    planted.seed = 99;
    planted.birthDay = 12;
    planted.headroom = 5;
    map[planted.key] = planted;
    TreeRecord gone;
    gone.key = 777;
    gone.removed = true;
    map[gone.key] = gone;
    CHECK(planted.planted() && !cut.planted());
    const auto bytes = trees_codec::encode(map);
    const auto file = trees_codec::decode(bytes);
    CHECK(file.trees == map);
    CHECK(trees_codec::decode(trees_codec::encode({})).trees.empty());
    // Version 1 sections (no flags byte) still load.
    {
        BinaryWriter w;
        w.write(uint32_t(1));
        w.write(uint64_t(1234));
        w.write(uint8_t(0));
        w.write(uint32_t(0));
        for (const int v : {1, 2, 3, 0}) w.write(int32_t(v));
        w.write(1.f);
        w.write(uint16_t(1));
        w.write(int32_t(3));
        w.write(0.5f);
        w.write(10.f);
        SectionContainer container;
        container.put("TREE", 1, w.take());
        const auto old = trees_codec::decode(container.encode());
        CHECK(old.trees.size() == 1);
        CHECK(!old.trees.at(1234).removed);
        CHECK(old.trees.at(1234).cuts.size() == 1);
        // Cuts before v3 are perpendicular.
        CHECK(old.trees.at(1234).cuts[0].normal == TreeVec{});
    }
    // A v3 cut plane outside +-3 is refused.
    {
        BinaryWriter w;
        w.write(uint32_t(1));
        w.write(uint64_t(5));
        w.write(uint8_t(0));
        w.write(uint32_t(0));
        for (const int v : {1, 2, 3, 0}) w.write(int32_t(v));
        w.write(1.f);
        w.write(uint8_t(0));
        w.write(uint16_t(1));
        w.write(int32_t(3));
        w.write(0.5f);
        w.write(1.f);
        w.write(int8_t(7));
        SectionContainer container;
        container.put("TREE", 3, w.take());
        test::expectThrows([&] { trees_codec::decode(container.encode()); },
                           "bad cut plane");
    }
    // A v3 grid plane code reads as its normal; v4 refuses non-unit normals.
    const auto cutRecord = [](uint16_t version, auto writePlane) {
        BinaryWriter w;
        w.write(uint32_t(1));
        w.write(uint64_t(5));
        w.write(uint8_t(0));
        w.write(uint32_t(0));
        for (const int v : {1, 2, 3, 0}) w.write(int32_t(v));
        w.write(1.f);
        w.write(uint8_t(0));
        w.write(uint16_t(1));
        w.write(int32_t(3));
        w.write(0.5f);
        w.write(1.f);
        writePlane(w);
        SectionContainer container;
        container.put("TREE", version, w.take());
        return container.encode();
    };
    {
        const auto v3 = trees_codec::decode(
            cutRecord(3, [](BinaryWriter& w) { w.write(int8_t(-2)); }));
        CHECK(v3.trees.at(5).cuts[0].normal == (TreeVec{0, -1, 0}));
        test::expectThrows(
            [&] {
                trees_codec::decode(cutRecord(4, [](BinaryWriter& w) {
                    w.write(2.f);
                    w.write(0.f);
                    w.write(0.f);
                }));
            },
            "non-unit cut normal");
    }
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

TEST(tree_repeated_branch_cuts) {
    TreeSkeleton base;
    base.height = 20;
    base.trunkRadius = 2;
    base.maturity = 100;
    base.headroom = TreeMaxHeight;
    TreeSegment limb;
    limb.a = {0, 6, 0};
    limb.b = {10, 6, 0};
    limb.r0 = limb.r1 = 1.2f;
    limb.birthB = limb.matureAge = 1;
    limb.order = 1;
    base.segments.push_back(limb);
    TreeSegment branch = limb;
    branch.a = {6, 6, 0};
    branch.b = {6, 6, 5};
    branch.parent = 0;
    branch.attachT = 0.6f;
    branch.order = 2;
    base.segments.push_back(branch);
    TreeFoliageSite leaf;
    leaf.position = branch.b;
    leaf.segment = 1;
    base.foliage.push_back(leaf);
    const auto& rule = species("trees.elder");
    std::vector<TreeCut> cuts;
    auto current = std::make_shared<const TreeSkeleton>(base);
    const TreeTileBox branchBox{Origin.x + 5, Origin.y + 6, Origin.z + 2,
                                Origin.x + 8, Origin.y + 8, Origin.z + 5};
    CHECK(countWood(base, branchBox, 100) > 0);
    for (const float t : {0.8f, 0.9f, 0.7f}) {
        cuts.push_back({0, t, 100});
        current = applyTreeCuts(*current, rule, cuts, cuts.size() - 1);
        const bool attached = cuts.size() < 3;
        CHECK(current->segments[1].live() == attached);
        CHECK(current->foliage[0].pruned == !attached);
        CHECK((countWood(*current, branchBox, 100) > 0) == attached);
        if (attached)
            CHECK(std::abs(current->segments[1].attachT *
                               current->segments[0].b.x -
                           6.f) < 1e-4f);
    }
    const auto replay = applyTreeCuts(base, rule, cuts);
    CHECK(!replay->segments[1].live());
    CHECK(replay->foliage[0].pruned);
    EQUAL(countWood(*replay, branchBox, 100), size_t(0),
          "detached branch stays removed after replay");
}

TEST(tree_cut_delta_local) {
    const TreeSpeciesTable table(content());
    const TreeSpeciesRule* rule = nullptr;
    for (const auto& r : table.species())
        if (r.species.name == "trees.test_oak") rule = &r;
    CHECK(rule);
    for (const bool settles : {false, true}) {
        Tree species = rule->species;
        species.cut.fell = settles ? TreeFell::Settle : TreeFell::Fall;
        TreeSkeletonInput input;
        input.seed = 7;
        const auto base = buildTreeSkeleton(species, input);
        const float age = std::ceil(base->matureAge() * 2);
        const int32_t trunk = trunkAt(*base, 3);
        CHECK(trunk >= 0);
        const auto& s = base->segments[size_t(trunk)];
        input.cuts = {{trunk, (3 - s.a.y) / (s.b.y - s.a.y), age}};
        auto tree = std::make_shared<TreeInstance>();
        tree->species = rule->id;
        tree->birthDay = -int(age);
        tree->anchor = {2, 400, 100};  // cross the X seam
        tree->cuts = input.cuts;
        tree->skeleton = buildTreeSkeleton(species, input);
        finishTreeInstance(*tree);
        size_t removals = 0, additions = 0;
        constexpr int Period = 256;
        for (unsigned z = unsigned(tree->wood.z0 / 16);
             z <= unsigned((tree->wood.z1 - 1) / 16); ++z)
            for (unsigned y = unsigned(tree->wood.y0 / 16);
                 y <= unsigned((tree->wood.y1 - 1) / 16); ++y)
                for (int x = int(std::floor(double(tree->wood.x0) / 16));
                     x <= int(std::floor(double(tree->wood.x1 - 1) / 16));
                     ++x) {
                    const ChunkCoord coord{
                        unsigned(WorldWrap::mod(x, Period / 16)), y, z};
                    TreeAnchor image = tree->anchor;
                    image.x += int(coord.x) * 16 - x * 16;
                    TreeColumn column;
                    column.trees.push_back({tree, image});
                    const auto expected =
                        treeWoodDelta(coord, column, table, -1, 0);
                    const auto actual = treeCutWoodDelta(
                        coord, *tree, tree->anchor, rule->woodBlock, 1, 0,
                        settles, Period);
                    const auto cells = [](const auto& changes) {
                        std::set<int> out;
                        for (const auto& c : changes) out.insert(c.index);
                        return out;
                    };
                    CHECK(cells(actual.remove) == cells(expected.remove));
                    CHECK(cells(actual.add) == cells(expected.add));
                    removals += actual.remove.size();
                    additions += actual.add.size();
                }
        CHECK(removals > 0);
        CHECK((additions > 0) == settles);
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
                                          unsigned& cz, bool wood = true) {
    for (cz = 16; cz < 22; ++cz)
        for (cx = 16; cx < 22; ++cx) {
            f.load(cx, cz);
            const auto column = f.chunks->treeColumn(cx, cz);
            for (const auto& e : column->trees) {
                const auto* rule =
                    f.chunks->treeSpecies().find(e.tree->species);
                if (rule->species.name != name || e.tree->wood.empty() == wood)
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

TEST(tree_cut_angled_branch) {
    for (const int direction : {-1, 1}) {
        TreeStream f;
        unsigned cx = 0, cz = 0;
        const auto found = findTree(f, "trees.test_oak", cx, cz);
        CHECK(found.has_value());
        for (unsigned z = cz - 1; z <= cz + 1; ++z)
            for (unsigned x = cx - 1; x <= cx + 1; ++x) f.load(x, z);
        const auto a = found->anchor;
        const uint8_t wood =
            f.chunks->treeSpecies().find(found->tree->species)->woodBlock;
        auto skeleton = std::make_shared<TreeSkeleton>();
        skeleton->trunkRadius = 3;
        skeleton->height = 50;
        skeleton->lifespan = 1000000;
        skeleton->headroom = TreeMaxHeight;
        TreeSegment limb;
        limb.a = {float(-12 * direction), 20, float(-4 * direction)};
        limb.b = {float(12 * direction), 28, float(4 * direction)};
        limb.r0 = limb.r1 = 6.f;
        limb.birthB = limb.matureAge = 1;
        limb.order = 1;
        skeleton->segments.push_back(limb);
        TreeFoliageSite leaves;
        leaves.segment = 0;
        leaves.position = limb.b;
        skeleton->foliage.push_back(leaves);
        auto tree = std::make_shared<TreeInstance>(*found->tree);
        tree->skeleton = skeleton;
        tree->birthDay = -100;
        finishTreeInstance(*tree);
        f.chunks->updateTree(tree, found->tree->bounds);
        // Replace this fixture's wood with the isolated angled limb.
        TreeColumn column;
        column.trees.push_back({tree, a});
        f.chunks->forEachReady([&](const ChunkCoord& coord, Chunk& chunk) {
            for (auto& slab : chunk.tiles)
                for (auto& row : slab)
                    for (auto& tile : row)
                        if (tile.block_id == wood) tile.block_id = 0;
            placeTreeWood(chunk, coord, column, f.chunks->treeSpecies(), 0);
        });
        WorldEditor editor(*f.chunks, nullptr, f.assets.getStorage<Block>());
        TreeSimulation sim(*f.chunks, editor);
        sim.update(0);
        std::vector<TilePos> slice;
        for (int y = a.y + 17; y <= a.y + 31; ++y)
            for (int z = a.z - 9; z <= a.z + 9; ++z)
                if (const Tile* tile = f.chunks->tileAt(a.x, y, z);
                    tile && tile->block_id == wood)
                    slice.push_back({a.x, y, z});
        CHECK(slice.size() > 10);
        const TilePos tip{a.x + 8 * direction, a.y + 27, a.z + 3 * direction};
        CHECK(f.chunks->tileAt(tip.x, tip.y, tip.z)->block_id == wood);
        // Wood holds by its faces: while a slice tile still joins wood on
        // both sides of the slit through faces, the limb stays attached.
        const auto isWood = [&](int x, int y, int z) {
            const Tile* t = f.chunks->tileAt(x, y, z);
            return t && t->block_id == wood;
        };
        std::vector<bool> bridge;
        for (const auto& p : slice)
            bridge.push_back(isWood(p.x - 1, p.y, p.z) &&
                             isWood(p.x + 1, p.y, p.z));
        CHECK(std::count(bridge.begin(), bridge.end(), true) > 10);
        for (size_t i = 0; i < slice.size(); ++i) {
            CHECK(editor.removeBlock(slice[i]) == EditStatus::Applied);
            if (std::find(bridge.begin() + long(i) + 1, bridge.end(), true) !=
                bridge.end())
                EQUAL(sim.cutCount(), size_t(0),
                      "remaining wood supports limb");
        }
        EQUAL(sim.cutCount(), size_t(1), "one-tile slit severs sloping limb");
        sim.finishClears();
        EQUAL(int(f.chunks->tileAt(tip.x, tip.y, tip.z)->block_id), 0,
              "detached wood removed");
        // The cut ends at the sawn row, not on a plane across the limb:
        // nothing is left beyond the slit (a wedge of blocks used to stay
        // next to it) and the stump keeps the row before it.
        const auto record = f.chunks->treeOverrides()->find(tree->key);
        CHECK(record != f.chunks->treeOverrides()->end());
        if (record != f.chunks->treeOverrides()->end()) {
            CHECK(!record->second.cuts.empty());
            if (!record->second.cuts.empty())
                CHECK(record->second.cuts.back().normal ==
                      (TreeVec{float(direction), 0, 0}));  // the sawn row
        }
        size_t beyond = 0, before = 0;
        for (int y = a.y + 10; y <= a.y + 40; ++y)
            for (int z = a.z - 12; z <= a.z + 12; ++z) {
                for (int k = 1; k <= 18; ++k)
                    if (const Tile* t =
                            f.chunks->tileAt(a.x + k * direction, y, z);
                        t && t->block_id == wood)
                        ++beyond;
                if (const Tile* t = f.chunks->tileAt(a.x - direction, y, z);
                    t && t->block_id == wood)
                    ++before;
            }
        EQUAL(beyond, size_t(0), "no wood left beyond the slit");
        CHECK(before > 10);
    }
}

TEST(tree_cut_slanted_slit) {
    // Slits that are not tile-grid slices: a one-tile staircase on the
    // plane x + y = c (slanted) or x + z = c (turned about y) through a
    // 12-block-wide limb along x. The last block of the slit cuts it, and
    // the cut follows the slit: nothing is left beyond it, the stump keeps
    // its side.
    for (const int variant : {0, 1}) {
        TreeStream f;
        unsigned cx = 0, cz = 0;
        const auto found = findTree(f, "trees.test_oak", cx, cz);
        CHECK(found.has_value());
        for (unsigned z = cz - 1; z <= cz + 1; ++z)
            for (unsigned x = cx - 1; x <= cx + 1; ++x) f.load(x, z);
        const auto a = found->anchor;
        const uint8_t wood =
            f.chunks->treeSpecies().find(found->tree->species)->woodBlock;
        auto skeleton = std::make_shared<TreeSkeleton>();
        skeleton->trunkRadius = 3;
        skeleton->height = 50;
        skeleton->lifespan = 1000000;
        skeleton->headroom = TreeMaxHeight;
        TreeSegment limb;
        limb.a = {-14, 22, 0};
        limb.b = {14, 26, 0};
        limb.r0 = limb.r1 = 6.f;
        limb.birthB = limb.matureAge = 1;
        limb.order = 1;
        skeleton->segments.push_back(limb);
        auto tree = std::make_shared<TreeInstance>(*found->tree);
        tree->skeleton = skeleton;
        tree->birthDay = -100;
        finishTreeInstance(*tree);
        f.chunks->updateTree(tree, found->tree->bounds);
        TreeColumn column;
        column.trees.push_back({tree, a});
        f.chunks->forEachReady([&](const ChunkCoord& coord, Chunk& chunk) {
            for (auto& slab : chunk.tiles)
                for (auto& row : slab)
                    for (auto& tile : row)
                        if (tile.block_id == wood) tile.block_id = 0;
            placeTreeWood(chunk, coord, column, f.chunks->treeSpecies(), 0);
        });
        WorldEditor editor(*f.chunks, nullptr, f.assets.getStorage<Block>());
        TreeSimulation sim(*f.chunks, editor);
        sim.update(0);
        // Slit coordinate s(x, y, z) relative to the anchor: the slit is
        // s == 0, the far side (toward +x) s > 0.
        const int y0 = a.y + 24;
        const auto slitOf = [&](int x, int y, int z) {
            return variant == 0 ? (x - a.x) + (y - y0) : (x - a.x) + (z - a.z);
        };
        std::vector<TilePos> slit;
        size_t far = 0, near = 0;
        for (int y = a.y + 10; y <= a.y + 40; ++y)
            for (int z = a.z - 12; z <= a.z + 12; ++z)
                for (int x = a.x - 20; x <= a.x + 20; ++x)
                    if (const Tile* t = f.chunks->tileAt(x, y, z);
                        t && t->block_id == wood) {
                        const int s = slitOf(x, y, z);
                        if (s == 0) slit.push_back({x, y, z});
                        far += s > 0;
                        near += s < 0;
                    }
        CHECK(slit.size() > 20 && far > 100 && near > 100);
        // A slit tile bridges while it has wood faces on both sides.
        const auto isWood = [&](int x, int y, int z) {
            const Tile* t = f.chunks->tileAt(x, y, z);
            return t && t->block_id == wood;
        };
        std::vector<bool> bridge;
        for (const auto& p : slit) {
            bool lo = false, hi = false;
            for (const auto [dx, dy, dz] :
                 {std::array{1, 0, 0}, std::array{-1, 0, 0},
                  std::array{0, 1, 0}, std::array{0, -1, 0},
                  std::array{0, 0, 1}, std::array{0, 0, -1}}) {
                if (!isWood(p.x + dx, p.y + dy, p.z + dz)) continue;
                const int s = slitOf(p.x + dx, p.y + dy, p.z + dz);
                lo |= s < 0;
                hi |= s > 0;
            }
            bridge.push_back(lo && hi);
        }
        CHECK(std::count(bridge.begin(), bridge.end(), true) > 10);
        for (size_t i = 0; i < slit.size(); ++i) {
            CHECK(editor.removeBlock(slit[i]) == EditStatus::Applied);
            if (std::find(bridge.begin() + long(i) + 1, bridge.end(), true) !=
                bridge.end())
                EQUAL(sim.cutCount(), size_t(0),
                      "remaining wood supports limb");
        }
        EQUAL(sim.cutCount(), size_t(1), "a slanted slit severs the limb");
        sim.finishClears();
        size_t beyond = 0, kept = 0;
        for (int y = a.y + 10; y <= a.y + 40; ++y)
            for (int z = a.z - 12; z <= a.z + 12; ++z)
                for (int x = a.x - 20; x <= a.x + 20; ++x)
                    if (const Tile* t = f.chunks->tileAt(x, y, z);
                        t && t->block_id == wood) {
                        beyond += slitOf(x, y, z) > 0;
                        kept += slitOf(x, y, z) < 0;
                    }
        EQUAL(beyond, size_t(0), "no wood left beyond the slit");
        // The stump keeps its side (a staircase may lose a few corners).
        CHECK(kept * 10 >= near * 9);
    }
}

TEST(tree_cut_relights_below) {
    // Sawing a sloping limb off: the air it shaded (down to the ground,
    // through every loaded slab of those columns) gets full sky again.
    TreeStream f;
    SunLightPropagator sunlight(*f.chunks, f.assets.getStorage<Block>(),
                                f.pool);
    const auto light = [&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            f.chunks->update();
            sunlight.update();
            f.pool.wait_for(std::chrono::milliseconds(20));
            CHECK(std::chrono::steady_clock::now() < deadline);
        } while (!sunlight.isIdle() || f.chunks->hasGenerationWork());
    };
    unsigned cx = 0, cz = 0;
    const auto found = findTree(f, "trees.test_oak", cx, cz);
    CHECK(found.has_value());
    for (unsigned z = cz - 1; z <= cz + 1; ++z)
        for (unsigned x = cx - 1; x <= cx + 1; ++x) f.load(x, z);
    const auto a = found->anchor;
    const uint8_t wood =
        f.chunks->treeSpecies().find(found->tree->species)->woodBlock;
    auto skeleton = std::make_shared<TreeSkeleton>();
    skeleton->trunkRadius = 3;
    skeleton->height = 50;
    skeleton->lifespan = 1000000;
    skeleton->headroom = TreeMaxHeight;
    TreeSegment limb;
    limb.a = {-12, 20, -4};
    limb.b = {12, 28, 4};
    limb.r0 = limb.r1 = 6.f;
    limb.birthB = limb.matureAge = 1;
    limb.order = 1;
    skeleton->segments.push_back(limb);
    auto tree = std::make_shared<TreeInstance>(*found->tree);
    tree->skeleton = skeleton;
    tree->birthDay = -100;
    finishTreeInstance(*tree);
    f.chunks->updateTree(tree, found->tree->bounds);
    TreeColumn column;
    column.trees.push_back({tree, a});
    std::set<uint64_t> columns;
    f.chunks->forEachReady([&](const ChunkCoord& coord, Chunk& chunk) {
        for (auto& slab : chunk.tiles)
            for (auto& row : slab)
                for (auto& tile : row)
                    if (tile.block_id == wood) tile.block_id = 0;
        placeTreeWood(chunk, coord, column, f.chunks->treeSpecies(), 0);
        columns.insert(uint64_t{coord.x} << 32 | coord.z);
    });
    for (const auto c : columns)
        sunlight.markColumnDirty(unsigned(c >> 32), unsigned(c));
    light();
    // Under the limb's tip half, above the ground: shaded now.
    const TilePos under{a.x + 6, a.y + 12, a.z + 2};
    CHECK(f.chunks->tileAt(under.x, under.y, under.z)->isAir());
    CHECK(f.chunks->tileAt(under.x, under.y, under.z)->lightLevel.sun_exposure <
          15);
    WorldEditor editor(*f.chunks, &sunlight, f.assets.getStorage<Block>());
    TreeSimulation sim(*f.chunks, editor);
    sim.update(0);
    for (int y = a.y + 17; y <= a.y + 31; ++y)
        for (int z = a.z - 9; z <= a.z + 9; ++z)
            if (const Tile* t = f.chunks->tileAt(a.x, y, z);
                t && t->block_id == wood)
                editor.removeBlock({a.x, y, z});
    EQUAL(sim.cutCount(), size_t(1), "limb sawn off");
    sim.finishClears();
    light();
    EQUAL(int(f.chunks->tileAt(a.x + 8, a.y + 27, a.z + 3)->block_id), 0,
          "limb gone");
    EQUAL(int(f.chunks->tileAt(under.x, under.y, under.z)
                  ->lightLevel.sun_exposure),
          15, "full sky where the limb was");
}

TEST(tree_cut_and_regrowth) {
    TreeStream f;
    unsigned cx = 0, cz = 0;
    const auto found = findTree(f, "trees.test_oak", cx, cz);
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
    // The trunk above the cut goes chunk by chunk over the next updates
    // (no physics: it disappears), the sawn blocks at once.
    CHECK(sim.pendingClears() > 0);
    sim.update(0);
    sim.finishClears();
    EQUAL(sim.pendingClears(), size_t(0), "all cleared");
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

TEST(tree_settle_in_world) {
    // A settling species in a streamed world: sawing through its trunk
    // leaves the tree standing; the sawn ring fills again once the queued
    // clears run (the trunk came down one tile).
    std::ifstream trees(std::string(SOIL_CONTENT_DIR) + "/trees/trees.json");
    Json settling;
    for (auto tree : Json::parse(trees))
        if (tree.at("name") == "trees.oak") settling = tree;
    settling["name"] = "trees.test_settling_oak";
    settling["size"]["trunk_radius_m"] = Json::array({1.6, 2.6});
    settling["wood"]["block"] = "blocks.test_oak_wood";
    settling["cut"]["fell"] = "settle";
    TreeStream f(forestWorld(), Json::array({settling}));
    unsigned cx = 0, cz = 0;
    const auto found = findTree(f, "trees.test_settling_oak", cx, cz);
    CHECK(found.has_value());
    if (!found) return;
    for (unsigned z = cz - 1; z <= cz + 1; ++z)
        for (unsigned x = cx - 1; x <= cx + 1; ++x) f.load(x, z);
    WorldEditor editor(*f.chunks, nullptr, f.assets.getStorage<Block>());
    TreeSimulation sim(*f.chunks, editor);
    sim.update(0);
    const auto a = found->anchor;
    const uint8_t wood =
        f.chunks->treeSpecies().find(found->tree->species)->woodBlock;
    const int cutY = a.y + 3;
    const size_t above = woodNear(f, a, cutY + 1, cutY + 8, 3, wood);
    CHECK(above > 0);
    for (int z = a.z - 6; z <= a.z + 6; ++z)
        for (int x = a.x - 6; x <= a.x + 6; ++x)
            if (const Tile* t = f.chunks->tileAt(x, cutY, z);
                t && t->block_id == wood)
                editor.removeBlock({x, cutY, z});
    EQUAL(sim.cutCount(), size_t(1), "one cut");
    EQUAL(sim.removedCount(), size_t(0), "still alive");
    EQUAL(woodNear(f, a, cutY, cutY + 1, 6, wood), size_t(0), "sawn");
    sim.update(0);
    sim.finishClears();
    CHECK(woodNear(f, a, cutY, cutY + 1, 6, wood) > 0);
    CHECK(woodNear(f, a, cutY + 1, cutY + 8, 3, wood) > 0);
}

TEST(tree_regrowth_starts_on_stump) {
    // Sawing a ring through a trunk: the stump keeps the rows below it,
    // the sawn row goes, and the regrowth starts exactly on the stump's top
    // face (it used to start half a tile up, leaving a visible gap between
    // the blocks and the sapling mesh).
    TreeStream f;
    unsigned cx = 0, cz = 0;
    const auto found = findTree(f, "trees.test_oak", cx, cz);
    CHECK(found.has_value());
    for (unsigned z = cz - 1; z <= cz + 1; ++z)
        for (unsigned x = cx - 1; x <= cx + 1; ++x) f.load(x, z);
    WorldEditor editor(*f.chunks, nullptr, f.assets.getStorage<Block>());
    TreeSimulation sim(*f.chunks, editor);
    sim.update(0);
    const auto a = found->anchor;
    const uint8_t wood =
        f.chunks->treeSpecies().find(found->tree->species)->woodBlock;
    const int cutY = a.y + 4;
    for (int z = a.z - 6; z <= a.z + 6; ++z)
        for (int x = a.x - 6; x <= a.x + 6; ++x)
            if (const Tile* t = f.chunks->tileAt(x, cutY, z);
                t && t->block_id == wood)
                editor.removeBlock({x, cutY, z});
    CHECK(sim.cutCount() >= 1);
    EQUAL(woodNear(f, a, cutY, cutY + 1, 3, wood), size_t(0), "row sawn");
    CHECK(woodNear(f, a, cutY - 1, cutY, 3, wood) > 0);  // the stump
    // The cut tree's regrowth root sits on the stump's top face (world Y
    // cutY: the sawn row's lower face).
    const auto column = f.chunks->treeColumn(cx, cz);
    const TreeInstance* tree = nullptr;
    for (const auto& e : column->trees)
        if (e.tree->key == found->tree->key) tree = e.tree.get();
    CHECK(tree != nullptr);
    bool root = false;
    for (const auto& seg : tree->skeleton->segments) {
        if (!seg.live() || seg.order != 0 ||
            !(seg.flags & TreeSegment::Sprout) || seg.parent < 0 ||
            (tree->skeleton->segments[size_t(seg.parent)].flags &
             TreeSegment::Sprout))
            continue;
        root = true;
        const float y = float(a.y + 1) + seg.a.y;
        CHECK(std::abs(y - float(cutY)) <= 0.1f);
    }
    CHECK(root);
}

TEST(tree_regrowth_takes_time) {
    // After a cut nothing grows back before the species' resprout delay,
    // and then only slowly: an instantly regrown trunk is infinite wood.
    for (const char* name : {"trees.test_oak", "trees.spruce"}) {
        TreeStream f;
        unsigned cx = 0, cz = 0;
        auto found = findTree(f, name, cx, cz);
        if (!found && std::string(name) == "trees.spruce") continue;
        CHECK(found.has_value());
        for (unsigned z = cz - 1; z <= cz + 1; ++z)
            for (unsigned x = cx - 1; x <= cx + 1; ++x) f.load(x, z);
        WorldEditor editor(*f.chunks, nullptr, f.assets.getStorage<Block>());
        TreeSimulation sim(*f.chunks, editor);
        const double day = 2;
        sim.update(day);
        const auto a = found->anchor;
        const uint8_t wood =
            f.chunks->treeSpecies().find(found->tree->species)->woodBlock;
        const int cutY = a.y + 4;
        for (int z = a.z - 6; z <= a.z + 6; ++z)
            for (int x = a.x - 6; x <= a.x + 6; ++x)
                if (const Tile* t = f.chunks->tileAt(x, cutY, z);
                    t && t->block_id == wood)
                    editor.removeBlock({x, cutY, z});
        CHECK(sim.cutCount() >= 1);
        const auto& rule = *f.chunks->treeSpecies().find(found->tree->species);
        const double delay = rule.species.cut.resprout_days.min;
        size_t after[3];
        int i = 0;
        for (const double later : {day, day + delay * 0.5, day + delay - 1}) {
            sweepAll(sim, later);
            after[i++] = woodNear(f, a, cutY, cutY + 12, 4, wood);
        }
        for (const auto n : after) EQUAL(n, size_t(0), name);
    }
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

TEST(tree_reload_same_day_cut) {
    // Chunks saved earlier on the day of a cut (stamp = that whole day)
    // still lose the cut wood when they load again that day. They used to
    // keep it: unseen high in a massive crown, it shaded the ground.
    TreeStream f;
    unsigned cx = 0, cz = 0;
    const auto found = findTree(f, "trees.test_oak", cx, cz);
    CHECK(found.has_value());
    const auto& tree = *found->tree;
    const auto a = found->anchor;
    const uint8_t wood = f.chunks->treeSpecies().find(tree.species)->woodBlock;
    const int cutY = a.y + 3;
    CHECK(woodNear(f, a, cutY + 2, cutY + 6, 4, wood) > 0);
    // Saved before the cut, on day 0.
    const auto snapshot = f.chunks->snapshotRegion({cx / 8, cz / 8}, true);
    const auto data = region_file::decode(
        region_file::encode(region_file::merge(RegionData{}, snapshot)));
    std::map<unsigned, ChunkBlob> stored, plants;
    for (unsigned sy = TreeStream::Low; sy < TreeStream::High; ++sy) {
        const auto index = RegionData::localIndex({cx, sy, cz});
        if (const auto it = data.chunks.find(index); it != data.chunks.end())
            stored[sy] = it->second;
        if (const auto it = data.flora.find(index); it != data.flora.end())
            plants[sy] = it->second;
    }
    CHECK(f.chunks->treeStamp({cx, unsigned(cutY / 16), cz}).day == 0);
    // Cut later the same day.
    WorldEditor editor(*f.chunks, nullptr, f.assets.getStorage<Block>());
    TreeSimulation sim(*f.chunks, editor);
    sim.update(0.6);
    for (int z = a.z - 6; z <= a.z + 6; ++z)
        for (int x = a.x - 6; x <= a.x + 6; ++x)
            if (const Tile* t = f.chunks->tileAt(x, cutY, z);
                t && t->block_id == wood)
                editor.removeBlock({x, cutY, z});
    EQUAL(sim.cutCount(), size_t(1), "one cut");
    // Reload the saved chunks, still on day 0.
    TreeStream g;
    g.chunks->setTreeOverrides(f.chunks->treeOverrides());
    g.chunks->setFloraDay(0);
    g.load(cx, cz, stored, plants);
    EQUAL(woodNear(g, a, cutY + 1, cutY + 8, 3, wood), size_t(0),
          "cut wood gone from chunks saved earlier that day");
    CHECK(woodNear(g, a, cutY - 3, cutY, 3, wood) > 0);  // the stump stays
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
    CHECK(sim.plant({x, y, z}, species("trees.test_oak").id, 0));
    const auto record =
        f.chunks->treeOverrides()->at(plantedTreeKey({x, y, z}));
    EQUAL(record.headroom, 5.f, "headroom under the ceiling");
    sweepAll(sim, 20000);
    const uint8_t wood =
        f.chunks->treeSpecies().find(species("trees.test_oak").id)->woodBlock;
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

namespace {
bool inColumn(TreeStream& f, unsigned cx, unsigned cz, uint64_t key) {
    const auto column = f.chunks->treeColumn(cx, cz);
    return column &&
           std::any_of(column->trees.begin(), column->trees.end(),
                       [&](const auto& e) { return e.tree->key == key; });
}
}  // namespace

TEST(tree_dies_without_ground) {
    // A thin (mesh-only) oak whose anchor tile is dug out dies and leaves
    // the world, for good.
    TreeStream f;
    unsigned cx = 0, cz = 0;
    const auto found = findTree(f, "trees.oak", cx, cz, false);
    CHECK(found.has_value());
    WorldEditor editor(*f.chunks, nullptr, f.assets.getStorage<Block>());
    TreeSimulation sim(*f.chunks, editor);
    sim.update(0);
    const auto key = found->tree->key;
    const auto a = found->anchor;
    CHECK(inColumn(f, cx, cz, key));
    // Digging beside it does nothing.
    editor.removeBlock({a.x + 3, a.y, a.z});
    EQUAL(sim.removedCount(), size_t(0), "still standing");
    editor.removeBlock({a.x, a.y, a.z});
    EQUAL(sim.removedCount(), size_t(1), "died");
    CHECK(sim.dirty());
    CHECK(!inColumn(f, cx, cz, key));
    const auto record = f.chunks->treeOverrides()->find(key);
    CHECK(record != f.chunks->treeOverrides()->end());
    CHECK(record->second.removed);
    // A reload (rebuilt from trees.dat) does not bring it back.
    TreeStream g;
    g.chunks->setTreeOverrides(f.chunks->treeOverrides());
    g.load(cx, cz);
    CHECK(!inColumn(g, cx, cz, key));
    // Nor does its billboard.
    const Vec2u region{cx / 8, cz / 8};
    const auto terrain =
        TerrainNeighborhood::capture(f.world, region, content());
    const auto at = [&](const std::vector<TreeBillboard>& set) {
        return std::any_of(set.begin(), set.end(), [&](const auto& b) {
            return b.x == float(found->tree->anchor.x) + 0.5f &&
                   b.z == float(found->tree->anchor.z) + 0.5f;
        });
    };
    RegionTreesInput input{&f.chunks->treeSpecies(), &terrain, region,
                           f.chunks->treePlacementSeed(), nullptr};
    CHECK(at(regionBillboards(input)));
    input.overrides = f.chunks->treeOverrides().get();
    CHECK(!at(regionBillboards(input)));
}

TEST(tree_dies_without_base_wood) {
    // Chopping away the whole first row of a trunk kills the tree: its
    // remaining wood is cleared and it is gone after a reload too.
    TreeStream f;
    unsigned cx = 0, cz = 0;
    const auto found = findTree(f, "trees.test_oak", cx, cz);
    CHECK(found.has_value());
    for (unsigned z = cz - 1; z <= cz + 1; ++z)
        for (unsigned x = cx - 1; x <= cx + 1; ++x) f.load(x, z);
    WorldEditor editor(*f.chunks, nullptr, f.assets.getStorage<Block>());
    TreeSimulation sim(*f.chunks, editor);
    sim.update(0);
    const auto key = found->tree->key;
    const auto a = found->anchor;
    const uint8_t wood =
        f.chunks->treeSpecies().find(found->tree->species)->woodBlock;
    CHECK(woodNear(f, a, a.y + 2, a.y + 10, 3, wood) > 0);
    const int row = a.y + 1;
    for (int z = a.z - 6; z <= a.z + 6; ++z)
        for (int x = a.x - 6; x <= a.x + 6; ++x)
            if (const Tile* t = f.chunks->tileAt(x, row, z);
                t && t->block_id == wood)
                editor.removeBlock({x, row, z});
    EQUAL(sim.removedCount(), size_t(1), "died");
    CHECK(!inColumn(f, cx, cz, key));
    // Its wood goes chunk by chunk over the next updates.
    CHECK(sim.pendingClears() > 0);
    sim.finishClears();
    EQUAL(woodNear(f, a, a.y + 1, a.y + 10, 3, wood), size_t(0),
          "no wood left");
    TreeStream g;
    g.chunks->setTreeOverrides(f.chunks->treeOverrides());
    g.load(cx, cz);
    CHECK(!inColumn(g, cx, cz, key));
    EQUAL(woodNear(g, a, a.y + 1, a.y + 10, 3, wood), size_t(0),
          "not regenerated");
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

TEST(tree_wood_lod_occupancy) {
    // Planning visits only the slabs a source may occupy: it must never
    // miss one the rasterizer fills (any level), and it must be sparse for
    // a branching crown.
    auto instance = std::make_shared<TreeInstance>();
    instance->skeleton = build("trees.redwood");
    instance->anchor = {300, 300, 700};
    finishTreeInstance(*instance);
    const float age = instance->skeleton->matureAge();
    const auto source = tree_wood_lod::voxels({instance, age, 9});
    const auto occupancy = source->occupancy();
    CHECK(occupancy != nullptr);
    const WorldWrap none{};
    for (int level = 0; level <= structure_voxels::MaxLevel; ++level) {
        const auto slabs = structure_voxels::rasterizeAll(*source, level, none);
        CHECK(!slabs.empty());
        for (const auto& [key, slab] : slabs) {
            const auto base = structure_voxels::baseOf(key);
            CHECK(occupancy->contains(level, int(base.x), int(base.y),
                                      int(base.z)));
        }
    }
    const auto& b = source->bounds;
    // A plan with occupancy keeps every filled slab of the plain plan.
    const auto every = structure_voxels::plan(b, 300, 340, 700, none, {});
    const auto sparse =
        structure_voxels::plan(b, 300, 340, 700, none, {}, occupancy.get());
    CHECK(sparse.size() <= every.size());
    // A massive crown is sparse in its box (limbs, not a block of wood).
    {
        auto elder = std::make_shared<TreeInstance>();
        elder->skeleton = build("trees.elder");
        elder->anchor = {4000, 300, 4000};
        finishTreeInstance(*elder);
        const auto voxels =
            tree_wood_lod::voxels({elder, elder->skeleton->matureAge(), 9});
        const auto& e = voxels->bounds;
        const size_t box = size_t((e.x1 - e.x0 + 15) / 16) *
                           size_t((e.y1 - e.y0 + 15) / 16) *
                           size_t((e.z1 - e.z0 + 15) / 16);
        CHECK(voxels->occupancy()->slabs[0].size() * 4 < box);
    }
    std::unordered_set<ChunkCoord> kept;
    for (const auto& p : sparse) kept.insert(p.key);
    for (const auto& p : every)
        if (source->rasterize(structure_voxels::levelOf(p.key), p.sx, p.sy,
                              p.sz))
            CHECK(kept.contains(p.key));
}

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

TEST(tree_settle_closes_gap) {
    // cut.fell = settle (massive trunks): the part above a trunk cut comes
    // down one tile onto the stump instead of being removed, so the sawn
    // ring closes and the tree stays whole, one tile shorter.
    Tree settling = species("trees.test_oak");
    settling.cut.fell = TreeFell::Settle;
    TreeSkeletonInput input;
    input.seed = 7;
    const auto whole = buildTreeSkeleton(settling, input);
    const float age = whole->matureAge();
    // The trunk segment at 3 m: cut on the lower face of the tile row
    // 3..4 m above the anchor point (layer 4).
    int32_t trunk = -1;
    float t = 0;
    for (size_t i = 0; i < whole->segments.size(); ++i) {
        const auto& s = whole->segments[i];
        if (!s.live() || s.order != 0 || s.b.y - s.a.y < 0.5f) continue;
        if (s.a.y <= 3 && s.b.y > 3) {
            trunk = int32_t(i);
            t = (3 - s.a.y) / (s.b.y - s.a.y);
        }
    }
    CHECK(trunk >= 0);
    input.cuts = {{trunk, t, age}};
    const auto settled = buildTreeSkeleton(settling, input);
    CHECK(!settled->dead);
    // No regrowth: the tree is whole.
    for (const auto& s : settled->segments)
        CHECK(!(s.live() && (s.flags & TreeSegment::Sprout) && s.cut == 1));
    CHECK(settled->matureAge() >= age);
    // Before the cut nothing settled exists; from the cut on the ring at
    // layer 4 is wood again.
    const int half = 12;
    EQUAL(countWood(*settled, layer(5, half), age - 1), size_t(0),
          "the settled part appears at the cut");
    EQUAL(countWood(*settled, layer(4, half), age - 1), size_t(0),
          "the stump stops flat at the cut");
    CHECK(countWood(*settled, layer(4, half), age) > 0);
    // treeWoodChanges' rule: wood now but not just before is added into
    // air (the sawn ring), cut wood not wood now is removed (the old top).
    std::vector<uint8_t> now, then, cutWood;
    const auto box = treeWoodBounds(*whole, Origin, true);
    markTreeWood(*settled, Origin, box, age, now);
    markTreeWood(*settled, Origin, box, age - 1e-3f, then);
    markTreeWood(*settled, Origin, box, age, cutWood, {1});
    size_t ringAdded = 0, removed = 0;
    for (int y = box.y0; y < box.y1; ++y)
        for (int z = box.z0; z < box.z1; ++z)
            for (int x = box.x0; x < box.x1; ++x) {
                const size_t m = box.index(x, y, z);
                if (y == Origin.y + 4 && now[m] && !then[m]) ++ringAdded;
                if (cutWood[m] && !now[m]) ++removed;
            }
    CHECK(ringAdded > 0);
    CHECK(removed > 0);
    // One tile shorter: the highest wood row comes down by one.
    const auto top = [&](const TreeSkeleton& k) {
        std::vector<uint8_t> mask;
        markTreeWood(k, Origin, box, age, mask);
        int highest = INT_MIN;
        for (int y = box.y0; y < box.y1; ++y)
            for (int z = box.z0; z < box.z1; ++z)
                for (int x = box.x0; x < box.x1; ++x)
                    if (mask[box.index(x, y, z)]) highest = y;
        return highest;
    };
    EQUAL(top(*settled), top(*whole) - 1, "one tile shorter");
    // Foliage came down with its branches.
    size_t leaves = 0, wholeLeaves = 0;
    for (const auto& f : settled->foliage) leaves += !f.pruned;
    for (const auto& f : whole->foliage) wholeLeaves += !f.pruned;
    EQUAL(leaves, wholeLeaves, "every clump kept");
    // A species that falls loses the part instead.
    Tree falling = species("trees.test_oak");
    const auto fallen = buildTreeSkeleton(falling, input);
    EQUAL(countWood(*fallen, layer(8, half), age), size_t(0),
          "no wood above a fallen cut");
}

TEST(tree_wood_lod_cut_is_local) {
    // A cut changes the voxels only around the cut branch: changedSince
    // bounds every slab that differs, and is much smaller than the tree.
    const auto skeleton = build("trees.redwood");
    const float age = skeleton->matureAge();
    // The trunk near its top (block wood; redwood branches are thinner
    // than a block): cut it there.
    int32_t branch = -1;
    float best = INFINITY;
    for (size_t i = 0; i < skeleton->segments.size(); ++i) {
        const auto& s = skeleton->segments[i];
        if (!s.live() || s.order != 0) continue;
        if (skeleton->radius(s, 0.5f, age) < 1) continue;
        const float off = std::abs(s.a.y - 0.85f * skeleton->height);
        if (off < best) {
            best = off;
            branch = int32_t(i);
        }
    }
    CHECK(branch >= 0);
    CHECK(branch >= 0);
    const float cutAge = age * 2;
    const auto cut =
        build("trees.redwood", 7, TreeMaxHeight, {{branch, 0.5f, cutAge}});
    const auto instance = [&](const TreeSkeletonPtr& k) {
        auto t = std::make_shared<TreeInstance>();
        t->key = 42;
        t->skeleton = k;
        t->anchor = {300, 300, 300};
        t->wood = treeWoodBounds(*k, t->anchor, true);
        return t;
    };
    const auto before = tree_wood_lod::voxels({instance(skeleton), age, 9});
    // Match TreeRenderer: a cut resets the quantized age to its regrowth
    // phase. Different ages must not force all mature wood to rebuild.
    const float phase = cut->growthPhaseStart();
    const float afterAge = tree_foliage::bucketAge(
        tree_foliage::bucket(cutAge, cut->matureAge(), phase), cut->matureAge(),
        phase);
    CHECK(afterAge != age);
    const auto after = tree_wood_lod::voxels({instance(cut), afterAge, 9});
    CHECK(before->revision != after->revision);
    const auto changed = after->changedSince(*before);
    CHECK(changed.has_value());
    CHECK(!changed->empty());
    CHECK(changed->volume() * 3 < before->bounds.volume());
    // Nothing differs outside the box (level 0: the real rasterizer).
    const WorldWrap none{};
    const auto a = structure_voxels::rasterizeAll(*before, 0, none);
    const auto b = structure_voxels::rasterizeAll(*after, 0, none);
    size_t differing = 0;
    for (const auto& [key, slab] : a) {
        const auto other = b.find(key);
        const bool same = other != b.end() && other->second.bits == slab.bits;
        if (same) continue;
        ++differing;
        const TileBox box{int(key.x) * 16,     int(key.y) * 16,
                          int(key.z) * 16,     int(key.x + 1) * 16,
                          int(key.y + 1) * 16, int(key.z + 1) * 16};
        CHECK(!changed->intersect(box).empty());
    }
    for (const auto& [key, slab] : b)
        if (!a.contains(key)) {
            const TileBox box{int(key.x) * 16,     int(key.y) * 16,
                              int(key.z) * 16,     int(key.x + 1) * 16,
                              int(key.y + 1) * 16, int(key.z + 1) * 16};
            CHECK(!changed->intersect(box).empty());
        }
    CHECK(differing > 0);
    // Age changes on the same mature skeleton require no wood rebuild.
    const auto mature = tree_wood_lod::voxels({instance(skeleton), age * 2, 9});
    const auto unchanged = mature->changedSince(*before);
    CHECK(unchanged.has_value() && unchanged->empty());
}
