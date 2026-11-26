#include <cmath>

#include "test.hpp"
#include "world/weather_history.hpp"
#include "world/weather_presentation.hpp"
#include "world/world.hpp"
#include "world/world_climate.hpp"
#include "world/world_weather.hpp"
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

bool near(float actual, float expected, float tolerance = 1e-4f) {
    return std::abs(actual - expected) <= tolerance;
}

bool sameSample(const world_weather::Sample& a,
                const world_weather::Sample& b) {
    return near(a.cloudCover, b.cloudCover) &&
           near(a.precipitationRate, b.precipitationRate) &&
           near(a.visualDensity, b.visualDensity) &&
           near(a.temperatureOffsetC, b.temperatureOffsetC) &&
           near(a.temperatureC, b.temperatureC) && a.condition == b.condition;
}

}  // namespace

TEST(weather_field_reproducible_and_seeded) {
    auto world = makeWorld(64, 32, 8.f);
    world.genSettings.seed = 2024;
    const auto first = world_weather::at(world, 1234.5, 2345.5, 12.25, 80.0);
    const auto second = world_weather::at(world, 1234.5, 2345.5, 12.25, 80.0);
    CHECK(sameSample(first, second));
    const auto climate = world_climate::at(world, 1234.5, 2345.5, 12.25, 80.0);
    EQUAL(near(first.baselineC, climate.temperatureC), true,
          "the weather sample carries the climate baseline");
    EQUAL(near(first.temperatureC, first.baselineC + first.temperatureOffsetC),
          true, "the offset is applied to the baseline");

    world.genSettings.seed = 2025;
    const auto other = world_weather::at(world, 1234.5, 2345.5, 12.25, 80.0);
    const float difference =
        std::abs(first.cloudCover - other.cloudCover) +
        std::abs(first.temperatureOffsetC - other.temperatureOffsetC) +
        std::abs(first.precipitationRate - other.precipitationRate);
    CHECK(difference > 1e-4f);
}

TEST(weather_x_seam_is_continuous) {
    constexpr int width = 16;
    constexpr int height = 5;
    auto world = makeWorld(width, height, 6.f);
    world.genSettings.seed = 77;
    const double size = WorldWrap::RegionSize;
    const double midZ = 2.5 * size;
    const double wrap = double(width) * size;

    const auto west = world_weather::at(world, -0.001, midZ, 10.3, 70.0);
    const auto east = world_weather::at(world, wrap - 0.001, midZ, 10.3, 70.0);
    CHECK(std::abs(west.cloudCover - east.cloudCover) < 1e-4f);
    CHECK(std::abs(west.precipitationRate - east.precipitationRate) < 1e-4f);
    CHECK(std::abs(west.temperatureOffsetC - east.temperatureOffsetC) < 1e-4f);
    CHECK(std::abs(west.temperatureC - east.temperatureC) < 1e-4f);
    EQUAL(int(west.condition), int(east.condition),
          "the X seam keeps the condition");

    const auto north =
        world_weather::at(world, .5 * size, size - .001, 3.0, 70.0);
    const auto south =
        world_weather::at(world, .5 * size, size + .001, 3.0, 70.0);
    CHECK(std::abs(north.cloudCover - south.cloudCover) < .02f);
    CHECK(std::abs(north.temperatureOffsetC - south.temperatureOffsetC) < .02f);
}

TEST(weather_time_is_continuous) {
    auto world = makeWorld(8, 5, 4.f);
    world.genSettings.seed = 42;
    const double x = 1.5 * WorldWrap::RegionSize;
    const double z = 2.5 * WorldWrap::RegionSize;
    const auto before = world_weather::at(world, x, z, 12.999999, 70.0);
    const auto after = world_weather::at(world, x, z, 13.000001, 70.0);
    CHECK(std::abs(before.temperatureC - after.temperatureC) < .01f);
    CHECK(std::abs(before.cloudCover - after.cloudCover) < .01f);
    CHECK(std::abs(before.precipitationRate - after.precipitationRate) < .01f);

    const auto yearBefore = world_weather::at(world, x, z, 359.999999, 70.0);
    const auto yearAfter = world_weather::at(world, x, z, 360.000001, 70.0);
    CHECK(std::abs(yearBefore.temperatureC - yearAfter.temperatureC) < .01f);
    CHECK(std::abs(yearBefore.cloudCover - yearAfter.cloudCover) < .01f);
    EQUAL(int(yearBefore.condition), int(yearAfter.condition),
          "the year boundary keeps the condition");
}

