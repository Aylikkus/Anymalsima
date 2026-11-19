#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "light_shadows.hpp"
#include "light_sources.hpp"
#include "lod_light_map.hpp"
#include "render/render_target.hpp"
#include "render/renderer_api.hpp"
#include "sky_occlusion_map.hpp"
#include "world_wrap.hpp"

// Main-thread GPU side of lights beyond the voxel near field:
//   - far lights (LightEmission::isFar): the u_lights / u_light_info
//     uniforms read by world_surface.sh, with cube shadow maps for the most
//     important ones (LightShadows);
//   - small lights on distant terrain: the LodLightMap glow texture;
//   - sky light under structures on surfaces without voxel light: the
//     SkyOcclusionMap texture;
//   - glare sprites for every light that is not walled in, so lighthouses
//     and towns read as bright points from far away. The sprites are
//     depth-tested, so terrain in front hides them.
class LightRenderer {
   public:
    enum class Program { Block, Lod };
    // Keep in sync with SURFACE_MAX_FAR_LIGHTS in shaders/world_surface.sh.
    static constexpr int MaxLights = 32;
    static constexpr int MaxGlares = 4096;

    // quality: TerrainLod quality index; sets the shadow budget.
    LightRenderer(IRenderer& renderer, int quality);
    ~LightRenderer();
    LightRenderer(const LightRenderer&) = delete;
    LightRenderer& operator=(const LightRenderer&) = delete;

    // 0 = full daylight, 1 = no sky light (debug night).
    void setSkyDarkness(float value) { skyDarkness = value; }
    float getSkyDarkness() const { return skyDarkness; }

    // Selects far lights and shadow slots, and refreshes the distant glow.
    // Call once per frame before any world draw. now: monotonic seconds.
    void update(const LightSourceRegistry& registry, const Camera& camera,
                WorldWrap wrap, double now);
    // Renders due shadow faces (casters draw them) and finalizes this
    // frame's light uniforms. Call after update(), before world draws.
    void renderShadows(RenderTarget& target,
                       const LightShadows::Casters& casters, double now);
    LightShadows& shadows() { return *shadowMaps; }
    // The structures' sky occlusion (TerrainLodManager::skyOcclusion);
    // uploaded when it changes, null for full sky.
    void setSkyOcclusion(std::shared_ptr<const SkyOcclusionMap> map);
    // Appends the light uniforms for a program including world_surface.sh.
    void appendUniforms(std::vector<ShaderProperty*>& uniforms,
                        Program program);
    void drawGlare(RenderTarget& target, const Camera& camera, Vec2 viewport);

    int lightCount() const { return activeLights; }
    int glareCount() const { return activeGlares; }
    int shadowedCount() const { return shadowMaps->shadowedCount(); }
    size_t glowLights() const { return glow.lightCount(); }

   private:
    void fillLightUniforms();
    void uploadGlow();

    std::unique_ptr<LightShadows> shadowMaps;
    LodLightMap glow;
    bgfx::TextureHandle glowTexture = BGFX_INVALID_HANDLE;
    double lastGlowBuild = -1e9;
    ShaderProperty lights;
    ShaderProperty info;
    ShaderProperty glowOrigin;
    ShaderProperty glowBlock;  // u_lod_light_map at the block stage
    ShaderProperty glowLod;    // ... at the terrain_lod stage
    std::shared_ptr<const SkyOcclusionMap> occlusion;
    bgfx::TextureHandle occlusionTexture = BGFX_INVALID_HANDLE;
    ShaderProperty occlusionOrigin;  // u_sky_occlusion_origin
    ShaderProperty occlusionBlock;   // u_sky_occlusion at the block stage
    ShaderProperty occlusionLod;     // ... at the terrain_lod stage
    MeshHandle quad;
    bgfx::InstanceDataBuffer instances{};
    std::string shader = "light_glare";
    const LightSourceRegistry* registry = nullptr;
    std::vector<FarLightView> visible;
    WorldWrap wrap;
    float skyDarkness = 0;
    int activeLights = 0;
    int activeGlares = 0;
};
