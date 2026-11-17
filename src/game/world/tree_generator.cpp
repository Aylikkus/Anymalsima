#include "tree_generator.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

#include "ground_cover_generator.hpp"

namespace {
constexpr double Area = 100.0;  // m² of density_per_100m2
constexpr int RegionTiles = TerrainTuning::RegionX;

uint32_t hash32(double unit) { return uint32_t(unit * 4294967295.0); }
}  // namespace

TreeSpeciesTable::TreeSpeciesTable(const IAssets& assets) : siteTable(assets) {
    if (assets.hasStorage<Block>())
        grownBlocks = naturalGrowthBlocks(assets.getStorage<Block>().items);
    if (!assets.hasStorage<Tree>() || !assets.hasStorage<Block>()) return;
    const auto& all = assets.getStorage<Tree>();
    const auto& blocks = assets.getStorage<Block>();
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
        if (!r.landmark) {
            extent = std::max(extent, double(tree.extent()));
            spacing = std::max(spacing, double(tree.spacing_m.max));
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
                if (region.land_height_m < SEA_LEVEL_Y) continue;
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
        if (!e.valid || e.surfaceY < SEA_LEVEL_Y) return false;
        low = std::min(low, e.surfaceY);
        high = std::max(high, e.surfaceY);
    }
    if (high - low > std::max(2.0, radius * 0.5)) return false;
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
    // Cut generated trees: their saved cuts, on their own skeleton.
    if (in.overrides)
        if (const auto it = in.overrides->find(key);
            it != in.overrides->end() && !it->second.cuts.empty()) {
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
    if (in.landmarks && !in.lightweight)
        for (const auto& site : *in.landmarks) {
            const auto* r = table.find(site.species);
            if (!r) continue;
            const double x = rx0 + double(WorldWrap::delta(site.x, x0, period));
            const double radius = r->species.size.trunk_radius_m.max *
                                      (1 + r->species.form_params.root_flare) +
                                  table.maxSpacing();
            landmarkDiscs.push_back({x + 0.5, double(site.z) + 0.5, radius});
            if (!owned(x, double(site.z))) continue;
            // A landmark stands on the lowest ground under its whole flared
            // base (up to ~100 m across), so no buttress floats; on a slope
            // the upper side is buried like roots. Never dropped for uneven
            // ground.
            const double base = r->species.size.trunk_radius_m.max *
                                (1 + r->species.form_params.root_flare);
            bool any = false;
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
                    anchorY = std::min(anchorY, e.surfaceY);
                }
            }
            if (!any || anchorY < SEA_LEVEL_Y) continue;
            out.push_back(build(*r, int64_t(x), anchorY, site.z, site.hash,
                                (uint64_t(0xFF) << 56) |
                                    (uint64_t(site.x) & 0xFFFFFFF) << 28 |
                                    (uint64_t(site.z) & 0xFFFFFFF)));
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
        if (beaten) continue;
        out.push_back(build(*a.rule, int64_t(std::floor(a.x)), a.anchorY,
                            int64_t(std::floor(a.z)), a.hash, a.key));
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a->key < b->key; });
    return out;
}

}  // namespace

std::vector<TreeBillboard> regionBillboards(RegionTreesInput input) {
    input.lightweight = true;
    input.overrides = nullptr;
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
    if (!record.cuts.empty())
        tree->skeleton =
            applyTreeCuts(*tree->skeleton, rule->species, record.cuts);
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
            // Highest wood over this column, from segments crossing it.
            const auto& k = *tree->skeleton;
            for (const auto& s : k.segments) {
                if (!s.live()) continue;
                const float flare = s.order == 0 ? 1 + k.flare.amount : 1;
                const float r = std::max(s.r0, s.r1) * flare;
                if (r < TreeVoxelRadius) continue;
                const float pad = r + 0.71f;
                const float sx0 = std::min(s.a.x, s.b.x) - pad + anchor.x,
                            sx1 = std::max(s.a.x, s.b.x) + pad + anchor.x;
                const float sz0 = std::min(s.a.z, s.b.z) - pad + anchor.z,
                            sz1 = std::max(s.a.z, s.b.z) + pad + anchor.z;
                if (sx1 < cx0 || sx0 > cx1 || sz1 < cz0 || sz0 > cz1) continue;
                const int top =
                    anchor.y + 1 + int(std::ceil(std::max(s.a.y, s.b.y) + pad));
                column.woodTop = std::max(column.woodTop, top);
            }
        }
    }
    std::sort(
        column.trees.begin(), column.trees.end(),
        [](const auto& a, const auto& b) { return a.tree->key < b.tree->key; });
    return column;
}

