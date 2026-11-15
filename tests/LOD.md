# Distant terrain

Gameplay renders generated land and opaque water to 4,096 tiles. Local terrain
still loads within the existing chunk radius. Region registration follows the
camera and releases generation caches as regions unload.

## Representation and ownership

`SurfaceSampler` combines the existing terrain sampler, soil profiles and rock
layers without allocating chunks. Rock selection is now seeded by world seed
and region coordinate: generated geology intentionally changes from the former
request-order-dependent sequence. There is no persisted terrain migration in
this version.

LOD patches have 32 cells per side, with sample spacing from 1 to 64 tiles.
Spacing 1 emits exact voxel tops and exposed side materials. Coarser patches
triangulate the generated height surface and carry a 64 by 64 material map,
independent of geometry density. Shared coarse-edge interpolation stitches
2:1 neighbors. Explicit vertical joins connect stepped and smooth patches.
World edges close down to the floor. Water triangles clip against the terrain
and use the same level as loaded water tiles.

`TerrainLodManager` submits at most two low-priority CPU jobs to the shared
pool. Each job owns its sampler/cache, cancellation flag and result, and reads
world/assets that remain immutable throughout gameplay. The manager cancels
and joins these jobs before its world/assets can be destroyed. Layout revision
checks reject obsolete results. All GPU creation and uploads happen on the
main thread, using copied memory and the renderer's deferred mesh deletion.

A complete old frontier remains drawable until all replacement patches are
uploaded. Startup displays coarse roots as they become available. Loaded
chunks replace LODs only in complete horizontal chunk footprints: all surface
slabs must initially have current uploaded meshes and ready neighbors. After
handover, chunk visibility belongs to a separate coverage tracker. Already
uploaded meshes remain visible through distant LOD changes and queued lighting
remeshes, until their chunks unload or leave the keep radius. New columns enter
within six chunks; existing columns remain through seven chunks to avoid
boundary oscillation. Surface summaries are cached independently of the distant
mesh layout, with at most four new summaries sampled per frame.

A coverage texture masks all LOD resolutions under detailed chunks, including
the owning side of vertical faces. Indexed vertical joins close the boundary
between detailed columns and the active coarse mesh. Joins use the actual LOD
triangulation, and a new frontier cannot commit until its joins fit the frame's
upload budget. Empty uploaded meshes count as completed work.

## Quality and budgets

Constants are in `src/game/world/terrain_lod.hpp`. Defaults are 4 km view
distance, 64 MiB LOD vertex/index/material-map residency and 2 MiB of patch
uploads per frame. The residency budget reserves 2 MiB for detail-to-LOD joins;
their updates share the same per-frame upload budget with patch uploads. The
small global coverage texture is additional (4 KiB).
GPU driver allocation overhead and frames awaiting resource retirement are
not included in this logical residency counter. Detailed voxel chunks retain
their existing separate allocation policy.

Selection starts at a two-pixel projected error target and relaxes it when
estimated patch cost exceeds the budget. It uses measured midpoint error for
previously built patches and a relief estimate for unseen patches; this is an
approximation, not a guaranteed global error bound. Selection changes at
32-tile camera bins to avoid continuous retessellation. The initial exact band
extends approximately 128 tiles around the detail focus; allocation pressure
can shrink it while preserving fallback coverage. Extending the far edge of the
view keeps the existing frontier. Large teleports or actual residency pressure may first replace the previous detailed LOD
frontier with coarse coverage to free transition memory. Loaded chunks retain
their independent visibility throughout that transition.

Terrain shares world-space material mapping, directional/ambient lighting and
haze with loaded blocks. Sky and haze share the horizon color. Artificial
light and local sunlight attenuation remain part of block shading.

Persistent edits, vegetation/structure proxies, cave interiors, overhangs,
water reflections and new river/lake generation are outside this version.
Coverage changes are atomic geometry replacements; there is no temporal
crossfade or geomorph. Large teleports can briefly show coarse terrain while
the destination refines.

## Reproduction

Run from the repository root:

```bash
cmake --build build --target app world_logic_tests lod_benchmark lod_render_smoke --parallel 4
ctest --test-dir build -L gameplay --output-on-failure --no-tests=error
build/tests/lod_benchmark
build/tests/lod_render_smoke build/lod_handover 1600 handover
build/tests/lod_render_smoke build/lod_coast 1600 coast
build/tests/lod_render_smoke build/lod_elevated 1600 elevated
build/tests/lod_render_smoke build/lod_travel 2400 travel
build/tests/lod_render_smoke build/lod_boundary 2000 handover_walk
build/tests/lod_render_smoke build/lod_cancel 1 cancel
```

The render tool is opt-in and requires Vulkan with texture readback. It opens
no window or audio device and uses production generation, meshes, LOD manager,
material assets and compiled shaders. It writes PPM captures under the given
prefix and prints residency, upload, draw and completion counters. Travel
captures three destinations, including a 512-tile move and a longer teleport.
The handover scenario streams and lights local chunks and draws their uploaded
meshes. The boundary scenario walks across a region boundary and back, forces
mesh revisions, and checks every frame that drawable columns remain visible;
it also captures a coarse-frontier transition. Cancel exits immediately with
LOD jobs outstanding. Rendering submits
the production command payloads through a minimal capture renderer; interactive
SDL/window/menu behavior still needs an application run.

The fixed synthetic landscape uses height seed 1234 and feature seed 5678, with
hills, a mountain ridge, cliff features and sea-level coasts. Its biome is
temperate, humidity 0.5 and temperature 15 C. The CPU benchmark uses synthetic
materials; the Vulkan tool uses runtime materials and soil definitions.

For interactive checks, run `./build/src/app/app`, create a world and enter
terrain near a coast or mountain. Use WASD/Shift to travel, right drag to
rotate, wheel to zoom and Ctrl+wheel to change height. Check both low and
raised views, travel beyond the starting regions, reverse direction during
loading and close during generation. The tile debug panel reports LOD patch,
job, backlog and residency counters when no tile is selected.
