#include "sky_renderer.hpp"

#include <string>

#include "render/render_views.hpp"
#include "render/renderer_api.hpp"
#include "resource_manager.hpp"

namespace {
const std::array<std::string, 2> TextureKeys{"sky.sun", "sky.moon"};
}

SkyRenderer::SkyRenderer(const ResourceManager& resources)
    : resources(resources),
      cameraUniform(
          bgfx::createUniform("u_sky_camera", bgfx::UniformType::Vec4)),
      sunUniform(bgfx::createUniform("u_sky_sun", bgfx::UniformType::Vec4)),
      weatherUniform(
          bgfx::createUniform("u_sky_weather", bgfx::UniformType::Vec4)),
      cloudOffsetUniform(
          bgfx::createUniform("u_sky_cloud_offset", bgfx::UniformType::Vec4)),
      seedUniform(bgfx::createUniform("u_sky_seed", bgfx::UniformType::Vec4)),
      gridProjection(
          bgfx::createUniform("u_weather_grid", bgfx::UniformType::Vec4)),
      gridLimits(
          bgfx::createUniform("u_weather_limits", bgfx::UniformType::Vec4)),
      gridAnchor(
          bgfx::createUniform("u_weather_anchor", bgfx::UniformType::Vec4)),
      samplers{
          bgfx::createUniform("u_sky_sun_map", bgfx::UniformType::Sampler),
          bgfx::createUniform("u_sky_moon_map", bgfx::UniformType::Sampler),
          bgfx::createUniform("u_weather_map", bgfx::UniformType::Sampler)} {}

SkyRenderer::~SkyRenderer() {
    for (const auto uniform : samplers)
        if (bgfx::isValid(uniform)) bgfx::destroy(uniform);
    for (const auto uniform :
         {cameraUniform, sunUniform, weatherUniform, cloudOffsetUniform,
          seedUniform, gridProjection, gridLimits, gridAnchor})
        if (bgfx::isValid(uniform)) bgfx::destroy(uniform);
}

void SkyRenderer::draw(const Camera& camera, float aspect,
                       const StaticMesh& quad) const {
    const std::array cameraData{camera.yaw, camera.pitch, aspect,
                                camera.underwater ? 1.f : 0.f};
    bgfx::setUniform(cameraUniform, cameraData.data());
    bgfx::setUniform(sunUniform, camera.sky.sun.data());
    bgfx::setUniform(weatherUniform, camera.sky.weather.data());
    bgfx::setUniform(cloudOffsetUniform, camera.sky.cloudOffset.data());
    bgfx::setUniform(seedUniform, camera.sky.seed.data());
    bgfx::setUniform(gridProjection, camera.weatherMap.grid.data());
    bgfx::setUniform(gridLimits, camera.weatherMap.limits.data());
    bgfx::setUniform(gridAnchor, camera.weatherMap.anchor.data());
    constexpr uint32_t flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    for (uint8_t stage = 0; stage < TextureKeys.size(); ++stage) {
        bgfx::setTexture(stage, samplers[stage],
                         resources.getTexture(TextureKeys[stage]), flags);
    }
    const auto weather = camera.weatherMap.texture;
    bgfx::setTexture(
        2, samplers[2],
        bgfx::isValid(weather) ? weather : resources.getTexture(TextureKeys[0]),
        flags);
    bgfx::setVertexBuffer(0, quad.vbh);
    bgfx::setIndexBuffer(quad.ibh);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(RenderViews::Sky, resources.getProgram("terrain_sky"));
}
