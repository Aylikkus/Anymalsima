#include "tree_generator.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

#include "generated_hydrology.hpp"
#include "ground_cover_generator.hpp"
#include "items/rock.hpp"
#include "items/soil.hpp"
#include "tree_wood_lod.hpp"

namespace {
constexpr double Area = 100.0;  // m² of density_per_100m2
constexpr int RegionTiles = TerrainTuning::RegionX;

uint32_t hash32(double unit) { return uint32_t(unit * 4294967295.0); }

void attachRoots(TreeInstance& tree, const Tree& species) {
    tree.roots = buildTreeRoots(species, *tree.skeleton, tree.seed);
    if (!tree.roots) return;
    tree.rootWood = treeWoodBounds(*tree.roots, tree.anchor);
    const auto& base = tree.roots->segments.front();
    tree.rootReach = std::max(species.roots.reach_m,
                              base.r0 * (1 + tree.roots->flare.amount) +
                                  std::hypot(base.a.x, base.a.z) + 0.5f);
    const int reach = int(std::floor(tree.rootReach));
    const auto& a = tree.anchor;
    tree.rootWood.x0 = std::max(tree.rootWood.x0, a.x - reach);
    tree.rootWood.x1 = std::min(tree.rootWood.x1, a.x + reach + 1);
    tree.rootWood.z0 = std::max(tree.rootWood.z0, a.z - reach);
    tree.rootWood.z1 = std::min(tree.rootWood.z1, a.z + reach + 1);
    tree.rootWood.y0 =
        std::max({0, tree.rootWood.y0,
                  tree.anchor.y - int(std::ceil(species.roots.depth_m)) + 1});
    const int rise =
        species.landmark()
            ? int(std::ceil(base.r0 * (1 + tree.roots->flare.amount)))
            : 0;
    tree.rootWood.y1 = tree.anchor.y + rise + 1;
}
}  // namespace

TreeSpeciesTable::TreeSpeciesTable(const IAssets& assets) : siteTable(assets) {
    if (assets.hasStorage<Block>())
        grownBlocks = naturalGrowthBlocks(assets.getStorage<Block>().items);
    if (!assets.hasStorage<Tree>() || !assets.hasStorage<Block>()) return;
    const auto& all = assets.getStorage<Tree>();
    const auto& blocks = assets.getStorage<Block>();
    const auto addSubstrate = [&](const std::string& name) {
        if (const auto it = blocks.indexByName.find(name);
            it != blocks.indexByName.end() && !blocks.items[it->second].light)
            rootSubstrates.set(blocks.items[it->second].id);
    };
    if (assets.hasStorage<Rock>())
        for (const auto& rock : assets.getStorage<Rock>().items)
            addSubstrate(rock.block_name);
    if (assets.hasStorage<Soil>())
        for (const auto& soil : assets.getStorage<Soil>().items) {
            addSubstrate(soil.block_name);
            if (soil.topsoil_block_name) addSubstrate(*soil.topsoil_block_name);
        }
    byId.assign(all.items.size(), -1);
    for (const auto& tree : all.items) {
        TreeSpeciesRule r;
        r.id = tree.id;
        r.seed = GroundCoverGenerator::nameHash(tree.name);
        r.species = tree;
        r.landmark = tree.landmark();
        floraPlacementRule(tree.placement, assets, r.site);
        r.site.id = tree.id;
        r.site.seed = r.seed;
        // One candidate per cell, at most one tree per spacing.
        const double perTree = Area / std::max(1e-9f, r.site.density.max);
        r.cell = std::max({1, int(std::lround(std::sqrt(perTree))),
                           int(std::ceil(tree.spacing_m.min))});
        if (const auto it = blocks.indexByName.find(tree.wood.block);
            it != blocks.indexByName.end())
            r.woodBlock = blocks.items[it->second].id;
        if (const auto it = blocks.indexByName.find(tree.wood.inner_block);
            it != blocks.indexByName.end())
            r.innerWoodBlock = blocks.items[it->second].id;
        if (!r.landmark) {
            extent = std::max(
                {extent, double(tree.extent()), double(tree.roots.reach_m)});
            spacing = std::max(spacing, double(tree.spacing_m.max));
        } else {
            landmarkExtent = std::max({landmarkExtent, double(tree.extent()),
                                       double(tree.roots.reach_m)});
        }
        byId[tree.id - 1] = int(rules.size());
        rules.push_back(std::move(r));
    }
}

TreeSkeletonPtr TreeSpeciesTable::variant(uint8_t species, int variant,
                                          int rotation) const {
    const auto* rule = find(species);
    if (!rule) return nullptr;
    const uint64_t key =
        uint64_t(species) << 8 | uint64_t(variant & 63) << 2 | (rotation & 3);
    return variants->get(key, [&] {
        TreeSkeletonInput input;
        input.seed = rule->seed ^ uint32_t(variant + 1) * 0x9e3779b9u;
        input.rotation = rotation;
        return buildTreeSkeleton(rule->species, input);
    });
}

