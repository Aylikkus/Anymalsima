#pragma once

#include <vector>

#include "chunk.hpp"
#include "save/world_meta.hpp"
#include "snow_settlement.hpp"
#include "soil_generator.hpp"
#include "terrain_sampler.hpp"
#include "world_climate.hpp"

namespace snow::polar {

// Generator 28 starts at 08:00 on the first day. Keep that date in the
// baseline even when visiting a footprint much later.
inline constexpr double StartingDay = 8.0 / 24.0;
inline constexpr int SeasonStepDays = 5;
inline constexpr int PermanentOnly = -1;
inline constexpr double FullSeasonalCoverC = -12.;
inline constexpr double BareSeasonalGroundC = 1.;
inline constexpr double SnowlineVariationC = 2.;

inline int seasonDay(double days) {
    return world_calendar::fromDays(days).dayOfYear / SeasonStepDays *
           SeasonStepDays;
}

inline bool supported(const World& world) {
    return world.generatorVersion >= WorldMeta::PolarPackVersion &&
           world.width > 0 && world.height > 1 && !world.regions.empty();
}

// Test the same region centres used by the continuous snow field. A patch
// saturated at either end of both seasons has identical seasonal stock.
inline bool seasonChanges(const World& world, int x0, int z0, int x1, int z1,
                          int before, int after) {
    if (before == after || !supported(world)) return false;
    constexpr int size = WorldWrap::RegionSize;
    const auto first = [](int tile) {
        return int(std::floor(double(tile) / size - .5));
    };
    const int endZ = std::clamp(first(z1) + 1, 0, world.height - 1);
    bool cold = true, warm = true;
    for (int z = std::max(0, first(z0)); z <= endZ; ++z) {
        const float oldOffset =
            world_climate::seasonalOffsetC(z, world.height, before);
        const float newOffset =
            world_climate::seasonalOffsetC(z, world.height, after);
        for (int x = first(x0); x <= first(x1) + 1; ++x) {
            const auto rx = world.wrap().wrapRegion(x);
            const float mean =
                world.regions[size_t(z) * world.width + rx].temperature_c;
            cold &= mean + std::max(oldOffset, newOffset) <=
                    FullSeasonalCoverC - SnowlineVariationC;
            warm &= mean + std::min(oldOffset, newOffset) >=
                    BareSeasonalGroundC + SnowlineVariationC;
            if (!cold && !warm) return true;
        }
    }
    return false;
}

inline double latitude(int height, int z) {
    return std::abs(2 * (z + .5) / (double(height) * WorldWrap::RegionSize) -
                    1);
}

inline bool eligibleLatitude(int generatorVersion, int height, int z) {
    return generatorVersion >= WorldMeta::PolarPackVersion && height > 1 &&
           z >= 0 && z < height * WorldWrap::RegionSize &&
           latitude(height, z) > .72;
}

inline bool eligibleLatitude(const World& world, int z) {
    return world.width > 0 && !world.regions.empty() &&
           eligibleLatitude(world.generatorVersion, world.height, z);
}

// Owner-thread sampler of the immutable starting climate. Only the visited
// region centres are evaluated and neighbouring footprints share the result.
inline std::optional<PackRun> fixturePack(const World& world,
                                          const TilePos& fixture, int x,
                                          int z) {
    const auto dx = world.wrap().deltaTile(x, fixture.x);
    if (dx < -3 || dx >= 3 || z < fixture.z || z >= fixture.z + 6)
        return std::nullopt;
    const auto base = fixture.y * UnitsPerCubicMetre;
    return PackRun{int32_t(base), int32_t(base + MaximumVolume)};
}

struct SurfaceSnapshot {
    uint8_t block = 0;
    bool initialPacks = true;
    int day = 0;
    std::optional<TilePos> fixture;
    std::map<ChunkColumn, std::shared_ptr<const PackTops>> edits;
    // Ground-resting loose snow tops. They ride on the current seasonal
    // pack, so the LOD keeps weather snow after its chunk unloads without
    // freezing the seasonal baseline beneath it.
    std::map<ChunkColumn, std::shared_ptr<const PackTops>> loose;

    std::optional<int32_t> editedTop(int x, int z) const {
        return lookup(edits, x, z);
    }
    std::optional<int32_t> looseTop(int x, int z) const {
        return lookup(loose, x, z);
    }

   private:
    static std::optional<int32_t> lookup(
        const std::map<ChunkColumn, std::shared_ptr<const PackTops>>& map,
        int x, int z) {
        const auto column = map.find(
            {unsigned(x / Chunk::SIZE_X), unsigned(z / Chunk::SIZE_Z)});
        if (column == map.end()) return std::nullopt;
        const auto found = column->second->find({x, z});
        if (found == column->second->end()) return std::nullopt;
        return found->second;
    }

