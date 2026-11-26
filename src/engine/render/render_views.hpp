#pragma once

#include <bgfx/bgfx.h>

#include <cstdint>

// bgfx view ids shared by the renderer and world systems.
namespace RenderViews {
inline constexpr bgfx::ViewId World = 0;  // camera-relative world
inline constexpr bgfx::ViewId Ui = 1;
inline constexpr bgfx::ViewId Sky = 2;
inline constexpr bgfx::ViewId Glow = 3;  // additive light glare
// Near rain/snow: drawn after the world, before glow, without depth writes.
inline constexpr bgfx::ViewId Weather = 4;
inline constexpr bgfx::ViewId DistantWeather = 5;
// Shadow caster passes (one cube face each) use ids from here on.
inline constexpr bgfx::ViewId ShadowBase = 16;
// Shadow views reserved every frame (LightShadows budgets stay below).
inline constexpr uint16_t MaxShadowViews = 16;

// Processing order: shadow passes first (the world samples them in the same
// frame), then sky, world, distant weather, near weather, glow and UI.
// bgfx::setViewOrder needs
// a permutation of every remapped id, so ids between are listed too.
// Rendering into the shadow atlas after a view sampled it in the same frame
// breaks on real drivers (a crash in vkCmdBeginRenderPass on RADV), so the
// renderer applies this order with MaxShadowViews every frame.
inline void applyOrder(uint16_t shadowViews) {
    bgfx::ViewId order[256];
    uint16_t n = 0;
    for (uint16_t i = 0; i < shadowViews; ++i)
        order[n++] = bgfx::ViewId(ShadowBase + i);
    for (bgfx::ViewId id : {Sky, World, DistantWeather, Weather, Glow, Ui})
        order[n++] = id;
    for (bgfx::ViewId id = DistantWeather + 1; id < ShadowBase; ++id)
        order[n++] = id;
    bgfx::setViewOrder(0, n, order);
}
}  // namespace RenderViews
