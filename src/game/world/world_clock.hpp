#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Game time (docs/handoff/flora.md, step 8): days since the world began,
// fractional. Saved in world.json (WorldMeta::worldDays). Plants evaluate
// their growth from it; the day/night cycle reads timeOfDay().
class WorldClock {
   public:
    // Real seconds per game day at speed 1.
    static constexpr double DayLengthSeconds = 4320;  // 72 minutes

    double days() const { return elapsed; }
    // Whole days since the world began (the growth day).
    int64_t day() const { return int64_t(std::floor(elapsed)); }
    // 0 = midnight, 0.5 = noon.
    double timeOfDay() const { return elapsed - std::floor(elapsed); }

    void set(double value) { elapsed = std::max(0.0, value); }
    // Debug speed control: game seconds per real second (1 = normal).
    double speed() const { return multiplier; }
    void setSpeed(double value) { multiplier = std::max(0.0, value); }
    void advance(double realSeconds) {
        elapsed += std::max(0.0, realSeconds) * multiplier / DayLengthSeconds;
    }

   private:
    double elapsed = 0;
    double multiplier = 1;
};
