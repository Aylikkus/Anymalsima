#pragma once

#include <cstdint>
#include <string>

#include "render/render_target.hpp"
#include "render/renderer_api.hpp"
#include "world/world_weather.hpp"

namespace snow {
class SnowTerrain;
}

// Near precipitation as world-space particles (docs/handoff/weather.md, step
// 09). Rain streaks and snowflakes live in a small box around the eye, are
// depth-tested against the loaded world and drift with the sampled wind. The
// far view is still the sky's cloud layer; this only supplies the near field.
class WeatherRenderer {
   public:
    // Particle counts at full visual density. Actual counts scale with
    // weather.visualDensity and split between rain and snow.
    static constexpr uint32_t MaxRainParticles = 1800;
    static constexpr uint32_t MaxSnowParticles = 1200;
    static constexpr uint32_t InstanceStride = 32;  // two vec4 per particle

    explicit WeatherRenderer(IRenderer& renderer);
    WeatherRenderer(const WeatherRenderer&) = delete;
    WeatherRenderer& operator=(const WeatherRenderer&) = delete;

    // weather: sampled presentation; gameDays drives deterministic fall and
    // drift, seed keeps particle placement stable for a world. Does nothing
    // when there is no precipitation or the eye is underwater.
    void render(RenderTarget& target, const Camera& camera,
                const world_weather::Sample& weather, double gameDays,
                double windTravelX, double windTravelZ, uint64_t seed,
                const snow::SnowTerrain& terrain);

    uint32_t particleCount() const { return drawn; }

   private:
    IRenderer& renderer;
    MeshHandle quad;
    ShaderProperty right;
    ShaderProperty up;
    ShaderProperty sun;
    ShaderProperty skyWeather;
    bgfx::InstanceDataBuffer instances{};
    std::string shader = "weather_particles";
    uint32_t drawn = 0;
};
