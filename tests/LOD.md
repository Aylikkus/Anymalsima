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
triangulate the generated height surface and carry a material map of the
preset's resolution (32 texels per side on Low, 64 on Medium and High). A patch mesh depends only on its own square
`(x, z, spacing)`: smooth patches hang a skirt below every inner edge (depth
`2 * error + spacing`, 4 to 256 tiles) and stepped patches extend boundary
side walls `SteppedSkirt` tiles below the lower column, so cracks against a
neighbour of any resolution are hidden and a patch never rebuilds when a
neighbour changes. Selection still keeps 2:1 balance to bound those cracks.
World edges close down to the floor. Water triangles clip against the terrain
and use the same level as loaded water tiles.

Inner smooth skirts use their patch's top material and upward shading; their
outward face normals still determine ownership for the coverage mask.
Stepped overlap walls use the column's top material for the upper two tiles
and the original strata underneath. Set `ANYMALSIMA_LOD_DEBUG_EDGES=1` for
magenta smooth skirts, cyan stepped boundary walls and orange detail joins.

`TerrainLodManager` runs up to `clamp(threads / 2, 2, 6)` CPU jobs on the
shared pool: root patches at normal priority, the rest low. Each job owns its
sampler, cancellation flag and result; region inputs (terrain neighbourhoods,
rock layers) come from a mutex-guarded `SurfaceInputCache` shared by all jobs.
Jobs read world/assets that remain immutable throughout gameplay. The manager
cancels and joins these jobs before its world/assets can be destroyed. A
layout change cancels only jobs whose patch is no longer wanted. All GPU creation and uploads happen on the
main thread, using copied memory and the renderer's deferred mesh deletion.

Patches are independent cache entries. Each frame `TerrainLod::Cover`
(`lod_cover.hpp`) draws, for every in-view part of each root, the finest
available patch: a split patch is replaced only once all of its in-view
children can be covered, and finer leftovers stay while a coarser desired
patch loads. Root patches are always requested first and kept, so startup
shows coarse roots as they arrive and no layout change waits for, or
discards, unrelated patches. When the GPU budget is full, cached patches that
are not wanted and were not drawn in the previous frame are evicted, least
recently drawn first; if that is not enough the pixel-error target is raised
by 1.5x (up to 16x) and relaxed again once residency falls below half. Loaded
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
triangulation of the patches drawn this frame; only drawn patches inside the
detail window trigger a rebuild. Empty uploaded meshes count as completed
work.

## Quality and budgets

Presets are `TerrainLod::Qualities` in `src/game/world/terrain_lod.hpp`,
selected by the `terrainDetail` setting (main menu, applied on world load):

| Preset | GPU budget | Upload/frame | View distance | Exact radius | Pixel error | Material map |
|--------|-----------:|-------------:|--------------:|-------------:|------------:|-------------:|
| Low    | 64 MiB     | 2 MiB        | 4096          | 128          | 2.0         | 32           |
| Medium | 128 MiB    | 3 MiB        | 6144          | 192          | 1.4         | 64           |
| High   | 256 MiB    | 4 MiB        | 8192          | 256          | 1.0         | 64           |

The view distance also drives fog and the far plane. The residency budget
reserves 2 MiB for detail-to-LOD joins; joins are rebuilt in the frame their
patches change. The
small global coverage texture is additional (4 KiB).
GPU driver allocation overhead and frames awaiting resource retirement are
not included in this logical residency counter. Detailed voxel chunks retain
their existing separate allocation policy.

Selection starts at a two-pixel projected error target and relaxes it when
estimated patch cost exceeds the budget. It uses measured midpoint error for
previously built patches and a relief estimate for unseen patches; this is an
approximation, not a guaranteed global error bound. Selection changes at
32-tile camera bins to avoid continuous retessellation, and is refined once
when newly measured errors arrive and the layout is complete. The exact band
extends the preset's radius around the detail focus. Loaded chunks retain
their independent visibility throughout every LOD change.

