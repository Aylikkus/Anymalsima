#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <map>
#include <optional>

#include "snow_settlement.hpp"
#include "weather_history.hpp"

namespace precipitation {

struct Surface {
    // Empty height means the ray passed through the bottom of the world.
    std::optional<double> height;
    int cellY = 0;
};

// An unknown column yields nullopt. Use the upper geometry bound to skip
// overhead air, then test the actual solid intervals along gravity.
inline std::optional<Surface> firstSurface(const snow::SnowTerrain& terrain,
                                           double x, double z) {
    const double x0 = std::floor(x), z0 = std::floor(z);
    const int tx = terrain.wrapX(int(x0)), tz = int(z0);
    if (!terrain.inBounds(tx, tz)) return std::nullopt;

    const Vec2 point{float(x - x0), float(z - z0)};
    for (int y = terrain.top(tx, tz); y >= terrain.minY(); --y) {
        const auto cell = terrain.cell({tx, y, tz});
        if (!cell) return std::nullopt;
        if (cell->empty()) continue;
        const auto gaps = cell->freeIntervals(point);
        if (gaps.size() == 1 && gaps.front().low == 0 && gaps.front().high == 1)
            continue;
        const double top =
            !gaps.empty() && gaps.back().high == 1 ? gaps.back().low : 1;
        return Surface{double(y) + top, y};
    }
    return Surface{};
}

// Four equal-area rays for accumulation and melt. The renderer uses the
// exact position of each drop instead.
inline auto footprint(const snow::SnowTerrain& terrain, int x, int z) {
    return std::array{
        firstSurface(terrain, x + .25, z + .25),
        firstSurface(terrain, x + .75, z + .25),
        firstSurface(terrain, x + .25, z + .75),
        firstSurface(terrain, x + .75, z + .75),
    };
}

// 1 mm on 1 m² is 1000 cm³ of water. Melt uses a degree-hour factor of
// 0.5 mm SWE per positive °C per game hour.
inline constexpr double Cm3PerMillimetre = 1000;
inline constexpr double MeltMmPerDegreeHour = .5;

inline double snowVolume(const world_weather::Sample& sample, double hours,
                         double area) {
    return sample.precipitationRate * sample.snowFraction * hours * area *
           Cm3PerMillimetre * snow::SnowPerWater;
}

inline double meltVolume(float temperatureC, double hours) {
    return std::max(0.f, temperatureC) * hours * MeltMmPerDegreeHour *
           Cm3PerMillimetre * snow::SnowPerWater;
}

// Replay one bounded batch against one geometry snapshot. Intercepted snow
// spends each interval at its receiving support for thermodynamics. Commit
// surviving snow through gravity once per support. Isolated supports give the
// same depth as interval-by-interval gravity, and the water ledger accounts
// for snow that melted before we needed to project it.
template <typename SampleAt, typename Wet>
double reconcile(snow::SnowSettlement& snow, const snow::SnowTerrain& terrain,
                 int x, int z, double target, bool lapse, SampleAt sampleAt,
                 Wet wetAt) {
    using namespace weather_history;
    x = terrain.wrapX(x);
    const double start = snow.weatherDay(x, z);
    if (target <= start) return start;
    const double limit =
        std::min(target, intervalStart(intervalAt(start) + MaxBatchIntervals));
    const auto hits = footprint(terrain, x, z);
    if (std::ranges::any_of(hits, [](const auto& hit) { return !hit; }))
        return start;

    struct Receiver {
        std::array<double, 4> snowfallHeights{};
        size_t rays = 0;
        int fromY = 0;
        double height = 0;
        double exposedArea = 0;
        snow::Volume original = 0;
        snow::Volume remaining = 0;
        snow::Volume fresh = 0;
        snow::Volume freshMelt = 0;
    };
    constexpr double QuarterArea = .25;
    std::map<int, Receiver> receivers;
    auto first = snow.columns().lower_bound({x, z, terrain.minY()});
    const auto last = snow.columns().upper_bound({x, z, terrain.maxY()});
    for (; first != last; ++first) {
        const auto& column = first->second;
        auto& receiver = receivers[column.baseY];
        receiver.height = double(column.baseY) + column.baseLocal;
        receiver.original = receiver.remaining = column.volume;
    }
    for (const auto& hit : hits) {
        if (!hit->height) continue;
        const auto wet = wetAt(*hit->height);
        if (!wet) return start;
        if (*wet) continue;
        const int fromY = hit->cellY + 1;
        const auto support = snow.weatherSupport(x, z, fromY);
        // A full solid at the world ceiling routes directly to the sink.
        if (!support && fromY != terrain.maxY()) return start;
        const int baseY = support ? support->baseY : terrain.maxY();
        auto& receiver = receivers[baseY];
        receiver.height =
            support ? double(baseY) + support->baseLocal : double(baseY);
        receiver.fromY = fromY;
        receiver.snowfallHeights[receiver.rays++] = *hit->height;
    }
    for (auto& [baseY, receiver] : receivers) {
        if (baseY == terrain.maxY()) continue;
        receiver.exposedArea =
            QuarterArea *
            double(std::ranges::count_if(hits, [&](const auto& hit) {
                return hit->height && hit->cellY <= baseY;
            }));
    }
    if (receivers.empty()) {
        snow.markWeatherApplied(x, z, limit);
        return limit;
    }

    const auto& forces = snow.weatherForces();
    auto change = forces.upper_bound(start);
    int force = change == forces.begin() ? 0 : std::prev(change)->second;
    double day = start;
    for (int segment = 0; day < limit && segment < 2 * MaxBatchIntervals;
         ++segment) {
        const auto interval = intervalAt(day);
        double next = std::min(limit, intervalStart(interval + 1));
        if (change != forces.end()) next = std::min(next, change->first);
        const double hours = (next - day) * 24;
        auto sample = sampleAt(interval);
        applyOverride(sample, force, true);
        const auto temperatureAt = [&](double height) {
            return sample.temperatureC -
                   (!force && lapse
                        ? float(height) * world_climate::ElevationLapseCPerMetre
                        : 0);
        };
        for (auto& [baseY, receiver] : receivers) {
            (void)baseY;
            const auto melt =
                std::llround(meltVolume(temperatureAt(receiver.height), hours) *
                             receiver.exposedArea);
            const auto existingMelt =
                std::min<snow::Volume>(melt, receiver.remaining);
            receiver.remaining -= existingMelt;
            const auto freshMelt =
                std::min<snow::Volume>(melt - existingMelt, receiver.fresh);
            receiver.fresh -= freshMelt;
            receiver.freshMelt += freshMelt;

            if (sample.precipitationRate > 0) {
                double added = 0;
                auto local = sample;
                for (size_t ray = 0; ray < receiver.rays; ++ray) {
                    local.snowFraction = world_weather::snowFractionAt(
                        temperatureAt(receiver.snowfallHeights[ray]));
                    added += snowVolume(local, hours, QuarterArea);
                }
                receiver.fresh += std::llround(added);
            }
        }
        day = next;
        if (change != forces.end() && day == change->first)
            force = (change++)->second;
    }
    for (const auto& [baseY, receiver] : receivers) {
        const snow::SnowKey key{x, z, baseY};
        snow.melt(key, receiver.original - receiver.remaining);
        snow.recordWeatherMelt(key, receiver.freshMelt);
    }
    for (const auto& [baseY, receiver] : receivers) {
        (void)baseY;
        if (receiver.fresh > 0)
            snow.snowfall(x, z, receiver.fromY, receiver.fresh);
    }
    snow.markWeatherApplied(x, z, day);
    return day;
}

}  // namespace precipitation
