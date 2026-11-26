#pragma once

#include <bgfx/bgfx.h>

#include <array>

class ResourceManager;
struct Camera;
struct StaticMesh;

class SkyRenderer {
   public:
    explicit SkyRenderer(const ResourceManager& resources);
    ~SkyRenderer();
    SkyRenderer(const SkyRenderer&) = delete;
    SkyRenderer& operator=(const SkyRenderer&) = delete;

    // The caller configures the sky view and supplies a 0..1 screen quad.
    void draw(const Camera& camera, float aspect, const StaticMesh& quad) const;

   private:
    const ResourceManager& resources;
    bgfx::UniformHandle cameraUniform;
    bgfx::UniformHandle sunUniform;
    bgfx::UniformHandle weatherUniform;
    bgfx::UniformHandle cloudOffsetUniform;
    bgfx::UniformHandle seedUniform;
    bgfx::UniformHandle gridProjection;
    bgfx::UniformHandle gridLimits;
    bgfx::UniformHandle gridAnchor;
    std::array<bgfx::UniformHandle, 3> samplers;
};