std::vector<TreeWoodChange> treeWoodChanges(const Chunk& chunk,
                                            const ChunkCoord& coord,
                                            const TreeColumn& column,
                                            const TreeSpeciesTable& table,
                                            double since, double today) {
    std::vector<TreeWoodChange> out;
    const TreeTileBox box{
        int(coord.x) * Chunk::SIZE_X,     int(coord.y) * Chunk::SIZE_Y,
        int(coord.z) * Chunk::SIZE_Z,     int(coord.x + 1) * Chunk::SIZE_X,
        int(coord.y + 1) * Chunk::SIZE_Y, int(coord.z + 1) * Chunk::SIZE_Z};
    const auto tileIndex = [&](int x, int y, int z) {
        return (z - box.z0) * Chunk::SIZE_X * Chunk::SIZE_Y +
               (y - box.y0) * Chunk::SIZE_X + (x - box.x0);
    };
    const Tile* tiles = &chunk.tiles[0][0][0];
    std::vector<uint8_t> now, then, cut;
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
        const float ageNow = tree.growthAge(today),
                    ageThen = tree.growthAge(since);
        markTreeWood(k, entry.anchor, part, ageNow, now);
        // Cuts made after `since`: clear the wood they took away.
        for (size_t i = 0; i < tree.cuts.size(); ++i) {
            if (double(tree.birthDay) + tree.cuts[i].age <= since) continue;
            markTreeWood(k, entry.anchor, part, ageNow, cut, {uint16_t(i + 1)});
            for (int y = part.y0; y < part.y1; ++y)
                for (int z = part.z0; z < part.z1; ++z)
                    for (int x = part.x0; x < part.x1; ++x) {
                        const size_t m = part.index(x, y, z);
                        const int t = tileIndex(x, y, z);
                        if (cut[m] && !now[m] &&
                            tiles[t].block_id == rule->woodBlock)
                            out.push_back({t, 0});
                    }
        }
        if (ageNow <= ageThen) continue;
        markTreeWood(k, entry.anchor, part, ageThen, then);
        for (int y = part.y0; y < part.y1; ++y)
            for (int z = part.z0; z < part.z1; ++z)
                for (int x = part.x0; x < part.x1; ++x) {
                    const size_t m = part.index(x, y, z);
                    const int t = tileIndex(x, y, z);
                    if (now[m] && !then[m] && tiles[t].isAir())
                        out.push_back({t, rule->woodBlock});
                }
    }
    // A tile listed twice (overlapping trees): keep the first change.
    std::vector<TreeWoodChange> unique;
    std::vector<uint8_t> seen(
        size_t(Chunk::SIZE_X * Chunk::SIZE_Y * Chunk::SIZE_Z));
    for (const auto& c : out)
        if (!seen[size_t(c.index)]) {
            seen[size_t(c.index)] = 1;
            unique.push_back(c);
        }
    return unique;
}

int placeTreeWood(Chunk& chunk, const ChunkCoord& coord,
                  const TreeColumn& column, const TreeSpeciesTable& table,
                  double day) {
    const TreeTileBox box{
        int(coord.x) * Chunk::SIZE_X,     int(coord.y) * Chunk::SIZE_Y,
        int(coord.z) * Chunk::SIZE_Z,     int(coord.x + 1) * Chunk::SIZE_X,
        int(coord.y + 1) * Chunk::SIZE_Y, int(coord.z + 1) * Chunk::SIZE_Z};
    int placed = 0;
    std::vector<uint8_t> mask;
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
        const float age = tree.growthAge(day);
        if (age <= 0) continue;
        markTreeWood(*tree.skeleton, entry.anchor, part, age, mask);
        for (int y = part.y0; y < part.y1; ++y)
            for (int z = part.z0; z < part.z1; ++z)
                for (int x = part.x0; x < part.x1; ++x) {
                    if (!mask[part.index(x, y, z)]) continue;
                    Tile& tile =
                        chunk.tiles[z - box.z0][y - box.y0][x - box.x0];
                    if (!tile.isAir()) continue;
                    tile.block_id = rule->woodBlock;
                    ++placed;
                }
    }
    return placed;
}
