#include "terrain_sampler.hpp"

#include <limits>

#include "generated_hydrology.hpp"
#include "natural_shape.hpp"
#include "save/world_meta.hpp"

namespace {
double smooth(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return value * value * (3 - 2 * value);
}
double relief(LandType land) {
    switch (land) {
        case LandType::PLAINS:
            return TerrainTuning::PlainRelief;
        case LandType::HILLS:
            return TerrainTuning::HillRelief;
        case LandType::MOUNTAINS:
        case LandType::PEAK:
            return TerrainTuning::MountainRelief;
        default:
            return TerrainTuning::WaterRelief;
    }
}
// Stateless gradient (Perlin) noise in [0, 1] with hashed lattice gradients.
// Unlike SoilGenerator::field its iso-lines are not aligned to the lattice,
// which keeps ridges from looking blocky.
// periodX > 0 makes it repeat along X every periodX tiles (see
// SoilGenerator::PeriodicAxis).
double gradientField(double x, double z, int scale, uint32_t seed,
                     int periodX) {
    const auto axis = SoilGenerator::PeriodicAxis::of(scale, periodX);
    x /= axis.cell;
    z /= scale;
    const auto gx = int64_t(std::floor(x)), gz = int64_t(std::floor(z));
    const double fx = x - gx, fz = z - gz;
    const auto dot = [&](int64_t cx, int64_t cz) {
        const double angle =
            SoilGenerator::unit(axis.wrap(cx), cz, seed) * 6.283185307179586;
        return std::cos(angle) * (x - cx) + std::sin(angle) * (z - cz);
    };
    const auto fade = [](double t) {
        return t * t * t * (t * (t * 6 - 15) + 10);
    };
    const double u = fade(fx), v = fade(fz);
    const double value =
        std::lerp(std::lerp(dot(gx, gz), dot(gx + 1, gz), u),
                  std::lerp(dot(gx, gz + 1), dot(gx + 1, gz + 1), u), v);
    // 2D Perlin stays within about ±0.71.
    return std::clamp(.5 + value * .7071, 0.0, 1.0);
}
}  // namespace

TerrainNeighborhood TerrainNeighborhood::capture(const World& world,
                                                 Vec2u center,
                                                 const IAssets& assets) {
    if (world.width <= 0 || world.height <= 0 ||
        world.regions.size() != size_t(world.width) * world.height ||
        center.x >= unsigned(world.width) || center.y >= unsigned(world.height))
        throw std::invalid_argument("Invalid terrain world or region");
    TerrainNeighborhood result;
    result.center = center;
    result.width = world.width;
    result.height = world.height;
    result.generatorVersion = world.generatorVersion;
    result.height_seed = world.genSettings.height_seed;
    result.feature_seed = world.genSettings.feature_seed;
    result.hydrology = world.hydrology;
    for (int dz = -2; dz <= 2; ++dz) {
        for (int dx = -2; dx <= 2; ++dx) {
            // X wraps around the world; Z clamps to its edge rows.
            const int x = int(world.wrap().wrapRegion(int64_t(center.x) + dx));
            const int z = std::clamp(int(center.y) + dz, 0, world.height - 1);
            const auto& source = world.regions[z * world.width + x];
            auto& target = result.regions[(dz + 2) * 5 + dx + 2];
            // Copy only the generation inputs, not entities and their storage.
            auto& r = target.region;
            r.coord = source.coord;
            r.land_height_m = source.land_height_m;
            r.land = source.land;
            r.biome = source.biome;
            r.features = source.features;
            r.vegetation = source.vegetation;
            r.humidity = source.humidity;
            r.temperature_c = source.temperature_c;
            target.soil = SoilGenerator::select(r, assets, result.feature_seed);
            if (!target.soil.soil_id) continue;
            const auto& definition =
                assets.getStorage<Soil>().get(target.soil.soil_id);
            target.soil_rules = definition.placement;
            target.cover = GroundCoverGenerator::select(
                r, target.soil.soil_id, assets, result.feature_seed);
            // Preserve the variant for land columns in a neighboring region.
            if (definition.topsoil_block_name)
                target.soil.topsoil_block_id =
                    assets.getStorage<Block>()
                        .get(*definition.topsoil_block_name)
                        .id;
        }
    }
    return result;
}

