#pragma once

#include <array>
#include <string>
#include <vector>

#include "light_sources.hpp"
#include "render/render_target.hpp"
#include "render/renderer_api.hpp"
#include "world_wrap.hpp"

// Main-thread GPU side of far lights (LightEmission::isFar): fills the
// u_lights / u_light_info uniforms read by world_surface.sh, and draws a glare
// sprite for each far light so lighthouses read as bright points from far
// away. The sprites are depth-tested, so terrain in front hides them.
class LightRenderer {
   public:
    // Keep in sync with SURFACE_MAX_FAR_LIGHTS in shaders/world_surface.sh.
    static constexpr int MaxLights = 32;
    static constexpr int MaxGlares = 64;

    explicit LightRenderer(IRenderer& renderer);

    // 0 = full daylight, 1 = no sky light (debug night).
    void setSkyDarkness(float value) { skyDarkness = value; }
    float getSkyDarkness() const { return skyDarkness; }

    // Selects the far lights for this frame. Call once per frame before any
    // world draw that appends uniforms().
    void update(const LightSourceRegistry& registry, const Camera& camera,
                WorldWrap wrap, Vec2 viewport);
    // Appends the light uniforms; every world_surface.sh program needs them.
    void appendUniforms(std::vector<ShaderProperty*>& uniforms);
    void drawGlare(RenderTarget& target, const Camera& camera);
    int lightCount() const { return activeLights; }
    int glareCount() const { return activeGlares; }

   private:
    struct Glare {
        ShaderProperty center;
        ShaderProperty color;
    };
    ShaderProperty lights;
    ShaderProperty info;
    MeshHandle quad;
    std::array<Glare, MaxGlares> glares;
    std::string shader = "light_glare";
    float skyDarkness = 0;
    int activeLights = 0;
    int activeGlares = 0;
};
