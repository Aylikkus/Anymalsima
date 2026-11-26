#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>

#include "snow_settlement.hpp"
#include "world_weather.hpp"

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

// Apply this interval at one resident footprint. Temperature is evaluated at
// the support height, including when a partial roof shelters only some rays.
template <typename Temperature, typename Wet>
void applyFootprint(snow::SnowSettlement& snow,
                    const snow::SnowTerrain& terrain, int x, int z,
                    double hours, float rate, Temperature temperatureAt,
                    Wet wetAt) {
    if (hours <= 0) return;
    const auto& columns = snow.columns();
    auto first = columns.lower_bound({x, z, terrain.minY()});
    const auto last = columns.upper_bound({x, z, terrain.maxY()});
    if (rate <= 0 && first == last) return;

    constexpr double QuarterArea = .25;
    const auto hits = footprint(terrain, x, z);
    for (; first != last;) {
        const auto& [key, column] = *first++;
        const double height = column.baseY + column.baseLocal;
        const auto exposed = std::ranges::count_if(hits, [&](const auto& hit) {
            return hit && hit->height && hit->cellY <= column.baseY;
        });
        snow.melt(key, std::llround(meltVolume(temperatureAt(height), hours) *
                                    double(exposed) * QuarterArea));
    }

    if (rate <= 0) return;
    std::map<int, double> volume;
    for (const auto& hit : hits) {
        if (!hit || !hit->height || wetAt(*hit->height)) continue;
        world_weather::Sample local;
        local.precipitationRate = rate;
        local.snowFraction =
            world_weather::snowFractionAt(temperatureAt(*hit->height));
        if (local.snowFraction > 0)
            volume[hit->cellY] += snowVolume(local, hours, QuarterArea);
    }
    for (const auto& [y, amount] : volume)
        snow.snowfall(x, z, y + 1, std::llround(amount));
}

}  // namespace precipitation