bool TerrainNeighborhood::naturalShapes() const {
    return generatorVersion >= WorldMeta::NaturalShapeVersion;
}

bool TerrainNeighborhood::ridgeShapes() const {
    return generatorVersion >= WorldMeta::NaturalShapeRidgeVersion;
}

bool TerrainNeighborhood::diagonalShapes() const {
    return generatorVersion >= WorldMeta::NaturalShapeDiagonalVersion;
}

// Half-region lattice: centers take their region ceiling, edge midpoints the
// lower adjoining ceiling, and corners the minimum of the four ceilings.
// Amplitudes use the same shared lattice with averaged terrain strengths.
double TerrainSampler::control(int gx, int gz, bool amplitude) const {
    const int rx = gx / 2;
    const int rz = gz / 2;
    const int firstX = gx % 2 == 0 ? rx - 1 : rx;
    const int firstZ = gz % 2 == 0 ? rz - 1 : rz;
    double result = amplitude ? 0 : std::numeric_limits<double>::max();
    int count = 0;
    for (int z = firstZ; z <= rz; ++z)
        for (int x = firstX; x <= rx; ++x) {
            const auto& r = data.at(x, z).region;
            if (amplitude)
                result += relief(r.land);
            else
                result = std::min(result, double(r.land_height_m));
            ++count;
        }
    return amplitude ? result / count : result;
}