TEST(weather_condition_thresholds) {
    world_weather::Sample sample;
    sample.baselineC = 10.f;

    sample.cloudCover = .10f;
    sample.precipitationRate = 0;
    auto clear = world_weather::derive(sample);
    EQUAL(int(clear.condition), int(world_weather::Condition::Clear),
          "thin cloud without precipitation is clear");
    EQUAL(clear.visualDensity, 0.f, "clear weather has no particles");

    sample.cloudCover = .80f;
    sample.precipitationRate = .05f;
    auto cloudy = world_weather::derive(sample);
    EQUAL(int(cloudy.condition), int(world_weather::Condition::Cloudy),
          "thick dry cloud is cloudy");
    EQUAL(cloudy.precipitationRate, 0.f, "sub-threshold precipitation is dry");
    EQUAL(cloudy.visualDensity, 0.f, "cloudy weather has no particles");

    sample.precipitationRate = 5.f;
    sample.temperatureOffsetC = 2.f;
    auto rain = world_weather::derive(sample);
    EQUAL(int(rain.condition), int(world_weather::Condition::Rain),
          "warm precipitation is rain");
    CHECK(rain.visualDensity > 0.f);
    CHECK(rain.snowFraction < .5f);

    sample.temperatureOffsetC = -3.f;
    sample.baselineC = 0.f;
    auto snow = world_weather::derive(sample);
    EQUAL(int(snow.condition), int(world_weather::Condition::Snow),
          "cold precipitation is snow");
    EQUAL(snow.visualDensity > rain.visualDensity, true,
          "snow is more visible per millimetre");
    CHECK(snow.snowFraction > .5f);

    EQUAL(world_weather::snowFractionAt(world_weather::FreezingPointC +
                                        world_weather::FreezingBandC * .5f),
          0.f, "the warm edge of the band is all rain");
    EQUAL(world_weather::snowFractionAt(world_weather::FreezingPointC -
                                        world_weather::FreezingBandC * .5f),
          1.f, "the cold edge of the band is all snow");
    EQUAL(
        near(world_weather::snowFractionAt(world_weather::FreezingPointC), .5f),
        true, "freezing is halfway through the band");
}

TEST(weather_temperature_and_rate_bounds) {
    auto world = makeWorld(32, 16, 0.f);
    world.genSettings.seed = 9001;
    for (int day = 0; day < 12; ++day)
        for (int z = 0; z < 16 * 128; z += 1471)
            for (int x = 0; x < 32 * 128; x += 3911) {
                const auto sample =
                    world_weather::at(world, double(x), double(z),
                                      double(day) + .37, double(50 + x % 60));
                CHECK(sample.cloudCover >= 0.f && sample.cloudCover <= 1.f);
                CHECK(sample.precipitationRate >= 0.f &&
                      sample.precipitationRate <=
                          world_weather::MaxPrecipitationRate);
                CHECK(sample.visualDensity >= 0.f &&
                      sample.visualDensity <= 1.f);
                CHECK(std::abs(sample.temperatureOffsetC) <=
                      world_weather::MaxTemperatureOffsetC);
                CHECK(near(sample.temperatureC,
                           sample.baselineC + sample.temperatureOffsetC));
                CHECK(sample.snowFraction >= 0.f && sample.snowFraction <= 1.f);
                CHECK(sample.windSpeed >= world_weather::MinWindSpeed &&
                      sample.windSpeed <= world_weather::MaxWindSpeed);
                CHECK(std::abs(std::hypot(sample.windX, sample.windZ) -
                               sample.windSpeed) < 1e-3f);
                if (sample.precipitationRate == 0)
                    CHECK(sample.visualDensity == 0.f);
                else
                    CHECK(sample.condition ==
                          (sample.snowFraction >= .5f
                               ? world_weather::Condition::Snow
                               : world_weather::Condition::Rain));
            }
}

TEST(weather_storms_cover_many_regions) {
    auto world = makeWorld(96, 96);
    world.genSettings.seed = 2024;
    int wet = 0, clear = 0, longest = 0;
    // Follow several west-to-east tracks at the same time. A storm core
    // should remain wet for more than a kilometre on at least one track.
    for (int z = 300; z < 11800; z += 900) {
        int run = 0;
        for (int x = 0; x < world.wrap().tiles; x += 128) {
            const auto sample = world_weather::at(world, x, z, 1. / 3.);
            if (sample.precipitationRate > 2.f) {
                CHECK(sample.cloudCover > .5f);
                ++wet;
                longest = std::max(longest, ++run);
            } else {
                run = 0;
                clear += sample.condition == world_weather::Condition::Clear;
            }
        }
    }
    CHECK(longest > 10);
    CHECK(wet > 200);
    CHECK(clear > 50);
}

TEST(weather_opposite_seasons) {
    constexpr int height = 301;
    auto world = makeWorld(4, height, 0.f);
    world.genSettings.seed = 31415;
    const double size = WorldWrap::RegionSize;
    const double x = 2.5 * size;
    const double north = .5 * size;
    const double south = (height - .5) * size;

    const auto northWinter = world_weather::at(world, x, north, 0, 64.0);
    const auto northSummer = world_weather::at(world, x, north, 180, 64.0);
    const auto southWinter = world_weather::at(world, x, south, 0, 64.0);
    const auto southSummer = world_weather::at(world, x, south, 180, 64.0);
    CHECK(northSummer.temperatureC > northWinter.temperatureC + 15.f);
    CHECK(southWinter.temperatureC > southSummer.temperatureC + 15.f);
    CHECK(northWinter.baselineC < -15.f);
    CHECK(southWinter.baselineC > 15.f);
}

