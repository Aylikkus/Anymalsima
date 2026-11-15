#include "terrain_sampler.hpp"

#include <limits>

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
    result.height_seed = world.genSettings.height_seed;
    result.feature_seed = world.genSettings.feature_seed;
    for (int dz = -2; dz <= 2; ++dz) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int x = std::clamp(int(center.x) + dx, 0, world.width - 1);
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
        if (rx + dx < 0 || rx + dx >= data.width || rz + dz < 0 ||
            rz + dz >= data.height)
            continue;
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
            dx ? edgeX * sx : x, dz ? edgeZ * sz : z, 32, salt);
        const double mask = smooth((segment - .25) / .35);
        lift = std::max(
            lift,
            ends * mask * (1 - smooth(distance / TerrainTuning::CliffWidth)));
    }
    return lift;
}

double TerrainSampler::surface(double x, double z) const {
    constexpr int sx = TerrainTuning::RegionX, sz = TerrainTuning::RegionZ;
    x = std::clamp(x, 0.0, double(data.width) * sx - 1);
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
    base =
        std::lerp(base, double(owner.land_height_m), cliffLift(rx, rz, x, z));
    const double reduction =
        interpolate(true) *
        (.75 * SoilGenerator::field(x, z, TerrainTuning::BroadScale,
                                    data.height_seed ^ 0x1459u) +
         .25 * SoilGenerator::field(x, z, TerrainTuning::FineScale,
                                    data.height_seed ^ 0x43a1u));
    double waterDistance = TerrainTuning::CoastWidth;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = rx + dx, nz = rz + dz;
            if (nx < 0 || nx >= data.width || nz < 0 || nz >= data.height)
                continue;
            if (data.at(nx, nz).region.land_height_m >= SEA_LEVEL_Y) continue;
            const double ax = std::max({nx * sx - x, 0.0, x - (nx + 1) * sx});
            const double az = std::max({nz * sz - z, 0.0, z - (nz + 1) * sz});
            waterDistance = std::min(waterDistance, std::hypot(ax, az));
        }
    const double inlandFloor =
        (SEA_LEVEL_Y + 1) * smooth(waterDistance / TerrainTuning::CoastWidth);
    return std::clamp(std::max(base - reduction, inlandFloor), 0.0,
                      double(std::clamp(owner.land_height_m, 0,
                                        Y_CHUNKS * Chunk::SIZE_Y - 1)));
}

TerrainSample TerrainSampler::sample(int64_t x, int64_t z) const {
    const double center = surface(x, z);
    // Compare against the center as well: central differences alone can hide
    // the slope at a narrow ridge or one-column cliff.
    double slope = 0;
    for (const auto& [dx, dz] :
         std::array<std::pair<int, int>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}})
        slope = std::max(slope, std::abs(surface(x + dx, z + dz) - center));
    return {int(std::floor(center)), slope};
}

SoilColumn TerrainSampler::soil(int64_t x, int64_t z,
                                const TerrainSample& terrain) const {
    constexpr int sx = TerrainTuning::RegionX, sz = TerrainTuning::RegionZ;
    const int rx = int(x / sx), rz = int(z / sz);
    Region local = data.at(rx, rz).region;
    local.land_height_m = terrain.surface_y;
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
        int priority;
    };
    std::array<Choice, 4> choices{};
    int bestPriority = std::numeric_limits<int>::lowest();
    double total = 0;
    int count = 0;
    for (int iz = 0; iz < 2; ++iz)
        for (int ix = 0; ix < 2; ++ix) {
            const double weight = (ix ? wx : 1 - wx) * (iz ? wz : 1 - wz);
            if (weight <= 0) continue;
            const auto& candidate =
                data.at(rx + (ix ? dx : 0), rz + (iz ? dz : 0));
            if (!candidate.soil.block_id) continue;
            int priority = std::numeric_limits<int>::lowest();
            bool eligible = false;
            for (const auto& rule : candidate.soil_rules)
                if (SoilGenerator::allowed(rule, local)) {
                    priority = std::max(priority, rule.priority);
                    eligible = true;
                }
            if (!eligible) continue;
            choices[count++] = {&candidate, weight, priority};
            bestPriority = std::max(bestPriority, priority);
        }
    for (int i = 0; i < count; ++i)
        if (choices[i].priority == bestPriority) total += choices[i].weight;
    if (total <= 0) return {};
    // Canonical spatial ordering prevents a coin flip at the owning-region
    // boundary, where the same candidates would otherwise swap order.
    std::sort(choices.begin(), choices.begin() + count,
              [](const auto& a, const auto& b) {
                  const auto ca = a.region->region.coord,
                             cb = b.region->region.coord;
                  return ca.y == cb.y ? ca.x < cb.x : ca.y < cb.y;
              });
    double choice =
        SoilGenerator::field(x, z, 16, data.feature_seed ^ 0x6187u) * total;
    const TerrainRegion* selected = nullptr;
    for (int i = 0; i < count; ++i) {
        if (choices[i].priority != bestPriority) continue;
        selected = choices[i].region;
        choice -= choices[i].weight;
        if (choice < 0) break;
    }
    auto profile = selected->soil;
    if (terrain.surface_y < SEA_LEVEL_Y ||
        local.vegetation == VegetationType::NONE)
        profile.topsoil_block_id = 0;
    auto cover = SoilGenerator::sample(profile, local, x, z);
    const double retention =
        1 -
        smooth((terrain.slope - TerrainTuning::SoilSlopeStart) /
               (TerrainTuning::SoilSlopeEnd - TerrainTuning::SoilSlopeStart));
    const int depth = int(std::floor(cover.depth_m * retention));
    SoilColumn result;
    result.depth_m = depth;
    for (int i = 0; i < cover.count; ++i) {
        auto layer = cover.layers[i];
        layer.thickness_m =
            std::min(layer.thickness_m, depth - layer.depth_from_surface_m);
        if (layer.thickness_m > 0) result.layers[result.count++] = layer;
    }
    return result;
}

ChunkColumnInputs TerrainSampler::columns(unsigned chunkX,
                                          unsigned chunkZ) const {
    constexpr int pitch = Chunk::SIZE_X + 2;
    std::array<double, pitch*(Chunk::SIZE_Z + 2)> heights{};
    const int64_t originX = int64_t(chunkX) * Chunk::SIZE_X;
    const int64_t originZ = int64_t(chunkZ) * Chunk::SIZE_Z;
    // Evaluate the halo once, then reuse adjacent samples for every slope.
    for (int z = -1; z <= Chunk::SIZE_Z; ++z)
        for (int x = -1; x <= Chunk::SIZE_X; ++x)
            heights[(z + 1) * pitch + x + 1] =
                surface(originX + x, originZ + z);
    ChunkColumnInputs out;
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int x = 0; x < Chunk::SIZE_X; ++x) {
            const int index = (z + 1) * pitch + x + 1;
            const double center = heights[index];
            double slope = 0;
            for (const int offset : {-1, 1, -pitch, pitch})
                slope =
                    std::max(slope, std::abs(heights[index + offset] - center));
            const TerrainSample terrain{int(std::floor(center)), slope};
            out.columns[z * Chunk::SIZE_X + x] = {
                terrain.surface_y, soil(originX + x, originZ + z, terrain)};
        }
    return out;
}
