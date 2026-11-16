#include "light_renderer.hpp"

#include <algorithm>
#include <cmath>

#include "render/vertex.hpp"

namespace {
// Glare fades in from here so it does not cover the lamp block up close.
constexpr double GlareNear = 24;
constexpr double GlareFull = 48;
// Smallest glare on screen, in pixels, so distant lights stay visible.
constexpr double GlareMinPixels = 6;
// World half size of the glare when the camera is close.
constexpr double GlareWorldSize = 1.5;
}  // namespace

LightRenderer::LightRenderer(IRenderer& renderer) {
    lights.initVec4Array("u_lights", MaxLights * 4);
    info.initVec4("u_light_info", {0, 0, 0, 0});
    for (auto& glare : glares) {
        glare.center.initVec4("u_glare_center", {0, 0, 0, 0});
        glare.color.initVec4("u_glare_color", {0, 0, 0, 0});
    }
    // createStaticMesh references (does not copy) the data: keep it static.
    static const PosUvColorVertex vertices[4] = {{-1, -1, 0, 0, 0, 0xffffffff},
                                                 {1, -1, 0, 1, 0, 0xffffffff},
                                                 {1, 1, 0, 1, 1, 0xffffffff},
                                                 {-1, 1, 0, 0, 1, 0xffffffff}};
    // Both windings, so the quad never gets culled.
    static const uint16_t indices[12] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    quad.initStatic(&renderer, vertices, 4, indices, 12);
}

void LightRenderer::update(const LightSourceRegistry& registry,
                           const Camera& camera, WorldWrap wrap,
                           Vec2 viewport) {
    const Vec3 eye = camera.eye();
    const auto visible = visibleFarLights(registry, {eye.x, eye.y, eye.z},
                                          camera.viewDistance, wrap.tiles);
    std::array<float, MaxLights * 16> data{};
    activeLights = std::min<int>(MaxLights, int(visible.size()));
    for (int i = 0; i < activeLights; ++i) {
        const auto& view = visible[i];
        const auto& e = view.source->emission;
        const auto& d = view.source->direction;
        float* v = &data[i * 16];
        v[0] = float(view.position[0]);
        v[1] = float(view.position[1]);
        v[2] = float(view.position[2]);
        v[3] = e.radius;
        v[4] = e.color[0] * e.intensity;
        v[5] = e.color[1] * e.intensity;
        v[6] = e.color[2] * e.intensity;
        v[7] = e.falloff;
        v[8] = d[0];
        v[9] = d[1];
        v[10] = d[2];
        v[11] = -2;  // no cone
        v[12] = 1;
        if (e.cone) {
            constexpr float rad = 3.14159265f / 180.f;
            v[11] = std::cos(e.cone->outerAngle * rad);
            // smoothstep needs inner above outer.
            v[12] = std::max(std::cos(e.cone->innerAngle * rad), v[11] + 1e-4f);
        }
        // Every far light also has a flood-filled near field (tiles and
        // entity lights alike), so the per-pixel term hands over from it.
        v[13] = 1;
    }
    lights.setVec4Array(data.data(), MaxLights * 4);
    info.setVec4({float(activeLights), skyDarkness, 0, 0});

    // Glare: nearest first.
    std::vector<const FarLightView*> glaring;
    for (const auto& view : visible)
        if (view.distance > GlareNear && view.distance < camera.viewDistance)
            glaring.push_back(&view);
    std::sort(glaring.begin(), glaring.end(),
              [](auto* a, auto* b) { return a->distance < b->distance; });
    activeGlares = std::min<int>(MaxGlares, int(glaring.size()));
    const double pixelWorld =
        2 * std::tan(30.0 * 3.14159265 / 180.0) / std::max(1.f, viewport.y);
    for (int i = 0; i < activeGlares; ++i) {
        const auto& view = *glaring[i];
        const auto& e = view.source->emission;
        double rel[3] = {view.position[0] - eye.x, view.position[1] - eye.y,
                         view.position[2] - eye.z};
        // Pull toward the eye so the lamp's own block never hides it.
        const double pull =
            std::min(view.distance * 0.5, 1.5 + view.distance * 0.01);
        for (auto& c : rel) c *= 1 - pull / view.distance;
        const double size = std::max(
            GlareWorldSize, GlareMinPixels * pixelWorld * view.distance);
        glares[i].center.setVec4(
            {float(rel[0]), float(rel[1]), float(rel[2]), float(size)});
        const double fade = std::clamp(
            (view.distance - GlareNear) / (GlareFull - GlareNear), 0.0, 1.0);
        // Spotlights glare only when the eye is inside their cone.
        float cone = 1;
        if (e.cone) {
            const float len = float(view.distance);
            cone = e.coneFactor({float(-rel[0]) / len, float(-rel[1]) / len,
                                 float(-rel[2]) / len},
                                view.source->direction);
        }
        const float k = float(fade) * e.intensity * cone;

        glares[i].color.setVec4(
            {e.color[0] * k, e.color[1] * k, e.color[2] * k, 0});
    }
}

void LightRenderer::appendUniforms(std::vector<ShaderProperty*>& uniforms) {
    uniforms.push_back(&lights);
    uniforms.push_back(&info);
}

void LightRenderer::drawGlare(RenderTarget& target, const Camera& camera) {
    for (int i = 0; i < activeGlares; ++i)
        target.drawVertexBuffer(
            quad.get(), &shader, {&glares[i].center, &glares[i].color}, true,
            relativeTransform(camera, 0, 0, 0), DrawLayer::GAME_GLOW);
}
