# Tile shapes, terrain ramps, and placement

**Current step: none — step 7 is complete; interactive in-game save travel
remains a manual check (recorded below).**
This handoff covers the terrain foundation for weather. Snowfall,
melting, temperature simulation, and NPC building generation have their own
future work.

## Progress

| Step | Deliverable | Depends on | Status |
| --- | --- | --- | --- |
| 1 | Shape geometry, optional chunk storage, snapshot safety | — | Complete |
| 2 | Durable edits, save compatibility, generated-terrain comparisons | 1 | Complete |
| 3 | Near mesh, material shading, grass and plant support | 1–2 | Complete |
| 4 | Accurate picking, debug placement and grounded traversal | 1–3 | Complete |
| 5 | Directional light transport and shadows | 1–3 | Complete |
| 6 | Natural ramps, distant terrain and structure handoff | 1–5 | Complete |
| 7 | End-to-end validation, performance and documentation | 1–6 | Complete (automated + device; manual save travel listed) |

Step 1 adds the shared geometry in `src/game/world/tile_shape.*` and the
paired tile/shape API in `tile.hpp`, `chunk.hpp`, and `chunk_manager.*`.
`LocationGenerator` clears old shapes once when reusing a slab. Mesh jobs
capture center and border chunks under the tile lock. Region snapshots copy
the tile arrays during eviction and autosave. Those copies share immutable
shape tables until an edit. Cached column inputs describe cube-only generated
terrain.

The Release app and logic test targets built successfully. The headless suite
passed all 362 CTest cases, including the new `tile_shape_*` cases in
`tests/unit/game/world/tile_shape_tests.cpp` and the expanded
`generation_reused_chunk` case. The six focused cases also passed under
ASan and UBSan with leak detection disabled because the sandbox prevents
LeakSanitizer from running. The new cases exercise the profile codes, geometry,
seams, ray intersections, storage reclamation, and snapshot isolation. The
first in-game device check belongs to the mesh and placement steps.

Step 2 wires shapes through `WorldEditor`, chunk snapshots, root substrate
provenance, and the generated terrain and structure comparisons in
`ChunkManager` and `TerrainEditTracker`. CHNK v6 adds a bounded sparse shape
section and a substrate shape byte per root. EDIT v4 stores ground shapes and
uses CHNK for positive structure cells. VOXL v2 stores per-voxel override
shapes. STRB saves those same shapes in the previous structure baseline.
Existing payloads imply cubes.

Shape changes use the existing save dirtiness, topology revision, border
remesh, and lighting notifications. The enclosed-slab shortcuts require
cube coverage. A partial solid occupies the whole fluid cell in ledgers
saved before weather step 17; newer worlds give water the geometry's free
volume and side openings (see [water](water.md)). Flora and tree support
checks currently require full cube support. Device validation begins with
shape meshing in step 3.

The Release app and logic-test targets built. All 366 CTest cases passed.
The 20 focused ASan/UBSan cases passed with leak detection disabled. They
covered uniform and malformed CHNK payloads, old root data, cross-chunk bulk
edits and region eviction, ground and structure index reconstruction,
generated baseline restoration, stale border mesh jobs, and a shaped river
barrier reloaded from a saved slab.

Step 3 clips shared faces in `chunk_mesher.*` against the neighbour's
boundary polygon, using the shape from the same snapshot as its tile.
Uncut cube squares still use greedy meshing. Explicit faces use the
profile's geometry and normals, including both junction facets. The block
shader uses the owning tile for cover density and measures side fringe down
from the rim. It samples light in the outward adjacent voxel with the
existing light volume. Partial-solid light transport is step 5.

`flora_generator.*`, `flora_simulation.cpp`, `flora_renderer.cpp` and
`world_editor.cpp` share the small-plant anchor rules. Small plants can use
slabs and suitable ramp pitches. Narrow wall tops must fit the plant's
ground radius across the strip. Tile-footprint plants and trees use
full-height landings. Shape edits recheck dependent plants, and reloaded
flora use the same support rules.

The Release app built and all 370 CTest cases passed. Seven focused cases
passed under ASan and UBSan with leak detection disabled. The cases cover
every shape rotation, winding and facet normals, horizontal and vertical
chunk faces, crossing and matching slopes, wall overlap, water culling,
cover ownership and plant support. The Vulkan `shapes` smoke scene reached
a settled frame with the grassy and bare ramps meeting across the chunk
face. Run `build/tests/lod_render_smoke /tmp/shapes 1200 shapes` to capture
the scene. The Vulkan `river` smoke scene also passed all four phases,
including the wrapped crossing and underwater view.