double TerrainSampler::cliffLift(int rx, int rz, double x, double z) const {
    const auto& owner = data.at(rx, rz).region;
    double lift = 0;
    constexpr int sx = TerrainTuning::RegionX, sz = TerrainTuning::RegionZ;
    for (const auto& [dx, dz] : std::array<std::pair<int, int>, 4>{
             {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
        if (rz + dz < 0 || rz + dz >= data.height) continue;
        const auto& other = data.at(rx + dx, rz + dz).region;
        const int drop = owner.land_height_m - other.land_height_m;
        if (drop <= 0) continue;
        const bool cliff = owner.hasFeature(FeatureMask::CLIFF) ||
                           other.hasFeature(FeatureMask::CLIFF);
        const bool ravine = (owner.hasFeature(FeatureMask::RAVINE) ||
                             other.hasFeature(FeatureMask::RAVINE)) &&
                            drop >= TerrainTuning::RavineDrop;
        if (!cliff && !ravine) continue;
        const double distance = dx < 0   ? x - rx * sx
                                : dx > 0 ? (rx + 1) * sx - x
                                : dz < 0 ? z - rz * sz
                                         : (rz + 1) * sz - z;
        if (distance > TerrainTuning::CliffWidth) continue;
        const double along = dx ? z - rz * sz : x - rx * sx;
        const double length = dx ? sz : sx;
        const double ends = smooth(along / 16) * smooth((length - along) / 16);
        // The high region determines the canonical edge coordinate, regardless
        // of the caller's neighborhood or chunk loading order.
        const int edgeX = dx ? rx + (dx > 0) : rx;
        const int edgeZ = dz ? rz + (dz > 0) : rz;
        const uint32_t salt = data.feature_seed ^ (dx ? 0x6791u : 0x9b23u);
        const double segment = SoilGenerator::field(
            dx ? edgeX * sx : x, dz ? edgeZ * sz : z, 32, salt, data.periodX());
        const double mask = smooth((segment - .25) / .35);
        lift = std::max(
            lift,
            ends * mask * (1 - smooth(distance / TerrainTuning::CliffWidth)));
    }
    return lift;
}

// Signed distance from (x, z) to the nearest land/water region boundary,
// positive on land, after warping the sample point. Only the 3x3 regions
// around the caller's region (rx, rz) are inspected: the warp is bounded by
// CoastWarpTotal, so any boundary outside them is farther than CoastCap.
double TerrainSampler::coastDistance(int rx, int rz, double x, double z) const {
    constexpr int sx = TerrainTuning::RegionX, sz = TerrainTuning::RegionZ;
    // A bounded warp cannot reach outside this neighborhood. If all
    // regions agree, even the jittered distance is beyond the beach and
    // shelf bands. Their clamped weights and belowSea are identical.
    static_assert(
        TerrainTuning::CoastCap - TerrainTuning::CoastJitter >=
        std::max({TerrainTuning::BeachWidth, TerrainTuning::ShelfWidth,
                  TerrainTuning::BlendWidth}));
    const bool ownerLand = data.at(rx, rz).region.land_height_m >= SEA_LEVEL_Y;
    bool uniform = true;
    for (int dz = -1; dz <= 1 && uniform; ++dz)
        for (int dx = -1; dx <= 1 && uniform; ++dx)
            uniform = (data.at(rx + dx, rz + dz).region.land_height_m >=
                       SEA_LEVEL_Y) == ownerLand;
    if (uniform)
        return ownerLand ? TerrainTuning::CoastCap : -TerrainTuning::CoastCap;

    const uint32_t seed = data.height_seed;
    const auto warp = [&](uint32_t salt) {
        double offset = 0;
        for (const auto& octave : TerrainTuning::CoastWarp) {
            offset += octave.amplitude *
                      (2 * SoilGenerator::field(x, z, octave.scale, seed ^ salt,
                                                data.periodX()) -
                       1);
            salt = salt * 0x9e3779b1u + 0x7f4au;
        }
        return offset;
    };
    // X stays unwrapped so distances below share one frame; at() wraps.
    const double qx = x + warp(0x2c1bu);
    const double qz =
        std::clamp(z + warp(0x58d3u), 0.0, double(data.height) * sz - 1e-3);
    const bool land =
        data.at(int(std::floor(qx / sx)), int(qz / sz)).region.land_height_m >=
        SEA_LEVEL_Y;
    double distance = TerrainTuning::CoastCap;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = rx + dx, nz = rz + dz;
            if (nz < 0 || nz >= data.height) continue;
            if ((data.at(nx, nz).region.land_height_m >= SEA_LEVEL_Y) == land)
                continue;
            const double ax = std::max({nx * sx - qx, 0.0, qx - (nx + 1) * sx});
            const double az = std::max({nz * sz - qz, 0.0, qz - (nz + 1) * sz});
            distance = std::min(distance, std::hypot(ax, az));
        }
    // Jitter the signed value: it stays continuous across the boundary.
    return (land ? distance : -distance) +
           TerrainTuning::CoastJitter *
               (2 * SoilGenerator::field(x, z, TerrainTuning::CoastJitterScale,
                                         seed ^ 0x3ad7u, data.periodX()) -
                1);
}

double TerrainSampler::surface(double x, double z) const {
    return heightsAt(x, z).ground;
}

int TerrainSampler::tileSurface(int64_t x, int64_t z) const {
    return int(std::floor(shapeHeights(x, z).ground));
}

TerrainSampler::Heights TerrainSampler::heightsAt(double x, double z) const {
    constexpr int sx = TerrainTuning::RegionX, sz = TerrainTuning::RegionZ;
    // X wraps around the world; Z clamps to its edges.
    const int period = data.periodX();
    if (period > 0) {
        if (x < 0 || x >= period) x = WorldWrap::modf(x, period);
    } else {
        x = std::max(x, 0.0);
    }
    z = std::clamp(z, 0.0, double(data.height) * sz - 1);
    const int rx = int(x / sx), rz = int(z / sz);
    const int gx = int(x / (sx / 2)), gz = int(z / (sz / 2));
    const double tx = smooth(x / (sx / 2) - gx);
    const double tz = smooth(z / (sz / 2) - gz);
    const auto interpolate = [&](bool amplitude) {
        return std::lerp(std::lerp(control(gx, gz, amplitude),
                                   control(gx + 1, gz, amplitude), tx),
                         std::lerp(control(gx, gz + 1, amplitude),
                                   control(gx + 1, gz + 1, amplitude), tx),
                         tz);
    };
    const auto& owner = data.at(rx, rz).region;
    double base = interpolate(false);
    const double elevation =
        std::max(0.0, base - double(HeightProfile::PlainsTopY));
    base =
        std::lerp(base, double(owner.land_height_m), cliffLift(rx, rz, x, z));
    const double broad = SoilGenerator::field(
        x, z, TerrainTuning::BroadScale, data.height_seed ^ 0x1459u, period);
    const double fine = SoilGenerator::field(
        x, z, TerrainTuning::FineScale, data.height_seed ^ 0x43a1u, period);
    double valleys = broad;
    if (elevation > 0) {
        // Ridged fBm: sharp crests where each octave crosses 0.5, wide valleys
        // between them. 1 on a crest, 0 deep in a valley.
        double ridge = 0;
        uint32_t salt = 0x6c2fu;
        for (const auto& octave : TerrainTuning::RidgeOctaves) {
            const double crest =
                1 -
                std::abs(2 * gradientField(x, z, octave.scale,
                                           data.height_seed ^ salt, period) -
                         1);
            ridge += octave.amplitude * crest * crest;
            salt = salt * 0x9e3779b1u + 0x51edu;
        }
        valleys = std::lerp(broad, 1 - ridge,
                            smooth(elevation / TerrainTuning::RidgeBlend));
    }
    const double reduction =
        interpolate(true) * (.75 * broad + .25 * fine) +
        TerrainTuning::ReliefPerMetre * elevation * valleys;
    const double terrain = base - reduction;

    const double coast = coastDistance(rx, rz, x, z);
    const double land =
        std::max(terrain, SEA_LEVEL_Y + 1 +
                              TerrainTuning::BeachRise *
                                  smooth(coast / TerrainTuning::BeachWidth));
    const double seabed =
        std::min(terrain, SEA_LEVEL_Y - 1 -
                              TerrainTuning::ShelfDrop *
                                  smooth(-coast / TerrainTuning::ShelfWidth));
    const double shore = std::lerp(
        seabed, land, smooth(.5 + coast / (2 * TerrainTuning::BlendWidth)));
    // The ceiling blends across region borders (as soil does): a low land
    // region caps its beach lower than the sea region beside it, and the
    // owner's ceiling alone would step the beach along the border.
    const auto axis = [](double pos, int size) {
        if (pos < TerrainTuning::SoilHalfWidth)
            return std::pair{
                -1, .5 * (1 - smooth(pos / TerrainTuning::SoilHalfWidth))};
        if (pos > size - TerrainTuning::SoilHalfWidth)
            return std::pair{
                1,
                .5 * (1 - smooth((size - pos) / TerrainTuning::SoilHalfWidth))};
        return std::pair{0, 0.0};
    };
    const auto [dx, wx] = axis(x - double(rx) * sx, sx);
    const auto [dz, wz] = axis(z - double(rz) * sz, sz);
    double ceiling = 0;
    for (int iz = 0; iz < 2; ++iz)
        for (int ix = 0; ix < 2; ++ix) {
            const double weight = (ix ? wx : 1 - wx) * (iz ? wz : 1 - wz);
            if (weight <= 0) continue;
            ceiling +=
                weight * TerrainTuning::surfaceCeiling(
                             data.at(rx + (ix ? dx : 0), rz + (iz ? dz : 0))
                                 .region.land_height_m);
        }
    const double original = std::clamp(shore, 0.0, ceiling);
    return {data.hydrology ? data.hydrology->carve(int(x), int(z), original)
                           : original,
            original};
}

bool TerrainSampler::belowSea(int x, int z) const {
    const int rx = x / TerrainTuning::RegionX;
    const int rz = z / TerrainTuning::RegionZ;
    const double coast = coastDistance(rx, rz, x, z);
    // The land branch and the ceiling both stay above the water fill.
    // The seabed branch stays below it. Only the blend needs full relief.
    if (coast <= -TerrainTuning::BlendWidth) return true;
    if (coast >= TerrainTuning::BlendWidth) return false;
    return surface(x, z) < SEA_LEVEL_Y;
}

TerrainSample TerrainSampler::sample(int64_t x, int64_t z) const {
    const auto center = shapeHeights(x, z);
    // Compare against the center as well: central differences alone can hide
    // the slope at a narrow ridge or one-column cliff.
    TerrainSample result{int(std::floor(center.ground))};
    for (const auto& [dx, dz] : std::array<std::pair<int, int>, 4>{
             {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
        const auto neighbor = shapeHeights(x + dx, z + dz);
        result.slope =
            std::max(result.slope, std::abs(neighbor.ground - center.ground));
        result.originalSlope =
            std::max(result.originalSlope,
                     std::abs(neighbor.original - center.original));
    }
    return result;
}

bool TerrainSampler::shapeEligible(int64_t x, int64_t z) const {
    if (!data.naturalShapes() || data.periodX() <= 0) return false;
    const uint64_t key = (uint64_t(x) << 32) ^ uint32_t(z);
    if (const auto found = shapeEligibleCache.find(key);
        found != shapeEligibleCache.end())
        return found->second;
    const auto center = shapeHeights(x, z);
    const int ground = int(std::floor(center.ground));
    // A partial solid owns its whole fluid cell, so keep ramps out of wet
    // ground and hydrology-controlled banks.
    const auto surfaceWater = water(x, z, ground);
    bool eligible = !(surfaceWater.wet() && surfaceWater.surfaceY > ground);
    double slope = 0, originalSlope = 0;
    if (eligible) {
        for (const auto& [dx, dz] : std::array<std::pair<int, int>, 4>{
                 {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
            const auto neighbor = shapeHeights(x + dx, z + dz);
            slope = std::max(slope, std::abs(neighbor.ground - center.ground));
            originalSlope = std::max(
                originalSlope, std::abs(neighbor.original - center.original));
        }
        // Carved river banks and coastal cuts have a different ground and
        // original profile; leave them to hydrology so a ramp cannot breach
        // the water barrier.
        eligible = slope <= 1.0 + 1e-6 && originalSlope <= slope + 1e-6;
    }
    constexpr int sx = TerrainTuning::RegionX, sz = TerrainTuning::RegionZ;
    const int period = data.periodX();
    const int wx = period > 0 ? int(WorldWrap::mod(x, period)) : int(x);
    if (eligible && data.hydrology && data.hydrology->riverAt(wx, int(z)))
        eligible = false;
    const int rx = wx / sx, rz = int(z) / sz;
    if (eligible && data.hydrology &&
        !data.hydrology->containsColumn(wx, int(z)))
        eligible = false;
    if (eligible) {
        const auto& region = data.at(rx, rz).region;
        eligible = !region.hasFeature(FeatureMask::CLIFF) &&
                   !region.hasFeature(FeatureMask::RAVINE);
    }
    if (shapeEligibleCache.size() >= (1u << 16)) shapeEligibleCache.clear();
    shapeEligibleCache.emplace(key, eligible);
    return eligible;
}

TerrainSampler::Heights TerrainSampler::shapeHeights(int64_t x,
                                                     int64_t z) const {
    const uint64_t key = (uint64_t(x) << 32) ^ uint32_t(z);
    if (const auto found = shapeHeightCache.find(key);
        found != shapeHeightCache.end())
        return found->second;
    if (shapeHeightCache.size() >= (1u << 16)) shapeHeightCache.clear();
    return shapeHeightCache.emplace(key, heightsAt(double(x), double(z)))
        .first->second;
}

void TerrainSampler::rememberShapeHeight(int64_t x, int64_t z,
                                         Heights value) const {
    const uint64_t key = (uint64_t(x) << 32) ^ uint32_t(z);
    if (shapeHeightCache.size() >= (1u << 16)) shapeHeightCache.clear();
    shapeHeightCache.emplace(key, value);
}

TileShape TerrainSampler::surfaceShape(int64_t x, int64_t z) const {
    if (!data.naturalShapes()) return {};
    if (!shapeField) {
        shapeField = std::make_unique<natural_shape::Field>(
            [this](int64_t px, int64_t pz) {
                return int(std::floor(shapeHeights(px, pz).ground));
            },
            [this](int64_t px, int64_t pz) { return shapeEligible(px, pz); },
            data.ridgeShapes(), data.diagonalShapes());
    }
    return shapeField->at(x, z);
}

WaterSurface TerrainSampler::water(int64_t x, int64_t z, int groundY) const {
    if (!data.hydrology) return WaterSurface::sea(groundY);
    return data.hydrology->waterAt(int(WorldWrap::mod(x, data.periodX())),
                                   int(z), groundY);
}

SoilColumn TerrainSampler::soil(int64_t x, int64_t z,
                                const TerrainSample& terrain) const {
    return soil(x, z, terrain, water(x, z, terrain.surface_y));
}

SoilColumn TerrainSampler::soil(int64_t x, int64_t z,
                                const TerrainSample& terrain,
                                WaterSurface surfaceWater) const {
    constexpr int sx = TerrainTuning::RegionX, sz = TerrainTuning::RegionZ;
    const int rx = int(x / sx), rz = int(z / sz);
    const double lx = double(x) - rx * sx, lz = double(z) - rz * sz;
    const auto axis = [](double pos, int size) {
        if (pos < TerrainTuning::SoilHalfWidth)
            return std::pair{
                -1, .5 * (1 - smooth(pos / TerrainTuning::SoilHalfWidth))};
        if (pos > size - TerrainTuning::SoilHalfWidth)
            return std::pair{
                1,
                .5 * (1 - smooth((size - pos) / TerrainTuning::SoilHalfWidth))};
        return std::pair{0, 0.0};
    };
    const auto [dx, wx] = axis(lx, sx);
    const auto [dz, wz] = axis(lz, sz);
    struct Choice {
        const TerrainRegion* region;
        double weight;
    };
    std::array<Choice, 4> choices{};
    const bool submerged = surfaceWater.wet();
    const bool riverSediment =
        data.hydrology && data.hydrology->usesRiverSediment() &&
        (surfaceWater.reach ||
         (!submerged && terrain.slope > terrain.originalSlope));
    double total = 0;
    int count = 0;
    for (int iz = 0; iz < 2; ++iz)
        for (int ix = 0; ix < 2; ++ix) {
            const double weight = (ix ? wx : 1 - wx) * (iz ? wz : 1 - wz);
            if (weight <= 0) continue;
            const auto& candidate =
                data.at(rx + (ix ? dx : 0), rz + (iz ? dz : 0));
            if (!candidate.soil.block_id) continue;
            // Each region already chose its soil (SoilGenerator::select, by
            // priority). Its rules are checked against that region's own
            // features and climate, at this column's height, so a soil
            // blends across the border instead of stopping at it, while the
            // underwater rule follows the shoreline of standing water.
            // River deposits inherit the local soil of the surrounding land.
            Region source = candidate.region;
            source.land_height_m = terrain.surface_y;
            const bool eligible =
                std::any_of(candidate.soil_rules.begin(),
                            candidate.soil_rules.end(), [&](const auto& rule) {
                                return SoilGenerator::allowed(
                                    rule, source, submerged && !riverSediment);
                            });
            if (!eligible) continue;
            choices[count++] = {&candidate, weight};
            total += weight;
        }
    if (total <= 0) return {};
    // Canonical spatial ordering prevents a coin flip at the owning-region
    // boundary, where the same candidates would otherwise swap order.
    std::sort(choices.begin(), choices.begin() + count,
              [](const auto& a, const auto& b) {
                  const auto ca = a.region->region.coord,
                             cb = b.region->region.coord;
                  return ca.y == cb.y ? ca.x < cb.x : ca.y < cb.y;
              });
    // Value noise (uniform-ish weights) sampled in a domain rotated by 30
    // degrees at a scale coprime with the chunk size, so the mixed pattern
    // follows neither the axes nor chunk borders.
    const auto rotated = [&](double px) {
        const double u = px * 0.8660254037844386 + double(z) * 0.5;
        const double v = double(z) * 0.8660254037844386 - px * 0.5;
        return SoilGenerator::field(u, v, 21, data.feature_seed ^ 0x6187u);
    };
    // A rotated lattice cannot repeat along X, so the last tiles before the
    // seam blend into the pattern continuing from x = 0.
    double pattern = rotated(double(x));
    if (const int period = data.periodX(); period > 0) {
        constexpr double SeamBlend = 64;
        const double into = double(x) - (period - SeamBlend);
        if (into > 0)
            pattern = std::lerp(pattern, rotated(double(x) - period),
                                smooth(into / SeamBlend));
    }
    double choice = pattern * total;
    const TerrainRegion* selected = choices[count - 1].region;
    for (int i = 0; i < count; ++i) {
        choice -= choices[i].weight;
        if (choice < 0) {
            selected = choices[i].region;
            break;
        }
    }
    // Topsoil (vegetation), bare-rock exposure and depth rules come from the
    // region the soil was taken from, not from the column's own region:
    // otherwise they switch along the region border even when both sides
    // use the same soil (e.g. a bare beach next to a vegetated one).
    Region source = selected->region;
    source.land_height_m = terrain.surface_y;
    if (data.hydrology &&
        data.hydrology->springRockAt(int(x), terrain.surface_y, int(z)))
        return {};
    auto profile = selected->soil;
    if (submerged || source.vegetation == VegetationType::NONE)
        profile.topsoil_block_id = 0;
    auto cover = SoilGenerator::sample(profile, source, x, z, data.periodX());
    // River cuts can create abrupt one-block steps on otherwise gentle land.
    const double slope = riverSediment
                             ? std::min(terrain.slope, terrain.originalSlope)
                             : terrain.slope;
    const double retention =
        1 -
        smooth((slope - TerrainTuning::SoilSlopeStart) /
               (TerrainTuning::SoilSlopeEnd - TerrainTuning::SoilSlopeStart));
    const int depth = int(std::floor(cover.depth_m * retention));
    SoilColumn result;
    result.depth_m = depth;
    result.riverSediment = riverSediment && depth > 0;
    for (int i = 0; i < cover.count; ++i) {
        auto layer = cover.layers[i];
        layer.thickness_m =
            std::min(layer.thickness_m, depth - layer.depth_from_surface_m);
        if (layer.thickness_m > 0) result.layers[result.count++] = layer;
    }
    // Cover grows on the top topsoil tile of dry land. It follows the soil's
    // source region, so it blends across region borders with the soil.
    if (result.count > 0 && result.layers[0].topsoil &&
        result.layers[0].depth_from_surface_m == 0 && !submerged &&
        selected->cover.cover_id) {
        result.cover = selected->cover.cover_id;
        result.coverDensity = GroundCoverGenerator::density(
            selected->cover, x, z, terrain.slope, data.periodX());
    }
    return result;
}

ChunkColumnInputs TerrainSampler::columns(unsigned chunkX,
                                          unsigned chunkZ) const {
    // The natural-shape rule inspects neighbours up to two cells away (a
    // candidate's landing can itself be inspected by an approach test). Use a
    // two-cell halo so the height field is evaluated once and shared by slope,
    // eligibility and shape lookups instead of re-sampling noise per query.
    constexpr int margin = 2;
    constexpr int pitch = Chunk::SIZE_X + margin * 2;
    std::array<Heights, pitch*(Chunk::SIZE_Z + margin * 2)> heights{};
    const int64_t originX = int64_t(chunkX) * Chunk::SIZE_X;
    const int64_t originZ = int64_t(chunkZ) * Chunk::SIZE_Z;
    for (int z = -margin; z < Chunk::SIZE_Z + margin; ++z)
        for (int x = -margin; x < Chunk::SIZE_X + margin; ++x) {
            const auto index = (z + margin) * pitch + x + margin;
            heights[index] = heightsAt(originX + x, originZ + z);
            rememberShapeHeight(originX + x, originZ + z, heights[index]);
        }
    ChunkColumnInputs out;
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int x = 0; x < Chunk::SIZE_X; ++x) {
            const int index = (z + margin) * pitch + x + margin;
            const auto center = heights[index];
            TerrainSample terrain{int(std::floor(center.ground))};
            for (const int offset : {-1, 1, -pitch, pitch}) {
                const auto neighbor = heights[index + offset];
                terrain.slope = std::max(
                    terrain.slope, std::abs(neighbor.ground - center.ground));
                terrain.originalSlope =
                    std::max(terrain.originalSlope,
                             std::abs(neighbor.original - center.original));
            }
            const auto surfaceWater =
                water(originX + x, originZ + z, terrain.surface_y);
            out.columns[z * Chunk::SIZE_X + x] = {
                terrain.surface_y,
                soil(originX + x, originZ + z, terrain, surfaceWater),
                terrain.slope, surfaceWater,
                data.naturalShapes() ? surfaceShape(originX + x, originZ + z)
                                     : TileShape{}};
        }
    return out;
}
