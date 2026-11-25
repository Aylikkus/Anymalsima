#include "flora_generator.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "ground_cover_generator.hpp"
#include "items/block.hpp"
#include "items/ground_cover.hpp"

namespace {

constexpr double Area = 100.0;  // m² of density_per_100m2

double smooth(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

}  // namespace

float floraHeightEstimate(const Flora& flora) {
    float stem = 0, top = 0;
    for (const auto& part : flora.parts)
        if (part.type == FloraPartType::Stem && part.height_m)
            stem = std::max(stem, part.height_m->max);
    for (const auto& part : flora.parts) {
        float extent = 0;
        if (part.height_m) extent = std::max(extent, part.height_m->max);
        if (part.length_m) extent = std::max(extent, part.length_m->max);
        if (part.card_size_m) extent = std::max(extent, part.card_size_m->max);
        if (part.shell)
            extent = std::max(extent, part.shell->lift_m.max +
                                          2 * part.shell->radii_m[1].max);
        if (part.attach == FloraAttach::Stem ||
            part.attach == FloraAttach::StemTip)
            extent += stem;
        top = std::max(top, extent);
    }
    return std::max(top, 0.1f) * 1.15f;
}

void floraPlacementRule(const FloraRules& p, const IAssets& assets,
                        FloraSpeciesRule& r) {
    const auto coverId = [&](const std::string& name) -> int {
        if (!assets.hasStorage<GroundCover>()) return -1;
        const auto& covers = assets.getStorage<GroundCover>();
        const auto it = covers.indexByName.find(name);
        return it == covers.indexByName.end() ? -1
                                              : covers.items[it->second].id;
    };
    r.priority = p.priority;
    r.persistentSites = p.persistent_sites;
    r.density = p.density_per_100m2;
    r.clusterScale = p.cluster_scale_m;
    r.clusterThreshold = p.cluster_threshold;
    r.maxSlope = p.max_slope;
    r.underwater = p.underwater;
    for (const auto& name : p.ground_cover_required)
        if (const int id = coverId(name); id > 0) r.coverRequired.set(id);
    for (const auto& name : p.ground_cover_forbidden)
        if (const int id = coverId(name); id > 0) r.coverForbidden.set(id);
    if (assets.hasStorage<Soil>())
        for (const auto& name : p.soils.names) {
            const auto& soils = assets.getStorage<Soil>();
            if (const auto it = soils.indexByName.find(name);
                it != soils.indexByName.end())
                r.soilAllowed.set(soils.items[it->second].id);
        }
    r.fertility = p.soils.fertility;
    r.retention = p.soils.water_retention;
    r.climate = p.climate;
    r.vegetation = p.vegetation;
}

FloraSpeciesTable::FloraSpeciesTable(const IAssets& assets) {
    if (!assets.hasStorage<Soil>()) return;
    for (const auto& soil : assets.getStorage<Soil>().items)
        soilTable.push_back({soil.properties, soil.propertiesFor(true)});
    if (assets.hasStorage<Block>()) {
        const auto& blocks = assets.getStorage<Block>();
        blockSoil.assign(blocks.items.size() + 1, {0, false});
        for (const auto& soil : assets.getStorage<Soil>().items) {
            const auto mark = [&](const std::string& name, bool top) {
                const auto it = blocks.indexByName.find(name);
                if (it != blocks.indexByName.end())
                    blockSoil[blocks.items[it->second].id] = {soil.id, top};
            };
            mark(soil.block_name, false);
            if (soil.topsoil_block_name) mark(*soil.topsoil_block_name, true);
        }
    }
    // Soil tables serve trees too, with or without plant species.
    if (!assets.hasStorage<Flora>()) return;
    const auto& all = assets.getStorage<Flora>();
    byId.assign(all.items.size(), -1);
    int maxFootprint = 0, maxCompanion = 0;
    for (const auto& f : all.items) {
        FloraSpeciesRule r;
        const auto& p = f.placement;
        r.id = f.id;
        r.seed = GroundCoverGenerator::nameHash(f.name);
        r.large = f.footprint.tiles.has_value();
        r.blocksPlacement = f.footprint.blocks_placement;
        r.radius = f.footprint.radius_m.max;
        if (r.large) {
            r.footprintX = (*f.footprint.tiles)[0];
            r.footprintZ = (*f.footprint.tiles)[1];
        }
        r.spacing = f.footprint.min_spacing_m;
        floraPlacementRule(p, assets, r);
        // Cells hold about one plant at the highest density; large plants
        // get at least their footprint plus clearance.
        // Rare species get cells as large as their density asks (about
        // 1000 m at one plant per km²): still about one candidate per cell.
        const double perPlant = Area / std::max(1e-7f, r.density.max);
        r.cell = std::max(1, int(std::lround(std::sqrt(perPlant))));
        if (r.large)
            r.cell = std::max(r.cell, std::max(r.footprintX, r.footprintZ) +
                                          int(std::ceil(r.spacing)));
        r.perCell =
            std::max(1, int(std::ceil(r.density.max * r.cell * r.cell / Area)));
        r.heightTiles = std::max(1, int(std::ceil(floraHeightEstimate(f))));
        double stages = 0;
        for (const auto& s : f.growth.stages)
            if (s) stages += s->max;
        r.maxAgeDays = f.growth.perennial
                           ? std::max(1.f, f.growth.lifespan_days.max)
                           : std::max(1.0, stages);
        if (r.large)
            maxFootprint =
                std::max(maxFootprint, std::max(r.footprintX, r.footprintZ) +
                                           2 * int(std::ceil(r.spacing)));
        maxHeightTiles = std::max(maxHeightTiles, r.heightTiles);
        maxFootprintTiles =
            std::max(maxFootprintTiles, std::max(r.footprintX, r.footprintZ));
        byId[f.id - 1] = int(rules.size());
        rules.push_back(std::move(r));
    }
    // Companions refer to species by name; only small companions spawn
    // (large ones would need their own conflict pass).
    for (const auto& f : all.items)
        for (const auto& c : f.placement.companions) {
            const auto it = all.indexByName.find(c.species);
            if (it == all.indexByName.end()) continue;
            const auto& companion = all.items[it->second];
            if (companion.footprint.tiles) continue;
            rules[size_t(byId[f.id - 1])].companions.push_back(
                {companion.id, c.radius_m, c.chance});
            maxCompanion = std::max(maxCompanion, int(std::ceil(c.radius_m)));
        }
    verdictTiles = maxFootprint + maxCompanion + 1;
    reachTiles = verdictTiles + maxFootprint + 1;
}

std::optional<float> floraSupportHeight(const FloraSpeciesRule& rule,
                                        const ShapedTile& cell, float x,
                                        float z) {
    if (!cell.tile.isSolid()) return std::nullopt;
    if (!cell.shape.code()) return 1.f;
    if (rule.large) return std::nullopt;
    const auto height = tile_geometry::supportHeight(cell.shape, x, z);
    if (!height || *height <= 0) return std::nullopt;
    const auto profile = cell.shape.profile();
    if (profile >= TileShapeProfile::Wall10 &&
        profile <= TileShapeProfile::Wall75) {
        // Test both sides of the circular footprint along the thin axis.
        const bool alongX = uint8_t(cell.shape.facing()) % 2 != 0;
        const float dx = alongX ? rule.radius : 0;
        const float dz = alongX ? 0 : rule.radius;
        if (!tile_geometry::supportHeight(cell.shape, x - dx, z - dz) ||
            !tile_geometry::supportHeight(cell.shape, x + dx, z + dz))
            return std::nullopt;
    } else {
        // A corner's two facets have equal pitch. Slabs have zero pitch.
        // Check the actual local slope in addition to the site's terrain slope.
        const auto surface = tile_geometry::surface(cell.shape);
        const auto n = surface.faces[surface.count - 1].plane.normal;
        if (std::hypot(n.x, n.z) / n.y > rule.maxSlope) return std::nullopt;
    }
    return height;
}

std::optional<float> floraCellSupportHeight(const FloraSpeciesRule& rule,
                                            const SolidCellSnapshot& cell,
                                            float x, float z) {
    if (!cell.compound) return floraSupportHeight(rule, cell.base, x, z);
    const auto geometry = cell.geometry();
    if (rule.large) {
        return geometry.boundaryOpenings(Face::PosY).empty()
                   ? std::optional(1.f)
                   : std::nullopt;
    }

    const auto supportAt = [&](float px, float pz) {
        if (px < 0 || px > 1 || pz < 0 || pz > 1)
            return std::optional<tile_geometry::PieceRayHit>{};
        return geometry.intersectRay({px, 1.001f, pz}, {0, -1, 0});
    };
    const auto center = supportAt(x, z);
    if (!center || center->hit.normal.y <= 0) return std::nullopt;
    const auto n = center->hit.normal;
    if (std::hypot(n.x, n.z) / n.y > rule.maxSlope) return std::nullopt;
    for (const auto d : {Vec2{1, 0}, Vec2{-1, 0}, Vec2{0, 1}, Vec2{0, -1}}) {
        const float px = x + d.x * rule.radius, pz = z + d.y * rule.radius;
        // Footprint that crosses a tile edge is checked by the plant layer.
        const auto edge =
            supportAt(std::clamp(px, 0.f, 1.f), std::clamp(pz, 0.f, 1.f));
        if (!edge || edge->hit.normal.y <= 0 ||
            std::abs(edge->hit.t - center->hit.t) >
                rule.radius * rule.maxSlope + 1e-5f)
            return std::nullopt;
    }
    return 1.001f - center->hit.t;
}

namespace {

struct Env {
    bool valid = false;
    int surfaceY = 0;
    bool submerged = false;
    double slope = 0;
    uint8_t cover = 0, coverDensity = 0;
    uint8_t soil = 0;
    bool topsoil = false;
    TileShape shape{};
    const Region* region = nullptr;
};

class Generator {
   public:
    explicit Generator(const FloraGenerationInput& in)
        : in(in),
          rules(*in.rules),
          sampler(*in.terrain),
          period(in.terrain->periodX()),
          originX(int64_t(in.coord.x) * Chunk::SIZE_X),
          originZ(int64_t(in.coord.z) * Chunk::SIZE_Z) {}

