#pragma once

#include <cmath>
#include <cstdint>

// Game calendar derived from WorldClock's fractional world days. Day 1 of
// month 1 of year 1 starts at elapsed day zero (docs/handoff/weather.md, step
// 08). A month has 30 days and a year 12 months (360 days). Dates are display
// and season helpers only; the authoritative time is still WorldClock.
namespace world_calendar {

inline constexpr int DaysPerMonth = 30;
inline constexpr int MonthsPerYear = 12;
inline constexpr int DaysPerYear = DaysPerMonth * MonthsPerYear;

struct Date {
    // 1-based fields for display.
    int day = 1;
    int month = 1;
    int64_t year = 1;
    // 0-based whole days since the world began and within the year.
    int64_t elapsedDay = 0;
    int dayOfYear = 0;
    // Fraction of the current day in [0, 1): 0 = midnight, .5 = noon.
    double timeOfDay = 0;
};

inline int64_t floorDay(double worldDays) {
    if (!std::isfinite(worldDays) || worldDays <= 0) return 0;
    return int64_t(std::floor(worldDays));
}

inline Date fromDays(double worldDays) {
    const int64_t elapsed = floorDay(worldDays);
    Date date;
    date.elapsedDay = elapsed;
    date.dayOfYear = int(elapsed % DaysPerYear);
    date.day = date.dayOfYear % DaysPerMonth + 1;
    date.month = date.dayOfYear / DaysPerMonth + 1;
    date.year = elapsed / DaysPerYear + 1;
    date.timeOfDay = worldDays - double(elapsed);
    return date;
}

// Continuous position within the annual cycle in [0, 1). Unlike fromDays this
// keeps the fractional day, so month and year boundaries have no step.
inline double yearFraction(double worldDays) {
    if (!std::isfinite(worldDays)) return 0;
    const double r = std::fmod(worldDays, double(DaysPerYear));
    return (r < 0 ? r + double(DaysPerYear) : r) / double(DaysPerYear);
}

}  // namespace world_calendar
