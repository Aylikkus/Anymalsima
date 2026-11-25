#include "light_renderer.hpp"

#include <bx/math.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/render_views.hpp"
#include "render/vertex.hpp"

namespace {
// Sampler stages; keep in sync with block.frag, terrain_lod.frag and
// light_glare.vert.
constexpr uint8_t BlockAtlasStage = 3, BlockGlowStage = 4;
constexpr uint8_t LodAtlasStage = 4, LodGlowStage = 5;
// Sky occlusion under structures; also foliage.frag (block uniforms).
constexpr uint8_t BlockOcclusionStage = 6, LodOcclusionStage = 7;
constexpr uint8_t GlareAtlasStage = 0;
// Glare fades in from here so it does not cover the lamp block up close.
constexpr double GlareNear = 24;
constexpr double GlareFull = 48;
// Smallest glare on screen, in pixels, so distant lights stay visible; small
// lights get smaller dots.
constexpr double GlareMinPixels = 6;
constexpr double SmallGlareMinPixels = 3;
// World half size of the glare when the camera is close.
constexpr double GlareWorldSize = 1.5;
// The distant glow map is rebuilt at most this often.
constexpr double GlowRebuildSeconds = 0.5;
constexpr uint16_t InstanceStride = 48;
}  // namespace

LightRenderer::LightRenderer(IRenderer& renderer, int quality)
    : shadowMaps(std::make_unique<LightShadows>(quality)) {
    lights.initVec4Array("u_lights", MaxLights * 4);
    info.initVec4("u_light_info", {0, 0, 0, 0});
    sun.initVec4("u_sky_sun", SkyState{}.sun);
    glowOrigin.initVec4("u_lod_light_origin", {0, 0, 0, 0});
    glowTexture =
        bgfx::createTexture2D(LodLightMap::Size, LodLightMap::Size, false, 1,
                              bgfx::TextureFormat::RGBA16F,
                              BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    glowBlock.initTexture("u_lod_light_map", BlockGlowStage, glowTexture);
    glowLod.initTexture("u_lod_light_map", LodGlowStage, glowTexture);
    occlusionOrigin.initVec4("u_sky_occlusion_origin", {0, 0, 0, 0});
    occlusionTexture = bgfx::createTexture2D(
        SkyOcclusionMap::Size, SkyOcclusionMap::Size, false, 1,
        bgfx::TextureFormat::RGBA16F,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT |
            BGFX_SAMPLER_MAG_POINT);
    static_assert(SkyOcclusionMap::Thresholds == 4, "one RGBA texel");
    occlusionBlock.initTexture("u_sky_occlusion", BlockOcclusionStage,
                               occlusionTexture);
    occlusionLod.initTexture("u_sky_occlusion", LodOcclusionStage,
                             occlusionTexture);
    // createStaticMesh references (does not copy) the data: keep it static.
    static const PosUvColorVertex vertices[4] = {{-1, -1, 0, 0, 0, 0xffffffff},
                                                 {1, -1, 0, 1, 0, 0xffffffff},
                                                 {1, 1, 0, 1, 1, 0xffffffff},
                                                 {-1, 1, 0, 0, 1, 0xffffffff}};
    // Both windings, so the quad never gets culled.
    static const uint16_t indices[12] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    quad.initStatic(&renderer, vertices, 4, indices, 12);
}

LightRenderer::~LightRenderer() {
    // bgfx defers destruction until frames referencing it finish.
    if (bgfx::isValid(glowTexture)) bgfx::destroy(glowTexture);
    if (bgfx::isValid(occlusionTexture)) bgfx::destroy(occlusionTexture);
}

void LightRenderer::setSkyOcclusion(
    std::shared_ptr<const SkyOcclusionMap> map) {
    if (map == occlusion) return;
    occlusion = std::move(map);
    if (!occlusion || !occlusion->valid()) {
        occlusionOrigin.setVec4({0, 0, 0, 0});
        return;
    }
    // The half-float image is built with the map on the pool; uploading
    // stays a copy.
    const auto& halves = occlusion->texelHalves();
    const bgfx::Memory* memory =
        bgfx::alloc(uint32_t(halves.size() * sizeof(uint16_t)));
    std::memcpy(memory->data, halves.data(), halves.size() * sizeof(uint16_t));
    bgfx::updateTexture2D(occlusionTexture, 0, 0, 0, 0, SkyOcclusionMap::Size,
                          SkyOcclusionMap::Size, memory);
    occlusionOrigin.setVec4({float(occlusion->originX()),
                             float(occlusion->originZ()),
                             1.f / SkyOcclusionMap::Extent, 1});
}

void LightRenderer::update(const LightSourceRegistry& sources,
                           const Camera& camera, WorldWrap worldWrap,
                           double now) {
    registry = &sources;
    wrap = worldWrap;
    const Vec3 eye = camera.eye();
    visible = visibleFarLights(sources, {eye.x, eye.y, eye.z},
                               camera.viewDistance, wrap.tiles);
    shadowMaps->assign(visible);
    if (glow.stale(camera.position.x, camera.position.z, sources.version()) &&
        now - lastGlowBuild >= GlowRebuildSeconds) {
        glow.build(sources, camera.position.x, camera.position.z, wrap.tiles);
        uploadGlow();
        lastGlowBuild = now;
    }
    fillLightUniforms();
}

void LightRenderer::uploadGlow() {
    const auto& texels = glow.texels();
    const bgfx::Memory* memory =
        bgfx::alloc(uint32_t(texels.size() * sizeof(uint16_t)));
    auto* half = reinterpret_cast<uint16_t*>(memory->data);
    for (size_t i = 0; i < texels.size(); ++i)
        half[i] = bx::halfFromFloat(std::min(texels[i], 60000.f));
    bgfx::updateTexture2D(glowTexture, 0, 0, 0, 0, LodLightMap::Size,
                          LodLightMap::Size, memory);
    glowOrigin.setVec4({float(glow.originX()), float(glow.originZ()),
                        1.f / LodLightMap::Extent, 1});
}

void LightRenderer::renderShadows(RenderTarget& target,
                                  const LightShadows::Casters& casters,
                                  double now) {
    shadowMaps->render(target, casters, now);
    RenderViews::applyOrder(RenderViews::MaxShadowViews);
    fillLightUniforms();
}

void LightRenderer::fillLightUniforms() {
    std::array<float, MaxLights * 16> data{};
    activeLights = 0;
    const size_t count = std::min(visible.size(), size_t(MaxLights));
    for (size_t i = 0; i < count; ++i) {
        const auto& view = visible[i];
        const int slot = shadowMaps->slot(view.id);
        // Sky exposure at the receiver cannot tell whether a wall separates
        // it from the light. Wait for shadow coverage before lighting it.
        if (slot < 0) continue;
        const auto& e = view.source->emission;
        const auto& d = view.source->direction;
        float* v = &data[activeLights++ * 16];
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
        v[14] = float(slot);
        v[15] = float(view.source->openFaces);
    }
    lights.setVec4Array(data.data(), MaxLights * 4);
    info.setVec4({float(activeLights), 0, 0, 0});
}

void LightRenderer::appendUniforms(std::vector<ShaderProperty*>& uniforms,
                                   Program program) {
    const bool block = program == Program::Block;
    uniforms.push_back(&lights);
    uniforms.push_back(&info);
    uniforms.push_back(&sun);
    uniforms.push_back(&glowOrigin);
    uniforms.push_back(shadowMaps->info());
    uniforms.push_back(
        shadowMaps->atlas(block ? BlockAtlasStage : LodAtlasStage));
    uniforms.push_back(block ? &glowBlock : &glowLod);
    uniforms.push_back(&occlusionOrigin);
    uniforms.push_back(block ? &occlusionBlock : &occlusionLod);
}

void LightRenderer::drawGlare(RenderTarget& target, const Camera& camera,
                              Vec2 viewport) {
    activeGlares = 0;
    if (!registry) return;
    const Vec3 eye = camera.eye();
    struct Candidate {
        const LightSource* source;
        LightSourceRegistry::Id id;
        std::array<double, 3> position;
        double distance;
    };
    std::vector<Candidate> candidates;
    registry->forEachNear(
        eye.x, eye.z, camera.viewDistance, wrap.tiles,
        [&](LightSourceRegistry::Id id, const LightSource& s) {
            if (!s.emission.emits() || s.sealed()) return;
            std::array<double, 3> p = s.position;
            p[0] = eye.x + wrap.deltaX(p[0], eye.x);
            const double dx = p[0] - eye.x, dy = p[1] - eye.y,
                         dz = p[2] - eye.z;
            const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d <= GlareNear || d >= camera.viewDistance) return;
            candidates.push_back({&s, id, p, d});
        });
    if (candidates.size() > size_t(MaxGlares)) {
        std::nth_element(candidates.begin(), candidates.begin() + MaxGlares,
                         candidates.end(), [](const auto& a, const auto& b) {
                             return a.distance < b.distance;
                         });
        candidates.resize(MaxGlares);
    }
    if (candidates.empty()) return;
    const uint32_t count = bgfx::getAvailInstanceDataBuffer(
        uint32_t(candidates.size()), InstanceStride);
    if (count == 0) return;
    bgfx::allocInstanceDataBuffer(&instances, count, InstanceStride);
    const double pixelWorld =
        2 * std::tan(30.0 * 3.14159265 / 180.0) / std::max(1.f, viewport.y);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& c = candidates[i];
        const auto& e = c.source->emission;
        double rel[3] = {c.position[0] - eye.x, c.position[1] - eye.y,
                         c.position[2] - eye.z};
        // Pull toward the eye so the lamp's own block never hides it.
        const double pull = std::min(c.distance * 0.5, 1.5 + c.distance * 0.01);
        for (auto& v : rel) v *= 1 - pull / c.distance;
        const bool far = e.isFar();
        const double minPixels =
            far ? GlareMinPixels
                : std::clamp(SmallGlareMinPixels *
                                 std::sqrt(e.radius / LightEmission::NearRange),
                             2.0, GlareMinPixels);
        const double size =
            std::max(far ? GlareWorldSize : GlareWorldSize * 0.5,
                     minPixels * pixelWorld * c.distance);
        const double fade = std::clamp(
            (c.distance - GlareNear) / (GlareFull - GlareNear), 0.0, 1.0);
        // Spotlights glare only when the eye is inside their cone.
        float cone = 1;
        if (e.cone) {
            const float len = float(c.distance);
            cone = e.coneFactor({float(-rel[0]) / len, float(-rel[1]) / len,
                                 float(-rel[2]) / len},
                                c.source->direction);
        }
        const float k = float(fade) * e.intensity * cone;
        const int slot = far ? shadowMaps->slot(c.id) : -1;
        float* v =
            reinterpret_cast<float*>(instances.data + i * InstanceStride);
        v[0] = float(rel[0]);
        v[1] = float(rel[1]);
        v[2] = float(rel[2]);
        v[3] = float(size);
        v[4] = e.color[0] * k;
        v[5] = e.color[1] * k;
        v[6] = e.color[2] * k;
        v[7] = 0;
        v[8] = float(slot);
        v[9] = e.radius;
        v[10] = float(pull);
        v[11] = 0;
    }
    activeGlares = int(count);
    target.drawInstanced(
        quad.get(), &shader,
        {shadowMaps->info(), shadowMaps->atlas(GlareAtlasStage)}, true,
        relativeTransform(camera, 0, 0, 0), DrawLayer::GAME_GLOW, &instances);
}