std::vector<TreeLandmark> placeTreeLandmarks(const TreeSpeciesTable& table,
                                             const World& world,
                                             uint32_t seed) {
    std::vector<TreeLandmark> sites;
    if (world.regions.empty()) return sites;
    const double regionArea = double(RegionTiles) * RegionTiles / Area;
    for (const auto& r : table.species()) {
        if (!r.landmark) continue;
        const uint32_t s = seed ^ r.seed;
        for (int rz = 0; rz < world.height; ++rz)
            for (int rx = 0; rx < world.width; ++rx) {
                const Region& region =
                    world.regions[size_t(rz) * world.width + rx];
                if (!world.hydrology && region.land_height_m < SEA_LEVEL_Y)
                    continue;
                if (!r.site.vegetation.empty() &&
                    std::find(r.site.vegetation.begin(),
                              r.site.vegetation.end(),
                              region.vegetation) == r.site.vegetation.end())
                    continue;
                if (std::none_of(r.site.climate.begin(), r.site.climate.end(),
                                 [&](const SoilPlacement& rule) {
                                     return SoilGenerator::allowed(rule,
                                                                   region);
                                 }))
                    continue;
                const double density = r.site.density.sample(
                    SoilGenerator::unit(rx, rz, s ^ 0x31u));
                if (SoilGenerator::unit(rx, rz, s ^ 0x32u) >=
                    density * regionArea)
                    continue;
                // Away from the region's edges, so its own terrain
                // neighbourhood covers the whole tree.
                const double margin = 24;
                TreeLandmark site;
                site.species = r.id;
                site.x =
                    int64_t(rx) * RegionTiles +
                    int64_t(margin + SoilGenerator::unit(rx, rz, s ^ 0x33u) *
                                         (RegionTiles - 2 * margin));
                site.z =
                    int64_t(rz) * RegionTiles +
                    int64_t(margin + SoilGenerator::unit(rx, rz, s ^ 0x34u) *
                                         (RegionTiles - 2 * margin));
                if (world.hydrology &&
                    world.hydrology->waterAt(site.x, site.z).wet())
                    continue;
                site.hash = hash32(SoilGenerator::unit(rx, rz, s ^ 0x35u));
                site.spacing = r.species.spacing_m.sample(
                    SoilGenerator::unit(rx, rz, s ^ 0x36u));
                sites.push_back(site);
            }
    }
    // Spacing between landmarks: a global, greedy pass in hash order.
    std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) {
        return a.hash != b.hash ? a.hash < b.hash
                                : std::tie(a.x, a.z) < std::tie(b.x, b.z);
    });
    const int64_t period = world.wrap().tiles;
    std::vector<TreeLandmark> kept;
    for (const auto& site : sites) {
        const bool crowded =
            std::any_of(kept.begin(), kept.end(), [&](const TreeLandmark& k) {
                const double dx = double(WorldWrap::delta(site.x, k.x, period));
                const double dz = double(site.z - k.z);
                const double gap = std::max(site.spacing, k.spacing);
                return dx * dx + dz * dz < gap * gap;
            });
        if (!crowded) kept.push_back(site);
    }
    return kept;
}

namespace {

class RegionGenerator {
   public:
    explicit RegionGenerator(const RegionTreesInput& in)
        : in(in),
          table(*in.table),
          sampler(*in.terrain),
          period(in.terrain->periodX()),
          x0(int64_t(in.region.x) * RegionTiles),
          z0(int64_t(in.region.y) * RegionTiles) {}

    TreeSet run();

   private:
    struct Env {
        bool valid = false;
        int surfaceY = 0;
        double slope = 0;
        FloraSite site;
    };
    struct Candidate {
        const TreeSpeciesRule* rule;
        double x, z;  // unwrapped, around the region
        int anchorY;
        double spacing;
        double rank;
        uint32_t hash;
        uint64_t key;
    };

    int64_t wrapX(int64_t x) const { return WorldWrap::mod(x, period); }
    const Env& env(int64_t x, int64_t z);
    bool level(const TreeSpeciesRule& r, double x, double z, int& anchorY);
    template <typename F>
    void candidates(const TreeSpeciesRule& r, double ax0, double az0,
                    double ax1, double az1, F&& fn);
    TreeInstancePtr build(const TreeSpeciesRule& r, int64_t x, int anchorY,
                          int64_t z, uint32_t hash, uint64_t key);

