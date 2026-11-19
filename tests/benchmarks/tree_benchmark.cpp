// The elder: the tree optimization sample (docs/handoff/trees.md). Builds one
// massive tree in a streamed test world and times what the game does with
// it on the main thread and the pool: breaking one block of its wood
// (trunk surface, no cut), cutting a thin branch, its crown meshes and its
// distant voxels.
//
//   build/tests/tree_benchmark [crown_m height_m trunk_m]
//
// Without arguments the shipped elder (assets/data/trees/trees.json) is
// used; arguments override its size (maxima of the ranges).

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "flora_assets.hpp"
#include "test_assets.hpp"
#include "world/chunk_manager.hpp"
#include "world/structure_voxels.hpp"
#include "world/tree_foliage_builder.hpp"
#include "world/tree_simulation.hpp"
#include "world/tree_wood_lod.hpp"
#include "world/world_editor.hpp"

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
        .count();
}

World forestWorld(int width, int height) {
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

void print(const char* what, double ms) {
    std::cout << what << "_ms=" << ms << '\n';
}
void print(const char* what, const TreeSimulation::BreakTimes& t,
           double total) {
    std::cout << what << "_ms=" << total << " owner=" << t.owner
              << " severed=" << t.severed << " rebuild=" << t.rebuild
              << " commit=" << t.commit << " clear=" << t.clear << '\n';
}
}  // namespace

