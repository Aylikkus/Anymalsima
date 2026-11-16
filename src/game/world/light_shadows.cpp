#include "light_shadows.hpp"

#include <bx/math.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

#include "render/render_views.hpp"
#include "world_wrap.hpp"

static_assert(std::all_of(std::begin(LightShadows::Budgets),
                          std::end(LightShadows::Budgets),
                          [](const auto& b) {
                              return b.facesPerFrame <=
                                     RenderViews::MaxShadowViews;
                          }),
              "shadow faces per frame exceed the reserved views");

LightShadows::LightShadows(int quality)
    : budget(Budgets[std::clamp(quality, 0, int(std::size(Budgets)) - 1)]) {
    const int tiles = budget.lights * LightShadowMath::FaceCount;
    rows = (tiles + LightShadowMath::Columns - 1) / LightShadowMath::Columns;
    const uint16_t width = uint16_t(LightShadowMath::Columns * TileSize);
    const uint16_t height = uint16_t(rows * TileSize);
    const auto* caps = bgfx::getCaps();
    if (!(caps->formats[bgfx::TextureFormat::R32F] &
          BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER))
        throw std::runtime_error("Shadow maps require R32F render targets");
    atlasTexture = bgfx::createTexture2D(
        width, height, false, 1, bgfx::TextureFormat::R32F,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
            BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP |
            BGFX_SAMPLER_V_CLAMP);
    depthTexture = bgfx::createTexture2D(width, height, false, 1,
                                         bgfx::TextureFormat::D32F,
                                         BGFX_TEXTURE_RT_WRITE_ONLY);
    const bgfx::TextureHandle attachments[] = {atlasTexture, depthTexture};
    frameBuffer = bgfx::createFrameBuffer(2, attachments, false);
    if (!bgfx::isValid(frameBuffer))
        throw std::runtime_error("Cannot create the shadow atlas");
    infoUniform.initVec4("u_shadow_info",
                         {float(TileSize), 1.f / width, 1.f / height,
                          caps->originBottomLeft ? 1.f : 0.f});
    for (int i = 0; i < budget.lights; ++i) {
        auto slot = std::make_unique<Slot>();
        slot->casterChunk.initVec4("u_shadow_caster", {1, 0, 0, 0});
        slot->casterLod.initVec4("u_shadow_caster", {1, 1, 0, 0});
        slot->light.initVec4("u_shadow_light", {0, 0, 0, 0});
        slots.push_back(std::move(slot));
    }
}

LightShadows::~LightShadows() {
    // bgfx defers destruction until frames referencing them finish.
    if (bgfx::isValid(frameBuffer)) bgfx::destroy(frameBuffer);
    if (bgfx::isValid(atlasTexture)) bgfx::destroy(atlasTexture);
    if (bgfx::isValid(depthTexture)) bgfx::destroy(depthTexture);
}

ShaderProperty* LightShadows::atlas(uint8_t stage) {
    for (auto& [s, property] : samplers)
        if (s == stage) return property.get();
    auto property = std::make_unique<ShaderProperty>();
    property->initTexture("u_shadow_atlas", stage, atlasTexture);
    samplers.emplace_back(stage, std::move(property));
    return samplers.back().second.get();
}

int LightShadows::shadowedCount() const {
    return int(std::count_if(slots.begin(), slots.end(), [](const auto& s) {
        return s->used && s->ready;
    }));
}

void LightShadows::assign(const std::vector<FarLightView>& ranked) {
    std::unordered_map<LightSourceRegistry::Id, int> rankOf;
    const size_t keep = size_t(std::ceil(budget.lights * (1 + Keep)));
    for (size_t i = 0; i < ranked.size() && i < keep; ++i)
        rankOf.emplace(ranked[i].id, int(i));
    for (auto& slot : slots)
        if (slot->used && !rankOf.contains(slot->id)) {
            slot->used = false;
            slot->ready = false;
        }
    for (size_t i = 0; i < ranked.size(); ++i) {
        const auto& view = ranked[i];
        const auto& source = *view.source;
        Slot* slot = nullptr;
        for (auto& s : slots)
            if (s->used && s->id == view.id) slot = s.get();
        if (!slot) {
            // New lights only enter within the budget proper.
            if (int(i) >= budget.lights) continue;
            for (auto& s : slots)
                if (!s->used) {
                    slot = s.get();
                    break;
                }
            if (!slot) continue;
            slot->used = true;
            slot->ready = false;
            slot->id = view.id;
            slot->radius = -1;  // forces the refresh below
        }
        slot->rank = int(i);
        slot->image = view.position;
        const auto faces =
            LightShadowMath::reachableFaces(source.emission, source.direction);
        if (slot->canonical != source.position ||
            slot->direction != source.direction ||
            slot->radius != source.emission.radius || slot->faces != faces) {
            slot->canonical = source.position;
            slot->direction = source.direction;
            slot->radius = source.emission.radius;
            slot->faces = faces;
            slot->dirty = faces;
            slot->ready = false;
        }
        slot->casterChunk.setVec4({1.f / slot->radius, 0, 0, 0});
        slot->casterLod.setVec4({1.f / slot->radius, 1, 0, 0});
        slot->light.setVec4({float(slot->image[0]), float(slot->image[1]),
                             float(slot->image[2]), 0});
    }
}

