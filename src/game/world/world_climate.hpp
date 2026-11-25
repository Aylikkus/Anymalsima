#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "region.hpp"
#include "world.hpp"
#include "world_calendar.hpp"
#include "world_wrap.hpp"

// Regional climate for weather, snow and ice (docs/handoff/weather.md, step
// 08). The generated Region::temperature_c is treated as the annual mean at
// the region's own surface height; it already includes the generator's
// altitude cooling. This module adds a continuous annual curve and a daily
// range, then corrects for local terrain height relative to the region.
//
// Convention: region row 0 is the northern pole and the last row the southern
// pole (matching WorldGenerator::generateTemperature). Month 1 is northern
// midwinter, so the northern curve is coldest at elapsed day 0 and the
// southern curve is warmest there. Seasonal amplitude vanishes smoothly at
// the equator so the two hemispheres meet without a step. All values are °C.
namespace world_climate {

inline constexpr float PoleSeasonalAmplitudeC = 18.0f;
// Above one, the annual amplitude's slope is zero at the equator, avoiding a
// kink where the two hemispheres meet.
inline constexpr float SeasonalLatitudeExponent = 1.5f;
inline constexpr float DailyRangeC = 8.0f;
// Warmest part of the day; the coolest is twelve hours earlier.
inline constexpr double DailyPeakHour = 15.0;
// Standard tropospheric lapse rate. Tiles are one metre.
inline constexpr float ElevationLapseCPerMetre = .0065f;
// Used by map-less fixtures that have no regional climate.
inline constexpr float FallbackMeanC = 15.0f;

struct Sample {
    float meanC = FallbackMeanC;  // generated regional annual mean
    float seasonalC = 0;          // annual curve
    float dailyC = 0;             // time-of-day curve
    float elevationC = 0;         // local height relative to the region
    float temperatureC = FallbackMeanC;
};

// -1 at the northern edge, 0 at the equator, +1 at the southern edge.
inline float signedLatitude(int regionY, int worldHeight) {
    if (worldHeight <= 1) return 0;
    const double t = 2.0 * double(regionY) / double(worldHeight - 1) - 1.0;
    return float(std::clamp(t, -1.0, 1.0));
}

inline float seasonalAmplitudeC(int regionY, int worldHeight) {
    const double latitude =
        std::abs(double(signedLatitude(regionY, worldHeight)));
    return float(double(PoleSeasonalAmplitudeC) *
                 std::pow(latitude, double(SeasonalLatitudeExponent)));
}

// Annual offset at a region row. worldDays keeps its fraction so month and
// year boundaries are continuous.
inline float seasonalOffsetC(int regionY, int worldHeight, double worldDays) {
    if (worldHeight <= 1) return 0;
    const double latitude = double(signedLatitude(regionY, worldHeight));
    const double wave = std::cos(2.0 * 3.14159265358979323846 *
                                 world_calendar::yearFraction(worldDays));
    const double amplitude =
        double(PoleSeasonalAmplitudeC) *
        std::pow(std::abs(latitude), double(SeasonalLatitudeExponent));
    // North (negative latitude) is coldest at day 0; south is warmest.
    const double sign = latitude < 0 ? -1.0 : (latitude > 0 ? 1.0 : 0.0);
    return float(sign * amplitude * wave);
}

inline float dailyOffsetC(double worldDays) {
    if (!std::isfinite(worldDays)) return 0;
    double timeOfDay = std::fmod(worldDays, 1.0);
    if (timeOfDay < 0) timeOfDay += 1.0;
    const double phase =
        2.0 * 3.14159265358979323846 * (timeOfDay - DailyPeakHour / 24.0);
    return float(.5 * DailyRangeC * std::cos(phase));
}

// Cooler above the region's generated surface, warmer below it.
inline float elevationOffsetC(double localY, double regionalY) {
    return float(-double(ElevationLapseCPerMetre) * (localY - regionalY));
}

inline Sample sampleRegion(const Region& region, int worldHeight,
                           double worldDays, double localY) {
    Sample sample;
    sample.meanC = region.temperature_c;
    sample.seasonalC =
        seasonalOffsetC(int(region.coord.y), worldHeight, worldDays);
    sample.dailyC = dailyOffsetC(worldDays);
    sample.elevationC = elevationOffsetC(localY, region.land_height_m);
    sample.temperatureC =
        sample.meanC + sample.seasonalC + sample.dailyC + sample.elevationC;
    return sample;
}

// Local climate at a world position, bilinear between the four nearest region
// centres exactly like ground_cover_climate::at: X wraps, Z clamps. The
// map-less fallback matches the mild default of that function.
inline Sample at(const World& world, double x, double z, double worldDays,
                 std::optional<double> localY = std::nullopt) {
    Sample sample;
    if (world.regions.empty() || world.width <= 0 || world.height <= 0) {
        sample.dailyC = dailyOffsetC(worldDays);
        sample.temperatureC = sample.meanC + sample.dailyC;
        return sample;
    }
    constexpr double size = WorldWrap::RegionSize;
    const double fx = x / size - .5, fz = z / size - .5;
    const auto gx = int64_t(std::floor(fx)), gz = int64_t(std::floor(fz));
    const double tx = fx - double(gx), tz = fz - double(gz);
    double mean = 0, seasonal = 0, regionalY = 0;
    for (int dz = 0; dz < 2; ++dz)
        for (int dx = 0; dx < 2; ++dx) {
            const auto rx = world.wrap().wrapRegion(gx + dx);
            const auto rz = std::clamp<int64_t>(gz + dz, 0, world.height - 1);
            const auto& region = world.regions[size_t(rz) * world.width +
                                               size_t(std::clamp<int64_t>(
                                                   rx, 0, world.width - 1))];
            const double w = (dx ? tx : 1 - tx) * (dz ? tz : 1 - tz);
            mean += w * region.temperature_c;
            seasonal += w * seasonalOffsetC(int(rz), world.height, worldDays);
            regionalY += w * region.land_height_m;
        }
    sample.meanC = float(mean);
    sample.seasonalC = float(seasonal);
    sample.dailyC = dailyOffsetC(worldDays);
    if (localY) sample.elevationC = elevationOffsetC(*localY, regionalY);
    sample.temperatureC =
        sample.meanC + sample.seasonalC + sample.dailyC + sample.elevationC;
    return sample;
}

}  // namespace world_climate