    const RegionTreesInput& in;
    const TreeSpeciesTable& table;
    TerrainSampler sampler;
    int period;
    int64_t x0, z0;
    std::unordered_map<uint64_t, Env> cache;
};

const RegionGenerator::Env& RegionGenerator::env(int64_t x, int64_t z) {
    const int64_t cx = wrapX(x);
    const uint64_t key = (uint64_t(uint32_t(cx)) << 32) | uint32_t(z);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    Env e;
    const int depth = in.terrain->height * RegionTiles;
    if (z >= 0 && (depth <= 0 || z < depth) && cx >= 0) {
        try {
            const auto t = sampler.sample(cx, z);
            const auto soil = sampler.soil(cx, z, t);
            e.surfaceY = t.surface_y;
            e.site.submerged = sampler.water(cx, z, t.surface_y).wet();
            e.slope = t.slope;
            e.site.surfaceY = t.surface_y;
            e.site.slope = t.slope;
            e.site.cover = soil.cover;
            e.site.coverDensity = soil.coverDensity;
            if (soil.count > 0) {
                e.site.soil = soil.layers[0].data_id;
                e.site.topsoil = soil.layers[0].topsoil;
            }
            e.site.region =
                &in.terrain->at(int(cx / RegionTiles), int(z / RegionTiles))
                     .region;
            e.valid = true;
        } catch (const std::out_of_range&) {
            // Outside the halo: treated as unusable ground.
        }
    }
    return cache.emplace(key, e).first->second;
}

// The ground under the trunk must be nearly level; the tree stands on the
// lowest point of its root disc, so the trunk never floats.
bool RegionGenerator::level(const TreeSpeciesRule& r, double x, double z,
                            int& anchorY) {
    const auto& p = r.species;
    const double radius = std::min(12.0, double(p.size.trunk_radius_m.max) *
                                             (1 + p.form_params.root_flare)) +
                          0.5;
    const int64_t cx = int64_t(std::floor(x)), cz = int64_t(std::floor(z));
    const auto& centre = env(cx, cz);
    if (!centre.valid) return false;
    int low = centre.surfaceY, high = centre.surfaceY;
    for (int i = 0; i < 8; ++i) {
        const double a = i * 0.785398163397448;
        const auto& e = env(int64_t(std::floor(x + std::cos(a) * radius)),
                            int64_t(std::floor(z + std::sin(a) * radius)));
        if (!e.valid || e.site.submerged) return false;
        low = std::min(low, e.surfaceY);
        high = std::max(high, e.surfaceY);
    }
    if (high - low > std::max(2.0, radius * 0.5)) return false;
    // A thin tree (mesh stem, no blocks) stands on its own column: put on
    // the lowest neighbour it would grow out of the side of a step, under
    // the higher ground. A neighbour more than a tile higher is a ledge.
    if (p.sizeClass() == TreeSizeClass::Thin) {
        if (high > centre.surfaceY + 1) return false;
        anchorY = centre.surfaceY;
        return true;
    }
    anchorY = low;
    return true;
}

template <typename F>
void RegionGenerator::candidates(const TreeSpeciesRule& r, double ax0,
                                 double az0, double ax1, double az1, F&& fn) {
    const auto axis = SoilGenerator::PeriodicAxis::of(r.cell, period);
    const double cellX = axis.cell, cellZ = r.cell;
    const auto i0 = int64_t(std::floor(ax0 / cellX)),
               i1 = int64_t(std::floor(ax1 / cellX));
    const auto j0 = int64_t(std::floor(az0 / cellZ)),
               j1 = int64_t(std::floor(az1 / cellZ));
    const double keep = cellX * cellZ / Area;
    const uint32_t s = in.seed ^ r.seed;
    for (int64_t j = j0; j <= j1; ++j)
        for (int64_t i = i0; i <= i1; ++i) {
            const int64_t wi = axis.wrap(i);
            const double x =
                (double(i) + SoilGenerator::unit(wi, j, s ^ 0x11u)) * cellX;
            const double z =
                (double(j) + SoilGenerator::unit(wi, j, s ^ 0x22u)) * cellZ;
            if (x < ax0 || x >= ax1 || z < az0 || z >= az1) continue;
            const double density =
                r.site.density.sample(SoilGenerator::unit(wi, j, s ^ 0x33u));
            double chance = std::min(1.0, density * keep);
            chance *= floraClusterMask(r.site, in.seed, x, z, period);
            if (SoilGenerator::unit(wi, j, s ^ 0x44u) >= chance) continue;
            Candidate c{&r,
                        std::floor(x) + 0.5,
                        std::floor(z) + 0.5,
                        0,
                        r.species.spacing_m.sample(
                            SoilGenerator::unit(wi, j, s ^ 0x55u)),
                        SoilGenerator::unit(wi, j, s ^ 0x66u),
                        hash32(SoilGenerator::unit(wi, j, s ^ 0x77u)),
                        (uint64_t(r.id) << 56) |
                            (uint64_t(uint32_t(wi)) & 0xFFFFFFF) << 28 |
                            (uint64_t(uint32_t(j)) & 0xFFFFFFF)};
            fn(c);
        }
}

TreeInstancePtr RegionGenerator::build(const TreeSpeciesRule& r, int64_t x,
                                       int anchorY, int64_t z, uint32_t hash,
                                       uint64_t key) {
    auto tree = std::make_shared<TreeInstance>();
    tree->key = key;
    tree->species = r.id;
    tree->seed = hash ^ in.seed;
    tree->anchor = {int(wrapX(x)), anchorY, int(z)};
    const auto& growth = r.species.growth;
    const double u = double(hash & 0xFFFF) / 65535.0;
    if (r.landmark) {
        // Ancient: always past maturity.
        tree->birthDay = -int32_t(growth.maturity_days.max * (1.5 + u));
    } else {
        // Mixed ages, mostly grown.
        const double oldest = std::min(double(growth.lifespan_days.min) * 0.8,
                                       double(growth.maturity_days.max) * 3);
        tree->birthDay = -int32_t(std::pow(u, 0.6) * oldest);
    }
    const float headroom = float(WORLD_HEIGHT_Y - (anchorY + 1));
    const int64_t ax = x;
    const auto ground = [this, ax, anchorY, z](float dx, float dz) -> float {
        const auto& e =
            env(ax + int64_t(std::lround(dx)), z + int64_t(std::lround(dz)));
        return e.valid ? float(e.surfaceY - anchorY) : -1e9f;
    };
    if (in.lightweight) {
        tree->variant = int8_t(hash % TreeSpeciesTable::TreeVariants);
        tree->rotation = uint8_t((hash >> 8) & 3);
        tree->skeleton = table.variant(r.id, tree->variant, 0);
        return tree;
    }
    if (!r.landmark) {
        // A shared shape, unless the ground would bury its branches or the
        // sky is too low for it.
        const int variant = int(hash % TreeSpeciesTable::TreeVariants);
        const int rotation = int((hash >> 8) & 3);
        auto shared = table.variant(r.id, variant, rotation);
        bool fits = shared && shared->boundsMax.y <= headroom;
        if (fits)
            for (const auto& seg : shared->segments)
                if (seg.live() && seg.order > 0 &&
                    seg.b.y < ground(seg.b.x, seg.b.z) - 0.3f) {
                    fits = false;
                    break;
                }
        if (fits) {
            tree->skeleton = std::move(shared);
            tree->variant = int8_t(variant);
            tree->rotation = uint8_t(rotation);
        }
    }
    if (!tree->skeleton) {
        TreeSkeletonInput input;
        input.seed = tree->seed;
        // Room up to the build limit of the overworld layer.
        input.headroom = headroom;
        input.ground = ground;
        tree->skeleton = buildTreeSkeleton(r.species, input);
    }
    attachRoots(*tree, r.species);
    // Cut generated trees: their saved cuts, on their own skeleton.
    if (in.overrides && !in.lightweight)
        if (const auto it = in.overrides->find(key);
            it != in.overrides->end() && !it->second.cuts.empty()) {
            tree->uncut = tree->skeleton;
            tree->skeleton =
                applyTreeCuts(*tree->skeleton, r.species, it->second.cuts);
            tree->variant = -1;
            tree->cuts = it->second.cuts;
        }
    finishTreeInstance(*tree);
    return tree;
}

TreeSet RegionGenerator::run() {
    TreeSet out;
    const double rx0 = double(x0), rz0 = double(z0);
    const double rx1 = rx0 + RegionTiles, rz1 = rz0 + RegionTiles;
    const auto owned = [&](double x, double z) {
        return x >= rx0 && x < rx1 && z >= rz0 && z < rz1;
    };
    // Landmark trunks exclude ordinary trees (their crowns do not).
    struct Disc {
        double x, z, radius;
    };
    std::vector<Disc> landmarkDiscs;
    const auto removed = [&](uint64_t key) {
        if (!in.overrides) return false;
        const auto it = in.overrides->find(key);
        return it != in.overrides->end() && it->second.removed;
    };
    if (in.landmarks)
        for (const auto& site : *in.landmarks) {
            const auto* r = table.find(site.species);
            if (!r) continue;
            const double x = rx0 + double(WorldWrap::delta(site.x, x0, period));
            const double radius = r->species.size.trunk_radius_m.max *
                                      (1 + r->species.form_params.root_flare) +
                                  table.maxSpacing();
            landmarkDiscs.push_back({x + 0.5, double(site.z) + 0.5, radius});
            // Billboards only need the exclusion: landmarks are drawn as
            // distant structures.
            if (in.lightweight || !owned(x, double(site.z))) continue;
            // A landmark stands on the lowest ground under its whole flared
            // base (up to ~100 m across), so no buttress floats; on a slope
            // the upper side is buried like roots. Never dropped for uneven
            // ground.
            const double base = r->species.size.trunk_radius_m.max *
                                (1 + r->species.form_params.root_flare);
            bool any = false;
            bool submerged = false;
            int anchorY = INT_MAX;
            for (int ring = 0; ring <= 4; ++ring) {
                const double radius = base * ring / 4;
                const int steps = ring ? 8 * ring : 1;
                for (int i = 0; i < steps; ++i) {
                    const double a = 6.283185307179586 * i / steps;
                    const auto& e =
                        env(int64_t(std::floor(x + 0.5 + std::cos(a) * radius)),
                            int64_t(std::floor(site.z + 0.5 +
                                               std::sin(a) * radius)));
                    if (!e.valid) continue;
                    any = true;
                    submerged |= e.site.submerged;
                    anchorY = std::min(anchorY, e.surfaceY);
                }
            }
            if (!any || submerged) continue;
            const uint64_t key = (uint64_t(0xFF) << 56) |
                                 (uint64_t(site.x) & 0xFFFFFFF) << 28 |
                                 (uint64_t(site.z) & 0xFFFFFFF);
            if (removed(key)) continue;
            out.push_back(
                build(*r, int64_t(x), anchorY, site.z, site.hash, key));
        }
    // Ordinary trees anchored here; rivals within spacing are evaluated
    // around the region (a local rule, so every region decides the same).
    const double ring = table.maxSpacing() + 1;
    std::vector<Candidate> all;
    for (const auto& r : table.species()) {
        if (r.landmark) continue;
        candidates(
            r, rx0 - ring, rz0 - ring, rx1 + ring, rz1 + ring,
            [&](Candidate c) {
                for (const auto& d : landmarkDiscs) {
                    const double dx = c.x - d.x, dz = c.z - d.z;
                    if (dx * dx + dz * dz < d.radius * d.radius) return;
                }
                const auto& e =
                    env(int64_t(std::floor(c.x)), int64_t(std::floor(c.z)));
                if (!e.valid || !floraSuits(table.sites(), r.site, e.site))
                    return;
                if (!level(r, c.x, c.z, c.anchorY)) return;
                all.push_back(c);
            });
    }
    const auto outranks = [](const Candidate& a, const Candidate& b) {
        if (a.rule->site.priority != b.rule->site.priority)
            return a.rule->site.priority > b.rule->site.priority;
        if (a.rule->species.extent() != b.rule->species.extent())
            return a.rule->species.extent() > b.rule->species.extent();
        if (a.rank != b.rank) return a.rank < b.rank;
        return a.hash < b.hash;
    };
    for (const auto& a : all) {
        if (!owned(a.x, a.z)) continue;
        const bool beaten =
            std::any_of(all.begin(), all.end(), [&](const Candidate& b) {
                if (&a == &b) return false;
                const double dx = a.x - b.x, dz = a.z - b.z;
                const double gap = std::max(a.spacing, b.spacing);
                return dx * dx + dz * dz < gap * gap && outranks(b, a);
            });
        // A removed tree still claims its spot: its rivals stay beaten.
        if (beaten || removed(a.key)) continue;
        out.push_back(build(*a.rule, int64_t(std::floor(a.x)), a.anchorY,
                            int64_t(std::floor(a.z)), a.hash, a.key));
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a->key < b->key; });
    return out;
}

}  // namespace