int LightShadows::slot(LightSourceRegistry::Id id) const {
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i]->used && slots[i]->ready && slots[i]->id == id)
            return int(i);
    return -1;
}

void LightShadows::geometryChanged(const std::array<double, 3>& min,
                                   const std::array<double, 3>& max,
                                   int periodX) {
    for (auto& slot : slots) {
        if (!slot->used) continue;
        const auto& l = slot->canonical;
        // The box copy nearest the light.
        const double cx = (min[0] + max[0]) / 2;
        const double shift = WorldWrap::deltaf(cx, l[0], periodX) + l[0] - cx;
        const double lo[3] = {min[0] + shift - l[0], min[1] - l[1],
                              min[2] - l[2]};
        const double hi[3] = {max[0] + shift - l[0], max[1] - l[1],
                              max[2] - l[2]};
        double gap = 0;
        bool inside = true;
        for (int a = 0; a < 3; ++a) {
            const double g = std::max({lo[a], 0.0, -hi[a]});
            gap += g * g;
            inside &= lo[a] <= 0 && hi[a] >= 0;
        }
        if (gap > double(slot->radius) * slot->radius) continue;
        uint8_t mask = 0x3f;
        if (!inside) {
            // Faces seen by the box corners and centre; a box smaller than a
            // face's view cannot straddle a face without touching a corner's
            // face or the centre's.
            mask = 0;
            double nx, ny;
            for (int c = 0; c < 9; ++c) {
                const double x =
                    c == 8 ? (lo[0] + hi[0]) / 2 : (c & 1 ? hi[0] : lo[0]);
                const double y =
                    c == 8 ? (lo[1] + hi[1]) / 2 : (c & 2 ? hi[1] : lo[1]);
                const double z =
                    c == 8 ? (lo[2] + hi[2]) / 2 : (c & 4 ? hi[2] : lo[2]);
                mask |= uint8_t(1u << LightShadowMath::face(x, y, z, nx, ny));
            }
        }
        slot->dirty |= uint8_t(mask & slot->faces);
    }
}

void LightShadows::lodChanged() {
    for (auto& slot : slots)
        if (slot->used) slot->lodStale = true;
}

void LightShadows::render(RenderTarget& target, const Casters& casters,
                          double now) {
    std::vector<Slot*> order;
    for (auto& slot : slots) {
        if (!slot->used) continue;
        if (slot->lodStale && now - slot->lastLod >= LodRefreshSeconds) {
            slot->dirty |= slot->faces;
            slot->lodStale = false;
            slot->lastLod = now;
        }
        if (slot->dirty) order.push_back(slot.get());
    }
    // Lights not yet shown first, then by importance.
    std::sort(order.begin(), order.end(), [](const Slot* a, const Slot* b) {
        return a->ready != b->ready ? !a->ready : a->rank < b->rank;
    });
    const bool homogeneous = bgfx::getCaps()->homogeneousDepth;
    int faces = 0;
    for (Slot* slot : order) {
        const int index =
            int(std::find_if(slots.begin(), slots.end(),
                             [&](const auto& s) { return s.get() == slot; }) -
                slots.begin());
        for (int f = 0; f < LightShadowMath::FaceCount; ++f) {
            if (!(slot->dirty & (1u << f))) continue;
            if (faces >= budget.facesPerFrame) break;
            const uint16_t view = uint16_t(RenderViews::ShadowBase + faces);
            int x, y;
            LightShadowMath::tileOrigin(index, f, TileSize, x, y);
            bgfx::setViewFrameBuffer(view, frameBuffer);
            bgfx::setViewRect(view, uint16_t(x), uint16_t(y), TileSize,
                              TileSize);
            bgfx::setViewClear(view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                               0xffffffff, 1.f, 0);
            const auto viewMatrix = LightShadowMath::viewMatrix(f);
            float proj[16], vp[16];
            bx::mtxProj(proj, float(LightShadowMath::guardFov(TileSize)), 1.f,
                        NearPlane, slot->radius, homogeneous);
            bgfx::setViewTransform(view, viewMatrix.data(), proj);
            bgfx::touch(view);
            bx::mtxMul(vp, viewMatrix.data(), proj);

            Face face;
            face.light = slot->image;
            face.radius = slot->radius;
            face.view = view;
            face.frustum = Frustum::fromViewProj(vp);
            face.shader = &casterShader;
            face.chunkUniforms = {&slot->casterChunk, &slot->light};
            face.lodUniforms = {&slot->casterLod, &slot->light};
            casters(target, face);
            slot->dirty &= uint8_t(~(1u << f));
            ++faces;
            ++renderedFaces;
        }
        if (!(slot->dirty & slot->faces)) slot->ready = true;
        if (faces >= budget.facesPerFrame) break;
    }
}