TEST(weather_history_dated_cells_wrap_and_clamp) {
    World world = makeWorld(4, 3);
    world.genSettings.seed = 2024;
    weather_history::Cache history;
    constexpr int bin = weather_history::IntervalsPerDay + 2;
    const auto west = history.at(world, -1, -5, bin);
    const auto east = history.at(world, 511, 0, bin);
    EQUAL(west.cellX, east.cellX, "wrapped weather cell key");
    EQUAL(west.cellZ, east.cellZ, "weather summary stays inside Z edge");
    EQUAL(west.interval, int64_t(bin), "summary date");
    CHECK(sameSample(east.weather,
                     world_weather::at(world, 504, 8, (bin + .5) / 48, 0)));
    CHECK(sameSample(west.weather, east.weather));
    const auto legacy =
        history.at(world, 511, 0, bin, world_weather::LegacyModelVersion);
    EQUAL(legacy.model, world_weather::LegacyModelVersion, "legacy cache key");
    CHECK(sameSample(legacy.weather,
                     world_weather::at(world, 504, 8, (bin + .5) / 48, 0,
                                       world_weather::LegacyModelVersion)));
    CHECK(
        !sameSample(east.weather, history.at(world, -1, -5, bin + 48).weather));
}

TEST(weather_presentation_fixture_wrap_and_handoff) {
    using namespace weather_presentation;
    auto world = makeWorld(96, 96, 10.f, 440);
    world.genSettings.seed = 2024;
    constexpr Fixture fixture{0, 48};
    constexpr double date = 1. / 3;
    Field field;
    field.begin(world, 36, 6242, date, 512);
    const auto clear = field.at(world, 0, 48, fixture)->weather;
    const auto rain = field.at(world, -1, 48, fixture)->weather;
    const auto snow = field.at(world, 0, 49, fixture)->weather;
    const auto cloudy = field.at(world, 95, 49, fixture)->weather;
    CHECK(clear.condition == world_weather::Condition::Clear);
    CHECK(cloudy.condition == world_weather::Condition::Cloudy);
    CHECK(clear.visualDensity == 0 && cloudy.visualDensity == 0);
    CHECK(rain.condition == world_weather::Condition::Rain);
    CHECK(rain.precipitationRate == 13.f && rain.visualDensity > .8f);
    CHECK(snow.condition == world_weather::Condition::Snow);
    CHECK(rain.snowFraction == 0 && snow.snowFraction == 1);
    CHECK(!field.at(world, 0, -1, fixture));
    CHECK(!field.at(world, 0, 96, fixture));
    CHECK(field.rebuilds() == 4);

    // Same paused date, crossing to the other image of the rain region.
    field.begin(world, world.wrap().tiles - 8, 6242, date, 1024);
    CHECK(sameSample(rain, field.at(world, 95, 48, fixture)->weather));
    CHECK(field.rebuilds() == 0);
    CHECK(regionWeight(-.001, 6242, -1, 48) < .00001f);
    CHECK(regionWeight(-8, 6242, -1, 48) == 1.f);
    CHECK(nearWeight(0) == 1.f && nearWeight(18) == 1.f);
    CHECK(near(nearWeight(22), .5f));
    CHECK(nearWeight(26) == 0.f && nearWeight(27) == 0.f);

    field.begin(world, 36, 6242, date, 512);
    const auto window = field.window(world);
    CHECK(window.x0 < -1 && window.x1 > 0);
    CHECK(window.z0 < 48 && window.z1 > 49);
    CHECK((window.x1 - window.x0) * (window.z1 - window.z0) <=
          int(Field::Capacity));
    field.begin(world, 36, 0, date, 1024);
    CHECK(field.window(world).z0 == 0);
    field.begin(world, 36, world.height * RegionSize - 1, date, 1024);
    CHECK(field.window(world).z1 == world.height);
}

TEST(weather_presentation_dated_reload_and_lod_cache) {
    using namespace weather_presentation;
    auto world = makeWorld(96, 96, 10.f, 440);
    world.genSettings.seed = 2024;
    constexpr int x = 48, z = 48, bin = 28;
    const double date = (bin + .5) / weather_history::IntervalsPerDay;
    const double eye = x * RegionSize + .5;
    Field field;
    field.begin(world, eye, eye, date, 512);
    auto actual = field.at(world, x, z)->weather;
    auto expected =
        weather_history::sampleCell(world, 48 * 8 + 4, 48 * 8 + 4, bin);
    expected.baselineC += world_climate::elevationOffsetC(440, 0);
    CHECK(sameSample(actual, world_weather::derive(expected)));
    CHECK(field.rebuilds() == 1);

    field.begin(world, eye, eye, date, 1024);
    CHECK(sameSample(actual, field.at(world, x, z)->weather));
    CHECK(field.rebuilds() == 0);

    const double jumped = date + 30;
    field.begin(world, eye, eye, jumped, 768);
    auto later = field.at(world, x, z)->weather;
    CHECK(field.rebuilds() == 1);
    CHECK(!sameSample(actual, later));
    Field reloaded;
    reloaded.begin(world, eye, eye, jumped, 768);
    CHECK(sameSample(later, reloaded.at(world, x, z)->weather));
    CHECK(reloaded.rebuilds() == 1);
}
