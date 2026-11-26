#pragma once

#include <array>
#include <deque>
#include <map>
#include <optional>
#include <tuple>

#include "world_weather.hpp"

namespace weather_history {

inline constexpr uint32_t SimulationVersion = 1;
inline constexpr int IntervalsPerDay = 48;
inline constexpr int CellSize = 16;
inline constexpr int MaxBatchIntervals = 7 * IntervalsPerDay;

inline int64_t intervalAt(double days) {
    // Dates formed from an interval index must round back to the same index.
    return int64_t(std::floor(days * IntervalsPerDay + 1e-8));
}

inline double intervalStart(int64_t interval) {
    return double(interval) / IntervalsPerDay;
}

inline void applyOverride(world_weather::Sample& sample, int force,
                          bool physical) {
    if (!force) return;
    const bool snow = force == 4;
    sample.cloudCover = std::array{.0f, .08f, .78f, .86f, .88f}[force];
    sample.precipitationRate = snow ? 4.f : force == 3 ? 6.f : 0.f;
    sample.visualDensity = snow ? .80f : force == 3 ? .70f : 0.f;
    if (force >= 3) sample.snowFraction = snow ? 1.f : 0.f;
    sample.condition = world_weather::Condition(force - 1);
    if (physical) {
        sample.temperatureC = snow ? -5.f : 5.f;
        sample.temperatureOffsetC = sample.temperatureC - sample.baselineC;
    }
}

struct IntervalSummary {
    int64_t interval;
    int cellX, cellZ;
    world_weather::Sample weather;

    static constexpr int WeatherVersion = world_weather::ModelVersion;
    static constexpr uint32_t SimulationVersion =
        weather_history::SimulationVersion;
};

// Dated weather cells can be queried without streaming voxel chunks. Cache
// whole days with lazy sampling so a normal update only pays for its interval.
class Cache {
   public:
    IntervalSummary at(const World& world, int x, int z, int64_t interval) {
        interval = std::max<int64_t>(0, interval);
        const int cx = int(world.wrap().wrapTile(x)) / CellSize;
        const int maxZ = std::max(0, world.height * WorldWrap::RegionSize - 1);
        const int cz =
            (world.height > 0 ? std::clamp(z, 0, maxZ) : z) / CellSize;
        const auto day = interval / IntervalsPerDay;
        const Key key{cx, cz, day};
        auto found = days.find(key);
        if (found == days.end()) {
            if (days.size() == Capacity) {
                days.erase(order.front());
                order.pop_front();
            }
            found = days.try_emplace(key).first;
            order.push_back(key);
        }
        auto& sample = found->second[size_t(interval % IntervalsPerDay)];
        if (!sample)
            sample = world_weather::at(
                world, (cx + .5) * CellSize, (cz + .5) * CellSize,
                (double(interval) + .5) / IntervalsPerDay, 0);
        return {interval, cx, cz, *sample};
    }

   private:
    using Key = std::tuple<int, int, int64_t>;
    static constexpr size_t Capacity = 64;
    std::map<Key,
             std::array<std::optional<world_weather::Sample>, IntervalsPerDay>>
        days;
    std::deque<Key> order;
};

}  // namespace weather_history
