---
name: bgfx-game-rendering
description: 'Implement and debug this game''s bgfx rendering, mesh uploads, GPU resources, draw state, and shader integration using the vendored bgfx API.'
---

# bgfx Game Rendering

## Project Map

Use `external/bgfx.cmake-1.143-9241-532/bgfx/` as the installed API authority. Start with its `README.md`, `include/bgfx/bgfx.h`, and the relevant examples. Online documentation follows newer revisions; check signatures locally before using them.

Rendering lives in `src/render/renderer.*`; asset ownership lives in `resource_manager.*`. Shared meshes, vertex layouts, commands, and uniforms live in `src/engine/render/`. Consult `shaders/AGENTS.md` when changing shader interfaces. The game uses bgfx with Vulkan; SDL supplies the native window.

## Implementation Decisions

- Trace a change from mesh producer through `RenderTarget` to submission. Match shader attributes to `VertexT::init`, index width to buffer flags, and active counts to allocated capacity. Static mesh helpers currently use 16-bit indices; dynamic helpers use `BGFX_BUFFER_INDEX32`.
- Audit uploads before changing worker result ownership. `bgfx::copy` copies immediately; `makeRef` borrows memory. The vendored header requires borrowed data to survive at least two `bgfx::frame` calls without a release callback. Prefer copying short-lived chunk vectors; for zero-copy transfers retain storage until the release callback, which can execute on another thread. Never reference a temporary vector.
- Initialize handles to `BGFX_INVALID_HANDLE`, check creation results, and give each handle one destruction owner. Use the existing scheduled mesh deletion path; prevent queued commands from referencing replaced or destroyed resources. Resource reload must respect borrowed texture CPU buffers too.
- Keep view configuration and frame advancement in the existing renderer flow. For this architecture, workers should produce CPU results and publish them for upload/submission. Introducing parallel submission requires separate encoders, proper encoder completion, and review of the installed threading rules.
- Bind every required uniform and texture for each draw. Review depth, blend, culling, scissor, view order, and transform together. Use sequential view ordering when UI overlap depends on submission order; default sorting is not an ordering guarantee.
- Check capabilities and transient-buffer availability before allocating optional rendering paths. Profile upload volume and draw counts before adding batching complexity.

## Verification & Sources

Build `app`; regenerate changed shaders and rebuild copied assets. Exercise chunk boundaries, repeated mesh replacement, resizing, transparent/UI overlap, and exit with pending work. For ownership changes, use relevant sanitizer builds when available.

Primary sources: [bgfx API](https://bkaradzic.github.io/bgfx/bgfx.html) and [threading/design overview](https://bkaradzic.github.io/bgfx/internals.html). Cross-check memory and submission rules in the installed header.