int main(int argc, char** argv) {
    test::FloraAssets assets;
    test::loadAllContent(assets);
    auto& trees = const_cast<DataStorage<Tree>&>(assets.getStorage<Tree>());
    Tree* elder = nullptr;
    for (auto& t : trees.items)
        if (t.landmark()) elder = &t;
    if (!elder) {
        std::cerr << "no massive tree species\n";
        return 1;
    }
    if (argc > 1) elder->size.crown_radius_m.max = std::stof(argv[1]);
    if (argc > 2) elder->size.height_m.max = std::stof(argv[2]);
    if (argc > 3) elder->size.trunk_radius_m.max = std::stof(argv[3]);
    elder->size.crown_radius_m.min = elder->size.crown_radius_m.max;
    elder->size.height_m.min = elder->size.height_m.max;
    // One per small map.
    elder->placement.density_per_100m2 = 0.05f;
    elder->spacing_m = 1e9f;
    std::cout << "species=" << elder->name
              << " crown_m=" << elder->size.crown_radius_m.max
              << " height_m=" << elder->size.height_m.max
              << " trunk_m=" << elder->size.trunk_radius_m.max << '\n';

    // Wide enough for the crown on both sides of the anchor.
    const int regions = std::max(
        8, 2 * int(std::ceil(elder->size.crown_radius_m.max / 128)) + 4);
    const World world = forestWorld(regions, regions);
    LocationGenerator generator;
    ThreadPool pool(4);
    ChunkMesher mesher(assets);
    ChunkManager chunks(generator, pool, &mesher, world, &assets);
    if (chunks.treeLandmarks().empty()) {
        std::cerr << "no landmark placed\n";
        return 1;
    }
    const auto site = chunks.treeLandmarks().front();

    // Streams slabs y0..y1 of a square of columns around (x, z).
    const auto load = [&](int x, int z, int radius, int y0, int y1) {
        const auto started = Clock::now();
        const int cx = x / 16, cz = z / 16;
        for (int dz = -radius; dz <= radius; ++dz)
            for (int dx = -radius; dx <= radius; ++dx) {
                const int c = cx + dx, d = cz + dz;
                if (c < 0 || d < 0 || c >= regions * 8 || d >= regions * 8)
                    continue;
                const auto& region =
                    world.regions[size_t(d / 8) * regions + size_t(c / 8)];
                const ChunkCoord origin{unsigned(c / 8 * 8), 0,
                                        unsigned(d / 8 * 8)};
                // Slab 0 stands for the column (as LocationManager
                // registers whole regions).
                if (!chunks.isRegistered({unsigned(c), 0, unsigned(d)}))
                    chunks.registerChunk({unsigned(c), 0, unsigned(d)}, origin,
                                         &region);
                for (int y = std::max(0, y0); y <= std::min(y1, Y_CHUNKS - 1);
                     ++y) {
                    const ChunkCoord coord{unsigned(c), unsigned(y),
                                           unsigned(d)};
                    if (!chunks.isRegistered(coord))
                        chunks.registerChunk(coord, origin, &region);
                    chunks.request(coord);
                }
            }
        do {
            chunks.update();
            pool.wait_for(std::chrono::milliseconds(20));
        } while (chunks.hasGenerationWork());
        return msSince(started);
    };

    // The trunk base first: it holds the landmark's column entry.
    const int surface =
        chunks.generatedSurfaceY(unsigned(site.x), unsigned(site.z));
    const double baseLoad =
        load(int(site.x), int(site.z), 2, surface / 16 - 1, surface / 16 + 3);
    const auto column =
        chunks.treeColumn(unsigned(site.x / 16), unsigned(site.z / 16));
    TreeInstancePtr tree;
    TreeAnchor anchor;
    for (const auto& e : column ? column->trees : decltype(column->trees){})
        if (chunks.treeSpecies().find(e.tree->species)->landmark) {
            tree = e.tree;
            anchor = e.anchor;
        }
    if (!tree) {
        std::cerr << "landmark not in its column\n";
        return 1;
    }
    const auto& k = *tree->skeleton;
    const double today = 0;
    const float age = tree->growthAge(today);
    size_t woodSegments = 0, thin = 0;
    for (const auto& s : k.segments)
        if (s.live() && std::max(s.r0, s.r1) >= TreeVoxelRadius) {
            ++woodSegments;
            if (std::max(s.r0, s.r1) <= 2) ++thin;
        }
    std::cout << "segments=" << k.segments.size()
              << " wood_segments=" << woodSegments << " thin_wood=" << thin
              << " foliage_sites=" << k.foliage.size()
              << " wood_box=" << tree->wood.sizeX() << 'x' << tree->wood.sizeY()
              << 'x' << tree->wood.sizeZ() << " age=" << age
              << " mature=" << k.matureAge() << '\n';
    print("load_base", baseLoad);

    WorldEditor editor(chunks, nullptr, assets.getStorage<Block>());
    TreeSimulation sim(chunks, editor);
    sim.update(today);
    const uint8_t wood = chunks.treeSpecies().find(tree->species)->woodBlock;

    // 1. One block off the trunk surface, a few tiles up: no cut.
    {
        const int y = anchor.y + 4;
        int x = anchor.x;
        while (x < anchor.x + 200) {
            const Tile* t = chunks.tileAt(x + 1, y, anchor.z);
            if (!t || t->block_id != wood) break;
            ++x;
        }
        const Tile* t = chunks.tileAt(x, y, anchor.z);
        if (t && t->block_id == wood) {
            const auto started = Clock::now();
            editor.removeBlock({x, y, anchor.z});
            print("break_trunk_surface", sim.lastBreak(), msSince(started));
        } else {
            std::cout << "break_trunk_surface: no wood found\n";
        }
    }

    // A large limb near the trunk: benchmark the mixed-resolution proxy
    // rebuild using the same quantized ages as TreeRenderer.
    if (std::getenv("TREE_BENCH_LARGE_CUT")) {
        int32_t limb = -1;
        for (size_t i = 0; i < k.segments.size(); ++i) {
            const auto& s = k.segments[i];
            if (s.live() && s.order == 1 && s.parent >= 0 &&
                k.segments[size_t(s.parent)].order == 0 &&
                (limb < 0 || s.r0 > k.segments[size_t(limb)].r0))
                limb = int32_t(i);
        }
        if (limb < 0) return 1;
        auto after = std::make_shared<TreeInstance>(*tree);
        after->cuts.push_back({limb, 0.25f, tree->age(today)});
        after->skeleton = applyTreeCuts(k, *elder, after->cuts);
        finishTreeInstance(*after);
        const auto quantized = [age](const TreeSkeleton& shape) {
            const float start = shape.growthPhaseStart();
            return tree_foliage::bucketAge(
                tree_foliage::bucket(age, shape.matureAge(), start),
                shape.matureAge(), start);
        };
        const auto before = tree_wood_lod::voxels({tree, quantized(k), wood});
        const auto next =
            tree_wood_lod::voxels({after, quantized(*after->skeleton), wood});
        const auto changed = next->changedSince(*before);
        const TreeVec at =
            lerp(k.segments[size_t(limb)].a, k.segments[size_t(limb)].b, 0.25f);
        const auto planned = structure_voxels::plan(
            next->bounds, anchor.x + at.x, anchor.y + at.y, anchor.z + at.z,
            world.wrap(), {});
        const auto started = Clock::now();
        size_t touched = 0, occupied = 0;
        for (const auto& p : planned) {
            if (changed &&
                changed->intersect(structure_voxels::boxOf(p)).empty())
                continue;
            ++touched;
            occupied += next->rasterize(structure_voxels::levelOf(p.key), p.sx,
                                        p.sy, p.sz)
                            .has_value();
        }
        std::cout << "large_limb_cut local=" << changed.has_value()
                  << " rebuilt_slabs=" << touched << '/' << planned.size()
                  << " occupied=" << occupied
                  << " worker_ms=" << msSince(started) << '\n';
        return 0;
    }

    // 2. A thin branch (radius <= 2): any removal cuts it. The highest
    // grown one with its middle tile loaded after streaming its area.
    const TreeSegment* branch = nullptr;
    for (const auto& s : k.segments)
        if (s.live() && k.visible(s, age) >= 1 &&
            k.radius(s, 0.5f, age) >= TreeVoxelRadius &&
            k.radius(s, 0.5f, age) <= 2 && (!branch || s.a.y > branch->a.y))
            branch = &s;
    if (branch) {
        const TreeVec m = lerp(branch->a, branch->b, 0.5f);
        const int bx = anchor.x + int(std::lround(m.x)),
                  by = anchor.y + int(std::lround(m.y + 0.5f)),
                  bz = anchor.z + int(std::lround(m.z));
        print("load_branch", load(bx, bz, 1, by / 16 - 1, by / 16 + 1));
        sim.update(today);
        // The nearest wood tile to the branch middle.
        std::optional<TilePos> hit;
        for (int r = 0; r <= 2 && !hit; ++r)
            for (int dy = -r; dy <= r && !hit; ++dy)
                for (int dz = -r; dz <= r && !hit; ++dz)
                    for (int dx = -r; dx <= r && !hit; ++dx)
                        if (const Tile* t =
                                chunks.tileAt(bx + dx, by + dy, bz + dz);
                            t && t->block_id == wood)
                            hit = TilePos{bx + dx, by + dy, bz + dz};
        if (hit) {
            const size_t before = sim.cutCount();
            const auto started = Clock::now();
            editor.removeBlock(*hit);
            print("cut_thin_branch", sim.lastBreak(), msSince(started));
            std::cout << "cut=" << (sim.cutCount() > before) << '\n';
            const size_t pending = sim.pendingClears();
            const auto clearing = Clock::now();
            sim.finishClears();
            std::cout << "branch_clears chunks=" << pending
                      << " ms=" << msSince(clearing) << '\n';
            // Distant voxels after the cut: only slabs near it rebuild.
            TreeInstancePtr after;
            chunks.forEachTree([&](const TreeInstancePtr& t) {
                if (t->key == tree->key) after = t;
            });
            if (after) {
                const auto oldVoxels =
                    tree_wood_lod::voxels({tree, k.matureAge(), wood});
                const auto& shape = *after->skeleton;
                const float start = shape.growthPhaseStart();
                const float quantized = tree_foliage::bucketAge(
                    tree_foliage::bucket(age, shape.matureAge(), start),
                    shape.matureAge(), start);
                const auto newVoxels =
                    tree_wood_lod::voxels({after, quantized, wood});
                const auto box = newVoxels->changedSince(*oldVoxels);
                const auto all = structure_voxels::plan(
                    newVoxels->bounds, anchor.x, anchor.y + 20, anchor.z,
                    world.wrap(), {});
                size_t touched = 0;
                for (const auto& p : all)
                    touched +=
                        box &&
                        !box->intersect(structure_voxels::boxOf(p)).empty();
                std::cout << "cut_voxels local=" << box.has_value()
                          << " rebuilt_slabs=" << (box ? touched : all.size())
                          << " of=" << all.size() << '\n';
            }
            if (std::getenv("TREE_BENCH_CUT_ONLY")) return 0;
        } else {
            std::cout << "cut_thin_branch: no wood found\n";
        }
    }

    // 2b. TREE_BENCH_SAW=1: saw through the trunk a few tiles up (every
    // wood tile of one row), as a player felling it would; a settling
    // trunk then comes down onto the stump.
    if (std::getenv("TREE_BENCH_SAW") || std::getenv("TREE_BENCH_SAW_ONLY")) {
        const int y = anchor.y + 4;
        const int reach = int(std::ceil(elder->size.trunk_radius_m.max *
                                        (1 + elder->form_params.root_flare))) +
                          4;
        print("load_saw",
              load(anchor.x, anchor.z, reach / 16 + 1, y / 16 - 1, y / 16 + 1));
        sim.update(today);
        std::vector<TilePos> ring;
        for (int z = anchor.z - reach; z <= anchor.z + reach; ++z)
            for (int x = anchor.x - reach; x <= anchor.x + reach; ++x)
                if (const Tile* t = chunks.tileAt(x, y, z);
                    t && t->block_id == wood)
                    ring.push_back({x, y, z});
        const size_t before = sim.cutCount();
        double worst = 0, sum = 0;
        TreeSimulation::BreakTimes last;
        for (const auto& p : ring) {
            const auto started = Clock::now();
            const size_t cutsBefore = sim.cutCount();
            editor.removeBlock(p);
            const double ms = msSince(started);
            if (sim.cutCount() != cutsBefore)
                std::cout << "cut_block ms=" << ms
                          << " owner=" << sim.lastBreak().owner
                          << " severed=" << sim.lastBreak().severed
                          << " rebuild=" << sim.lastBreak().rebuild
                          << " commit=" << sim.lastBreak().commit
                          << " queue=" << sim.lastBreak().clear
                          << " pending=" << sim.pendingClears() << " at "
                          << p.x - anchor.x << ',' << p.y - anchor.y << ','
                          << p.z - anchor.z << '\n';
            sum += ms;
            if (ms > worst) {
                worst = ms;
                last = sim.lastBreak();
            }
        }
        std::cout << "saw_ring tiles=" << ring.size() << " total_ms=" << sum
                  << " mean_ms=" << (ring.empty() ? 0 : sum / ring.size())
                  << " cuts=" << (sim.cutCount() - before) << '\n';
        print("saw_worst_block", last, worst);
        const size_t pending = sim.pendingClears();
        const auto started = Clock::now();
        // Columns may still be rebuilding their tree lists: keep streaming.
        for (int i = 0; i < 2000 && sim.pendingClears(); ++i) {
            chunks.update();
            pool.wait_for(std::chrono::milliseconds(5));
            sim.finishClears();
        }
        std::cout << "settle_clears chunks=" << pending
                  << " ms=" << msSince(started) << '\n';
        size_t refilled = 0;
        for (const auto& p : ring)
            if (const Tile* t = chunks.tileAt(p.x, p.y, p.z);
                t && t->block_id == wood)
                ++refilled;
        std::cout << "ring_refilled=" << refilled << '/' << ring.size() << '\n';
        if (std::getenv("TREE_BENCH_SAW_ONLY")) return 0;
    }

    // 3. Crown meshes (TreeRenderer builds them on the main thread).
    const auto& rule = *chunks.treeSpecies().find(tree->species);
    const auto& materials = assets.getStorage<FoliageMaterial>();
    for (const auto detail :
         {tree_foliage::Detail::Near, tree_foliage::Detail::Far,
          tree_foliage::Detail::DistantCrown, tree_foliage::Detail::Distant}) {
        const auto started = Clock::now();
        const auto parts = tree_foliage::buildParts(
            *tree->skeleton, rule.species, materials, age, detail, false);
        size_t vertices = 0;
        for (const auto& part : parts) vertices += part.vertices.size();
        std::cout << "crown_mesh detail=" << int(detail)
                  << " ms=" << msSince(started) << " parts=" << parts.size()
                  << " vertices=" << vertices << '\n';
    }

    // 4. Distant voxels: every slab at each level (StructureLod plans a mix).
    const auto source = tree_wood_lod::voxels({tree, age, wood});
    for (int level = 0; level <= structure_voxels::MaxLevel; ++level) {
        const auto started = Clock::now();
        const auto slabs =
            structure_voxels::rasterizeAll(*source, level, world.wrap());
        size_t enclosed = 0;
        for (const auto& [key, slab] : slabs) enclosed += slab.enclosed;
        std::cout << "voxels level=" << level << " ms=" << msSince(started)
                  << " slabs=" << slabs.size() << " enclosed=" << enclosed
                  << '\n';
    }
    pool.wait();
    return 0;
}