   public:
};

// Wide, nearly level ground can keep a deep pack. Relief in the
// surrounding metre limits both the starting stock and later accumulation.
inline double retention(const ColumnGenerationInput& ground) {
    const double slope = std::max(ground.slope, ground.snowSlope);
    const double shelter = std::max(0., 1 - slope);
    const double grade = slope / .10;
    return shelter * shelter / (1 + grade * grade);
}

inline Volume capacity(const ColumnGenerationInput& ground) {
    // A ramp holds at most one cell of draped snow.
    if (ground.groundShape.code()) return UnitsPerCubicMetre;
    return std::llround(MaximumVolume * retention(ground));
}

class Baselines {
   public:
    explicit Baselines(const World& world) : world(world) {}

    double depth(int x, int z, int day) const {
        if (!supported(world) || z < 0 ||
            z >= world.height * WorldWrap::RegionSize)
            return 0;

        constexpr double size = WorldWrap::RegionSize;
        const double fx = (world.wrap().wrapTile(x) + .5) / size - .5;
        const double fz = (z + .5) / size - .5;
        const int gx = int(std::floor(fx)), gz = int(std::floor(fz));
        const double tx = fx - gx, tz = fz - gz;
        double mean = 0, humidity = 0, amplitude = 0, offset = 0;
        if (annual.empty()) annual.resize(world.height, Missing);
        if (date != day) {
            date = day;
            seasonal.assign(world.height, Missing);
        }
        for (int dz = 0; dz < 2; ++dz) {
            const int rz = std::clamp(gz + dz, 0, world.height - 1);
            if (annual[rz] == Missing)
                annual[rz] =
                    world_climate::seasonalAmplitudeC(rz, world.height);
            if (seasonal[rz] == Missing)
                seasonal[rz] =
                    day == PermanentOnly
                        ? 0.f
                        : world_climate::seasonalOffsetC(rz, world.height, day);
            for (int dx = 0; dx < 2; ++dx) {
                const auto rx = world.wrap().wrapRegion(gx + dx);
                const auto& region =
                    world.regions[size_t(rz) * world.width + rx];
                const double weight = (dx ? tx : 1 - tx) * (dz ? tz : 1 - tz);
                mean += weight * region.temperature_c;
                humidity += weight * region.humidity;
                amplitude += weight * annual[rz];
                offset += weight * seasonal[rz];
            }
        }

        const double supply = .35 + .65 * humidity;
        const double polar = smooth((latitude(world.height, z) - .55) / .36);
        const double permanent =
            std::max(0., -(mean + amplitude + 2.)) * supply * polar;
        if (day == PermanentOnly)
            return std::min(permanent, double(MaximumDepth));
        const double temp = mean + offset;
        double seasonalDepth = 0.;
        if (temp <= FullSeasonalCoverC - SnowlineVariationC) {
            seasonalDepth = .5 * supply;
        } else if (temp < BareSeasonalGroundC + SnowlineVariationC) {
            const double variation =
                (2 * SoilGenerator::field(x, z, 48,
                                          world.genSettings.feature_seed,
                                          world.wrap().tiles) -
                 1) *
                SnowlineVariationC;
            seasonalDepth = .5 * supply *
                            smooth((BareSeasonalGroundC - temp - variation) /
                                   (BareSeasonalGroundC - FullSeasonalCoverC));
        }
        return std::min(permanent + seasonalDepth, double(MaximumDepth));
    }

    std::optional<PackRun> at(int x, int z, const ColumnGenerationInput& ground,
                              int day = 0) const {
        if (ground.generatedWet) return std::nullopt;
        if (!ground.groundShape.code() && ground.slope >= 1)
            return std::nullopt;
        return run(ground, depth(x, z, day));
    }

    // Deep snow fills the space above a ramp up to the ramp's own cell top.
    // It never builds a tower above the adjacent ground. A shallow stock
    // leaves only a toe wedge.
    static std::optional<PackRun> run(const ColumnGenerationInput& ground,
                                      double stock) {
        if (stock <= 0) return std::nullopt;
        const int base = ground.surface_y;
        if (ground.groundShape.code()) {
            if (stock >= .05)
                return PackRun{int32_t(base * UnitsPerCubicMetre),
                               int32_t((base * UnitsPerCubicMetre) +
                                       std::llround(std::min(stock, 1.) *
                                                    UnitsPerCubicMetre))};
            const Volume wedge =
                std::llround(std::min(stock, .5) * .1 * UnitsPerCubicMetre);
            if (!wedge) return std::nullopt;
            return PackRun{int32_t(base * UnitsPerCubicMetre),
                           int32_t(base * UnitsPerCubicMetre + wedge)};
        }
        const int groundBase = base + 1;
        const Volume volume =
            std::llround(std::clamp(stock * retention(ground), 0.,
                                    double(WORLD_HEIGHT_Y - groundBase)) *
                         UnitsPerCubicMetre);
        if (!volume) return std::nullopt;
        return PackRun{int32_t(groundBase * UnitsPerCubicMetre),
                       int32_t(groundBase * UnitsPerCubicMetre + volume)};
    }

   private:
    static double smooth(double value) {
        const double t = std::clamp(value, 0., 1.);
        return t * t * (3. - 2. * t);
    }

    const World& world;
    static constexpr float Missing = 1000.f;
    mutable std::vector<float> annual;
    mutable std::vector<float> seasonal;
    mutable int date = -2;
};

}  // namespace snow::polar
