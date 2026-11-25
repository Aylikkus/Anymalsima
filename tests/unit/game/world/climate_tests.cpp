#include <cmath>

#include "test.hpp"
#include "world/save/world_meta.hpp"
#include "world/world.hpp"
#include "world/world_calendar.hpp"
#include "world/world_climate.hpp"
#include "world/world_clock.hpp"
#include "world/world_wrap.hpp"

namespace {

World makeWorld(int width, int height, float meanC = 10.f, int surfaceY = 64) {
    World world;
    world.width = width;
    world.height = height;
    world.min_temp_c = -40;
    world.max_temp_c = 50;
    world.regions.resize(size_t(width) * size_t(height));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            auto& region = world.regions[size_t(y) * width + size_t(x)];
            region.coord = {unsigned(x), unsigned(y)};
            region.temperature_c = meanC;
            region.land_height_m = surfaceY;
        }
    return world;
}

bool near(float actual, float expected, float tolerance = 1e-3f) {
    return std::abs(actual - expected) <= tolerance;
}

}  // namespace

TEST(climate_calendar_dates) {
    const auto start = world_calendar::fromDays(0);
    EQUAL(start.day, 1, "elapsed day zero is day 1");
    EQUAL(start.month, 1, "elapsed day zero is month 1");
    EQUAL(start.year, 1, "elapsed day zero is year 1");
    EQUAL(start.dayOfYear, 0, "elapsed day zero starts the year");

    const auto endFirstMonth = world_calendar::fromDays(29);
    EQUAL(endFirstMonth.day, 30, "day 29 is the 30th");
    EQUAL(endFirstMonth.month, 1, "day 29 stays in month 1");
    EQUAL(endFirstMonth.year, 1, "day 29 stays in year 1");

    const auto secondMonth = world_calendar::fromDays(30);
    EQUAL(secondMonth.day, 1, "day 30 rolls the day");
    EQUAL(secondMonth.month, 2, "day 30 rolls the month");
    EQUAL(secondMonth.year, 1, "day 30 stays in year 1");

    const auto endFirstYear = world_calendar::fromDays(359);
    EQUAL(endFirstYear.day, 30, "day 359 is the 30th");
    EQUAL(endFirstYear.month, 12, "day 359 is in month 12");
    EQUAL(endFirstYear.year, 1, "day 359 stays in year 1");

    const auto secondYear = world_calendar::fromDays(360);
    EQUAL(secondYear.day, 1, "day 360 rolls the day");
    EQUAL(secondYear.month, 1, "day 360 rolls the month");
    EQUAL(secondYear.year, 2, "day 360 rolls the year");

    const auto fractional = world_calendar::fromDays(30.75);
    EQUAL(fractional.day, 1, "fractional day keeps the calendar day");
    EQUAL(fractional.month, 2, "fractional day keeps the calendar month");
    EQUAL(bool(near(float(fractional.timeOfDay), .75f)), true,
          "fractional day carries the time of day");
}

TEST(climate_year_curve_continuous) {
    constexpr int height = 101;
    const auto north = world_climate::signedLatitude(0, height);
    EQUAL(bool(near(north, -1.f)), true, "row zero is the northern edge");

    const float monthEdgeA = world_climate::seasonalOffsetC(0, height, 29.999);
    const float monthEdgeB = world_climate::seasonalOffsetC(0, height, 30.001);
    EQUAL(std::abs(monthEdgeA - monthEdgeB) < .001f, true,
          "month boundary has no seasonal step");

    const float yearEdgeA = world_climate::seasonalOffsetC(0, height, 359.999);
    const float yearEdgeB = world_climate::seasonalOffsetC(0, height, 360.001);
    EQUAL(std::abs(yearEdgeA - yearEdgeB) < .001f, true,
          "year boundary has no seasonal step");

    const float start = world_climate::seasonalOffsetC(0, height, 0);
    const float nextYear = world_climate::seasonalOffsetC(0, height, 360);
    EQUAL(std::abs(start - nextYear) < .001f, true,
          "the annual curve repeats at 360 days");
    EQUAL(start < -17.9f, true, "northern month 1 is midwinter");
}

TEST(climate_opposite_seasons_and_latitude) {
    constexpr int height = 301;
    auto world = makeWorld(1, height, 0.f);
    const auto& north = world.regions[0];
    const auto& equator = world.regions[size_t(height / 2)];
    const auto& south = world.regions[size_t(height - 1)];
    EQUAL(int(north.coord.y), 0, "north fixture row");
    EQUAL(int(equator.coord.y), height / 2, "equator fixture row");

    const auto northWinter =
        world_climate::sampleRegion(north, height, 0, north.land_height_m);
    const auto northSummer =
        world_climate::sampleRegion(north, height, 180, north.land_height_m);
    EQUAL(bool(near(northWinter.seasonalC, -18.f)), true,
          "northern midwinter is the coldest point");
    EQUAL(bool(near(northSummer.seasonalC, 18.f)), true,
          "northern midsummer is the warmest point");
    EQUAL(northSummer.temperatureC > northWinter.temperatureC, true,
          "north warms from month 1 to month 7");

    const auto southWinter =
        world_climate::sampleRegion(south, height, 0, south.land_height_m);
    const auto southSummer =
        world_climate::sampleRegion(south, height, 180, south.land_height_m);
    EQUAL(bool(near(southWinter.seasonalC, 18.f)), true,
          "southern month 1 is midsummer");
    EQUAL(bool(near(southSummer.seasonalC, -18.f)), true,
          "southern month 7 is midwinter");

    const auto equatorWinter =
        world_climate::sampleRegion(equator, height, 0, equator.land_height_m);
    const auto equatorSummer = world_climate::sampleRegion(
        equator, height, 180, equator.land_height_m);
    EQUAL(bool(near(equatorWinter.seasonalC, 0.f)), true,
          "the equator has no annual sign");
    EQUAL(bool(near(equatorSummer.seasonalC, 0.f)), true,
          "the equator has almost no annual swing");
    EQUAL(std::abs(equatorSummer.temperatureC - equatorWinter.temperatureC) <
              .01f,
          true, "equator seasons stay close");
}

