#pragma once

#include <cstdint>
#include <string>

#include "render/render_target.hpp"
#include "render/renderer_api.hpp"
#include "weather_presentation.hpp"

namespace snow {
class SnowSettlement;
class SnowTerrain;
}  // namespace snow

// Dated regional curtains and exposed, world-space near particles share the
// weather field and depth-test against the world.
class WeatherRenderer {
   public:
    // Particle counts at full visual density. Actual counts scale with
    // weather.visualDensity and split between rain and snow.
    static constexpr uint32_t MaxRainParticles = 1800;
    static constexpr uint32_t MaxSnowParticles = 1200;
    static constexpr uint32_t InstanceStride = 32;  // two vec4 per particle

    explicit WeatherRenderer(IRenderer& renderer);
    ~WeatherRenderer();
    WeatherRenderer(const WeatherRenderer&) = delete;
    WeatherRenderer& operator=(const WeatherRenderer&) = delete;

    struct Frame {
        double modelDays, visualDays;
        double windTravelX, windTravelZ;
        float distance, aspect;
        int force = 0;
        std::optional<weather_presentation::Fixture> fixture;
        bool drawPrecipitation = true;
        int64_t firstCurrentWeatherInterval = 0;
    };

    void render(RenderTarget& target, Camera& camera, const World& world,
                const world_weather::Sample& localWeather, const Frame& frame,
                const snow::SnowTerrain& terrain,
                const snow::SnowSettlement& snow);

    uint32_t particleCount() const { return drawn; }
    uint32_t regionCount() const { return farDrawn ? wetRegions : 0; }
    unsigned summaryRebuilds() const { return field.rebuilds(); }

   private:
    void updateWeatherMap(Camera& camera, const World& world,
                          const Frame& frame);
    void renderDistant(RenderTarget& target, const Camera& camera,
                       const Frame& frame);
    Vec2 particleDensity(const Camera& camera, float x, float z) const;

    MeshHandle quad;
    ShaderProperty right;
    ShaderProperty up;
    ShaderProperty sun;
    ShaderProperty skyWeather;
    ShaderProperty curtainFade;
    ShaderProperty curtainTime;
    ShaderProperty gridTexture;
    ShaderProperty gridProjection;
    ShaderProperty gridLimits;
    ShaderProperty gridAnchor;
    bgfx::TextureHandle weatherTexture = BGFX_INVALID_HANDLE;
    bgfx::InstanceDataBuffer instances{};
    bgfx::InstanceDataBuffer farInstances{};
    std::string shader = "weather_particles";
    std::string curtainShader = "weather_curtains";
    weather_presentation::Field field;
    using WeatherPixel = std::array<uint8_t, 4>;
    std::array<WeatherPixel, weather_presentation::Field::Capacity> pixels{};
    uint32_t wetRegions = 0;
    bool uploaded = false;
    uint32_t drawn = 0;
    uint32_t farDrawn = 0;
};