std::vector<TreeBillboard> regionBillboards(RegionTreesInput input) {
    // Overrides only remove trees here; cuts keep the shared shape.
    input.lightweight = true;
    std::vector<TreeBillboard> out;
    for (const auto& tree : generateRegionTrees(input)) {
        if (!tree->skeleton) continue;
        const auto& k = *tree->skeleton;
        TreeBillboard b;
        b.x = float(tree->anchor.x) + 0.5f;
        b.y = float(tree->anchor.y + 1);
        b.z = float(tree->anchor.z) + 0.5f;
        b.height = std::max(1.f, k.boundsMax.y);
        b.crown = std::max({1.f, -k.boundsMin.x, k.boundsMax.x, -k.boundsMin.z,
                            k.boundsMax.z});
        b.maturity = k.matureAge();
        b.lifespan = k.lifespan;
        b.curve = k.curve;
        b.birthDay = tree->birthDay;
        b.hash = tree->seed * 0x9e3779b9u;
        b.species = tree->species;
        out.push_back(b);
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.hash < b.hash; });
    return out;
}

TreeSet generateRegionTrees(const RegionTreesInput& input) {
    if (!input.table || input.table->empty() || !input.terrain) return {};
    auto out = RegionGenerator(input).run();
    // Planted trees anchored in this region.
    if (input.overrides) {
        for (const auto& [key, record] : *input.overrides) {
            if (!record.planted() ||
                record.anchor.x / RegionTiles != int(input.region.x) ||
                record.anchor.z / RegionTiles != int(input.region.y))
                continue;
            if (auto tree = buildPlantedTree(*input.table, record))
                out.push_back(std::move(tree));
        }
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a->key < b->key; });
    }
    return out;
}