Terrain shares world-space material mapping, directional/ambient lighting and
haze with loaded blocks. Sky and haze share the horizon color. Terrain and
proxies without voxel light volumes estimate ocean skylight from depth:
full at the surface, approximately 7/15 one tile down and zero two tiles
down, apart from the usual minimum light. Artificial light is unchanged.
When the camera eye enters water, nearby geometry remains visible and the
world fades into water-colored fog by 48 tiles. The camera uses loaded water
tiles when available and the LOD column otherwise.

Gameplay edits are shown at distance (`TerrainEdits`, see
`src/game/AGENTS.md`). Ground overrides (digging, removed mountains, filled
pits) enter `SurfaceSampler`, so every patch kind follows them; pit walls keep
the generated strata, and a block replaced at the original height is not
shown. Affected cached patches are marked stale and stay drawn until their
rebuilt mesh replaces them. Structures (solid tiles above the generated
surface, including overhangs) are voxel proxies, one greedy mesh per 16^3
slab (`StructureLod`), fully sky-lit and capped at 1/8 of the preset's GPU
budget. Their border faces read the neighbouring slabs, so structures may
span chunks and regions without seam faces. A proxy slab is hidden exactly
when `GameplayState` draws its real chunk (detailed column, uploaded mesh,
inside `DETAIL_CHUNK_RADIUS`). Vegetation, cave interiors, coarse structure
meshes for very distant towns, water reflections and new river/lake
generation are outside this version.
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
build/tests/lod_render_smoke build/lod_wrap 1500 wrap
build/tests/lod_render_smoke build/lod_cancel 1 cancel
build/tests/lod_render_smoke build/lod_structure 3000 structure
build/tests/lod_render_smoke build/lod_seams 4000 seams
```

The render tool is opt-in and requires Vulkan with texture readback. It opens
no window or audio device and uses production generation, meshes, LOD manager,
material assets and compiled shaders. It writes PPM captures under the given
prefix and prints residency, upload, draw and completion counters. Travel
captures three destinations, including a 512-tile move and a longer teleport.
The handover scenario streams and lights local chunks and draws their uploaded
meshes. The boundary scenario walks across a region boundary and back, forces
mesh revisions, and checks every frame that drawable columns remain visible;
it also captures a coarse-to-fine transition. The structure scenario digs a
pit and builds a pillar with an overhanging platform through `WorldEditor`,
then moves 400 tiles away (its regions unload) and captures them from there.
The seams scenario uses a 480-region landscape (x near 60000), builds a
patterned wall full of greedy-mesh T-junctions in front of a magenta
background and sweeps 12 oblique views; any magenta pixel inside the wall is a
crack (`seam_pixels_total` must be 0). The lights scenario captures a day
view; places six `blocks.test_light` lamps and a "town" of 40 small entity
lights 150 tiles ahead and dims the sky (block light, far light, distant glow,
glare); views them from 350 tiles away and high above; then, back home, builds
a hollow stone box and puts an 8 km entity light inside, in an opaque block
like a lamp. With its shadow map the capture must match the box without the
light (`boxed_light_changed_pixels` at most 200), and the same light with the
walls removed must change at least 150000 pixels (the control; a light
shadowed by its own block changes about 59000). `SINGLE_LAMP=1` places one
test lamp instead of six, so it is always within the shadow budget;
`LAMP_HEIGHT=n` raises it n tiles; `LAMP_BOX=1` walls it in on five faces
(open toward -z), `LAMP_BOX=2` on all six; `SMOKE_QUALITY=0..2` picks the
shadow budget preset. With `SHADOW_DUMP=1` and `LAMP_BOX` the lamp's cube
faces are dumped too, plus face 0 at full resolution (`<prefix>_face0_<phase>.pgm`,
white = no caster within 5% of the radius). `NO_GLARE=1` renders without glare and
`SHADOW_DUMP=1` writes the boxed light's six cube faces to `<prefix>_shadow.pgm`. llvmpipe did not show cracks even
before the camera-relative fix, so in-game checks on real GPUs still matter. Cancel exits immediately with
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