Step 4 uses `tile_raycast.hpp` for grid traversal and intersects the shape
inside each visited cell. Hits carry the surface normal and a cardinal
placement offset. Grid ties visit Z, Y, then X. Geometry breaks ties between
facets in canonical profile order. Tile coordinates wrap on X and the hit
position stays in the ray's world copy.

Use **Place Piece** to pick a shape and adjust its rotation, wall
thickness, or ramp form, run and segment. The debug panel shows the hovered
piece and the placement result. See [Playcheck A](weather.md#playcheck-a)
for material, height and same-tile controls. Use **Walk mode** with the orbit target
above loaded ground. Move with WASD, hold Shift to run, and drag with the
right mouse button to look around. **Orbit camera** returns to the free view.

`grounded_probe.*` queries the maximum top height under a 0.5 m square
footprint using `tile_shape.*`. The 1.7 m tall probe follows slopes in
horizontal substeps of at most 4 cm, falls when support ends, and checks
head clearance and lateral overlap against the same geometry. The orbit
camera remains the saved camera mode. Walk mode uses its own near plane
for close surfaces. The renderer uses glyph fallback per character so
Japanese labels can display their numbers and cardinal directions.

The Release app and all 377 CTest cases passed. Eleven focused cases passed
under ASan and UBSan with leak detection disabled. The seven new cases in
`tests/unit/game/world/tile_interaction_tests.cpp` exercise the open strip
of a thin wall, top and bottom placement, pick distance and ties, both
junction facets, all straight runs and facings, both corner approaches,
head clearance, missing support, and both ordinary and wrapped chunk seams.

A Vulkan gameplay check on a copy of a save cycled the controls, shortened a
run from three pieces to one, placed and picked a rotated inner corner,
removed it, moved in Walk mode, and returned to orbit. The controls passed
at 1280×720 and 80% text scale, then at 1000×600 and 120% text scale in
English, Russian and Japanese. The ramp and corner traversal matrix ran
headlessly.

Step 5 adds shared face and overhead transmission in `voxel_light.*`.
The sunlight and block-light propagators capture optional shape data with
material opacity. Shape edits invalidate the relevant workers and emitter
faces across chunk borders. Light publication still follows the light-volume
update path. `sky_column.*`, `sky_height_map.*`, and `save/sky_codec.cpp`
carry profiles through unloaded estimates and saved overrides. SKYO v2
adds profiles to the opaque cutoff and each span, with cube defaults for v1.
The coarse distant occlusion texture uses the fully blocked cutoff. Partial
attenuation is available in the heightmap's per-tile estimates and the swept
voxel light.

`chunk_mesher.cpp`, `light_volume.cpp`, and `shaders/block/block.vert`
keep light for the open part of a shaped cell. Inset faces sample that
cell's texel. Boundary faces sample the outward adjacent cell. The existing
`LightShadows` render pass uses the shaped chunk and structure proxy meshes.
Chunk mesh uploads invalidate affected shadow faces, and proxy uploads
advance the distant caster revision.

The Release app built and all 380 CTest cases passed. Nine focused cases
passed under ASan and UBSan with leak detection disabled. They cover shared
face overlap, translucent walls, slab roofs, exposed ramps, sky estimates
before and after the first sweep, saved overrides, and stale results after
edits and unloads. The Vulkan `shape_lighting` smoke scene measured shadow
depths of 32, 1.5, 1.5, and 32 metres for the empty cell, cube, flush wall,
and quarter-turned wall at the same chunk edge. The night capture showed
light on both grassy junction facets and across their diagonal. The Vulkan
`lights` regression scene passed all nine phases.

Step 6 adds `natural_shape.*`, a bounded pure rule over the cube-only height
field. A high cell becomes a ramp when its only lower cardinal neighbour is a
one-metre step and the opposite cell keeps the old top as a landing; two
adjacent lows form an outer corner when both perpendicular approaches are
straight ramps, and one low diagonal between two approaches forms an inner
corner. Candidate and landing checks keep a second cut from removing the first
ramp's high edge. Cliffs, hydrology-carved banks, wet ground and tree/plant
footprints that need level support stay uncut. `terrain_sampler.*` records the
profile in `ColumnGenerationInput::groundShape` and in `SurfaceSampler`, shared
by chunk generation, the edit baseline, flora and LOD. `LocationGenerator`,
`ChunkManager::isEdited`, `TerrainEditTracker`, `flora_generator.*` and
`tree_generator.*` consume the same value.

`WorldMeta::NaturalShapeVersion` (generator 25) gates new worlds through
`World::generatorVersion`, `save_catalog::readWorldMap` and
`WorldGenerator::generate`. Saved worlds below that version keep cube-only
terrain, including unseen chunks; codec versions are independent.

`terrain_lod.*` draws spacing-one shaped tops from the actual profile,
including both junction facet normals and the corner diagonal, clips boundary
sides against the neighbour's real profile, and carries the shaped rim into
`meshHeight` and detail joins. `structure_lod.*` supplies the ground profile
when a proxy wall rests on a partially occupied substrate cell; edited walls,
slabs and rotated ramps already arrive through CHNK/EDIT and the structure
voxel baselines.

The Release logic target and app built. All 389 CTest cases passed. New cases
cover straight ramps, both corner forms and rotations, landing conflicts, the X
seam, generation wiring, generator-version gating and save reload,
generated-shape edit baselines, and stepped LOD shape height/normals. The
Vulkan `shapes` smoke scene passed as a regression. The natural-terrain device
check (natural ramp, both junction forms, near/LOD transition, shoreline and
structure foundation) is recorded as remaining for step 7.

Step 7 validates and profiles the rollout. Generator 26
(`WorldMeta::NaturalShapeRidgeVersion`) replaces the step-6 requirement that
both ends of a ramp stay whole cubes with a world-space comparison of the
shared edge. Ramps may now meet another ramp at a ridge, meet a higher cell as
a wall at their high edge, and stand on a two-cell plateau. Generator 25 worlds
keep the older `Cube`-landing rule through
`TerrainNeighborhood::ridgeShapes()`; generator 26 is the current
`WorldMeta::GeneratorVersion`. `natural_shape::Field` has an explicit
compatibility switch so both rules stay testable and a saved 25 world never
changes under an unseen chunk.

The natural-shape query path was the measurable regression. Step 6 rebuilt a
`natural_shape::Field` per `TerrainSampler::surfaceShape` call, re-sampled
noise for every rule query, and created a fresh `TerrainSampler` per LOD
column, so a new world spent 340.9 ms of column evaluation for 64 chunks and
12.51 s building the 244-patch LOD benchmark. `TerrainSampler` now memoizes the
column heights used by the pure rule (`shapeHeights`), shares them with
`sample`, `tileSurface` and `surfaceShape`, and keeps one `Field` per sampler.
`SurfaceSampler` keeps one sampler per region (`samplerFor`), and
`ChunkMesher` copies the sparse table into a dense 16³ lookup per build so the
greedy sweep does not binary-search it for every visited cell. The same
workloads now take 30.9–37.3 ms for 64 chunks and 4.07–4.14 s for the LOD
benchmark, against 22.6 ms and 3.21 s cube-only. At the step-7 baseline the
near mesh was 16.9 ms per 64 surface chunks (12.0 ms cube-only); the 2,696
shapes occupied 11,973 resident sparse-table bytes including capacity and
encoded to 14,799 bytes (8,460 cube-only). The diagonal follow-up below raises
those counts; the cache gains are unchanged. The dense worst case (4,096
shaped cells in one chunk) needs 12,336 table bytes, 12,302 encoded bytes and
10.3 ms to mesh, so the sparse table still wins and no dense representation
was added. The LOD scene's triangles and resident bytes are slightly lower
with shapes (624,362 versus 652,124 triangles; 54.02 versus 54.70 MiB).
Profiling tooling is `tests/benchmarks/shape_benchmark.cpp` (`--legacy`,
`--stats` and `--map`); the full numbers and reproduction commands are in
`tests/README.md`.

Tests: the Release app and `world_logic_tests` built. All 396 registered CTest
cases passed. The focused natural-shape, meshing, codec and revision cases
(35) also passed under ASan and UBSan in a clang
`-DANYMALSIMA_TEST_SANITIZERS=ON` build with leak detection disabled because
the sandbox blocks LeakSanitizer. New cases cover the legacy cube-landing
rule, ridge pairs, a ramp against a higher wall, query-order-independent
caches, and a generated natural chunk surviving CHNK encode/decode with all
4,096 cells compared.

Device captures used `lod_render_smoke` (Vulkan, 1280×720, default Low terrain
detail) with the shared `lodScene` seed 1234/5678. The `shapes` scene settled
(2 edited columns, no failed meshes); `shape_lighting` reported shadow depths
32, 1.5, 1.5 and 32 m for the empty, cube, flush wall and quarter-turned wall
phases. The new `natural` scene enables generator 26, searches region 45,45,
and picks the densest dry window at (5793, 5876): 2,522 natural shape cells and
23 corners in the region, centre height 409, peak 416, first corner at
(5766, 5762). It streamed chunks, built near and LOD terrain and settled with
no backlog or failed meshes. Natural junctions are sparse in this smooth scene;
both junction forms remain covered by the manual `shapes` and `shape_lighting`
scenes.

Automated persistence now includes a generated natural chunk through CHNK and
the existing region/eviction shape tests. The remaining manual check is the
step-7 device walkthrough on a disposable save: place each variant, rotate a
boundary piece, travel three regions away, return, exit and reload (including
an older save copy), and stress edits while meshing, saving and relighting.
That was not run interactively here; the headless codec, region persistence,
stale-job and provider tests are the current evidence. The known visual
limitation is that a diagonal terrace corner with two low cardinal neighbours
and a low diagonal has no continuous single-profile top; step 7 left it a
whole block, and the follow-up below adds a diagonal-ramp approximation.

## Follow-up: diagonal corner ramps

Generator 27 (`WorldMeta::NaturalShapeDiagonalVersion`) adds profile 24, a
single-plane `DiagonalRamp` with top `(x + z) / 2` and rotations for the four
diagonal orientations. It is the shape used for a high cell with two
perpendicular one-metre-low cardinal neighbours and a taller hillside behind
them, where a level straight ramp or an outer corner would need a
discontinuity inside the cell. The old `OuterCorner` is still chosen first
when its two approaches and all four shared edges agree; the diagonal ramp is
the fallback, not a replacement for valid junctions. Generator 26 worlds keep
their block corners and generator 25 keeps its original rule.

The rule change is in `natural_shape.*` (`Kind::Diagonal`,
`diagonalCorners`), the profile and geometry in `tile_shape.*`, and the
light/coverage table bound in `voxel_light.cpp`. `gameplay_state` describes a
hovered diagonal ramp as a corner ramp; its catalog string is in
`translations/game.pot` and the PO catalogs. No chunk codec version change is
needed because profile bytes were already versioned by value; an older build
will reject a save that contains profile 24, so do not downgrade a world after
using generator 27.

The first device check exposed sky through the close stepped LOD over the new
ramps. The stepped cube path emits no wall for a same-level cube neighbour
(it relies on the shaped cell covering the shared face), and `shapedColumn`
only drew the shaped cell's own side down to its top, so the part of the
cube's face above a sloped top was open. `shapedColumn` now draws that exposed
cube-side ribbon with the neighbour's material when the neighbour is an
unshaped column at the same ground height. `lod_shape_cube_gap_closed` builds
a flat chunk with one diagonal ramp and asserts the vertical face exists; it
fails without the fix and passes with it.

In the shared scene (region 16,16, seed 1234/5678) generator 27 raises natural
coverage from 2,696 to 4,181 shaped surface cells (16.5% to 25.5%): 2,657
straight ramps, 1,485 diagonal ramps, 20 outer corners and 19 inner corners.
The 64-chunk near mesh is 17,346 triangles (18.7 ms), 16,254 resident shape
bytes and 18,096 encoded bytes. LOD is about 4.2 s for the 244-patch scene,
still about 1.25x the cube-only 3.3 s. All 396 registered CTest cases pass;
the 35 focused shape/mesh/codec/revision/LOD cases pass under ASan+UBSan with
leak detection disabled. The `natural` smoke capture at region 45,45 now
reports 3,392 shaped cells, 23 junctions and 871 diagonal ramps at window
(5794, 5855) and settles without backlog.

The remaining visual trade-off is explicit: a two-axis step cannot match all
four neighbours with the current one-metre profiles. The cube reads as a
protruding block; the diagonal ramp reads as a shallow diagonal notch. The
ramp is the smoother silhouette and removes the vertical downhill faces, but
a true smooth diagonal would require a denser height field or profiles taller
than one cell. Treating the diagonal-notch cells as inner corners was tried
and produced deep square pits, so those cells still stay whole blocks.

When picking this up, update the current step and its row. Before marking a row
done, record the affected paths, tests run and outcome in the row or immediately
below it. Record any remaining device check, blocking dependency, or changed
decision explicitly. Use the acceptance checks in each step to resume midway.
Natural ramps stay gated by the saved generator version; never enable the
edge-matching (26) rule for a world that recorded 25 or older. The only
outstanding step-7 item is the manual in-game save/travel/reload walkthrough
listed above; extend it with screenshots when it is performed.

## 1. Define geometry once and keep ordinary chunks cheap

Start in `src/game/world/tile.hpp` and `chunk.hpp`. A chunk stores 16³ six-byte
tiles by value in `tiles[z][y][x]`. Y is up and each cell is one metre wide.
Keep the existing size assertion and material IDs. Add a game-world shape
type and geometry queries independent of bgfx, Jolt, and live chunks.

Use an unsigned byte with profile in bits 2–7 and facing in bits 0–1. Proposed
stable assignments below. Facing 0, 1, 2, 3 denotes +Z, +X, −Z, −X respectively.
For straight ramps this points uphill. For junctions it rotates the canonical
corner described below. For walls it points toward the occupied edge.
Define the canonical +Z shape once, then rotate about the cell centre.

| Profile ID | Geometry in cell-local coordinates |
| --- | --- |
| 0 | Full cube. Default code 0. |
| 1 | Lower half slab, height 0.5 m across the footprint. |
| 2 | Ramp from 0 to 1 m. Canonical top height is local Z. |
| 3, 4, 5 | Low, middle, high thirds of a 3 m ramp. For segment k = 0, 1, 2, top height is (k + local Z) / 3. Each piece fills down to its own cell floor. |
| 6, 7, 8, 9 | Full-height wall with thickness 0.10, 0.25, 0.50, 0.75 m respectively. Canonical footprint spans all X and Z from 1 − thickness to 1. |
| 10, 11 | Low and high halves of a 2 m ramp. For k = 0, 1, top height is (k + local Z) / 2. |
| 12–17 | Outer corner junction. In order, the six height intervals are the full rise, low/high halves, and low/middle/high thirds. |
| 18–23 | Inner corner junction, in the same interval order. |
| 24 | Diagonal corner ramp: a single plane whose top height is (x + z) / 2 in canonical coordinates, high at the (+X,+Z) corner. Rotations cover the four diagonal orientations. |
| 25–63 | Reserved for later profiles. |

**Ramp runs.** Each straight flight rises 1 m over a horizontal run of 1, 2,
or 3 m. Each piece is one cell wide across the slope. For run length L and
segment k (starting at zero), the piece spans heights a = k/L to b = (k+1)/L
above its cell floor. Its top is a + (b−a)z for canonical local Z coordinate z.

**Directional junctions.** Two perpendicular straight pieces can meet a
one-cell corner piece across their sloping side edges. For local x,z in [0,1],
use the same interval [a,b] as the adjoining pieces.

| Canonical junction | Top height | Edges that accept straight slopes |
| --- | --- | --- |
| Outer corner, one high corner at (+X,+Z) | a + (b−a) min(x,z) | +X edge rises along +Z. +Z edge rises along +X. The other two edges are level at a. |
| Inner corner, one low corner at (−X,−Z) | a + (b−a) max(x,z) | −X edge rises along +Z. −Z edge rises along +X. The other two edges are level at b. |

For example, place a full-rise outer corner, then put a +Z ramp on its
+X side and a +X ramp on its +Z side. Both shared
edges rise from 0 to 1 at exactly the same rate as the corner. A walker can
turn from one ramp onto the other across the junction diagonal. Rotation
covers all four corner orientations. Each top consists of two planar
triangles meeting on x=z in canonical coordinates. Use the same diagonal
for drawing, collision, picking, shadows and the nearest LOD.

Check connections by matching **both world-space endpoint heights** on the
shared edge in the same endpoint order. All profiles above have linear edges,
so this guarantees a continuous crossing along the entire edge. Different
pitches can also meet at a level landing where their ends reach the same
elevation. Use a flat landing for T-shaped or four-way approaches. Store each
junction's own profile and facing so editing or unloading a neighboring
piece cannot silently reshape it.

Canonicalize rotationally symmetric profiles to facing 0. Keep the facing
convention available to future directional materials. Use integer profile
parameters or shared rational endpoints so independent halves, thirds, and
junctions meet. Each cell owns one material and one shape. A long ramp uses
two or three independently editable cells at the same base Y. Wall corners use
adjacent cells or a cube corner post. Two intersecting wall pieces in the same
cell would require a separate corner profile. A snow slab can eventually sit
on a flat tile. Snow draped over a ramp will need a surface-relative layer with
its own material and depth. Keep its future ownership separate from grass density.

Provide queries for occupied volume, optional support height at local X/Z,
outward planes/normals, boundary-face coverage, and ray/convex intersection.
Treat the two triangles of a junction as the exposed top of one solid. For
convex-only queries, split at the diagonal into two filled triangular wedges
and discard hits on their internal face. A point outside the wall footprint
has no support there. Keep `Tile::isSolid()` as a coarse whole-cell occupancy test. Callers needing full face coverage,
support, or transmission must query geometry explicitly.

Use an optional per-chunk table keyed by `Chunk::cell`. Start with sorted
16-bit cell indices and parallel one-byte codes behind a single nullable owner.
Insertion, replacement and erase go through the chunk API. Absence means the
block's default cube geometry, and air/liquid-only cells have no entry. Drop
the table on the last erase. This costs three bytes of element payload per
shaped cell, plus container capacity and one owner per chunk. The one-byte
budget applies to the shape code. Measure sparse and heavily built chunks
before adding a dense representation. Give the no-shapes case a fast path
in meshing and generation.

Publish tile and shape together. Audit copied chunks, border reads, region
snapshots and cached worker inputs in `chunk_manager.*`. A chunk copy must
remain stable after an edit. Use immutable shared tables with copy-on-write
under the same lock as the tile, or an owning deep copy. Include shapes in
the existing dirty/revision checks for pending work. Fetch a cell's tile and
shape from the same revision.

**Accept when** every profile/facing stays inside its cell, all four rotations
round-trip, and halves and thirds match at their seams. Test both corner
profiles in all six height intervals and four rotations against two straight
neighbor edges. Check endpoint order, midpoint height, both triangle interiors,
and the diagonal. Test a missed wall
footprint, empty-table reclamation, and editing a chunk while a previous
snapshot is retained. Verify a cube-only chunk allocates no shape table.

## 2. Make edits durable before enabling placement

- Extend `WorldEditor`, `EditRequest` and `BlockChange` in `world_editor.*`.
  Existing callers that place a material get a cube. Carry before/after shapes
  through the same locked operation, including shape-only replacement of an
  unchanged material. Removing a block clears its entry. Identical repeated
  writes use the existing unchanged path. Preserve batch ordering, root
  ownership, cover bookkeeping and plant placement checks. Record shape in
  `RootVoxel` when growth displaces substrate and restore it with the soil.
- Trigger save dirtiness, neighbor remesh, light invalidation, terrain edit
  tracking, shadow updates and dependent support checks on a shape change.
  Audit bulk generation, root restoration, structure stamping, and direct
  block-ID writes for stale entries. In particular, gate fully enclosed chunk
  mesh shortcuts on full coverage. A slab of occupied wall cells can contain
  a visible passage.
- Keep the existing whole-cell liquid displacement and local-water placement
  restrictions for this release. A partial solid owns the fluid cell as a
  whole. Audit reservoir projection, saved local water, river barriers and
  visible water faces against that rule. Test an edit beside water and reload
  it. Sub-cell fluid storage would require separate volume accounting.
- Version `save/chunk_codec.*` (v5 at the time of writing). Keep the existing
  palette/index compression and add a length-delimited optional shape section
  and versioned root substrate shape data. Define how a uniform material
  palette can still have shape exceptions. Encode sorted unique indices and
  nondefault codes only. Check length, count, index range, ordering, occupied
  cell, and legal profile/facing before publishing a decoded chunk. Old
  payloads imply cubes, including displaced substrate. Keep lighting derived.
- Extend `terrain_edits.*`, `terrain_edit_tracker.*`, and
  `save/terrain_edits_codec.*` for the ground surface shape and above-ground
  edited structure shapes. Trace positive/suppressed structure cells through
  `structure_voxels.*` and any persisted structure override that can carry
  them. A change of facing alone must survive unloading the source chunk and
  be visible to distant readers. Compare material **and** canonical shape
  against the actual generation baseline in `ChunkManager::isEdited` and the
  tracker. Restoring generated geometry should remove the redundant override.
  Keep the previous ground shape when rebuilding from partially loaded slabs.

**Accept when** a shape-only edit and an equivalent bulk edit survive region
eviction, save/reload and index reconstruction. Cover a uniform-material chunk
with one ramp, rotated junctions in half and third intervals, a two-piece
ramp across a chunk boundary, last-entry removal, a root displacing a ramp,
old cube-only save fixtures, malformed tails and an interrupted/stale mesh
job. Extend the existing save codec, region persistence, terrain edit and
world editor tests.

## 3. Mesh the occupied surface and attach cover to it

Keep the full-cube greedy path in `chunk_mesher.*`. Give it a neighbor query
that includes shape data across every chunk face. Where two cells meet, emit
the portion of the current boundary polygon left uncovered by its neighbor,
using the existing material visibility rules. Skip an empty remainder. Handle
the cube side of a cube–ramp pair too. Reuse a greedy face only when the
visible polygon is the entire square and its merge attributes match. Emit
inset wall faces, slab tops and ramp tops from the local shape. Triangulate
with consistent winding and omit zero-area triangles at the low ramp edge.
Use local coordinates and the existing camera-relative draw transform. Give
each junction top triangle its own plane normal. Cull the internal diagonal
face and apply grass continuously across the crease. A matching ramp edge
has no exposed side face.

Trace `BlockVertex` through `shaders/block/block.vert`, `block.frag` and the
shared surface shader. Pass the actual geometric normal. Today light sampling
offsets half a cell along the face normal, and cover sampling steps back from
that point. These assumptions fail on inset faces. Supply or reconstruct a
stable owning-cell coordinate, a lit-side sample, and the distance below the
profile top on exposed sides. Preserve interpolation across greedy cube faces
and the existing water vertex attribute meanings. Check other block-shader
users with dummy volumes. Keep triplanar texture scale, cover tint, and
density-volume updates working for cubes and shaped pieces alike.

Existing ground cover is painted on the substrate. Apply its top treatment
over a ramp and measure its side fringe from the sloping rim. For separate
plant instances, trace `flora_generator`, `flora_simulation`, mesh/instance
placement, and tree support checks. Resolve the actual height at the anchor.
Skip species whose footprint or support rule cannot be satisfied there. Use
a consistent policy for generated and loaded flora, and recheck support after
rotation/removal. Keep trees requiring level ground on suitable landings.

**Accept when** rotations, crossing ramps, cube/slab interfaces and walls on
opposite chunk edges expose exactly the expected area. Test opposing slopes
whose boundary profiles intersect halfway. Inspect normals, winding, and
coincident surfaces. Compare bare and grassy slopes crossing a chunk edge.
Grass should follow the incline without sampling the neighboring tile's cover.

## 4. Make the shapes placeable, pickable, and walkable

- Keep the grid traversal in `tile_raycast.hpp` as the broad phase. Intersect
  the actual occupied shape within the ray's interval in each cell. Continue
  through empty portions. Return distance, hit position, geometric normal,
  canonical tile, and a separate integer placement offset. Use the upper
  adjacent cell for sloped and slab tops, outward cardinal neighbor for a
  side, and lower neighbor for a bottom. Preserve the existing inside-hit
  convention and optional liquid picking. Define deterministic edge ties.
- Extend the existing Place Block tool in `states/gameplay_state.*`. Provide
  labeled buttons to cycle cube/slab/ramp/wall, rotate by a quarter turn,
  select wall thickness, and configure the run length (1, 2,
  or 3 m), segment (1 through run length), and form (straight, outer corner,
  or inner corner). Clamp the segment when the run is shortened. Show current
  settings and the picked tile's shape/facing in the debug panel. Each click places one piece. Preserve Remove, Place Light,
  Harvest and Plant Tree. Use existing widgets, translated labels, and UI
  event consumption. Verify resizing and UI scaling.
- Gameplay currently drives an orbit camera. Implement a grounded debug
  traversal mode/probe using the shared geometry so walking can be exercised
  in game now. Keep free camera available. Give the probe a finite footprint
  and height, continuous support on ascents/descents, lateral blocking by
  thin walls, and head clearance under obstacles. Sweep or substep fast moves
  so a 10 cm wall cannot be skipped. Use the same analytic boxes/wedges as
  inputs to the future character collision integration described in
  [the physics handoff](physics.md).

**Accept when** a ray through the open part of a thin-wall cell can select a
block behind it, a ray on its occupied strip hits the wall, and clicking a
slope places in the intended adjacent cell. Walk up and down 1, 2, and 3 m
runs in all facings. Walk from each approach of both corner forms, cross the
diagonal in both directions, and pick each facet. Repeat across a chunk
edge and the wrapped X seam. Check
side approaches, low ceilings, backing off an edge, and removing the middle
third. The remaining pieces retain their geometry and the gap is traversable
only where solid support exists.

## 5. Light an occupied surface at voxel resolution

Start with `sunlight_propagator`, `block_light_propagator`, `sky_column`,
`sky_height_map`, `light_volume_manager`, and `light_sources`. Keep the two
4-bit light channels and the existing volume resolution. Extend immutable
opacity snapshots with optional shape information. Define one CPU transmission
rule used by live chunks, sky estimates, emitter open faces, and background
jobs. Keep material opacity/translucency in the calculation.

For a first approximation, compute open area at a shared face from both
profiles in the same face coordinates. A full opaque contact blocks the
crossing and a partly open contact attenuates by covered area. Also compute
vertical column transmission from the projected footprint along the sky ray.
An opaque slab spans the whole footprint and blocks direct sun below. A
10 cm wall takes about a tenth of the overhead light but can close its entire
outward side. Keep direct and indirect contributions distinct. Document the
rounding to the current light range. One light value per cell approximates
lighting in its open portion. Use the neighboring lit cell to shade the
outside of a sealed boundary and check inset surfaces from both sides.

Invalidate opacity and emitter face caches on rotation, including the next
chunk across a boundary. Trace distant sky overrides and `light_shadows`
caster geometry so a wall rotates in its cast shadow too. Preserve the
existing behavior where a light-only update refreshes volumes without
remeshing. Test stale jobs published after an edit or unload.

The voxel approximation weights each boundary polygon by its material
opacity. Overlapping coverage counts once at the higher opacity. The
vertical sweep weights opacity by the profile's footprint and gives the
receiving cell its incoming light before subtracting its own loss. Both
losses round to the nearest 4-bit level, with half levels rounding up.
Only full opaque coverage reaches 15. Indirect paths lose at least one
level per crossing, and cube-only paths retain their existing material
penalty. The straight near field of a far light tests actual shape
intersections to match the shadow map. Both light paths let a block
emitter leave its own tile and apply the neighboring coverage.

**Accept when** a roof slab blocks direct sky below, an exposed ramp top
receives sky, and a torch placed across a wall casts a different shadow after
a quarter turn. Compare against a cube and an empty-cell control with a
fixed light position. Light both facets of a grassy junction from the side
and check for a spurious shadow or light leak along the diagonal. Check a
sealed emitter, translucent material, a chunk
edge, and the change from estimated to swept light.

## 6. Generate ramps and match the distant surface

Use `terrain_sampler.*` and its captured neighborhood to choose a surface
profile once per column. Share the result with chunk generation, the edit
baseline, flora placement and LOD sampling. Use a named generator-version gate
in `save/world_meta.*` and the load/create paths. Keep the previous terrain
rule for existing worlds, including previously unseen chunks. Legacy worlds
can still receive explicitly placed shapes. Keep codec versioning separate
from generator selection.

Start with a deterministic **cut into the high cell**. If its surface tile is
at Y=h, its old top is h+1. Choose a ramp when the low neighbor's top is h
and the opposite landing is h+1. Orient uphill from the low neighbor. Sample
the unmodified height field for eligibility. Use a straight ramp for a unique
low direction with suitable side neighbors. At a bend, match the two
perpendicular approach profiles and both remaining edges against the rotated
inner/outer corner templates from step 1. In a candidate pass, read original
heights. In a compatibility pass, compare proposed shared edges in world
coordinates. Accept a corner only if both approaches survive and the level
edges meet their intended landings. Use an uncut cell where a neighborhood
has no compatible arrangement. Cover a convex spur and a concave hillside
bend with small deterministic fixtures, including one straddling X wrap.

Require dry supported soil. Exclude cliffs, banks controlled by hydrology, and
structure footprints that require level support. Test candidate/landing pairs to
prevent a second candidate from cutting away the first one's landing. Make
the rule a bounded pure function of the seed and coordinates, independent of
chunk load order. X wraps and Z has edges. Natural terrain uses the 1 m run
and its matching corner junctions. The debug/build tool also exposes the 2 m
and 3 m runs and their junctions.

Carry base Y and profile to `terrain_lod.*` and `terrain_lod_manager.*`.
Spacing-one geometry needs the actual edge endpoints, the profile’s top
diagonal for corners, and appropriate vertical faces. Flipping the diagonal
changes an inner corner into a different walking surface even when the four
corner heights agree. Preserve distinct heights on either side of a cliff. Coarser patches
can simplify the interior but joins, skirts, normals, cover and materials must
agree where the near and distant meshes meet. Extend `structure_lod.*` and
edited-ground reconstruction for player walls, slabs and rotated ramps. A
thin wall sharing the highest ground cell may require a structure proxy over
the exposed substrate to represent its partial footprint. Track that case
explicitly. Compare real geometry across the handoff with fixed lighting
before attributing a visible transition to shadows.

**Accept when** a fixed-seed hillside has walkable rises, flat landings, intact
cliffs and reproducible shape codes in different generation orders. Exercise
a region corner and the X seam, a shoreline and a structure foundation.
Compare the same natural ramp and edited wall in near chunks, spacing-one LOD
and a coarser patch on Low/Medium/High detail. Edit, unload and return.
Existing worlds must match their old near and distant surface before edits.

## 7. Verify the rollout and record results

1. Extend the relevant drivers in `tests/unit/game/world/` and
   `tests/integration/`. Register cases using the project's existing CTest
   setup. Build the affected tests and app, run the focused cases and then
   the registered headless suite. Record commands, counts and failures.
2. For shader changes, verify the shaderc path in `generate_assets.py`, run
   the generator, inspect changed binaries and manifest, then rebuild the app.
   Use the existing device smoke target or gameplay view for visual evidence.
   Capture a grassy two-piece ramp and an inner/outer junction from both
   approaches, lit walls before/after rotation, and a near/LOD transition at the same location. Record seed,
   position, time of day and terrain detail for reproduction.
3. In a disposable save, place each variant, rotate a boundary piece, travel
   at least three regions away, return, exit and reload. Repeat with an older
   save copy. Stress edits while meshing, saving and relighting, then exit
   with work pending. Confirm material, geometry, cover, picking and distant
   silhouettes agree after every handoff.
4. Compare a cube-only scene, a naturally ramped hillside, and a chunk densely
   filled with shaped pieces. Record resident shape-table bytes including
   capacity, encoded chunk size, mesh build time, triangle count and lighting
   update cost against the same pre-change scene/settings. Use these figures
   to decide whether dense lookup is warranted.
5. Update the module guides and affected flora, physics, lighting, water,
   structures and modding documents where behavior or a file format changed.
   Record completed and outstanding checks in this handoff. A headless pass
   and an in-game visual/traversal pass are separate results.
