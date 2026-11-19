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
#include <climits>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "flora_assets.hpp"
#include "test_assets.hpp"
#include "world/chunk_manager.hpp"
#include "world/sky_occlusion_map.hpp"
#include "world/structure_voxels.hpp"
#include "world/sunlight_propagator.hpp"
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
    // TREE_BENCH_LIMB_LIGHT: sunlight under a sawn limb (created before any
    // chunk loads, so it knows every chunk).
    std::unique_ptr<SunLightPropagator> sunlight;
    if (std::getenv("TREE_BENCH_LIMB_LIGHT"))
        sunlight = std::make_unique<SunLightPropagator>(
            chunks, assets.getStorage<Block>(), pool);
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
                // registers whole regions); with sunlight every slab is
                // registered, so it can load the sky slabs it needs.
                for (int y = 0; y < (sunlight ? Y_CHUNKS : 1); ++y)
                    if (!chunks.isRegistered(
                            {unsigned(c), unsigned(y), unsigned(d)}))
                        chunks.registerChunk(
                            {unsigned(c), unsigned(y), unsigned(d)}, origin,
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
            if (sunlight) sunlight->update();
            pool.wait_for(std::chrono::milliseconds(20));
        } while (chunks.hasGenerationWork() ||
                 (sunlight && !sunlight->isIdle()));
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

    WorldEditor editor(chunks, sunlight.get(), assets.getStorage<Block>());
    TreeSimulation sim(chunks, editor);
    sim.update(today);
    const uint8_t wood = chunks.treeSpecies().find(tree->species)->woodBlock;

    // 0. TREE_BENCH_LIMB_LIGHT=1: saw through the thickest primary limb near
    // the trunk with the ground under its middle loaded as in play, then
    // check that the air it shaded gets full sky (and columns loaded only
    // afterwards too).
    if (sunlight) {
        // The thickest primary limb reaching sideways (not the leader).
        int32_t limb = -1;
        for (size_t i = 0; i < k.segments.size(); ++i) {
            const auto& s = k.segments[i];
            const TreeVec dir = (s.b - s.a).normalized();
            if (s.live() && s.order == 1 && s.parent >= 0 &&
                k.segments[size_t(s.parent)].order == 0 &&
                std::abs(dir.y) < 0.8f &&
                (limb < 0 || s.r0 > k.segments[size_t(limb)].r0))
                limb = int32_t(i);
        }
        if (limb < 0) return 1;
        // Follow the limb's axis outward; saw the first part clear of the
        // flared trunk, and sample the ground under the parts after it.
        std::vector<TreeVec> under;
        // Clear of the trunk at the limb's height.
        const float clear =
            k.segments[size_t(k.segments[size_t(limb)].parent)].r0 +
            k.segments[size_t(limb)].r0;
        int32_t sawn = -1;
        for (int32_t at = limb; at >= 0;) {
            const auto& seg = k.segments[size_t(at)];
            if (std::getenv("TREE_BENCH_LIMB_CHAIN"))
                std::cout << "chain seg=" << at << " r0=" << seg.r0
                          << " a=" << seg.a.x << ',' << seg.a.y << ','
                          << seg.a.z << " len=" << (seg.b - seg.a).length()
                          << '\n';
            if (sawn >= 0)
                under.push_back(lerp(seg.a, seg.b, 0.5f));
            else if (std::hypot(seg.a.x, seg.a.z) > clear)
                sawn = at;
            int32_t next = -1;
            for (size_t i = 0; i < k.segments.size(); ++i)
                if (k.segments[i].parent == at && k.segments[i].attachT >= 1 &&
                    k.segments[i].live())
                    next = int32_t(i);
            at = next;
        }
        if (sawn < 0 || under.empty()) return 1;
        const auto& s = k.segments[size_t(sawn)];
        const auto groundAt = [&](const TreeVec& p) {
            return chunks.generatedSurfaceY(unsigned(anchor.x + p.x),
                                            unsigned(anchor.z + p.z));
        };
        // Load like the game: chunk radius 8 around a camera on the ground
        // under the first sample, up to the limb.
        const TreeVec near = under[std::min<size_t>(3, under.size() - 1)];
        const int gy = groundAt(near);
        std::cout << "limb r0=" << s.r0 << " r1=" << s.r1
                  << " parts=" << under.size() << " at=" << near.x << ','
                  << near.y << ',' << near.z << " ground=" << gy
                  << " limb_y=" << anchor.y + near.y << '\n';
        load(anchor.x + int(near.x), anchor.z + int(near.z), 8, gy / 16 - 8,
             gy / 16 + 8);
        const auto sunUnder = [&](const TreeVec& p) {
            const int x = anchor.x + int(std::lround(p.x)),
                      z = anchor.z + int(std::lround(p.z));
            const int y = groundAt(p) + 3;
            const Tile* t = chunks.tileAt(x, y, z);
            return t && t->isAir() ? int(t->lightLevel.sun_exposure) : -1;
        };
        for (size_t i = 0; i < under.size(); ++i)
            std::cout << "before part=" << i << " sun=" << sunUnder(under[i])
                      << '\n';
        // Saw through the limb at a quarter of its first segment: every wood
        // tile in the slice across its dominant axis.
        const TreeVec dir = (s.b - s.a).normalized();
        const float d[3]{dir.x, dir.y, dir.z};
        int dim = 0;
        for (int i = 1; i < 3; ++i)
            if (std::abs(d[i]) > std::abs(d[dim])) dim = i;
        const TreeVec cutAt = lerp(s.a, s.b, 0.5f);
        const int reach = int(std::ceil(std::max(s.r0, s.r1))) + 4;
        int c[3]{anchor.x + int(std::lround(cutAt.x)),
                 anchor.y + int(std::lround(cutAt.y + 0.5f)),
                 anchor.z + int(std::lround(cutAt.z))};
        print("load_limb_cut",
              load(c[0], c[2], reach / 16 + 1, c[1] / 16 - reach / 16 - 1,
                   c[1] / 16 + reach / 16 + 1));
        std::vector<TilePos> slice;
        // TREE_BENCH_SLANT: a slanted slit (normal 35 degrees off the axis)
        // as a digital plane thick enough to separate through faces.
        const bool slant = std::getenv("TREE_BENCH_SLANT") != nullptr;
        TreeVec slitNormal = dir;
        if (slant) {
            const TreeVec side = std::abs(dir.y) < 0.9f
                                     ? dir.cross(TreeVec{0, 1, 0}).normalized()
                                     : dir.cross(TreeVec{1, 0, 0}).normalized();
            slitNormal = (dir * 0.82f + side * 0.57f).normalized();
        }
        const float thick =
            0.5f * (std::abs(slitNormal.x) + std::abs(slitNormal.y) +
                    std::abs(slitNormal.z));
        // Signed distance past the slit (toward the limb's far side).
        const auto slitDistance = [&](int x, int y, int z) {
            const TreeVec q{float(x - c[0]), float(y - c[1]), float(z - c[2])};
            if (!slant) {
                const float p[3]{q.x, q.y, q.z};
                return p[dim] * (d[dim] < 0 ? -1.f : 1.f);
            }
            return q.dot(slitNormal);
        };
        for (int y = c[1] - 2 * reach; y <= c[1] + 2 * reach; ++y)
            for (int z = c[2] - 2 * reach; z <= c[2] + 2 * reach; ++z)
                for (int x = c[0] - 2 * reach; x <= c[0] + 2 * reach; ++x) {
                    if (!slant) {
                        const int p[3]{x - c[0], y - c[1], z - c[2]};
                        if (p[dim] != 0 || std::abs(p[(dim + 1) % 3]) > reach ||
                            std::abs(p[(dim + 2) % 3]) > reach)
                            continue;
                    } else if (std::abs(slitDistance(x, y, z)) > thick) {
                        continue;
                    }
                    if (const Tile* t = chunks.tileAt(x, y, z);
                        t && t->block_id == wood)
                        slice.push_back({x, y, z});
                }
        const size_t cutsBefore = sim.cutCount();
        double worst = 0, total = 0;
        size_t cutBlock = 0, worstBlock = 0;
        TreeSimulation::BreakTimes worstTimes;
        std::vector<double> times;
        for (size_t i = 0; i < slice.size(); ++i) {
            const auto started = Clock::now();
            editor.removeBlock(slice[i]);
            const double ms = msSince(started);
            total += ms;
            times.push_back(ms);
            if (ms > worst) {
                worst = ms;
                worstBlock = i + 1;
                worstTimes = sim.lastBreak();
            }
            if (!cutBlock && sim.cutCount() > cutsBefore) cutBlock = i + 1;
        }
        std::cout << "limb_saw slant=" << slant << " tiles=" << slice.size()
                  << " cuts=" << sim.cutCount() - cutsBefore
                  << " cut_at_block=" << cutBlock << " mean_ms="
                  << (slice.empty() ? 0 : total / double(slice.size()))
                  << " worst_ms=" << worst << '\n';
        std::sort(times.begin(), times.end());
        if (!times.empty())
            std::cout << "limb_saw p99_ms=" << times[times.size() * 99 / 100]
                      << " worst_block=" << worstBlock
                      << " owner=" << worstTimes.owner
                      << " severed=" << worstTimes.severed
                      << " rebuild=" << worstTimes.rebuild
                      << " commit=" << worstTimes.commit
                      << " clear=" << worstTimes.clear << '\n';
        for (int i = 0; i < 4000 && sim.pendingClears(); ++i) {
            chunks.update();
            sunlight->update();
            pool.wait_for(std::chrono::milliseconds(5));
            sim.finishClears();
        }
        load(anchor.x + int(near.x), anchor.z + int(near.z), 8, gy / 16 - 8,
             gy / 16 + 8);
        // Wood left beyond the slit around the cut (the limb's far side).
        size_t beyond = 0;
        for (int y = c[1] - 2 * reach; y <= c[1] + 2 * reach; ++y)
            for (int z = c[2] - 2 * reach; z <= c[2] + 2 * reach; ++z)
                for (int x = c[0] - 2 * reach; x <= c[0] + 2 * reach; ++x)
                    if (slitDistance(x, y, z) > thick)
                        if (const Tile* t = chunks.tileAt(x, y, z);
                            t && t->block_id == wood &&
                            TreeVec{float(x - c[0]), float(y - c[1]),
                                    float(z - c[2])}
                                    .dot(dir) > 0 &&
                            (TreeVec{float(x - c[0]), float(y - c[1]),
                                     float(z - c[2])} -
                             dir * TreeVec{float(x - c[0]), float(y - c[1]),
                                           float(z - c[2])}
                                       .dot(dir))
                                    .length() < float(reach))
                            ++beyond;
        std::cout << "limb_wood_beyond_slit=" << beyond << '\n';
        for (size_t i = 0; i < under.size(); ++i)
            std::cout << "after part=" << i << " sun=" << sunUnder(under[i])
                      << '\n';
        // Columns loaded only now, under the far part of the limb.
        const TreeVec far = under.back();
        const int fy = groundAt(far);
        load(anchor.x + int(far.x), anchor.z + int(far.z), 2, fy / 16 - 2,
             fy / 16 + 2);
        std::cout << "later far sun=" << sunUnder(far) << '\n';
        return 0;
    }

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
        // As StructureLod: only slabs the new revision may occupy.
        const auto occupancy = next->occupancy();
        const auto planned = structure_voxels::plan(
            next->bounds, anchor.x + at.x, anchor.y + at.y, anchor.z + at.z,
            world.wrap(), {}, occupancy.get());
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

    // 3b. Planning the distant voxels near the trunk (StructureLod, main
    // thread): every slab of the wood box, or only the occupied ones
    // (structure_voxels::Source::occupancy, built once on the pool).
    {
        const auto source = tree_wood_lod::voxels({tree, age, wood});
        auto started = Clock::now();
        const auto occupancy = source->occupancy();
        const double built = msSince(started);
        started = Clock::now();
        const auto every =
            structure_voxels::plan(source->bounds, anchor.x + 300,
                                   anchor.y + 20, anchor.z, world.wrap(), {});
        const double plainMs = msSince(started);
        started = Clock::now();
        const auto sparse = structure_voxels::plan(
            source->bounds, anchor.x + 300, anchor.y + 20, anchor.z,
            world.wrap(), {}, occupancy.get());
        const double sparseMs = msSince(started);
        size_t occupied = 0;
        for (const auto& level : occupancy->slabs) occupied += level.size();
        std::cout << "voxel_plan occupancy_ms=" << built
                  << " occupied_slabs=" << occupied
                  << " planned_all=" << every.size()
                  << " plan_all_ms=" << plainMs
                  << " planned_occupied=" << sparse.size()
                  << " plan_occupied_ms=" << sparseMs << '\n';
        // 3c. What the tree casts on the ground (step 2 of
        // docs/handoff/structures.md): its footprint (pool, once per
        // revision) and a sky occlusion map around it (pool, on recentre).
        started = Clock::now();
        const auto cast = structure_voxels::footprint(*source, occupancy.get());
        const double footprintMs = msSince(started);
        started = Clock::now();
        SkyOcclusionMap map;
        map.build({cast}, anchor.x, anchor.z, world.wrap().tiles);
        std::cout << "sky_occlusion footprint_ms=" << footprintMs
                  << " columns=" << cast->tops.size()
                  << " map_ms=" << msSince(started)
                  << " occluded_texels=" << map.occluded() << '\n';
        // 3d. The tree seen from the sky (docs/handoff/sky-light.md): its
        // wood tops for the whole box at once, and per chunk column as
        // SkyHeightMap builds them (the trunk's column and the mean).
        {
            std::vector<int> tops;
            const auto box = tree->wood;
            started = Clock::now();
            source->skyTops(box, tops);
            const double wholeMs = msSince(started);
            size_t columns = 0;
            for (const int t : tops) columns += t != INT_MIN;
            double worst = 0, sum = 0;
            int chunkColumns = 0;
            for (int cz = box.z0 / 16; cz <= (box.z1 - 1) / 16; ++cz)
                for (int cx = box.x0 / 16; cx <= (box.x1 - 1) / 16; ++cx) {
                    const TileBox part{cx * 16,      box.y0, cz * 16,
                                       cx * 16 + 16, box.y1, cz * 16 + 16};
                    const auto t0 = Clock::now();
                    source->skyTops(part, tops);
                    const double ms = msSince(t0);
                    worst = std::max(worst, ms);
                    sum += ms;
                    ++chunkColumns;
                }
            std::cout << "sky_tops whole_ms=" << wholeMs
                      << " wood_columns=" << columns
                      << " chunk_columns=" << chunkColumns
                      << " per_column_mean_ms=" << sum / chunkColumns
                      << " per_column_max_ms=" << worst << '\n';
        }
        if (std::getenv("TREE_BENCH_PLAN_ONLY")) return 0;
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
