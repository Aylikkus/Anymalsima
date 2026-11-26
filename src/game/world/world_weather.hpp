#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "region.hpp"
#include "world.hpp"
#include "world_climate.hpp"
#include "world_wrap.hpp"

// Regional weather for presentation and later exposure
// (docs/handoff/weather.md, step 09). The field is deterministic from the world
// seed, the model version, the world position and world time. It is a sum of
// long plane waves: spatially coherent over several regions, continuous in
// time, and periodic in X with the world's wrap period so the seam is
// invisible. Region-centre samples are then blended bilinearly with the same
// wrapping convention as world_climate and ground_cover_climate.
//
// Precipitation rate is a physical rain-equivalent rate in millimetres per
// hour. Visual particle density is a separate 0..1 presentation value; snow is
// more visible per millimetre than rain, and dry cloud never creates particles.
namespace world_weather {

// Bump when the field's shape changes so saved/fixture expectations can be
// identified. It participates in the seed hash and never changes saved data.
inline constexpr int ModelVersion = 1;
inline constexpr float FreezingPointC = 0.0f;
// Total width of the reproducible rain/snow transition around freezing.
inline constexpr float FreezingBandC = 1.0f;
inline constexpr float MaxPrecipitationRate = 16.0f;  // mm/h
inline constexpr float DryPrecipitationRate = 0.15f;  // mm/h
inline constexpr float ClearCloudCover = 0.35f;
inline constexpr float MaxTemperatureOffsetC = 8.0f;
// Guaranteed light drift through the seeded direction field.
inline constexpr float MaxWindSpeed = 14.0f;
inline constexpr float MinWindSpeed = 0.7f;

enum class Condition : uint8_t { Clear, Cloudy, Rain, Snow };

inline const char* conditionName(Condition condition) {
    switch (condition) {
        case Condition::Clear:
            return "Clear";
        case Condition::Cloudy:
            return "Cloudy";
        case Condition::Rain:
            return "Rain";
        case Condition::Snow:
            return "Snow";
    }
    return "Clear";
}

struct Sample {
    // Regional weather. The temperature offset is added to the climate
    // baseline to produce the local outdoor temperature.
    float cloudCover = 0;
    float precipitationRate = 0;  // physical rain equivalent, mm/h
    float visualDensity = 0;      // presentation only, 0..1
    float temperatureOffsetC = 0;
    // Climate baseline from world_climate::at and the resulting outdoor °C.
    float baselineC = world_climate::FallbackMeanC;
    float temperatureC = world_climate::FallbackMeanC;
    // Surface wind in m/s: components and magnitude. Presentation uses the
    // direction for cloud drift and both for near particles.
    float windX = 0;
    float windZ = 0;
    float windSpeed = 0;
    // 1 = snow, 0 = rain; halfway through the freezing band.
    float snowFraction = 0;
    Condition condition = Condition::Clear;
};

namespace detail {

inline uint64_t mix64(uint64_t value) {
    uint64_t z = value + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

inline double phaseOf(uint64_t bits) {
    // 53 significant bits keep the phase uniform without losing precision.
    return double(bits >> 11) / 9007199254740992.0;
}

// One unit-rank field in [0, 1]. Every component wraps in X over periodX.
inline float fieldRank(uint64_t seed, int field, double x, double z,
                       double worldDays, double periodX) {
    constexpr double tau = 6.28318530717958647692;
    constexpr float amplitude[] = {.55f, .30f, .15f};
    periodX = std::max(1.0, periodX);
    double sum = 0;
    for (int i = 0; i < 3; ++i) {
        uint64_t bits =
            mix64(seed ^ (uint64_t(field) << 56 | uint64_t(i) << 48) ^
                  uint64_t(ModelVersion) * 0x9e3779b97f4a7c15ull);
        // 20 to 119 cycles per world width: roughly 4 to 26 regions per wave
        // in a new 512-region world.
        const double kx = tau * double(20 + bits % 100) / periodX;
        bits >>= 8;
        const double kz = tau / (420.0 + double(bits % 2600));
        bits >>= 16;
        const double days = .55 + double(bits % 260) / 100.0;
        bits >>= 8;
        const double phase = tau * phaseOf(bits);
        sum += double(amplitude[i]) *
               std::sin(kx * x + kz * z + tau * worldDays / days + phase);
    }
    return float(.5 + .5 * sum);
}

inline float smooth(float value) {
    value = std::clamp(value, 0.f, 1.f);
    return value * value * (3.f - 2.f * value);
}

inline float cloudCoverFrom(float rank) { return smooth((rank - .18f) / .70f); }

inline float wetnessFrom(float rank) { return smooth((rank - .30f) / .60f); }

}  // namespace detail

inline float snowFractionAt(float temperatureC) {
    const float half = FreezingBandC * .5f;
    return detail::smooth((FreezingPointC + half - temperatureC) /
                          FreezingBandC);
}

// Fills derived fields from the raw weather values. Tests feed it directly to
// exercise the condition thresholds without depending on a particular sample.
inline Sample derive(Sample sample) {
    sample.cloudCover = std::clamp(sample.cloudCover, 0.f, 1.f);
    sample.precipitationRate =
        std::clamp(sample.precipitationRate, 0.f, MaxPrecipitationRate);
    sample.temperatureOffsetC =
        std::clamp(sample.temperatureOffsetC, -MaxTemperatureOffsetC,
                   MaxTemperatureOffsetC);
    sample.temperatureC = sample.baselineC + sample.temperatureOffsetC;
    sample.snowFraction = snowFractionAt(sample.temperatureC);
    if (sample.precipitationRate < DryPrecipitationRate) {
        sample.precipitationRate = 0;
        sample.visualDensity = 0;
        sample.condition = sample.cloudCover < ClearCloudCover
                               ? Condition::Clear
                               : Condition::Cloudy;
    } else {
        sample.condition =
            sample.snowFraction >= .5f ? Condition::Snow : Condition::Rain;
        // Snowflakes are visible at lower water-equivalent rates than rain.
        const float scale =
            sample.condition == Condition::Snow ? 9.f : MaxPrecipitationRate;
        sample.visualDensity = std::pow(
            std::clamp(sample.precipitationRate / scale, 0.f, 1.f), .65f);
    }
    return sample;
}

// Regional weather at a world tile. X wraps and Z clamps through the climate
// sampler. A world without a map still samples the seeded field directly and
// keeps the mild climate fallback.
inline Sample at(const World& world, double x, double z, double worldDays,
                 std::optional<double> localY = std::nullopt) {
    const auto climate = world_climate::at(world, x, z, worldDays, localY);
    Sample sample;
    sample.baselineC = climate.temperatureC;
    const double periodX = world.width > 0
                               ? double(world.width) * WorldWrap::RegionSize
                               : 512.0 * WorldWrap::RegionSize;
    float ranks[5] = {};
    if (world.regions.empty() || world.width <= 0 || world.height <= 0) {
        for (int field = 0; field < 5; ++field)
            ranks[field] = detail::fieldRank(world.genSettings.seed, field, x,
                                             z, worldDays, periodX);
    } else {
        constexpr double size = WorldWrap::RegionSize;
        const double fx = x / size - .5, fz = z / size - .5;
        const auto gx = int64_t(std::floor(fx)), gz = int64_t(std::floor(fz));
        const double tx = fx - double(gx), tz = fz - double(gz);
        for (int dz = 0; dz < 2; ++dz)
            for (int dx = 0; dx < 2; ++dx) {
                const auto rx = world.wrap().wrapRegion(gx + dx);
                const auto rz =
                    std::clamp<int64_t>(gz + dz, 0, world.height - 1);
                const double w = (dx ? tx : 1 - tx) * (dz ? tz : 1 - tz);
                const double cx = (double(rx) + .5) * size;
                const double cz = (double(rz) + .5) * size;
                for (int field = 0; field < 5; ++field)
                    ranks[field] += float(
                        w * detail::fieldRank(world.genSettings.seed, field, cx,
                                              cz, worldDays, periodX));
            }
    }
    sample.cloudCover = detail::cloudCoverFrom(ranks[0]);
    const float wetness = detail::wetnessFrom(ranks[1]);
    // Dry gaps in a broken cloud deck must not drizzle from a nearly clear
    // sky, so precipitation fades in with the cloud cover itself.
    const float rainGate = detail::smooth((ranks[0] - .32f) / .34f);
    sample.precipitationRate = MaxPrecipitationRate *
                               std::pow(sample.cloudCover, 1.7f) * wetness *
                               wetness * rainGate;
    sample.temperatureOffsetC = (2.f * ranks[2] - 1.f) * MaxTemperatureOffsetC;
    constexpr float tau = 6.28318530717958647692f;
    const float windAngle = tau * ranks[3];
    sample.windSpeed =
        MinWindSpeed + (MaxWindSpeed - MinWindSpeed) *
                           std::pow(std::clamp(ranks[4], 0.f, 1.f), 1.4f);
    sample.windX = std::cos(windAngle) * sample.windSpeed;
    sample.windZ = std::sin(windAngle) * sample.windSpeed;
    return derive(sample);
}

}  // namespace world_weather