void finishTreeInstance(TreeInstance& tree) {
    const auto& k = *tree.skeleton;
    const auto& a = tree.anchor;
    tree.wood = treeWoodBounds(k, a, true);
    tree.bounds = {a.x + int(std::floor(k.boundsMin.x)),
                   a.y + int(std::floor(k.boundsMin.y)),
                   a.z + int(std::floor(k.boundsMin.z)),
                   a.x + int(std::ceil(k.boundsMax.x)) + 1,
                   a.y + int(std::ceil(k.boundsMax.y)) + 2,
                   a.z + int(std::ceil(k.boundsMax.z)) + 1};
    if (!tree.wood.empty())
        tree.bounds = {std::min(tree.bounds.x0, tree.wood.x0),
                       std::min(tree.bounds.y0, tree.wood.y0),
                       std::min(tree.bounds.z0, tree.wood.z0),
                       std::max(tree.bounds.x1, tree.wood.x1),
                       std::max(tree.bounds.y1, tree.wood.y1),
                       std::max(tree.bounds.z1, tree.wood.z1)};
    if (!tree.rootWood.empty())
        tree.bounds = {std::min(tree.bounds.x0, tree.rootWood.x0),
                       std::min(tree.bounds.y0, tree.rootWood.y0),
                       std::min(tree.bounds.z0, tree.rootWood.z0),
                       std::max(tree.bounds.x1, tree.rootWood.x1),
                       std::max(tree.bounds.y1, tree.rootWood.y1),
                       std::max(tree.bounds.z1, tree.rootWood.z1)};
    tree.discRadius = k.trunkRadius * (1 + k.flare.amount) + 0.5;
}

