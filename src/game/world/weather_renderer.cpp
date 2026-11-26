#include "weather_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "precipitation.hpp"
#include "render/vertex.hpp"

namespace {

using namespace weather_presentation;
constexpr float BoxHeight = 16.f;
constexpr float BoxCentreY = 6.f;
constexpr int DistanceSlices = 10;
constexpr uint16_t DistanceStride = 16;

uint64_t mix64(uint64_t value) {
    uint64_t z = value + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

float unit(uint64_t value) { return float(value >> 40) * (1.f / 16777216.f); }

uint64_t particleHash(uint64_t seed, uint32_t index, int salt) {
    return mix64(seed + uint64_t(index) * 0x9e3779b97f4a7c15ull +
                 uint64_t(salt) * 0xbf58476d1ce4e5b9ull);
}

float wrapBox(double value, double radius) {
    const double span = radius * 2.0;
    double wrapped = std::fmod(value + radius, span);
    if (wrapped < 0) wrapped += span;
    return float(wrapped - radius);
}

}  // namespace

WeatherRenderer::WeatherRenderer(IRenderer& renderer) {
    static const PosVertex vertices[] = {
        {-1.f, -1.f, 0.f}, {1.f, -1.f, 0.f}, {1.f, 1.f, 0.f}, {-1.f, 1.f, 0.f}};
    static constexpr uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    quad.initStatic(&renderer, vertices, std::size(vertices), indices,
                    std::size(indices));
    right.initVec4("u_weather_right", {1, 0, 0, 0});
    up.initVec4("u_weather_up", {0, 1, 0, 0});
    sun.initVec4("u_sky_sun", {0, 1, 0, 1});
    skyWeather.initVec4("u_sky_weather", {});
    curtainFade.initVec4("u_curtain_fade", {NearFadeStart, NearRadius, 0, 0});
    curtainTime.initVec4("u_curtain_time", {});
    weatherTexture = bgfx::createTexture2D(
        Field::Side, Field::Side, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    gridTexture.initTexture("u_weather_map", 2, weatherTexture);
    gridProjection.initVec4("u_weather_grid", {});
    gridLimits.initVec4("u_weather_limits", {});
    gridAnchor.initVec4("u_weather_anchor", {});
}

WeatherRenderer::~WeatherRenderer() {
    if (bgfx::isValid(weatherTexture)) bgfx::destroy(weatherTexture);
}

void WeatherRenderer::render(RenderTarget& target, Camera& camera,
                             const World& world,
                             const world_weather::Sample& localWeather,
                             const Frame& frame,
                             const snow::SnowTerrain& terrain) {
    drawn = farDrawn = wetRegions = 0;
    const auto eye = camera.eye();
    updateWeatherMap(camera, world, frame);
    if (camera.underwater || !frame.drawPrecipitation) return;

    const float yaw = camera.yaw, pitch = camera.pitch;
    const float cosYaw = std::cos(yaw), sinYaw = std::sin(yaw);
    const float cosPitch = std::cos(pitch), sinPitch = std::sin(pitch);
    right.setVec4({cosYaw, 0, -sinYaw, 0});
    up.setVec4({-sinPitch * sinYaw, cosPitch, -sinPitch * cosYaw, 0});
    sun.setVec4(camera.sky.sun);
    skyWeather.setVec4(camera.sky.weather);
    renderDistant(target, camera, frame);

    const bool mapped = camera.weatherMap.grid[2] > 0;
    const Vec2 localDensity{
        localWeather.visualDensity * (1.f - localWeather.snowFraction),
        localWeather.visualDensity * localWeather.snowFraction};
    Vec2 maxDensity = mapped ? Vec2{} : localDensity;
    // The near box can interpolate between, at most, these nine texels.
    if (mapped)
        for (int rz = Field::Side / 2 - 1; rz <= Field::Side / 2 + 1; ++rz)
            for (int rx = Field::Side / 2 - 1; rx <= Field::Side / 2 + 1;
                 ++rx) {
                const auto& p = pixels[size_t(rz) * Field::Side + rx];
                maxDensity.x = std::max(maxDensity.x, p[1] / 255.f);
                maxDensity.y = std::max(maxDensity.y, p[2] / 255.f);
            }
    const uint32_t rainTarget =
        uint32_t(std::ceil(MaxRainParticles * maxDensity.x));
    const uint32_t snowTarget =
        uint32_t(std::ceil(MaxSnowParticles * maxDensity.y));
    const uint32_t targetCount = rainTarget + snowTarget;
    if (!targetCount) return;
    const uint32_t count =
        bgfx::getAvailInstanceDataBuffer(targetCount, InstanceStride);
    if (count == 0) return;
    bgfx::allocInstanceDataBuffer(&instances, count, InstanceStride);

    const double seconds = frame.visualDays * 86400.0;
    for (uint32_t i = 0; i < count; ++i) {
        const bool isSnow = i >= rainTarget;
        const uint32_t phaseIndex = isSnow ? i - rainTarget : i;
        const uint64_t h =
            particleHash(world.genSettings.seed, phaseIndex, isSnow ? 1 : 0);
        const float baseX = (unit(h) * 2.f - 1.f) * NearRadius;
        const float baseZ = (unit(mix64(h ^ 1)) * 2.f - 1.f) * NearRadius;
        const double drift = isSnow ? .55 : .22;
        const float x = wrapBox(baseX + frame.windTravelX * drift, NearRadius);
        const float z = wrapBox(baseZ + frame.windTravelZ * drift, NearRadius);
        const float weight = nearWeight(std::hypot(x, z));
        if (weight <= 0) continue;
        const double worldX = double(eye.x) + x, worldZ = double(eye.z) + z;
        const Vec2 density =
            mapped ? particleDensity(camera, x, z) : localDensity;
        const float phaseDensity = isSnow ? density.y : density.x;
        const float population = std::clamp(
            phaseDensity * (isSnow ? MaxSnowParticles : MaxRainParticles) -
                float(phaseIndex),
            0.f, 1.f);
        if (population <= 0) continue;
        const float phase = unit(mix64(h ^ 2));
        const float fallSpeed = isSnow ? 1.2f + unit(mix64(h ^ 3)) * 1.4f
                                       : 9.f + unit(mix64(h ^ 3)) * 5.f;
        const double fall =
            std::fmod(double(phase) * BoxHeight + double(fallSpeed) * seconds,
                      double(BoxHeight));
        const float y = BoxCentreY + BoxHeight * .5f - float(fall);

        const float size = unit(mix64(h ^ 4));
        const float halfWidth =
            isSnow ? .032f + .028f * size : .018f + .014f * size;
        const float halfHeight =
            isSnow ? halfWidth : .26f + .38f * unit(mix64(h ^ 5));
        const auto surface =
            precipitation::firstSurface(terrain, worldX, worldZ);
        if (!surface || (surface->height &&
                         double(eye.y) + y - halfHeight < *surface->height))
            continue;
        float alpha = isSnow ? .58f + .30f * unit(mix64(h ^ 6))
                             : .22f + .28f * unit(mix64(h ^ 6));
        alpha *= (.55f + .45f * (density.x + density.y)) * weight * population;

        float* data = reinterpret_cast<float*>(instances.data +
                                               size_t(drawn) * InstanceStride);
        data[0] = x;
        data[1] = y;
        data[2] = z;
        data[3] = isSnow ? 1.f : 0.f;
        data[4] = halfWidth;
        data[5] = halfHeight;
        data[6] = alpha;
        data[7] = 0;
        ++drawn;
    }
    if (!drawn) return;
    instances.num = drawn;

    target.drawInstanced(quad.get(), &shader, {&right, &up, &sun, &skyWeather},
                         true, relativeTransform(camera, 0, 0, 0),
                         DrawLayer::GAME_WEATHER, &instances);
}

void WeatherRenderer::updateWeatherMap(Camera& camera, const World& world,
                                       const Frame& frame) {
    camera.weatherMap = {};
    const auto eye = camera.eye();
    const auto wrap = world.wrap();
    const float range = std::min({frame.distance, wrap.tiles * .5f,
                                  float(MaxRangeRegions * RegionSize)});
    field.begin(world, eye.x, eye.z, frame.modelDays, range,
                frame.firstCurrentWeatherInterval);
    if (!bgfx::isValid(weatherTexture) || world.regions.empty() || range <= 0)
        return;

    // A stable, camera-centred texture with one texel per dated summary.
    // A ring of guard texels supplies bilinear samples out to the range edge.
    constexpr float invWidth = 1.f / (Field::Side * RegionSize);
    const int x0 = regionAt(eye.x) - Field::Side / 2;
    const int z0 = regionAt(eye.z) - Field::Side / 2;
    const int reach = int(std::ceil(range / RegionSize)) + 1;
    const auto channel = [](float n) {
        return uint8_t(std::lround(std::clamp(n, 0.f, 1.f) * 255.f));
    };
    std::array<WeatherPixel, Field::Capacity> next{};
    for (int z = Field::Side / 2 - reach; z <= Field::Side / 2 + reach; ++z)
        for (int x = Field::Side / 2 - reach; x <= Field::Side / 2 + reach;
             ++x) {
            const auto summary =
                field.at(world, x0 + x, z0 + z, frame.fixture, frame.force);
            if (!summary) continue;
            const auto& weather = summary->weather;
            const auto rain =
                channel(weather.visualDensity * (1.f - weather.snowFraction));
            const auto snow =
                channel(weather.visualDensity * weather.snowFraction);
            const auto& region = world.regions[size_t(z0 + z) * world.width +
                                               wrap.wrapRegion(x0 + x)];
            const float cloudBase = float(region.land_height_m) + 125.f;
            next[size_t(z) * Field::Side + x] = {
                channel(weather.cloudCover), rain, snow,
                channel((cloudBase - MinCloudBase) /
                        (MaxCloudBase - MinCloudBase))};
            wetRegions += rain != 0 || snow != 0;
        }
    if (!uploaded || pixels != next) {
        pixels = next;
        bgfx::updateTexture2D(weatherTexture, 0, 0, 0, 0, Field::Side,
                              Field::Side,
                              bgfx::copy(pixels.data(), sizeof(pixels)));
        uploaded = true;
    }
    camera.weatherMap = {
        weatherTexture,
        {float((double(eye.x) - double(x0) * RegionSize) * invWidth),
         float((double(eye.z) - double(z0) * RegionSize) * invWidth), invWidth,
         eye.y},
        {-eye.z, world.height * float(RegionSize) - eye.z, range,
         frame.fixture ? 1.f : 0.f},
        {float(wrap.wrapX(eye.x)), eye.z, float(wrap.tiles), 0.f}};
}

Vec2 WeatherRenderer::particleDensity(const Camera& camera, float x,
                                      float z) const {
    const auto& grid = camera.weatherMap.grid;
    const auto& limits = camera.weatherMap.limits;
    if (z < limits[0] || z >= limits[1]) return {};
    const float u = (grid[0] + x * grid[2]) * Field::Side;
    const float v = (grid[1] + z * grid[2]) * Field::Side;
    const auto texel = [&](int s, int t) {
        const auto& p = pixels[size_t(t) * Field::Side + s];
        return Vec2{p[1] / 255.f, p[2] / 255.f};
    };
    if (limits[3] > .5f) {
        const int s = int(std::floor(u)), t = int(std::floor(v));
        const float a = u - s, b = v - t;
        const float edge = world_weather::detail::smooth(
            std::min({a, 1.f - a, b, 1.f - b}) * RegionSize / RegionFeather);
        const auto value = texel(s, t);
        return {value.x * edge, value.y * edge};
    }
    const int s = int(std::floor(u - .5f)), t = int(std::floor(v - .5f));
    const float a = u - .5f - s, b = v - .5f - t;
    const auto p = texel(s, t), q = texel(s + 1, t);
    const auto r = texel(s, t + 1), w = texel(s + 1, t + 1);
    return {std::lerp(std::lerp(p.x, q.x, a), std::lerp(r.x, w.x, a), b),
            std::lerp(std::lerp(p.y, q.y, a), std::lerp(r.y, w.y, a), b)};
}

void WeatherRenderer::renderDistant(RenderTarget& target, const Camera& camera,
                                    const Frame& frame) {
    const float range = camera.weatherMap.limits[2];
    if (!wetRegions || range <= NearRadius) return;
    farDrawn = bgfx::getAvailInstanceDataBuffer(DistanceSlices, DistanceStride);
    if (!farDrawn) return;
    bgfx::allocInstanceDataBuffer(&farInstances, farDrawn, DistanceStride);
    constexpr float vertical = .57735027f;  // tan(60 degrees / 2)
    for (uint32_t i = 0; i < farDrawn; ++i) {
        // Integrate from back to front. Logarithmic spacing keeps much of
        // the depth resolution near the particle handover.
        const auto depth = [&](uint32_t n) {
            return NearFadeStart *
                   std::pow(range / NearFadeStart, float(n) / farDrawn);
        };
        const float back = depth(farDrawn - i), front = depth(farDrawn - i - 1);
        const float middle = .5f * (back + front);
        auto* data =
            reinterpret_cast<float*>(farInstances.data + i * DistanceStride);
        data[0] = middle;
        data[1] = middle * vertical * frame.aspect;
        data[2] = middle * vertical;
        data[3] = back - front;
    }
    curtainFade.setVec4({NearFadeStart, NearRadius, CloudThickness, 0});
    curtainTime.setVec4(
        {float(std::fmod(frame.visualDays * 86400., 1024.)), 0, 0, 0});
    gridProjection.setVec4(camera.weatherMap.grid);
    gridLimits.setVec4(camera.weatherMap.limits);
    gridAnchor.setVec4(camera.weatherMap.anchor);
    target.drawInstanced(
        quad.get(), &curtainShader,
        {&right, &up, &sun, &curtainFade, &curtainTime, &gridTexture,
         &gridProjection, &gridLimits, &gridAnchor},
        true, relativeTransform(camera, 0, 0, 0),
        DrawLayer::GAME_DISTANT_WEATHER, &farInstances);
}
