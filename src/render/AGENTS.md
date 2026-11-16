# Rendering Guidelines

See the [repository guidelines](../../AGENTS.md) for shared conventions.

## Responsibilities & Structure

`renderer.hpp/.cpp` implements `IRenderer`, submits bgfx drawing commands, manages clipping and deferred mesh deletion, and performs text layout. `render_cache.hpp` defines cache support. `resource_manager.hpp/.cpp` implements `IAssets`, loading textures, shaders, fonts, sound data, music paths, and material texture arrays. `Renderer` owns the resource manager and audio system.

Shared command and vertex definitions live in `../engine/render/`. Shader sources live in `../../shaders/`, compiled binaries in `../../assets/shaders/`, and resource mappings in `../../assets/definitions.json`.

## Making Changes

Keep render-command producers and consumers consistent when changing command layouts. Match shader inputs, vertex layouts, uniforms, and material array indices. Preserve bgfx handle ownership and deferred deletion so pending frames do not reference freed meshes. Review cache invalidation for text, clipping, font, and scale changes. Audio buffers supplied by the resource manager must outlive playback.

The world view (view 0) is camera-relative: `Renderer` places the eye at the origin, and every `DrawLayer::GAME` draw passes `relativeTransform(camera, meshOrigin)` (`engine/render/renderer_api.hpp`) as its transform. Chunk and structure meshes are chunk-local, so their origin is the chunk origin; LOD patches and joins hold world positions and use origin (0,0,0). Never pass an identity transform to a world draw, and keep frustum culling in world space. Float world coordinates near x = 60000 are exact only to ~0.004 tiles, which opens visible cracks between greedy-meshed quads.

## Build & Validation

From the repository root, build `app`. For shader or asset mapping changes, run `python3 generate_assets.py` after verifying its `shaderc` path, then rebuild to copy resources. Inspect world geometry, material textures, clipped widgets, text wrapping, caret/selection placement, and Russian/Japanese fonts as relevant. Capture screenshots for visible changes.
