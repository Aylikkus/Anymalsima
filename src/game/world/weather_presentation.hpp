#pragma once

#include <array>
#include <optional>

#include "weather_history.hpp"

namespace weather_presentation {

inline constexpr int RegionSize = WorldWrap::RegionSize;
inline constexpr float NearFadeStart = 18.f;
inline constexpr float NearRadius = 26.f;
inline constexpr float RegionFeather = 8.f;
inline constexpr int MaxRangeRegions = 16;
inline constexpr float MinCloudBase = 540.f;
inline constexpr float MaxCloudBase = 1020.f;
inline constexpr float CloudThickness = 125.f;

inline int regionAt(double tile) { return int(std::floor(tile / RegionSize)); }

inline float nearWeight(float distance) {
    return 1.f - world_weather::detail::smooth((distance - NearFadeStart) /
                                               (NearRadius - NearFadeStart));
}

inline float regionWeight(double x, double z, int rx, int rz) {
    const double u = x - double(rx) * RegionSize;
    const double v = z - double(rz) * RegionSize;
    return world_weather::detail::smooth(
        float(std::min({u, v, RegionSize - u, RegionSize - v})) /
        RegionFeather);
}

// Clear at the anchor, rain to its west, snow to its south, and cloudy/dry
// between the wet neighbours. All other regions are clear during the fixture.
struct Fixture {
    int x, z;

    int force(WorldWrap wrap, int rx, int rz) const {
        const int west = int(wrap.wrapRegion(x - 1));
        if (rz == z) return rx == west ? 3 : 1;
        if (rz == z + 1 && rx == x) return 4;
        if (rz == z + 1 && rx == west) return 2;
        return 1;
    }

    void apply(world_weather::Sample& weather, WorldWrap wrap, int rx,
               int rz) const {
        const int mode = force(wrap, rx, rz);
        weather_history::applyOverride(weather, mode, false);
        if (mode == 3) {
            weather.precipitationRate = 13.f;
            weather.visualDensity = world_weather::visualDensityFor(
                weather.precipitationRate, weather.condition);
        }
    }
};

struct Summary {
    world_weather::Sample weather;
};

// One world and one pair of dated samples per region. Slots roll with the
// view window, retaining lookups across adjacent regions and the X seam.
class Field {
   public:
    struct Window {
        int x0, x1, z0, z1;  // unwrapped X, exclusive upper bounds
    };

    void begin(const World& world, double eyeX, double eyeZ, double days,
               float distance, int64_t firstCurrentWeatherInterval = 0) {
        rebuildCount = 0;
        const int canonical = int(world.wrap().wrapRegion(regionAt(eyeX)));
        centreX += int(world.wrap().deltaRegion(canonical, centreX));
        centreZ = regionAt(eyeZ);
        radius = std::clamp(int(std::ceil(distance / RegionSize)) + 1, 1,
                            MaxRangeRegions + 1);
        // Step 11 samples are dated at the midpoint of each half hour.
        // Interpolate between those dates so fronts do not jump at the bin.
        const double date =
            std::max(0., days * weather_history::IntervalsPerDay - .5);
        interval = int64_t(std::floor(date));
        fraction = float(date - double(interval));
        firstCurrentInterval = firstCurrentWeatherInterval;
    }

    Window window(const World& world) const {
        return {std::max(centreX - radius, centreX - world.width / 2),
                std::min(centreX + radius + 1, centreX + (world.width + 1) / 2),
                std::clamp(centreZ - radius, 0, world.height),
                std::clamp(centreZ + radius + 1, 0, world.height)};
    }

    int imageRegion(const World& world, int rx) const {
        return centreX + int(world.wrap().deltaRegion(rx, centreX));
    }

    std::optional<Summary> at(const World& world, int rx, int rz,
                              std::optional<Fixture> fixture = {},
                              int force = 0) {
        if (world.width <= 0 || rz < 0 || rz >= world.height ||
            world.regions.empty())
            return std::nullopt;
        rx = imageRegion(world, rx);
        const int canonical = int(world.wrap().wrapRegion(rx));
        auto& entry = cache[size_t(WorldWrap::mod(rz, Side) * Side +
                                   WorldWrap::mod(rx, Side))];
        const float ground = float(
            world.regions[size_t(rz) * world.width + canonical].land_height_m);
        const bool newDate =
            entry.x != canonical || entry.z != rz || entry.interval != interval;
        if (newDate) {
            constexpr int cells = RegionSize / weather_history::CellSize;
            const auto sample = [&](int64_t bin) {
                return weather_history::sampleCell(
                    world, canonical * cells + cells / 2,
                    rz * cells + cells / 2, bin,
                    bin < firstCurrentInterval
                        ? world_weather::LegacyModelVersion
                        : world_weather::ModelVersion);
            };
            const bool reuse = entry.x == canonical && entry.z == rz &&
                               entry.interval + 1 == interval;
            entry.first = reuse ? entry.next : sample(interval);
            entry.next = sample(interval + 1);
            entry.x = canonical;
            entry.z = rz;
            entry.interval = interval;
            ++rebuildCount;
        }
        if (newDate || fraction != entry.fraction) {
            const auto& a = entry.first;
            const auto& b = entry.next;
            world_weather::Sample weather;
            weather.cloudCover =
                std::lerp(a.cloudCover, b.cloudCover, fraction);
            weather.precipitationRate =
                std::lerp(a.precipitationRate, b.precipitationRate, fraction);
            weather.baselineC = std::lerp(a.baselineC, b.baselineC, fraction) +
                                world_climate::elevationOffsetC(ground, 0);
            weather.temperatureOffsetC =
                std::lerp(a.temperatureOffsetC, b.temperatureOffsetC, fraction);
            weather.windX = std::lerp(a.windX, b.windX, fraction);
            weather.windZ = std::lerp(a.windZ, b.windZ, fraction);
            weather.windSpeed = std::hypot(weather.windX, weather.windZ);
            entry.presentation = world_weather::derive(weather);
            entry.fraction = fraction;
        }
        auto weather = entry.presentation;
        if (fixture)
            fixture->apply(weather, world.wrap(), canonical, rz);
        else
            weather_history::applyOverride(weather, force, false);
        return Summary{weather};
    }

    unsigned rebuilds() const { return rebuildCount; }
    static constexpr int Side = 2 * (MaxRangeRegions + 1) + 1;
    static constexpr size_t Capacity = Side * Side;

   private:
    struct Entry {
        int x = -1, z = -1;
        int64_t interval = -2;
        float fraction = 0;
        world_weather::Sample first, next, presentation;
    };

    std::array<Entry, Capacity> cache{};
    int centreX = 0, centreZ = 0, radius = 1;
    int64_t interval = 0;
    int64_t firstCurrentInterval = 0;
    float fraction = 0;
    unsigned rebuildCount = 0;
};

}  // namespace weather_presentation
