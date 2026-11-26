#include "weather_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "precipitation.hpp"
#include "render/vertex.hpp"

namespace {

constexpr float BoxRadius = 26.f;
constexpr float BoxHeight = 16.f;
constexpr float BoxCentreY = 6.f;

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

WeatherRenderer::WeatherRenderer(IRenderer& renderer) : renderer(renderer) {
    // Both windings: a camera-facing quad is never culled.
    static const PosVertex vertices[] = {
        {-1.f, -1.f, 0.f}, {1.f, -1.f, 0.f}, {1.f, 1.f, 0.f}, {-1.f, 1.f, 0.f}};
    static const uint16_t indices[] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    quad.initStatic(&renderer, vertices, std::size(vertices), indices,
                    std::size(indices));
    right.initVec4("u_weather_right", {1, 0, 0, 0});
    up.initVec4("u_weather_up", {0, 1, 0, 0});
    sun.initVec4("u_sky_sun", {0, 1, 0, 1});
    skyWeather.initVec4("u_sky_weather", {});
}

void WeatherRenderer::render(RenderTarget& target, const Camera& camera,
                             const world_weather::Sample& weather,
                             double gameDays, double windTravelX,
                             double windTravelZ, uint64_t seed,
                             const snow::SnowTerrain& terrain) {
    drawn = 0;
    const float density = std::clamp(weather.visualDensity, 0.f, 1.f);
    if (density <= .001f || camera.underwater) return;

    const float snow = std::clamp(weather.snowFraction, 0.f, 1.f);
    const uint32_t rainTarget =
        uint32_t(float(MaxRainParticles) * density * (1.f - snow));
    const uint32_t snowTarget =
        uint32_t(float(MaxSnowParticles) * density * snow);
    const uint32_t targetCount = rainTarget + snowTarget;
    if (targetCount == 0) return;
    const uint32_t count =
        bgfx::getAvailInstanceDataBuffer(targetCount, InstanceStride);
    if (count == 0) return;
    bgfx::allocInstanceDataBuffer(&instances, count, InstanceStride);

    const auto eye = camera.eye();
    const double seconds = gameDays * 86400.0;
    for (uint32_t i = 0; i < count; ++i) {
        const bool isSnow = i >= rainTarget;
        const uint64_t h = particleHash(seed, i, isSnow ? 1 : 0);
        const float baseX = (unit(h) * 2.f - 1.f) * BoxRadius;
        const float baseZ = (unit(mix64(h ^ 1)) * 2.f - 1.f) * BoxRadius;
        const float phase = unit(mix64(h ^ 2));
        const float fallSpeed = isSnow ? 1.2f + unit(mix64(h ^ 3)) * 1.4f
                                       : 9.f + unit(mix64(h ^ 3)) * 5.f;
        const double fall =
            std::fmod(double(phase) * BoxHeight + double(fallSpeed) * seconds,
                      double(BoxHeight));
        const float y = BoxCentreY + BoxHeight * .5f - float(fall);
        const double drift = isSnow ? .55 : .22;
        const float x = wrapBox(double(baseX) + windTravelX * drift, BoxRadius);
        const float z = wrapBox(double(baseZ) + windTravelZ * drift, BoxRadius);

        const float size = unit(mix64(h ^ 4));
        const float halfWidth =
            isSnow ? .032f + .028f * size : .018f + .014f * size;
        const float halfHeight =
            isSnow ? halfWidth : .26f + .38f * unit(mix64(h ^ 5));
        const auto surface = precipitation::firstSurface(
            terrain, double(eye.x) + x, double(eye.z) + z);
        if (!surface || (surface->height &&
                         double(eye.y) + y - halfHeight < *surface->height))
            continue;
        float alpha = isSnow ? .58f + .30f * unit(mix64(h ^ 6))
                             : .22f + .28f * unit(mix64(h ^ 6));
        alpha *= .55f + .45f * density;

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

    const float yaw = camera.yaw, pitch = camera.pitch;
    const float cosYaw = std::cos(yaw), sinYaw = std::sin(yaw);
    const float cosPitch = std::cos(pitch), sinPitch = std::sin(pitch);
    // Camera right and up, matching the sky shader's basis.
    right.setVec4({cosYaw, 0, -sinYaw, 0});
    up.setVec4({-sinPitch * sinYaw, cosPitch, -sinPitch * cosYaw, 0});
    sun.setVec4(camera.sky.sun);
    skyWeather.setVec4(camera.sky.weather);
    target.drawInstanced(quad.get(), &shader, {&right, &up, &sun, &skyWeather},
                         true, relativeTransform(camera, 0, 0, 0),
                         DrawLayer::GAME_WEATHER, &instances);
}