uint64_t plantedTreeKey(TreeAnchor anchor) {
    uint64_t h = 0x9e3779b97f4a7c15ULL;
    for (const int v : {anchor.x, anchor.y, anchor.z}) {
        h ^=
            uint64_t(uint32_t(v)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h *= 0xbf58476d1ce4e5b9ULL;
    }
    return (h & ~(3ULL << 62)) | TreeRecord::PlantedBit;
}

TreeInstancePtr buildPlantedTree(const TreeSpeciesTable& table,
                                 const TreeRecord& record) {
    const auto* rule = table.find(record.species);
    if (!rule) return nullptr;
    auto tree = std::make_shared<TreeInstance>();
    tree->key = record.key;
    tree->species = record.species;
    tree->seed = record.seed;
    tree->anchor = record.anchor;
    tree->birthDay = record.birthDay;
    TreeSkeletonInput input;
    input.seed = record.seed;
    input.headroom = record.headroom;
    tree->skeleton = buildTreeSkeleton(rule->species, input);
    attachRoots(*tree, rule->species);
    if (!record.cuts.empty()) {
        tree->uncut = tree->skeleton;
        tree->skeleton =
            applyTreeCuts(*tree->skeleton, rule->species, record.cuts);
    }
    tree->cuts = record.cuts;
    finishTreeInstance(*tree);
    return tree;
}

TreeColumn makeTreeColumn(unsigned chunkX, unsigned chunkZ,
                          const std::vector<const TreeSet*>& sets,
                          int periodX) {
    TreeColumn column;
    const int cx0 = int(chunkX) * Chunk::SIZE_X,
              cz0 = int(chunkZ) * Chunk::SIZE_Z;
    const int cx1 = cx0 + Chunk::SIZE_X, cz1 = cz0 + Chunk::SIZE_Z;
    for (const auto* set : sets) {
        if (!set) continue;
        for (const auto& tree : *set) {
            // The copy of the tree nearest this column.
            const int shift =
                int(WorldWrap::delta(tree->anchor.x, cx0, periodX) -
                    (int64_t(tree->anchor.x) - cx0));
            const TreeAnchor anchor{tree->anchor.x + shift, tree->anchor.y,
                                    tree->anchor.z};
            const auto& b = tree->bounds;
            const double dx = anchor.x + 0.5, dz = anchor.z + 0.5;
            const double nearX = std::clamp(dx, double(cx0), double(cx1));
            const double nearZ = std::clamp(dz, double(cz0), double(cz1));
            const double gap = std::hypot(dx - nearX, dz - nearZ);
            if (gap < tree->discRadius + TreeDiscMargin)
                column.discs.push_back({dx, dz, tree->discRadius});
            if (b.x1 + shift <= cx0 || b.x0 + shift >= cx1 || b.z1 <= cz0 ||
                b.z0 >= cz1)
                continue;
            column.trees.push_back({tree, anchor});
        }
    }
    std::sort(
        column.trees.begin(), column.trees.end(),
        [](const auto& a, const auto& b) { return a.tree->key < b.tree->key; });
    return column;
}

TreeWoodDelta treeWoodDelta(const ChunkCoord& coord, const TreeColumn& column,
                            const TreeSpeciesTable& table, double since,
                            double today, bool legacyBark) {
    TreeWoodDelta out;
    const TreeTileBox box{
        int(coord.x) * Chunk::SIZE_X,     int(coord.y) * Chunk::SIZE_Y,
        int(coord.z) * Chunk::SIZE_Z,     int(coord.x + 1) * Chunk::SIZE_X,
        int(coord.y + 1) * Chunk::SIZE_Y, int(coord.z + 1) * Chunk::SIZE_Z};
    const auto tileIndex = [&](int x, int y, int z) {
        return (z - box.z0) * Chunk::SIZE_X * Chunk::SIZE_Y +
               (y - box.y0) * Chunk::SIZE_X + (x - box.x0);
    };
    std::vector<uint8_t> now, then, cut, otherWood;
    for (const auto& entry : column.trees) {
        const auto& tree = *entry.tree;
        const auto* rule = table.find(tree.species);
        if (!rule || !rule->woodBlock) continue;
        const int shift = entry.anchor.x - tree.anchor.x;
        TreeTileBox wood = tree.wood;
        wood.x0 += shift;
        wood.x1 += shift;
        const auto part = wood.intersect(box);
        if (part.empty()) continue;
        const auto& k = *tree.skeleton;
        const float ageNow = tree.woodAgeForDay(today),
                    ageThen = tree.growthAge(since);
        const bool growing = ageNow > ageThen;
        const bool changingShape = growing && ageThen < k.matureAge();
        const bool classify = legacyBark || changingShape;
        const bool preserveEdits = rule->species.cut.fell == TreeFell::Settle;
        if (!classify && tree.cuts.empty()) continue;
        markTreeWoodLayers(k, tree.uncut.get(), entry.anchor, part, ageNow,
                           classify ? rule->woodShell() : 0, now);
        // Cuts the chunk's wood may predate: clear the wood they took away.
        // Stamps are whole days (the day the wood was produced), so a cut
        // made later on that same day counts, and cut ages are floats (an
        // elder's is exact to about 1/64 day). Only cuts clearly before the
        // stamp day began are skipped; clearing a cut again is harmless
        // (only wood the tree no longer has goes).
        for (size_t i = 0; i < tree.cuts.size(); ++i) {
            if (double(tree.birthDay) + tree.cuts[i].age < since - 0.1)
                continue;
            markTreeWood(k, entry.anchor, part, ageNow, cut, {uint16_t(i + 1)});
            for (int y = part.y0; y < part.y1; ++y)
                for (int z = part.z0; z < part.z1; ++z)
                    for (int x = part.x0; x < part.x1; ++x) {
                        const size_t m = part.index(x, y, z);
                        if (cut[m] && !now[m])
                            out.remove.push_back({tileIndex(x, y, z),
                                                  rule->woodBlock, 0,
                                                  rule->innerWoodBlock});
                    }
        }
        if (!classify) continue;
        markTreeWood(k, entry.anchor, part, ageThen, then);
        // Without per-cell crown provenance, shared bark IDs in overlapping
        // trees are ambiguous. Leave existing material alone there.
        std::bitset<Chunk::SIZE_X * Chunk::SIZE_Y * Chunk::SIZE_Z> sharedBark;
        if (rule->innerWoodBlock)
            for (const auto& other : column.trees) {
                if (other.tree->key == tree.key) continue;
                const auto* otherRule = table.find(other.tree->species);
                if (!otherRule || !otherRule->ownsWood(rule->woodBlock))
                    continue;
                auto bounds = other.tree->wood;
                const int shift = other.anchor.x - other.tree->anchor.x;
                bounds.x0 += shift;
                bounds.x1 += shift;
                if (part.intersect(bounds).empty()) continue;
                markTreeWood(*other.tree->skeleton, other.anchor, part,
                             other.tree->growthAge(today), otherWood);
                for (size_t i = 0; i < otherWood.size(); ++i)
                    if (otherWood[i]) sharedBark.set(i);
            }
        for (int y = part.y0; y < part.y1; ++y)
            for (int z = part.z0; z < part.z1; ++z)
                for (int x = part.x0; x < part.x1; ++x) {
                    const size_t m = part.index(x, y, z);
                    if (!now[m]) continue;
                    const auto block =
                        now[m] == 2 ? rule->innerWoodBlock : rule->woodBlock;
                    const int cell = tileIndex(x, y, z);
                    if (growing && !then[m])
                        out.add.push_back({.index = cell,
                                           .block = block,
                                           .preserveEdit = preserveEdits});
                    if (now[m] == 2 && (legacyBark || then[m]) &&
                        !sharedBark.test(m))
                        out.recolor.push_back(
                            {cell, block, 0, rule->woodBlock});
                }
    }
    return out;
}

TreeWoodDelta treeCutWoodDelta(const ChunkCoord& coord,
                               const TreeInstance& tree, TreeAnchor anchor,
                               uint8_t woodBlock, uint16_t cut, double today,
                               bool settles, int periodTiles,
                               uint8_t innerBlock, int shell) {
    TreeWoodDelta out;
    if (!tree.skeleton || !woodBlock || !cut) return out;
    const TreeTileBox box{
        int(coord.x) * Chunk::SIZE_X,     int(coord.y) * Chunk::SIZE_Y,
        int(coord.z) * Chunk::SIZE_Z,     int(coord.x + 1) * Chunk::SIZE_X,
        int(coord.y + 1) * Chunk::SIZE_Y, int(coord.z + 1) * Chunk::SIZE_Z};
    // Use this chunk's image of the immutable cut snapshot at the X seam.
    const WorldWrap wrap{periodTiles};
    anchor.x = int(box.x0 + wrap.deltaTile(anchor.x, box.x0));
    const auto& k = *tree.skeleton;
    const float age = tree.growthAge(today);
    std::vector<uint8_t> removed, now, then;
    markTreeWood(k, anchor, box, age, removed, {cut});
    // Most chunks in a limb's bounding box are empty. Do not rasterize the
    // whole living tree there, and never process its older cuts or growth.
    if (!settles && std::none_of(removed.begin(), removed.end(),
                                 [](uint8_t v) { return v != 0; }))
        return out;
    markTreeWoodLayers(k, tree.uncut.get(), anchor, box, age,
                       settles && innerBlock ? shell : 0, now);
    if (settles) markTreeWood(k, anchor, box, tree.growthAge(today - 1), then);
    // Only the actual saw row can close across player edits. Other space
    // in the settling upper piece is subject to the ordinary growth filter.
    const TreeSegment* stump = nullptr;
    if (settles)
        for (const auto& s : k.segments)
            if (!s.live() && s.cut == cut && s.order == 0 && s.parent >= 0 &&
                s.attachT >= 1 &&
                (k.segments[size_t(s.parent)].flags & TreeSegment::CutEnd) &&
                k.segments[size_t(s.parent)].b == s.a)
                stump = &k.segments[size_t(s.parent)];
    const TreeVec axis = stump ? (stump->b - stump->a).normalized() : TreeVec{};
    for (int y = box.y0; y < box.y1; ++y)
        for (int z = box.z0; z < box.z1; ++z)
            for (int x = box.x0; x < box.x1; ++x) {
                const size_t i = box.index(x, y, z);
                const int tile = (z - box.z0) * Chunk::SIZE_X * Chunk::SIZE_Y +
                                 (y - box.y0) * Chunk::SIZE_X + x - box.x0;
                if (removed[i] && !now[i])
                    out.remove.push_back({tile, woodBlock, 0, innerBlock});
                if (settles && y > anchor.y && now[i] && !then[i]) {
                    const TreeVec center{float(x - anchor.x),
                                         float(y - anchor.y) - 0.5f,
                                         float(z - anchor.z)};
                    const float fromFace =
                        stump ? (center - stump->b).dot(axis) : 0.f;
                    out.add.push_back(
                        {.index = tile,
                         .block = now[i] == 2 ? innerBlock : woodBlock,
                         .preserveEdit = fromFace <= 0 || fromFace > 1});
                }
            }
    return out;
}

TreeWoodDelta treeWoodRemoval(const ChunkCoord& coord, const TreeInstance& tree,
                              TreeAnchor anchor, uint8_t woodBlock, float age,
                              int periodTiles, uint8_t innerBlock) {
    TreeWoodDelta out;
    if (!tree.skeleton || !woodBlock) return out;
    const int x0 = int(coord.x) * Chunk::SIZE_X,
              y0 = int(coord.y) * Chunk::SIZE_Y,
              z0 = int(coord.z) * Chunk::SIZE_Z;
    const int shift = anchor.x - tree.anchor.x;
    const TreeTileBox wood{tree.wood.x0 + shift, tree.wood.y0, tree.wood.z0,
                           tree.wood.x1 + shift, tree.wood.y1, tree.wood.z1};
    // The chunk's copy overlapping the box (it spans less than half the
    // world).
    int cx0 = x0;
    if (periodTiles > 0)
        cx0 = x0 + int(std::lround(double(wood.x0 - x0) / periodTiles)) *
                       periodTiles;
    const auto part = wood.intersect({cx0, y0, z0, cx0 + Chunk::SIZE_X,
                                      y0 + Chunk::SIZE_Y, z0 + Chunk::SIZE_Z});
    if (part.empty()) return out;
    std::vector<uint8_t> mask;
    markTreeWood(*tree.skeleton, anchor, part, age, mask);
    for (int y = part.y0; y < part.y1; ++y)
        for (int z = part.z0; z < part.z1; ++z)
            for (int x = part.x0; x < part.x1; ++x)
                if (mask[part.index(x, y, z)])
                    out.remove.push_back(
                        {(z - z0) * Chunk::SIZE_X * Chunk::SIZE_Y +
                             (y - y0) * Chunk::SIZE_X + (x - cx0),
                         woodBlock, 0, innerBlock});
    return out;
}

std::vector<TreeWoodChange> filterTreeWood(const Chunk& chunk,
                                           const TreeWoodDelta& delta) {
    const Tile* tiles = &chunk.tiles[0][0][0];
    // A tile listed twice (overlapping trees, a removal and an addition):
    // keep the first change.
    std::vector<uint8_t> seen(
        size_t(Chunk::SIZE_X * Chunk::SIZE_Y * Chunk::SIZE_Z));
    std::vector<TreeWoodChange> out;
    for (const auto& c : delta.remove)
        if (!seen[size_t(c.index)] &&
            (tiles[c.index].block_id == c.block ||
             (c.previousBlock && tiles[c.index].block_id == c.previousBlock)) &&
            !chunk.rootAt(uint16_t(c.index))) {
            seen[size_t(c.index)] = 1;
            out.push_back({c.index, 0});
        }
    for (const auto& c : delta.recolor)
        if (!seen[size_t(c.index)] &&
            tiles[c.index].block_id == c.previousBlock &&
            !chunk.rootAt(uint16_t(c.index)) &&
            !chunk.rootEdited(uint16_t(c.index))) {
            seen[size_t(c.index)] = 1;
            out.push_back(c);
        }
    for (const auto& c : delta.add)
        if (!seen[size_t(c.index)] && tiles[c.index].isAir() &&
            (!c.preserveEdit || !chunk.rootEdited(uint16_t(c.index)))) {
            seen[size_t(c.index)] = 1;
            out.push_back(c);
        }
    return out;
}

std::vector<TreeWoodChange> treeWoodChanges(const Chunk& chunk,
                                            const ChunkCoord& coord,
                                            const TreeColumn& column,
                                            const TreeSpeciesTable& table,
                                            double since, double today,
                                            bool legacyBark) {
    return filterTreeWood(
        chunk, treeWoodDelta(coord, column, table, since, today, legacyBark));
}

std::vector<TreeWoodChange> treeRootChanges(
    const Chunk& chunk, const ChunkCoord& coord, const TreeColumn& column,
    const TreeSpeciesTable& table, const ChunkColumnInputs& terrain,
    double since, double today, bool legacyBark) {
    std::vector<TreeWoodChange> changes;
    if (chunk.legacyRootTerrain || (since >= today && !legacyBark))
        return changes;
    constexpr int Side = Chunk::SIZE_X;
    const int x0 = int(coord.x) * Side, y0 = int(coord.y) * Side,
              z0 = int(coord.z) * Side;
    const TreeTileBox slab{x0, y0, z0, x0 + Side, y0 + Side, z0 + Side};
    int surfaceTop = -1;
    for (const auto& sample : terrain.columns)
        surfaceTop = std::max(surfaceTop, sample.surface_y);
    if (y0 > surfaceTop) return changes;
    std::vector<uint8_t> before, after;
    std::bitset<Side * Side * Side> claimed;
    for (const auto& entry : column.trees) {
        const auto& tree = *entry.tree;
        if (!tree.roots) continue;
        const float start = tree.growthAge(since), end = tree.growthAge(today);
        const bool growing = start < tree.roots->maturity && end > start;
        if (!growing && !legacyBark) continue;
        const int shift = entry.anchor.x - tree.anchor.x;
        auto box = tree.rootWood;
        box.x0 += shift;
        box.x1 += shift;
        box.y1 = std::min(box.y1, surfaceTop + 1);
        box = box.intersect(slab);
        if (box.empty()) continue;
        const auto* species = table.find(tree.species);
        if (!species || !species->woodBlock) continue;
        const float reach = tree.rootReach;
        markTreeWoodLayers(*tree.roots, nullptr, entry.anchor, box, end,
                           species->woodShell(), after);
        if (std::none_of(after.begin(), after.end(),
                         [](auto value) { return value; }))
            continue;
        markTreeWood(*tree.roots, entry.anchor, box, start, before);
        for (int z = box.z0; z < box.z1; ++z)
            for (int x = box.x0; x < box.x1; ++x) {
                const float dx = float(x - entry.anchor.x);
                const float dz = float(z - entry.anchor.z);
                if (dx * dx + dz * dz > reach * reach) continue;
                const int lx = x - x0, lz = z - z0;
                const int surface = terrain.columns[lz * Side + lx].surface_y;
                for (int y = box.y0; y <= std::min(box.y1 - 1, surface); ++y) {
                    const size_t i = box.index(x, y, z);
                    if (!after[i]) continue;
                    const int ly = y - y0;
                    const auto cell = Chunk::cell(lx, ly, lz);
                    const Tile& tile = chunk.tiles[lz][ly][lx];
                    const auto block = after[i] == 2 ? species->innerWoodBlock
                                                     : species->woodBlock;
                    if (const auto* root = chunk.rootAt(cell);
                        root && root->owner == tree.key && after[i] == 2 &&
                        tile.block_id == species->woodBlock &&
                        (before[i] || legacyBark)) {
                        changes.push_back(
                            {cell, block, tree.key, species->woodBlock});
                        continue;
                    }
                    if (!growing || before[i]) continue;
                    if (claimed.test(cell) || !tile.isSolid() ||
                        tile.isLiquid() ||
                        !table.rootSubstrate(tile.block_id) ||
                        chunk.rootEdited(cell))
                        continue;
                    claimed.set(cell);
                    changes.push_back({cell, block, tree.key});
                }
            }
    }
    return changes;
}

int placeTreeWood(Chunk& chunk, const ChunkCoord& coord,
                  const TreeColumn& column, const TreeSpeciesTable& table,
                  double day) {
    const auto structures =
        tree_wood_lod::columnVoxels(coord.x, coord.z, column, table, day, {});
    const auto slab = structures->rasterize(coord.y);
    return slab ? structure_voxels::stamp(chunk, *slab) : 0;
}