    FloraLayer run();

   private:
    struct Candidate {
        const FloraSpeciesRule* rule;
        double x, z;         // base position, unwrapped frame around the chunk
        int64_t minX, minZ;  // footprint corner (large plants)
        double rank;         // lower wins among equal priorities
        uint32_t hash;
    };

    const Env& env(int64_t x, int64_t z);
    bool suits(const FloraSpeciesRule& r, const Env& e) const;
    bool suits(const Candidate& c);
    template <typename F>
    void candidates(const FloraSpeciesRule& r, double x0, double z0, double x1,
                    double z1, F&& fn);
    bool conflict(const Candidate& a, const Candidate& b) const;
    bool owned(double x, double z) const {
        return x >= originX && x < originX + Chunk::SIZE_X && z >= originZ &&
               z < originZ + Chunk::SIZE_Z;
    }
    int64_t wrapX(int64_t x) const {
        if (period <= 0) return x;
        const int64_t r = x % period;
        return r < 0 ? r + period : r;
    }
    void add(FloraLayer& out, const FloraSpeciesRule& r, double x, double z,
             uint32_t hash);

    const FloraGenerationInput& in;
    const FloraSpeciesTable& rules;
    TerrainSampler sampler;
    int period;
    int64_t originX, originZ;
    std::unordered_map<uint64_t, Env> cache;
};

const Env& Generator::env(int64_t x, int64_t z) {
    const int64_t cx = wrapX(x);
    const uint64_t key = (uint64_t(uint32_t(cx)) << 32) | uint32_t(z);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    Env e;
    const int depth = in.terrain->height * TerrainTuning::RegionZ;
    if (z >= 0 && (depth <= 0 || z < depth) && cx >= 0) {
        // Own columns come from the generation inputs; elsewhere the same
        // values are sampled (TerrainSampler::columns uses these calls).
        const int64_t lx = cx - originX, lz = z - originZ;
        SoilColumn soil;
        if (in.columns && lx >= 0 && lx < Chunk::SIZE_X && lz >= 0 &&
            lz < Chunk::SIZE_Z) {
            const auto& c = in.columns->columns[lz * Chunk::SIZE_X + lx];
            e.surfaceY = c.surface_y;
            e.submerged = c.water.wet();
            e.slope = c.slope;
            e.shape = c.groundShape;
            soil = c.soil;
        } else {
            const auto t = sampler.sample(cx, z);
            e.surfaceY = t.surface_y;
            e.submerged = sampler.water(cx, z, t.surface_y).wet();
            e.slope = t.slope;
            e.shape = sampler.surfaceShape(cx, z);
            soil = sampler.soil(cx, z, t);
        }
        e.cover = soil.cover;
        e.coverDensity = soil.coverDensity;
        if (soil.count > 0) {
            e.soil = soil.layers[0].data_id;
            e.topsoil = soil.layers[0].topsoil;
        }
        e.region = &in.terrain
                        ->at(int(cx / TerrainTuning::RegionX),
                             int(z / TerrainTuning::RegionZ))
                        .region;
        e.valid = true;
    }
    return cache.emplace(key, e).first->second;
}

bool Generator::suits(const FloraSpeciesRule& r, const Env& e) const {
    if (!e.valid) return false;
    return floraSuits(rules, r,
                      {e.surfaceY, e.slope, e.cover, e.coverDensity, e.soil,
                       e.topsoil, e.region, e.submerged});
}

bool Generator::suits(const Candidate& c) {
    if (in.treeDiscs) {
        // A large plant's footprint is a box; pad by its half diagonal.
        const double pad = c.rule->large ? 0.5 * std::hypot(c.rule->footprintX,
                                                            c.rule->footprintZ)
                                         : 0;
        for (const auto& disc : *in.treeDiscs)
            if (disc.contains(c.x, c.z, pad)) return false;
    }
    const auto& anchor =
        env(int64_t(std::floor(c.x)), int64_t(std::floor(c.z)));
    if (!suits(*c.rule, anchor)) return false;
    if (!c.rule->large) return true;
    // A large plant needs level ground under its whole footprint, and no
    // ground rising beside it (its canopy overhangs the footprint a little,
    // and must not grow into a terrace wall).
    for (int dz = -1; dz <= c.rule->footprintZ; ++dz)
        for (int dx = -1; dx <= c.rule->footprintX; ++dx) {
            const auto& e = env(c.minX + dx, c.minZ + dz);
            const bool under = dx >= 0 && dz >= 0 && dx < c.rule->footprintX &&
                               dz < c.rule->footprintZ;
            if (!e.valid) return false;
            if (under ? e.surfaceY != anchor.surfaceY
                      : e.surfaceY > anchor.surfaceY)
                return false;
            // Large plants need full cube landings: a ramp or slab under any
            // part of the footprint would leave their roots unsupported.
            if (e.shape.code()) return false;
        }
    return true;
}

template <typename F>
void Generator::candidates(const FloraSpeciesRule& r, double x0, double z0,
                           double x1, double z1, F&& fn) {
    const auto axis = SoilGenerator::PeriodicAxis::of(r.cell, period);
    const double cellX = axis.cell, cellZ = r.cell;
    const auto i0 = int64_t(std::floor(x0 / cellX)),
               i1 = int64_t(std::floor(x1 / cellX));
    const auto j0 = int64_t(std::floor(z0 / cellZ)),
               j1 = int64_t(std::floor(z1 / cellZ));
    // Expected plants per candidate = density * keep (at most 1 by the
    // choice of cell and perCell); large cells of rare species keep more.
    const double keep = cellX * cellZ / Area / r.perCell;
    for (int64_t j = j0; j <= j1; ++j)
        for (int64_t i = i0; i <= i1; ++i) {
            const int64_t wi = axis.wrap(i);
            for (int k = 0; k < r.perCell; ++k) {
                const uint32_t base = r.persistentSites && in.clusterSeed
                                          ? in.clusterSeed
                                          : in.seed;
                const uint32_t s = base ^ r.seed ^ uint32_t(k * 0x9e3779b1u);
                const double x =
                    (double(i) + SoilGenerator::unit(wi, j, s ^ 0x11u)) * cellX;
                const double z =
                    (double(j) + SoilGenerator::unit(wi, j, s ^ 0x22u)) * cellZ;
                if (x < x0 || x >= x1 || z < z0 || z >= z1) continue;
                const double density =
                    r.density.sample(SoilGenerator::unit(wi, j, s ^ 0x33u));
                double chance = std::min(1.0, density * keep);
                chance *= floraClusterMask(
                    r, in.clusterSeed ? in.clusterSeed : in.seed, x, z, period);
                if (SoilGenerator::unit(wi, j, s ^ 0x44u) >= chance) continue;
                Candidate c{&r,
                            x,
                            z,
                            0,
                            0,
                            SoilGenerator::unit(wi, j, s ^ 0x55u),
                            uint32_t(SoilGenerator::unit(wi, j, s ^ 0x66u) *
                                     4294967295.0)};
                if (r.large) {
                    c.minX = int64_t(std::floor(x)) - r.footprintX / 2;
                    c.minZ = int64_t(std::floor(z)) - r.footprintZ / 2;
                    c.x = double(c.minX) + r.footprintX * 0.5;
                    c.z = double(c.minZ) + r.footprintZ * 0.5;
                }
                fn(c);
            }
        }
}

bool Generator::conflict(const Candidate& a, const Candidate& b) const {
    const double gap = std::max(a.rule->spacing, b.rule->spacing);
    const double ax1 = a.minX + a.rule->footprintX + gap,
                 az1 = a.minZ + a.rule->footprintZ + gap;
    const double bx1 = b.minX + b.rule->footprintX + gap,
                 bz1 = b.minZ + b.rule->footprintZ + gap;
    return a.minX - gap < bx1 && b.minX - gap < ax1 && a.minZ - gap < bz1 &&
           b.minZ - gap < az1;
}

void Generator::add(FloraLayer& out, const FloraSpeciesRule& r, double x,
                    double z, uint32_t hash) {
    const auto& e = env(int64_t(std::floor(x)), int64_t(std::floor(z)));
    if (e.surfaceY / Chunk::SIZE_Y != int(in.coord.y)) return;
    const int ly = e.surfaceY - int(in.coord.y) * Chunk::SIZE_Y;
    const int lx16 = std::clamp(int((x - originX) * 16), 0, 255);
    const int lz16 = std::clamp(int((z - originZ) * 16), 0, 255);
    if (in.tiles) {
        // Live tiles: the ground under the plant (its footprint, as far as
        // it lies in this chunk) must still be solid with open air above.
        const int sx = r.large ? r.footprintX : 1;
        const int sz = r.large ? r.footprintZ : 1;
        const int x0 =
            r.large ? int(std::floor(x - originX - sx * 0.5)) : lx16 / 16;
        const int z0 =
            r.large ? int(std::floor(z - originZ - sz * 0.5)) : lz16 / 16;
        for (int tz = std::max(0, z0); tz < std::min(Chunk::SIZE_Z, z0 + sz);
             ++tz)
            for (int tx = std::max(0, x0);
                 tx < std::min(Chunk::SIZE_X, x0 + sx); ++tx) {
                const auto ground = in.tiles->cellAt(tx, ly, tz);
                if (!floraSupportHeight(r, ground, (lx16 % 16 + .5f) / 16,
                                        (lz16 % 16 + .5f) / 16))
                    return;
                for (int h = 1; h <= r.heightTiles && ly + h < Chunk::SIZE_Y;
                     ++h)
                    if (!in.tiles->tiles[tz][ly + h][tx].isAir()) return;
            }
    }
    FloraInstance p;
    p.species = r.id;
    p.seed = uint16_t(hash >> 16);
    p.x = uint8_t(lx16);
    p.z = uint8_t(lz16);
    p.y = uint8_t(ly);
    p.rotation = uint8_t(hash >> 8);
    // Generated plants germinated at different times before day 0.
    p.birthDay = -int32_t(double(hash & 0xFF) / 255.0 * r.maxAgeDays * 0.9);
    out.instances.push_back(p);
}

FloraLayer Generator::run() {
    FloraLayer out;
    const int reach = rules.reach();
    const double x0 = double(originX), z0 = double(originZ);
    const double x1 = x0 + Chunk::SIZE_X, z1 = z0 + Chunk::SIZE_Z;
    // Large plants: candidates around the chunk; a candidate survives when
    // no conflicting valid candidate outranks it (a local rule, so every
    // chunk decides the same without the global greedy order).
    std::vector<Candidate> large;
    for (const auto& r : rules.species())
        if (r.large)
            candidates(r, x0 - reach, z0 - reach, x1 + reach, z1 + reach,
                       [&](const Candidate& c) {
                           if (suits(c)) large.push_back(c);
                       });
    const auto outranks = [](const Candidate& a, const Candidate& b) {
        if (a.rule->priority != b.rule->priority)
            return a.rule->priority > b.rule->priority;
        if (a.rank != b.rank) return a.rank < b.rank;
        return a.hash < b.hash;
    };
    const double inner = rules.verdict();
    std::vector<const Candidate*> kept;
    for (const auto& a : large) {
        // Only candidates that can matter here need a verdict; their
        // rivals lie within the evaluated ring.
        if (a.x < x0 - inner || a.x >= x1 + inner || a.z < z0 - inner ||
            a.z >= z1 + inner)
            continue;
        const bool beaten =
            std::any_of(large.begin(), large.end(), [&](const Candidate& b) {
                return &a != &b && conflict(a, b) && outranks(b, a);
            });
        if (!beaten) kept.push_back(&a);
    }
    const auto inFootprint = [&](double x, double z) {
        const auto tx = int64_t(std::floor(x)), tz = int64_t(std::floor(z));
        return std::any_of(kept.begin(), kept.end(), [&](const Candidate* c) {
            return tx >= c->minX && tx < c->minX + c->rule->footprintX &&
                   tz >= c->minZ && tz < c->minZ + c->rule->footprintZ;
        });
    };
    for (const auto* c : kept)
        if (owned(c->x, c->z)) add(out, *c->rule, c->x, c->z, c->hash);
    // Small plants of the chunk, cleared from large footprints.
    for (const auto& r : rules.species())
        if (!r.large)
            candidates(r, x0, z0, x1, z1, [&](const Candidate& c) {
                if (!inFootprint(c.x, c.z) && suits(c))
                    add(out, r, c.x, c.z, c.hash);
            });
    // Companions grow around kept large plants, wherever those stand.
    for (const auto* parent : kept)
        for (size_t ci = 0; ci < parent->rule->companions.size(); ++ci) {
            const auto& companion = parent->rule->companions[ci];
            const auto* rule = rules.find(companion.species);
            if (!rule) continue;
            for (int attempt = 0; attempt < 3; ++attempt) {
                const uint32_t s = parent->hash ^ uint32_t(ci * 0x632be5abu) ^
                                   uint32_t(attempt * 0x85ebca6bu);
                const auto u = [&](uint32_t salt) {
                    return SoilGenerator::unit(int64_t(s), attempt, salt);
                };
                if (u(1) >= companion.chance) continue;
                const double minR = 0.5 * std::max(parent->rule->footprintX,
                                                   parent->rule->footprintZ) +
                                    0.3;
                const double radius =
                    minR +
                    (std::max(double(companion.radius), minR) - minR) * u(2);
                const double angle = u(3) * 6.283185307179586;
                const double x = parent->x + std::cos(angle) * radius;
                const double z = parent->z + std::sin(angle) * radius;
                if (!owned(x, z) || inFootprint(x, z)) continue;
                const Candidate c{
                    rule, x, z, 0, 0, 0, uint32_t(u(4) * 4294967295.0)};
                if (suits(c)) add(out, *rule, x, z, c.hash);
            }
        }
    if (out.instances.size() > 16384) out.instances.resize(16384);
    return out;
}

}  // namespace

double floraClusterMask(const FloraSpeciesRule& r, uint32_t worldSeed, double x,
                        double z, int periodX) {
    if (r.clusterScale <= 0) return 1;
    const double n = SoilGenerator::field(x, z, r.clusterScale,
                                          worldSeed ^ r.seed ^ 0xc1u, periodX);
    return smooth((n - r.clusterThreshold) / 0.15);
}

bool floraSuits(const FloraSpeciesTable& table, const FloraSpeciesRule& r,
                const FloraSite& e) {
    if (!e.soil || !e.region) return false;
    if (e.submerged && !r.underwater) return false;
    if (e.slope > r.maxSlope) return false;
    if (r.coverForbidden.test(e.cover)) return false;
    if (r.coverRequired.any() &&
        (!r.coverRequired.test(e.cover) || e.coverDensity < 3))
        return false;
    if (r.soilAllowed.any() && !r.soilAllowed.test(e.soil)) return false;
    if (e.soil > table.soils().size()) return false;
    const auto& soil = table.soils()[e.soil - 1];
    const auto& properties = e.topsoil ? soil.topsoil : soil.base;
    if (!r.fertility.contains(properties.fertility) ||
        !r.retention.contains(properties.water_retention))
        return false;
    if (!r.vegetation.empty() &&
        std::find(r.vegetation.begin(), r.vegetation.end(),
                  e.region->vegetation) == r.vegetation.end())
        return false;
    Region region = *e.region;
    region.land_height_m = e.surfaceY;
    return std::any_of(r.climate.begin(), r.climate.end(), [&](auto& rule) {
        return SoilGenerator::allowed(rule, region, e.submerged);
    });
}

FloraLayer generateFlora(const FloraGenerationInput& input) {
    if (!input.rules || input.rules->empty() || !input.terrain) return {};
    return Generator(input).run();
}