TEST(climate_position_continuity) {
    constexpr int width = 8;
    constexpr int height = 5;
    auto world = makeWorld(width, height, 0.f);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            world.regions[size_t(y) * width + size_t(x)].temperature_c =
                float(x * 10);
    const double size = WorldWrap::RegionSize;
    const double midZ = 2.5 * size;

    const float west = world_climate::at(world, -0.001, midZ, 0).meanC;
    const float east =
        world_climate::at(world, width * size - 0.001, midZ, 0).meanC;
    EQUAL(std::abs(west - east) < 1e-4f, true,
          "the wrapped X seam samples the same blend");

    const float before = world_climate::at(world, -0.001, midZ, 0).temperatureC;
    const float after = world_climate::at(world, 0.001, midZ, 0).temperatureC;
    EQUAL(std::abs(before - after) < .05f, true, "the X seam is continuous");

    const double equatorZ = 2.5 * size;
    const float northSide =
        world_climate::at(world, .5 * size, equatorZ - .001, 0).temperatureC;
    const float southSide =
        world_climate::at(world, .5 * size, equatorZ + .001, 0).temperatureC;
    EQUAL(std::abs(northSide - southSide) < .01f, true,
          "the equator is continuous");

    const float pole =
        world_climate::at(world, .5 * size, .5 * size, 0).seasonalC;
    const float oppositePole =
        world_climate::at(world, .5 * size, (height - .5) * size, 0).seasonalC;
    EQUAL(pole < -17.f && oppositePole > 17.f, true,
          "the two poles have opposite seasons");
}

TEST(climate_daily_range_and_elevation) {
    constexpr int height = 1;
    constexpr int surfaceY = 100;
    auto world = makeWorld(1, height, 10.f, surfaceY);
    const auto& region = world.regions[0];

    const auto afternoon =
        world_climate::sampleRegion(region, height, 15.0 / 24.0, surfaceY);
    const auto beforeDawn =
        world_climate::sampleRegion(region, height, 3.0 / 24.0, surfaceY);
    EQUAL(bool(near(afternoon.dailyC, 4.f)), true,
          "15:00 is the daily warm peak");
    EQUAL(bool(near(beforeDawn.dailyC, -4.f)), true,
          "03:00 is the daily cool trough");
    EQUAL(bool(near(afternoon.seasonalC, 0.f)), true,
          "a one-row world has no annual curve");

    const auto high =
        world_climate::sampleRegion(region, height, 0, surfaceY + 100);
    const auto low =
        world_climate::sampleRegion(region, height, 0, surfaceY - 100);
    EQUAL(bool(near(high.elevationC, -.65f)), true,
          "100 m higher is 0.65 C cooler");
    EQUAL(bool(near(low.elevationC, .65f)), true,
          "100 m lower is 0.65 C warmer");
    EQUAL(bool(near(high.temperatureC, high.meanC + high.seasonalC +
                                           high.dailyC + high.elevationC)),
          true, "the sample combines every term");
}

TEST(climate_map_less_fallback) {
    World world;
    const auto sample = world_climate::at(world, 12.3, 45.6, .3, 50.0);
    EQUAL(std::isfinite(sample.temperatureC), true,
          "the fallback is a finite temperature");
    EQUAL(bool(near(sample.meanC, world_climate::FallbackMeanC)), true,
          "the fallback mean is mild");
    EQUAL(bool(near(sample.seasonalC, 0.f)), true,
          "the fallback has no seasons");
    EQUAL(bool(near(sample.elevationC, 0.f)), true,
          "the fallback has no regional height");
    EQUAL(bool(near(sample.dailyC, world_climate::dailyOffsetC(.3))), true,
          "the fallback keeps the daily curve");
}

TEST(climate_clock_pause_and_midnight_save) {
    WorldClock clock;
    clock.set(29.999);
    clock.setSpeed(0);
    clock.advance(1e6);
    EQUAL(std::abs(clock.days() - 29.999) < 1e-9, true,
          "paused time does not advance");
    EQUAL(world_calendar::fromDays(clock.days()).day, 30,
          "a paused clock keeps its calendar day");
    EQUAL(world_calendar::fromDays(clock.days()).month, 1,
          "a paused clock keeps its month");

    WorldMeta saved;
    saved.worldDays = clock.days();
    WorldMeta loaded;
    loaded.deserialize(saved.serialize());
    EQUAL(std::abs(loaded.worldDays - clock.days()) < 1e-12, true,
          "the fractional day survives save and reload");
    EQUAL(world_calendar::fromDays(loaded.worldDays).day, 30,
          "the reloaded date matches before midnight");

    // 7200 game seconds per real second crosses midnight in one second.
    clock.setSpeed(7200);
    clock.advance(1);
    const auto after = world_calendar::fromDays(clock.days());
    EQUAL(after.month == 2 && after.day == 2, true,
          "accelerated time rolls past midnight into month 2");

    saved.worldDays = clock.days();
    WorldMeta reloaded;
    reloaded.deserialize(saved.serialize());
    const auto reloadedDate = world_calendar::fromDays(reloaded.worldDays);
    EQUAL(reloadedDate.day, after.day, "the saved day matches");
    EQUAL(reloadedDate.month, after.month, "the saved month matches");
    EQUAL(reloadedDate.year, after.year, "the saved year matches");
}
