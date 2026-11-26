# Weather, temperature, snow and ice

**Steps 01–10 complete · Step 11 pending.**

The first playable target is a snow-covered landing at the foot of a stone ramp. A light snowfall slides down the 45° face. When the standing depth on the landing reaches 5 cm, snow fills the bottom 5 cm of open space at the toe and the upper rock stays bare. The same tile can hold stone and snow. Later a builder can use that capability for stone and timber, and a lake can have a walkable ice lid with liquid below it.

This handoff owns the environmental simulation and its integration with terrain, construction and water. Severe weather hazards, fire spread and its LOD, NPC building design, survival damage, drowning, and the Fahrenheit display option are future consumers. V1 supplies regional weather, local °C, signed thermal sources, persistent snow up to 16 m, gradual ice growth and thaw, and a way to demonstrate weight breaking ice.

## Using this handoff

Take the first pending step in order. Mark it **In progress**. Implement its deliverable through the normal game path and run its acceptance checks. Record the commands, results, fixture seed or save, and any deviations in the execution log. For a playcheck, mark **Awaiting maintainer** and stop before the following step. Provide the maintainer the launch procedure, debug controls, coordinates, and expected result. Mark **Complete** after they confirm the result. A device smoke capture is useful evidence for a playcheck, but only the maintainer signs it off. A failed check stays on the same step with its reproduction recorded.

| Step | Deliverable | Status | Maintainer playcheck before advancing |
|---|---|---|---|
| 01 | Reproducible terrain, water and performance baseline | Complete | |
| 02 | Geometry for positioned pieces and horizontal layers | Complete | |
| 03 | Sparse compound storage and immutable snapshots | Complete | |
| 04 | Durable piece edits and save migration | Complete | |
| 05 | Build, pick, walk on and render compound tiles | Complete | A · Construction |
| 06 | Gravity settlement at slopes and ledges | Complete | |
| 07 | Visible dusting, toe fill and snow collision | Complete | B · Five-centimetre ramp |
| 08 | Calendar and seasonal ambient temperature | Complete | |
| 09 | Regional weather, presentation and inspection | Complete | C · Weather and seasons |
| 10 | Exposure, snowfall and outdoor melt | Complete | |
| 11 | Unloaded weather history and elapsed-time reconciliation | Pending | D · Shelter and travel |
| 12 | Regional weather at distance | Pending | E · Weather from afar |
| 13 | Polar baselines, vertical runs and excavation | Pending | |
| 14 | Deep-pack LOD, structures and travel | Pending | F · Arctic shelter |
| 15 | Local heating and cooling from blocks and items | Pending | |
| 16 | Room air, wall heat storage and local melt | Pending | G · Heated room |
| 17 | Partial-cell water capacity and connectivity | Pending | |
| 18 | Water–ice transfers, persistence and meltwater | Pending | |
| 19 | Surface ice in still and flowing water | Pending | H · Frozen shore |
| 20 | Load-bearing ice and local break-through | Pending | I · Thin ice |
| 21 | Migration, extended travel and performance | Pending | J · End-to-end |

## Starting points and ownership

Read the [game module guide](../../src/game/AGENTS.md), the [world vision](../../IDEA.md), and the affected module guides before implementing a step. The following boundaries matter across the whole rollout.

| Concern | Existing implementation | Ownership after this work |
|---|---|---|
| Solid occupancy | Six-byte [Tile](../../src/game/world/tile.hpp), optional shape storage in chunks, shared [tile geometry](../../src/game/world/tile_shape.hpp). The [shape handoff](tile-shapes.md) documents partial solid cells acting as full fluid barriers. | Chunk solids and generated structure sources own construction. A read of one cell revision combines those pieces with any environmental deposits. Geometry serves queries and rendering. |
| Editing and distant detail | [WorldEditor](../../src/game/world/world_editor.hpp), [terrain edit tracker](../../src/game/world/terrain_edit_tracker.cpp), [structure sources](../../src/game/world/structure_voxels.hpp). Trees have their own overrides and displaced substrate. | Preserve provenance per piece. Removing wood from a shared tile can reveal generated rock or snow. A weather update invalidates affected views without turning natural terrain into a player edit. |
| Climate and time | Regions already store humidity and °C. [Ground cover climate](../../src/game/world/ground_cover_climate.hpp) blends region centres. [WorldClock](../../src/game/world/world_clock.hpp) persists fractional days at 72 real minutes per game day. | Region generation owns the climate baseline. Weather is sampled from seed, location, model version and world time. Measured local temperature combines that sample with shelter and local thermal state. |
| Snow | Ground cover currently paints a solid and has reserved state bits. | Environmental records own deposited snow, excavations and pending downhill transfers. Dusting, solid snow, sky heights, collision and distant surfaces are projections of those records. Grass survives being buried. |
| Water and ice | [Reservoirs](../../src/game/world/reservoirs.hpp), [local water cells](../../src/game/world/local_water_cells.hpp) and [WaterSurface](../../src/game/world/water_surface.hpp). Projected water is omitted from ordinary chunk saves. See the [water handoff](water.md). | The water owner commits water–ice transfers. A frozen record carries volume and origin and projects a distinct ice block. Shared geometry describes the ice; visible ice and liquid never become independent claims on the same volume. |
| Active and remote work | Chunk jobs capture inputs. Region unload and autosave can overlap later edits. X wraps and Z ends at the map edges. | Canonical keys, immutable snapshots, revision checks and bounded owner-thread publication apply to environment jobs too. Distant weather, snow and ice summaries must be queryable without requesting voxel chunks. |

## Steps and acceptance

### 01. Establish the comparison scene

Extend the existing shape and river fixtures, or provide a reproducible placement sequence for them. Include a flat landing, straight ramp in four rotations, diagonal and inner/outer corner, two consecutive ramps ending at a drop, rim across a chunk face, solid and transparent roofs, a shallow pool and a moving reach. Put a second case at the wrapped X seam. Keep a disposable copy of a pre-feature save containing shaped edits, flora, structure overrides and water. Record build type, seed, camera, detail preset and clock time.

**Verify.** Build the app and gameplay tests, run the gameplay suite, and capture the original shape and river views. Record bytes per ordinary chunk, mesh build time, median and p95 frame time, worker queue high-water mark and save size for a settled view and a streaming walk. Reuse these measurements in step 21. Commands and device requirements are in the [test guide](../../tests/README.md).

### 02. Define geometry for more than one solid in a tile

Extend the shared geometry API with cell-local placement, rotation, material and a piece identifier suitable for editing. Include horizontal thicknesses of 5, 10, 25, 50 and 75 cm and a full metre. Reuse the existing slab for the 50 cm shape. Keep existing profile codes stable. Allow elevated pieces and complementary wedges, including diagonal and corner facets. Specify bottom and top surfaces explicitly where a single support height cannot describe the solid. Use the profile geometry to clip candidate pieces at cell boundaries.

**Verify.** Test containment, positive-volume overlap, face contact, exterior faces, ray hit and support over a footprint. A slab over a gap, complementary wedges and a narrow vertical wall beside a layer should agree across queries in all rotations. Reject intersecting interiors deterministically. Support a direct query for free intervals at a point and boundary openings for later liquid and light queries.

### 03. Keep compound storage sparse

Retain the existing empty, cube and single-profile paths. Allocate an optional per-chunk table for cells with several pieces or a placement the original format cannot express. Canonicalize an edited cell back to its simplest representation. Give reads and mesh jobs one immutable revision containing the tile, pieces, materials and edit provenance. Retain the ability to select a particular piece across unrelated edits, or reject a stale selection by revision. Bound decoded counts and allocations.

**Verify.** Empty and ordinary terrain allocate no compound entries and retain the six-byte base tile. Exercise insertion, independent removal, clearing the last entry, border snapshots, chunk reuse and an edit made during a mesh job. Record cost for ordinary terrain and for a dense compound wall.

### 04. Persist edits and generated pieces

Extend the chunk codec, edit summaries, and the applicable structure override and baseline codecs together. Preserve stable material IDs and old profile codes. Block IDs are assigned in data-file order, so adding a snow or ice definition later must preserve existing numeric IDs. Route placement and removal through the editor with the normal border, mesh, light, flora, sky and save notifications. Structure reconciliation must preserve a player-added piece when its generated neighbour changes.

**Verify.** Round-trip old and new saves, malformed counts, an edited compound on a chunk seam, and a generated piece removed from a mixed tile. Evict and reopen the region during edits. Rebuild the distant edit index and compare to the saved near geometry.

### 05. Make compound construction usable

Give the debug builder piece placement, rotation, preview and per-piece removal. Use the same occupancy rules for a small generated structure fixture. Clip internal faces, preserve material assignment and the ordinary greedy cube path, and integrate near and distant structure meshes, shadow casters, sunlight openings, ray picking, plant support and the grounded probe. Make a piece visible and selectable from underneath an overhang.

**Playcheck A.** Build a stone base and timber piece in one tile, reject an overlapping placement, pick each material, and remove just the timber. Walk on the base and look through the opening. Inspect a generated example across the near/far handover, travel three regions, return, then quit and reload. Confirm the surviving material and light opening in both views.

### 06. Settle snow under gravity

Use deposited volume per horizontal footprint and world gravity. Compute downhill exits from the actual upward-facing facets. A 45° or steeper facet sheds loose snowfall to the lowest reachable support. A ledge with an open drop delivers it to the support below. A wall closes an exit. At a receiving landing, fill available space from the bottom to a horizontal level, clipped against the underlying rock and neighbouring pieces. Rising snow on the landing backs into the toe of the ramp. The landing level caps the inverse wedge above the rock profile. On a 45° ramp, a 5 cm level fills only the first 5 cm uphill from the toe. For gentler and multi-facet slopes, start with shallow pockets building from their lowest supported points and spilling at their rims. Treat the gentler-slope response as a tuning choice at playcheck B.

**Numerical fixture.** On a one-metre-wide ramp rising one metre over one horizontal metre, let the base be at zero and the landing snowline be 0.05 m. The clipped triangular pocket extends 0.05 m uphill. Its volume is 0.00125 m³. With an adjoining 1 m² flat landing bounded on its other sides, supply 0.05125 m³ to reach that line. “5 cm on the landing” specifies a level; the input required depends on the receiving area. This case catches both a parallel coating and accidental creation of snow.

**Verify.** Test that exact wedge, a freely draining ramp ending over a lower floor, a blocked toe, a diagonal split, opposed slopes forming a trough, two stacked ledges and a cross-region route. Resolve equal outlets in a deterministic order. Use bounded work with a persisted pending quantity when a transfer cannot finish immediately. A unit of volume must be found either at a receiver or in transit after eviction, retry or cancellation. At a bounded map edge specify an accounted sink or a closed edge.

### 07. Render and walk on the settled result

Expose debug controls to add a known volume, inspect a landing level and clear snow. First accumulation appears as a dusting on the receiving footprint. Choose a visible-to-solid transition and record it with the fixture; 5 cm must have measurable thickness. Derive the dusting mask and solid geometry from the same deposit. Expose the original material as the snow melts or is removed. Mesh the clipped top and exposed sides, including borders and material joins. Picking and walking must agree with the surface.

**Playcheck B.** From the side, add the step 06 fixture amount. Inspect bare rock uphill, a level 5 cm top continuous with the landing and the short white wedge at the toe. Repeat at a chunk seam and on a diagonal. Open the downhill drop and repeat with fresh snow; inspect the lower floor. Increase snowfall until the landing buries the ramp, then clear it and inspect grass, rock and walking height. Record a side-view capture and the accepted gentler-slope behaviour before moving on.

### 08. Derive the calendar and seasonal temperatures

Derive day, month and year from saved fractional world days. Display day 1/month 1/year 1 at elapsed day zero. Use 30 days per month and 12 months per year. Sample a continuous annual curve for each region's seasonal temperature range. Proposed starting convention is northern midwinter in month 1, southern midsummer at the same date, and smaller seasonal swings near the equator. Combine the seasonal baseline with a daily range. Generated regional temperature already includes altitude cooling; apply any local elevation correction relative to regional terrain. Use Celsius internally throughout.

**Verify.** Check days 0, 29, 30, 359 and 360, pause, accelerated time and save/reload across midnight. Compare the same region in opposite seasons, both poles at one date, and the equator. Avoid discontinuities at month boundaries, the equator and the X seam. Retain the map-less fixture fallback.

### 09. Sample and show regional weather

Use a spatially coherent, temporally continuous seeded field for cloudiness, precipitation rate and weather temperature offset. Blend across region boundaries using the existing wrapping convention. Derive clear, cloudy/dry, rain and snow from those values and outdoor °C, with a narrow reproducible transition around freezing. Separate physical rates from visual particle density. Render cloud cover and near rain/snow; provide date, climate baseline, weather offset, resulting °C and precipitation rate in a debug view. Supply reproducible controls for time advance and weather inspection. Keep debug forcing scoped to a test session or persist it explicitly in test fixtures.

**Playcheck C.** Revisit one temperate region in winter and summer and compare daily ranges. Inspect both pole and equator. Follow a band into the next region and across wrapped X; watch cloudy dry conditions, rain, snowfall and a transition near freezing. Pause and reload at a known date and confirm the weather sample. Check regional contrast and transition speed with the maintainer.

### 10. Apply exposure, accumulation and outdoor melt

Use a precipitation query along gravity that finds the first intercepting surface. Reuse sky-height acceleration where valid and consult actual shape coverage for openings. Account for transparent roofs, ledges, constructed pieces and terrain edits. Define tree interception for the first version against the existing trunk/canopy representations and document it in the fixture. Stop visible precipitation under a closed roof. Integrate precipitation rate over elapsed world time, feed snow to step 06, and melt according to sustained positive local temperature. Track snow water equivalent using a documented conversion; measured snow depth and liquid water depth have different units of accounting. Track meltwater awaiting routing until step 18.

**Verify.** Test roof, glass, open hatch, cave, roof edge above a ramp, and opening or closing a hatch halfway through snowfall. Pause produces zero change. Ten small intervals and their combined interval agree within the recorded quantization tolerance; rainfall does not add snow. Preserve buried vegetation and uncover its prior state.

### 11. Catch up when the player is away

Introduce dated regional precipitation and temperature integrals (or deterministic interval summaries) with simulation version and last-applied times. Preserve the order of freezing, snowfall and melt intervals; a winter storm followed by a thaw cannot be reconstructed from a yearly mean. Use summaries for unvisited terrain and retain sparse exceptions for edits, pending transfers and dated changes of exposure. Cache repeated intervals and bound catch-up work on load. Define which coarse results are exact and record error bounds for approximations. Publish projections against geometry and environment revisions.

**Playcheck D.** Set up matching covered and exposed sites. Keep one pair active and leave the other pair for several regions during snow and a thaw. Return, inspect the depths, and reload during catch-up. Covered floors stay dry, exposed deposits agree within the measured tolerance, and there is no second snowfall on loading. Automated cases should also compare different visit orders and a full-year skip.

### 12. Show regional weather at distance

Use the dated regional precipitation and temperature summaries from step 11 to draw weather in neighbouring regions without requesting their chunks. Build one presentation summary per visible weather cell or region: condition, rain or snow, physical rate, visual density, cloud darkness and drift. Render distant rain as depth-aware shafts or curtains and snow as a lighter haze or veil, grouped by summary so a heavy neighbour reads clearly from a ridge while the current region stays clear. The near rain and snow from step 09 own the detail radius; distant weather fades in beyond it and must hand over without a double layer or a pop. Separate physical rate from visual density exactly as in step 09. Bound the work: one summary-driven batch per frame, cached by region, step 11 revision, model date and LOD distance; no voxel-chunk requests, no full-world sweep and no per-frame allocation growth. Respect wrapped X and the Z map edge. Provide a session-only debug force for regional conditions so the same path can be exercised without waiting for weather.

**Verify.** Use a fixed seed and date with four adjacent regions: clear, heavy rain, snow and cloudy/dry. Inspect from a ridge, from the region boundary, across wrapped X and through a pause and reload. Confirm the rain neighbour is visible, snow is a lighter veil, clear stays empty, and no chunks are requested for unloaded neighbours. Check Low, Medium and High detail: distant precipitation fades out as the near particles take over, with no double layer, pop or draw-through at the camera. Record summary rebuilds, draw calls or instances and frame time while stationary and while crossing regions. A time jump changes the summary only through step 11's dated data; pause freezes it.

**Playcheck E.** Stand in a clear region beside a heavy-rain region and a snow region. From a ridge, see rain falling in the neighbour while the local sky stays clear; move to the boundary and watch the near particles take over. Repeat at a second detail preset, across the wrapped X seam, and after pausing and reloading. Confirm no rain appears in the clear region and no chunks stream for the distant weather.

### 13. Generate and edit deep polar packs

Generate established packs for eligible new-world polar terrain using climate, exposure and a deterministic starting date. Implement continuous depths from dusting through **16 m**. Store undisturbed pack as compact vertical runs or equivalent reproducible intervals. Record exceptions at arbitrary heights and surface caps. The cap is measured as accumulated depth above the relevant support; retain a documented overflow policy at saturation for the water budget. Crossing a vertical chunk boundary must preserve continuous support and volume. Version the baseline. Proposed migration for an existing world is to start weather at its saved date with zero initial deposit, including unseen regions of that world; this keeps existing buildings accessible and avoids a boundary between explored and unexplored terrain.

**Verify.** Generate a 16 m fixture with the pack crossing a slab boundary. Excavate the middle of a column and keep the snow above and below. Remove the roof of a buried room and apply new snowfall. A side entrance under a roof stays sheltered. Validate compact storage, excavation across region borders, save/reload and the old-world policy.

### 14. Carry snow into distant terrain and structures

Make terrain LOD sample the same dated pack summaries and edited surface intervals. Resolve roofs and snow-covered structures at the structure handover. Update silhouettes, sky occlusion, shadows, flora visibility and walkable surfaces as depth changes. Limit invalidation by changed footprint and quantized geometry changes. Underground voids belong to exact nearby geometry; preserve them in the saved data when a far patch simplifies the exterior.

**Playcheck F.** Find the seeded 16 m pack, cut a side entrance and room across a chunk boundary, and add a roof or structural support. Watch snowfall outside. Leave to distant view, approach again, travel three regions away, and reload. Inspect the silhouette, tunnel headroom, snow above it and grass or rock at the excavated floor.

### 15. Measure local heat and cold

Add a data-defined thermal source with signed output and an effective range. Expose it from a placed block and from an item at its current world position; use a movable debug item if the inventory path is unavailable. Attach, move and remove sources through their owners' lifecycles. Provide a local °C query usable by snow, ice and future actors. Use an indexed local neighbourhood and measure attenuation with distance and barriers. Provide a dated regional contribution interface for future large fires. Record the handover convention so a source contributes once where coarse and detailed simulation meet.

**Verify.** Use one heater, one cooler and an overlapping pair. Sample at specified distances and behind shelter. Move a carried source across a region boundary, drop or remove it, save and return. Check for residual registered sources and temperature jumps at handover. Document configurable units and ranges alongside the item data.

### 16. Let rooms and walls store heat

Compute connected indoor air in the active area with leakage through openings, exposure to outdoors, and heat exchange with surrounding material. Treat warmed walls as finite stores that can release heat into adjacent air. Use baseline ground temperature and insulation for sheltered caves. Bound connectivity work across unloaded boundaries; record the approximation there and invalidate it on geometry changes. Persist significant stored heat and its timestamp; cooling during absence should match the documented approximation. Feed the same local °C into snowmelt and later phase changes.

**Playcheck G.** Measure a heater outdoors, then inside the fixture room. Open the door, close it, turn off the heater and observe air and walls cooling at different rates. Remove a roof piece during snowfall and check exposure and heat loss. Repeat with the cooler. Inspect a patch of snow beside the warm wall and another farther away. Reload while the wall is still warm.

### 17. Teach water about partial-cell solids

Audit solid checks in projection, barriers, local flow, face culling, submersion and picking. Use the occupied geometry to calculate available liquid volume at a given head and face openings shared with neighbouring cells. Distinguish stacked free intervals and sealed pockets. Fill only connected space up to its hydraulic head. Version and migrate any persisted local-volume assumptions. Check old edited dams and reservoir barriers explicitly because existing partial solids used to seal an entire cell. Preserve established legacy barriers through migration where opening them would drain a saved body.

**Verify.** Test a slab above water, an elevated lid over connected liquid, a fully closed face, a wall with a side gap, a ramp shoreline and two disconnected cavities. Check the local, independent/legacy and reservoir-projected water paths. An unchanged full cube still dams a river. Existing river, lake, drawdown and save/load tests must pass.

### 18. Give freeze, thaw and runoff one balance sheet

Keep authoritative phase state beside the water volume it changes. Record source/body or local owner, column or interval, frozen water equivalent, residual phase progress and last-applied time. Use fixed volume units or an explicit remainder when converting between the water system's fill levels and centimetres. Commit the liquid debit and frozen credit together. Make a load, retry, stale worker result, or interruption between region and world saves safe to replay; test recovery at each write boundary. For legacy independent water, establish one owner before conversion. Keep projected ice out of ordinary solid edit saves.

Route rain and credited snowmelt through dated, once-only inputs to a local cell, reservoir, reach or accounted outflow. Use the water handoff's planned weather input boundary. Scope this to water transfer; settlement relocation is separate. Define V1 density, clamping and overflow for each phase. In a closed test basin, audit liquid + ice water equivalent + snow water equivalent + pending transfers against initial storage and external precipitation.

**Verify.** Cycle through freeze, thaw, drain, refill, excavation and reload. Include a full destination, changed reservoir head, frozen and unloaded regional boundary, and one source sending to two receivers. Verify the balance and ensure remote catch-up and local detail cannot both apply a transfer.

### 19. Grow surface ice and show the water underneath

Use prolonged subzero or positive local temperatures to drive phase progress. Show successive 5, 10, 25, 50, 75 and 100 cm stages and retain sub-stage progress across reversals and loads. Expose thickness and remaining liquid depth to gameplay. Start at the fractional mean waterline and grow down into available water; limit growth by depth and supply. Use the mean river stage for simulation. Suppress animated liquid waves where covered. Choose and record a reduced freezing rate for flowing reaches. Reconcile changing stage, stranded ice and open patches without recreating liquid from a projection. A dated frozen fraction or equivalent summary should give distant lakes and rivers the same broad ice footprint as nearby cells.

**Playcheck H.** Inspect a shallow bank and a deeper pool from above and below while accelerating a cold spell. Measure the first lid at a non-integer waterline; the shallow bank freezes through first. Compare with a moving reach. Heat one patch, reload midway through thaw, then cross near/far range. Inspect the opening, liquid beneath the remaining ice and the reported volume balance.

### 20. Break ice under a load

Keep ice strength a separate gameplay rule reading thickness, support and applied load. Provide data-defined test thresholds and a grounded debug probe with selectable loads until actors supply their own. Break the contacted patch once, remove its support, and let the probe fall into the exposed liquid. Route the displaced ice through a documented budget policy; an acceptable V1 approximation is to credit the broken mass back to local liquid. Persist the opening and reset local growth progress so it refreezes over elapsed time.

**Playcheck I.** Put a light and a heavy load on matching thin lids, then the heavy load on thick ice. Break through near the shore and at a chunk edge. Inspect the fall, adjacent intact ice, and the saved hole; wait for refreezing. Confirm breakage and thaw cannot both credit the same ice.

### 21. Verify the combined rollout

Repeat step 01 with ordinary terrain, a compound village fixture, active snowfall, deep polar terrain, and a freezing river. Record memory, frame times, meshes built, worker backlog, catch-up latency and save growth at the same presets and routes. Separate the cost of a settled scene from active streaming. Investigate regressions against the baseline, especially allocations per ordinary tile, full-world sweeps, remeshing on every temperature sample, and repeated reconstruction of deep packs. Run old-save and malformed-save tests, the full gameplay suite, the relevant device scenes, and exit with background work pending. Update the water, shape and modding guides where their described behaviour or schemas have actually changed.

**Playcheck J.** In a new world, travel from rain to a polar shelter, alter a shore and excavate under the pack. Spend a season away, return and reload. In the disposable pre-feature world, verify the stored date, original buildings, old dam and terrain edits before advancing through its first weather. Record final performance evidence, any accepted approximations, and the maintainer result.

## Execution log

### Step 01 · 2026-11-25

Added the `weather`, `weather_walk` and `weather_seam` scenes to
`lod_render_smoke`. See the [test guide](../../tests/README.md#weather-comparison)
for replay and loading the disposable save. The original `shapes` and
seed-2024 `river` scenes supply the shape and moving-water references. The
river captures include the mouth, the wrapped reach, the waterfall during
remeshing and the submerged landing. The harness reported above water in
phases 0–2 and submerged in phase 3.

The comparison uses seed 2024 on the synthetic 96 × 96 region terrain
(height seed 1234, feature seed 5678), generator 27, saved clock day 1 at
08:00 (elapsed day 1/3). Both cameras use yaw −0.18 rad, pitch −0.75 rad
and distance 18 m. The Low terrain preset uses a four-chunk radius.
The Vulkan capture uses 1280 × 720 and four worker threads.

| Site | Origin (X, Y, Z) | Camera target (X, Y, Z) |
|---|---|---|
| Chunk face | 6144, 467, 6148 | 6144, 468, 6148.5 |
| Wrapped face | 0, 387, 6148 | 0, 388, 6148.5 |

Use these offsets from either origin. Negative X at the wrapped site is stored
at the east end of the world. The flat slate landing is at local Y=0.
The roof comparison uses opaque slate and a zero-opacity quartzite proxy in
the harness. Both are solids and both render with their shipped opaque textures.
The pool uses independent water with a fill of 96/255 per cell. The moving
reach is in the original river fixture.

| Feature | Local positions (X, Y, Z) |
|---|---|
| Straight ramps in quarter-turn order | X −5, −3, −1, 1 at Y 1 and Z −4 |
| Diagonal, inner and outer corners | X −5, −3, −1 at Y 1 and Z −2 |
| Two ramps and a drop | X 3 at (Y 3, Z 4) then (Y 2, Z 3), dropping toward Z 2 |
| Rim across the boundary | X −2 through 1 at Y 1 and Z 5 |
| Opaque and light-transmitting roofs | X −5 through −4 and −3 through −2 respectively, at Y 3 and Z 1–2 |
| Shallow pool | X 2–3 at Y 1 and Z −1 |

```bash
cmake --build build --target app world_logic_tests lod_render_smoke shape_benchmark --parallel 4
ctest --test-dir build -L gameplay --output-on-failure --no-tests=error
mkdir -p build/weather-baseline
build/tests/lod_render_smoke build/weather-baseline/shapes 1200 shapes
build/tests/lod_render_smoke build/weather-baseline/river 2000 river
SMOKE_TERRAIN=0 build/tests/lod_render_smoke build/weather-baseline/weather-walk 2800 weather_walk
SMOKE_TERRAIN=0 build/tests/lod_render_smoke build/weather-baseline/weather-seam 1600 weather_seam
build/tests/shape_benchmark --legacy
build/tests/shape_benchmark
```

Build and 391/391 gameplay tests passed in Release with Tracy disabled.
The Vulkan commands passed on the Ryzen 7 3700X host (AMD Vulkan device
`1002:73ff`). The sandboxed Vulkan launch failed before rendering. The
captures completed with device access outside the sandbox.

| View | Frames | Median frame (ms) | p95 frame (ms) | Sampled worker queue peak | Checkpoint save (bytes) |
|---|---:|---:|---:|---:|---:|
| Chunk face, settled | 60 | 2.43 | 3.44 | 0 | 138,517 |
| Streaming walk | 500 | 10.88 | 19.07 | 26 | 1,560,963 |
| Wrapped face, settled | 60 | 2.86 | 3.53 | 0 | 135,134 |

The route goes from X/Z 6144/6148.5 to 5144/6898.5. The save sizes are
logical file sizes across the slot after flushing the checkpoint. The
streaming save includes newly visited terrain. Frame time includes CPU work
through renderer flush and any wait inside submission. Queue peaks count
waiting jobs at the two per-frame sample points. Use the same sample points
and route in step 21.

The CPU mesh benchmark supplies per-chunk costs for both workloads using
the same 64 surface slabs per run. An ordinary chunk uses 24,576 bytes of
tiles, 24,616 bytes for the chunk object, and zero sparse shape bytes.
The cube-only slabs encoded to a mean 132.19 bytes and meshed in a mean
0.1852 ms (11.9 ms total). The natural-shape slabs encoded to a mean
282.75 bytes and meshed in a mean 0.2808 ms (18.0 ms total).
These timings cover isolated CPU builds with air neighbors. In the settled
window, streaming work has already drained.

Captures and run logs are under `build/weather-baseline/`. The original
views are `shapes.ppm` and `river_*.ppm`. The comparison views are
`weather-walk_0.ppm`, `weather-walk_2.ppm` and `weather-seam_0.ppm`.
The disposable pre-feature slot is
`build/weather-baseline/weather-walk_saves/2024_2026-11-25_22-28-13/`.
The matching wrapped slot is
`build/weather-baseline/weather-seam_saves/2024_2026-11-25_22-28-30/`.
The run decoded the shaped edit, flora, planted oak override and shallow
water from both slots, including the ordinary slot after streaming.
Opening the save through the interactive app was outside this verification.

### Step 02 · 2026-11-25

The shared [cell geometry API](../../src/game/world/tile_piece.hpp) uses
the existing profile planes and quarter-turn facings. A piece carries its
cell-local translation, cell-scoped ID and block ID for material lookup.
The 50 cm layer uses the slab profile. Other thicknesses use a horizontal
ceiling on the cube profile. A profile facet can serve as the top of a
lower piece or as the underside of a complementary piece. The cell clips
both at its boundaries.

Placement rejects overlapping interiors and allows face, edge and point
contact. Geometry comparisons treat distances within one micrometre as
contact. Exterior faces and ray hits report the owning piece and block.
Footprint support can query below an overhang by setting a height limit.
Free intervals and boundary openings describe the unoccupied space for
future fluid and light consumers.

The [headless cases](../../tests/unit/game/world/tile_piece_tests.cpp)
use synthetic one-cell fixtures with fixed coordinates and materials. No
seed or save is required. They exercise the six thicknesses, an elevated
slab, complementary straight, diagonal and corner profiles in all four
rotations, a wall beside a layer in each rotation, a capped 5 cm ramp toe,
boundary clipping and rejected placements. Compound storage and snapshots
start in step 03. In-game construction, picking and rendering start in
step 05.

```bash
cmake --build build --target app world_logic_tests --parallel 4
ctest --test-dir build -R 'game.world.tile_(piece|shape)' --output-on-failure --no-tests=error
ctest --test-dir build -L gameplay -j 4 --output-on-failure --no-tests=error

mkdir -p build/weather-geometry
clang++ -std=c++20 -Wall -Wextra -Wpedantic -g -O1 \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -Isrc/game -Isrc/engine -Itests/support \
  src/game/world/tile_shape.cpp src/game/world/tile_piece.cpp \
  tests/support/test_main.cpp tests/unit/game/world/tile_piece_tests.cpp \
  -o build/weather-geometry/tile_piece_sanitized
(
  for case in layers complementary_profiles wall_and_layer clipping_and_rejection; do
    ASAN_OPTIONS=detect_leaks=0 build/weather-geometry/tile_piece_sanitized "tile_piece_$case" || exit 1
  done
)
```

The Release app and logic tests built. All 9 focused CTest cases and all
395 gameplay cases passed. The four new geometry cases passed under ASan
and UBSan with LeakSanitizer disabled for sandbox execution. This step's
acceptance checks ran headlessly. The first device playcheck remains
scheduled after step 05.

### Step 03 · 2026-11-25

[Chunks](../../src/game/world/chunk.hpp) keep the six-byte tile and existing
shape table. The optional piece table stores cells with several pieces or
positioned geometry. A single piece that fits an existing profile goes back
to that profile. If it has an explicit ID or provenance that the old tile
cannot supply, a metadata-only entry preserves it. Clearing the final entry
releases the table. Compound cells project the lowest-ID piece into the
legacy tile path until step 05.

Cell reads own the base tile, shape and piece data. Mesh jobs copy the center
and loaded neighbors under one lock and share their immutable sparse tables
and structure baselines. A loaded cell selection is checked against its cell
index, chunk identity and topology revision. Editing a border invalidates the
neighbor's mesh revision, including across wrapped X. Chunk reuse invalidates
selections from its previous residency.

The storage entry point accepts up to 32 pieces per cell and the table has
at most 4,096 cell keys per chunk. It checks the count before allocating and
rejects invalid or overlapping pieces before replacing the cell. The legacy
chunk decoder now reads uncompressed indices from the input directly and
checks their expected size. Compound save records and editor integration are
scheduled for step 04.

The [headless fixtures](../../tests/unit/game/world/chunk_piece_tests.cpp) use
two half-metre pieces in one cell and a synthetic one-region-wide world.
The seam edit is at `(-1, 72, 72)`, canonical `(127, 72, 72)`, with the
neighboring mesh in chunk `(0, 4, 4)`. A worker gate holds the mesh job
through the edit. No save or world seed is required for the fixture.

```bash
cmake --build build --target app world_logic_tests shape_benchmark --parallel 4
focused='game.world.(chunk_piece|tile_(piece|shape)|mesh_skips_hidden_chunks)|chunk_codec_rejects_corrupt'
ctest --test-dir build -R "$focused" --output-on-failure --no-tests=error
ctest --test-dir build -L gameplay -j 4 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 3
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang -R "$focused" --output-on-failure --no-tests=error

build/tests/shape_benchmark --legacy
build/tests/shape_benchmark
build/tests/shape_benchmark --compound
```

The Release app and logic tests built with Tracy disabled. All 398 gameplay
cases passed. The 14 focused cases also passed with ASan and UBSan.
The initial sanitizer build hit a Clang frontend failure while reading a
system header. The build and tests passed on retry with the same compiler
and flags. Acceptance ran headlessly.

The ordinary benchmark uses the step 01 seed pair and 64 surface slabs per
run. The slab's tile array is still 24,576 bytes, and the chunk object is
24,632 bytes, up from 24,616. Cube-only and natural terrain allocated zero
piece-table bytes. Across three runs, the median of the mean CPU mesh time
was 0.1919 ms per cube-only slab and 0.2998 ms per natural-shape slab.
Mean encoded sizes were 132.19 and 282.75 bytes respectively.

The dense benchmark fills a 16 × 16 wall with two pieces per cell. It used
40,984 bytes of piece storage and 816 bytes of legacy shape storage,
excluding allocator and shared-pointer control-block overhead. Median
times across three runs were 0.9371 ms to insert the wall, 0.0105 ms to
capture one chunk snapshot, 0.0072 ms to edit one cell while a snapshot
held the old table, and 0.0653 ms to scan the snapshot's 4,096 cells.
This measures storage and reads. Compound mesh timings start in step 05.

### Step 04 · 2026-11-25

Chunk v7 saves positioned pieces and their provenance, plus explicit
generated-piece removals keyed by cell, source owner and piece ID. Ordinary
chunks still write v6. Region structure baselines and the distant edit index
carry the same piece payload. A partially edited generated cell is suppressed
in the distant index, which stores the exact surviving cell in its edit slab.
VOXL v3 saves compound structure overrides and reads v1 and v2. Asset
regeneration appends new block files to the existing manifest order, keeping
the IDs already written in saves.

The seed-2024 [region fixture](../../tests/integration/region_persistence_tests.cpp)
edits `(-1, 480, 66)`, canonical `(383, 480, 66)`, on a wrapped seam in a
three-region-wide synthetic world. It rejects overlap and stale selections,
checks invalidation across the seam, and reopens the region during its eviction
write. The source disappears in one revision and returns in another. The
player piece and the generated-piece removal both survive. The test rebuilds
the distant edit index from the saved region and compares it piece for piece
with the loaded cell. The scratch region saves are under
`build/tests/scratch/compound_wrapped_rebase/`.

A read-only codec probe checked the two pre-feature slots from step 01. All
10,199 chunk payloads across 36 region files round-tripped byte-for-byte.
Both slots' world maps and terrain edit indexes also round-tripped byte-for-byte,
and structure records survived migration. The region fixture covers a nonempty
compound baseline. A separate asset scan added temporary snow and ice filenames
that sort before the original files and confirmed the original load order.

```bash
cmake --build build --target app world_logic_tests --parallel 5
focused='chunk_codec_pieces|structure_override_codec_versions|region_pieces_persist|structure_revision_rebases'
ctest --test-dir build -R "$focused" --output-on-failure --no-tests=error
ctest --test-dir build -L gameplay -j 5 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 2
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang -R "$focused" --output-on-failure --no-tests=error
```

The Release app built and all 401 gameplay cases passed. All four focused
cases passed with ASan and UBSan after correcting the fixture's editor and
tracker destruction order. Acceptance ran headlessly. Interactive
construction is covered by the step 05 playcheck.


### Step 05 · 2026-11-25

The builder previews the requested piece in green for a valid placement and
red for a rejected placement. The tile readout identifies the picked block
and piece ID. Chunk meshes and level-zero structure proxies use the same
exterior faces and clip them at neighbouring cells. Those meshes also cast
shadows. The voxel sunlight sweep and sky heightmap use projected piece
coverage, and the sky override codec retains it on reload. Plant support and
the grounded probe query the actual solid surfaces and clearances.

Distant structure levels 1 and 2 still simplify to coarse voxels. The coarse
GPU sky map uses opaque heights on its existing four-metre grid. The exact
heightmap and level-zero handover are used to check the one-metre beam
opening. Per-piece water capacity is deferred to step 17.

#### Execution checks

```bash
python3 generate_assets.py
cmake --build build --target app world_logic_tests shape_benchmark --parallel 5
ctest --test-dir build -L gameplay -j 5 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 2
focused='compound_|chunk_piece_|region_pieces_persist|structure_revision_rebases'
focused="$focused|structure_voxels_plan_chunk_handover|probe_|ray_shape_|sky_"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang -R "$focused" -j 4 \
  --output-on-failure --no-tests=error

build/tests/shape_benchmark --legacy
build/tests/shape_benchmark
build/tests/shape_benchmark --compound
```

The Release app built and all 404 gameplay cases passed. All 30 selected
cases passed with ASan and UBSan. The first sanitizer run exposed a
negative shift in the vendored half-float conversion used by the coarse
sky texture. The game now packs those heights directly and checks the
open-sky sentinel, the world top and a subnormal value at compile time.
The timber asset was appended to the block manifest and the previous load
order was verified.

The mesh benchmark uses the Step 01 terrain seeds and the Step 03 dense
wall. The median of three Release runs was 0.213 ms per ordinary cube slab,
0.335 ms per natural-shape slab, and 5.94 ms for the 256-cell compound wall
with 2,240 triangles. Cube and natural terrain still allocated zero
piece-table bytes.

The seed-2024 scratch copy came from the Step 01 walk save. A read-only
probe confirmed the deck height, empty cells at the construction site, and
known-empty saved structure baselines for the four affected slabs. The
automated checks run headlessly. Interactive rendering, the three-region
trip and reload in the app are part of the maintainer playcheck.

#### Playcheck A

From the repository root, launch the disposable seed-2024 copy at Low terrain
detail. Use the same environment variable when reloading this fixture. The
fixture regenerates around the save's start region without modifying the
baseline slot. If the scratch folder is missing, copy the Step 01 slot
into it from the repository root.

```bash
mkdir -p build/construction-playcheck/saves
cp -a build/weather-baseline/weather-walk_saves/2024_2026-11-25_22-28-13 \
    build/construction-playcheck/saves/
ln -s ../src/app/assets build/construction-playcheck/assets
```

```bash
cd build/construction-playcheck
ANYM_CONSTRUCTION_FIXTURE=1 ../src/app/app
```

In **Load World**, select seed 2024 dated 2026-11-25 22:28:13. Click
**Construction site** and wait for streaming to settle. The HUD shows the
deck and camera coordinates. If using another disposable save, apply the
same offsets from the deck reported there.

| Feature | Coordinates in the seed-2024 copy |
|---|---|
| Deck centre | (6208, 457, 6208) |
| Generated mixed stone and timber | (6207, 458, 6207) |
| Player construction tile | (6210, 458, 6207) |
| Raised timber soffit | X 6206–6209, Y 460, Z 6209, +Z strip |
| Travel point in region (48, 51) | X 6208, Z 6592 |

1. Arm **Place Piece**, choose **Layer**, leave thickness at **5 cm** and
   material at **Stone**. With **Next tile** and **Height 0 cm**, click the
   top of the deck tile at (6210, 457, 6207). The preview should be green.
2. Choose **Wall**, **25 cm**, **+Z**, **Oak timber**, **This tile** and
   **Height 5 cm**. Click the visible stone layer. Try the same placement
   again. The first timber fits, and the second attempt reports an overlap.
   Hover the exposed stone and then the timber. Their names and IDs should
   differ while the tile coordinates stay the same.
3. Arm **Remove Piece** and click the timber. Confirm the 5 cm stone layer
   remains. In **Walk mode**, cross it from neighbouring deck tiles and
   walk under the raised beam. Drag with the right mouse button to look up.
   The timber underside should be visible and pickable. Hover a downward
   beam face and inspect the adjacent sky readout once lighting settles.
   The exact heightmap gives **11** direct light below the opaque strip,
   whose footprint covers one quarter of its cell.
4. Return to **Orbit camera** and use **Construction site** to restore yaw.
   Hold S to decrease Z until the camera is about 70–90 m from the deck,
   then return. Watch the deck and beam across the Low-detail handover.
   The surviving stone layer and beam opening should match the nearer
   geometry. If the camera leaves mouse picking range, use the silhouette
   until returning to range.
5. With the orbit camera's reset yaw, use Shift+W to travel to Z 6592.
   Raise the orbit camera with Ctrl+wheel if the terrain obscures the route.
   Let streaming settle, click **Construction site**, and check both views
   again. Exit with Escape, load the same slot, and confirm the stone layer,
   untouched generated mixed cell and beam opening after reload.


#### Playcheck A follow-up · 2026-11-26

The maintainer confirmed that placement worked after the seed-2024 fixture
settled and in a newly generated world. Early fixture edits briefly showed
their shading without visible pieces. Above-ground chunk drawing now keeps
the last surface band while the edited band rebuilds. Retest placement soon
after entering the fixture and as a chunk hands over between views. The
Release app rebuilt, all 404 gameplay cases passed, and the coverage
refresh, complementary profiles, and build/pick/walk cases passed under
ASan and UBSan. The maintainer confirmed the visual retest and approved
Playcheck A on 2026-11-26.

**Ramp infill** fills the open space above the matching ramp profile up to
**Fill to**. The default **100 cm** fills the full complement. To preview the
snow geometry with construction materials, place a straight 1 m ramp, select
**Ramp infill** with the same facing, select **This tile**, and set **Fill to
5 cm** and **Height 0 cm**. Only a short wedge at the toe should fill. Its top
is horizontal and the higher ramp stays exposed. In step 06 the landing's
snow level sets this cap automatically.

### Step 06 · 2026-11-26

[SnowSettlement](../../src/game/world/snow_settlement.hpp) owns volume in
cubic centimetres per one-square-metre footprint column. A column fills the
surface-connected free space above its support; the capacity curve comes from
the actual convex facets through the new
[CellGeometry](../../src/game/world/tile_piece.hpp) queries
(`freeVolumeBelow`, `lowestUpwardSupport`, `lowestOpening`). Deposited volume
enters the column under its lowest upward support. Adjacent columns exchange
volume over the lowest shared boundary opening: when the combined volume
exceeds the capacity at that sill the basins merge into one horizontal level;
otherwise the basin above the sill spills into the receivers below. Equal
sills split the excess evenly, and the remainder goes to the first footprint
in discovery order. This produces the exact clipped ramp toe from the landing
level, drains a steep face to a lower floor, closes a walled toe, fills a
trough, splits a ridge, and stacks ledges.

`CellGeometry::freeVolumeBelow` clips each convex part with a horizontal plane
and integrates the closed surface. The 5 cm example on a 45° ramp gives
0.00125 m³; the adjoining 1 m² landing gives 0.05 m³, so 0.05125 m³ reaches the
5 cm line. The state carries each column's base cell and volume, pending
deposits, accepted input and accounted sink volume. It is encoded with the
engine binary stream (`SNOW` v1) so an over-budget transfer survives an
eviction or save hand-off. A bounded solve never commits partial results: the
deposit stays in the pending queue and counts as in transit.

Fixtures are headless one-cell geometry scenes; no seed or save is required.
The wedge landing is at (0, 10, −1), the ramp at (0, 10, 0), with full stone at
y=9. The cross-region case uses a two-tile wrap with the ridge at (1, 10, 0)
and the lower floor at (0, 9, 0). The diagonal-facet case has its low corner
adjacent to lower floors at (−1, 9, 0) and (0, 9, −1). The pending corridor is
100 footprints at z=0 with a ten-visit budget. All tests use `Volume` in cm³
and compare the stored, pending and sink ledger.

```bash
cmake --build build --target world_logic_tests app --parallel 6
ctest --test-dir build -R 'snow_settlement|tile_piece_free_volume' \
  --output-on-failure --no-tests=error -j 4
ctest --test-dir build -L gameplay -j 6 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang \
  -R 'snow_settlement|tile_piece_free_volume' \
  --output-on-failure --no-tests=error -j 4
```

The Release app and logic tests built. All 15 focused cases and all 419
gameplay cases passed. The same 15 focused cases passed with ASan and UBSan.
Acceptance ran headlessly; no playcheck is scheduled for this step.

V1 resolves communicating columns at one horizontal level and uses the lowest
shared opening as the spill sill. The explicit 45° friction threshold is not a
separate term yet: a steep face is shallow in free volume, so it drains to the
lowest opening, while a gentler face builds a cell-sized pocket that spills at
the next sill. This is the tuning choice reserved for playcheck B. Enclosed
cavities under an overhang stay outside the surface-connected model until
steps 10 and 16. Z edges are closed; volume above the world top is an
accounted sink. The settlement has no world save section or debug controls
yet; step 07 supplies rendering and interaction, and steps 10–11 supply dated
weather input and unloaded history.

### Step 07 · 2026-11-26

[SnowCover](../../src/game/world/snow_cover.hpp) projects the settled
columns onto the actual free space of each cell. It subtracts the cell's
convex solids from the horizontal interval between the column's support and
its measured level, producing one environmental solid with disjoint convex
parts. The landing becomes a 5 cm layer, the ramp toe the exact 1,250 cm³
wedge, and a diagonal or multi-facet support keeps its clipped pockets.
Deposits below **1 cm** are treated as dusting; anything thinner than **2 mm**
is lifted to a 2 mm visible skin so the first accumulation is visible without
z-fighting. At and above 1 cm the geometry is the measured deposit.

`ChunkManager` carries the projection as an immutable environment map. The
chunk mesher, picking, ground probe, sunlight opening and block queries
combine it with chunk pieces for the same cell revision. It is never written
into a chunk save, never marks a chunk as player-edited, and clearing it
remeshes the affected chunk and its six neighbours, revealing the original
rock, soil or grass. The new `blocks.snow` definition uses the dedicated
`weather.snow` albedo and normal textures; assets were regenerated and the
block file was appended after the existing files, preserving numeric block
IDs. Snow state is still in memory only in this step; the world save format
is unchanged and dated accumulation plus persistence arrive with steps 10–11.

#### Debug controls and fixture

Start the app from the repository root with `ANYM_SNOW_FIXTURE=1
./build/src/app/app`, open or create a world, and click **Snow site**. The
fixture is centred at

```text
x = startRegion.x * 128 + 64
z = startRegion.y * 128 + 64
y = highest sampled terrain top + 2
```

where the HUD prints the absolute `Snow fixture landing [x, y, z]`. The
landing is one square metre, so **SNOW 5 cm fixture** adds the step 06 amount
of 51,250 cm³ and reaches the 5 cm line exactly. The buttons arm a tool;
click the receiving surface to apply it. **SNOW +50 cm**, **SNOW +5 m** and
**CLEAR SNOW** cover faster burial and removal. Hovering a footprint shows
its column volume, level and depth under **Snow** in the tile panel.

| Feature | Position relative to the HUD landing |
|---|---|
| Numeric landing / straight ramp / bare upper rock | (0, 0, 0) / (0, 0, +1) / +2 through +3 |
| Chunk-seam landing and ramps | (−1 through 0, 0, −3) and (−1 through 0, 0, −2) |
| Diagonal basin and low floors | lower floors (+4, 0, 0), (+4, 0, +1), (+5, 0, 0); ramp (+5, 0, +1) |
| Draining ramp and lower floor | ramp (0, 0, +6), lower floor (0, −1, +5) |

The seam landing is two square metres; use the fixture amount twice, or
**SNOW +50 cm**, and read the HUD depth. **Snow drop** moves the camera to the
open ramp. **Walk mode** then crosses the landing and uses the same projected
surface as the ray.

#### Execution checks

```bash
python3 generate_assets.py
cmake --build build --target app world_logic_tests --parallel 6
ctest --test-dir build \
  -R 'game.world.(tile_piece_free_space_between|snow_settlement_|snow_cover_projects_settlement|environment_snow_pick_walk_mesh)' \
  --output-on-failure --no-tests=error -j 4
ctest --test-dir build -L gameplay -j 6 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang \
  -R 'game.world.(tile_piece_free_space_between|snow_settlement_|snow_cover_projects_settlement|environment_snow_pick_walk_mesh)' \
  --output-on-failure --no-tests=error -j 4
```

The Release app and logic tests built. All 17 focused cases and all 422
gameplay cases passed. The same 17 focused cases passed with ASan and UBSan.
The new `tile_piece_free_space_between` case checks the exact 0.00125 m³ wedge
and its contact faces; `snow_cover_projects_settlement` checks landing, toe,
dusting and that the support remains a chunk solid;
`environment_snow_pick_walk_mesh` checks the same projection through the chunk
ray, probe and mesher, then clears it and picks the underlying block again.
The maintainer accepted Playcheck B on 2026-11-26 and replaced the temporary
chalk material with the dedicated snow albedo and normal textures.

#### Playcheck B · Five-centimetre ramp

1. At **Snow site**, apply **SNOW 5 cm fixture** to the numeric landing.
   From the side, the upper ramp and rock stay bare, the landing top is a
   continuous level about 5 cm above the stone, and a short white wedge fills
   the toe. Hover the landing and toe: the tile panel reports about 5 cm depth
   at both. Walk from the landing into the wedge; the feet follow the snow
   surface, not the underlying ramp.
2. Repeat on the two-cell **chunk-seam** landing and on the **diagonal**
   basin. The seam must not break the level or the wedge, and the diagonal
   must split according to its low corner. Record the accepted gentler-slope
   behaviour; the 1 cm dusting transition and the wedge profile are the
   tuning points.
3. Click **Snow drop**, apply snow on the open ramp, and inspect the floor one
   metre below. The ramp face should remain essentially bare and the lower
   floor should receive the volume.
4. Apply **SNOW +5 m** to the numeric landing until it buries the ramp, then
   **CLEAR SNOW**. Inspect the restored grass/rock, the absent white geometry
   and the walking height. Capture the side view before and after clearing.

### Step 08 · 2026-11-26

[WorldCalendar](../../src/game/world/world_calendar.hpp) derives the display
date from the saved fractional `worldDays`. Elapsed day zero is day 1 of month
1 of year 1; months have 30 days and years 12 months. Day 29 is D30/M1, day 30
rolls to D1/M2, day 359 is D30/M12 and day 360 starts D1/M1/Y2. The annual
curve uses the fractional day, so month and year boundaries have no step.

[WorldClimate](../../src/game/world/world_climate.hpp) treats the generated
`Region::temperature_c` as the region's annual mean at its own surface height;
it already includes the generator's altitude cooling. The seasonal curve is a
cosine over the 360-day year with a maximum amplitude of 18 °C at the poles
and a latitude exponent of 1.5, which makes the amplitude vanish with zero
slope at the equator. Region row 0 is the northern pole, so month 1 is
northern midwinter and southern midsummer. The daily curve has an 8 °C range,
warmest at 15:00 and coolest at 03:00. Local elevation adds a 0.0065 °C/m
correction relative to the bilinearly blended regional surface. Position
sampling follows `ground_cover_climate::at`: X wraps and Z clamps, so the X
seam and the equator stay continuous. A world without a map keeps the mild
15 °C fallback and the daily curve.

The HUD time button now reads `D<day>/M<month>/Y<year> HH:MM x<speed>` and its
speed cycle includes paused time. The tile panel gained a **Climate** section
showing the date, regional mean, seasonal and daily offsets, elevation
correction and resulting local °C. The world save format is unchanged:
`worldDays` already carries the fractional day through `world.json`.

```bash
cmake --build build --target app world_logic_tests --parallel 6
ctest --test-dir build -R 'climate_' --output-on-failure --no-tests=error -j 4
ctest --test-dir build -L gameplay -j 6 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang -R 'climate_' \
  --output-on-failure --no-tests=error -j 4
```

The Release app and logic tests built. All 7 climate cases and all 429
gameplay cases passed. The same 7 cases passed with ASan and UBSan. Acceptance
ran headlessly with synthetic worlds; no seed or save fixture is required.
`climate_calendar_dates` checks days 0, 29, 30, 359 and 360;
`climate_year_curve_continuous` checks month and year boundaries;
`climate_opposite_seasons_and_latitude` checks both poles and the equator;
`climate_position_continuity` checks the wrapped X seam and the equator;
`climate_daily_range_and_elevation` checks the daily curve and lapse rate;
`climate_map_less_fallback` keeps the map-less default;
`climate_clock_pause_and_midnight_save` checks pause, accelerated time and a
`world.json` round-trip across midnight.

V1 keeps the seasonal amplitude latitude-only and the daily range constant.
Weather offsets, regional precipitation and shelter arrive in steps 09–16.
Ground-cover tints still use the annual `ground_cover_climate::at` values so
seasonal climate does not remesh terrain.

### Step 09 · 2026-11-26

[WorldWeather](../../src/game/world/world_weather.hpp) samples cloud cover,
precipitation, the weather temperature offset and surface wind from the master
seed, a model version, world position and `WorldClock` days. The field is a sum
of long plane waves whose X frequencies are multiples of the world's wrap
period, so it is spatially coherent, continuous in time and seamless across X.
Region centres are blended bilinearly exactly like `world_climate::at` and
`ground_cover_climate::at`; a map-less world samples the field directly.

The physical precipitation rate is a rain-equivalent in mm/h. A separate
0..1 visual density drives presentation; snow reaches a higher visual density
per millimetre, and dry cloud produces no particles. Conditions are `Clear`
(below 35% cloud), `Cloudy` (dry), `Rain` and `Snow`. The rain/snow choice
uses a deterministic 1 °C band around 0 °C, with `Snow` at and below the
halfway point. Precipitation now fades in with cloud cover, so a broken deck
does not drizzle from a nearly clear sky.

Presentation extends `SkyState` with cloud cover, visual precipitation,
snow fraction, animation time and a wind direction and speed. The sky program
draws a flat-shaded cumulus layer after the sun and moon, dimming them and
covering stars. Its three tones, crisp body edge and sun-facing crown follow
the reference art; precipitation darkens the same palette. Clouds are sampled
from a world-anchored three-dimensional shell, so the field no longer stretches
into curtains at the horizon, and wind translates the layer along the ground
plane instead of spinning it around a sky projection. Near rain and snow are
world-space instanced billboards in the dedicated `RenderViews::Weather` view
after the world and before glow: they fall under gravity, drift with the wind,
are depth-tested against blocks and never write depth. Rain streaks fall
downward and stay vertical; snow uses camera-facing flakes. Their counts scale
with visual density and blend across the freezing band. Cloud and particle
presentation follow a visual clock capped at 5× game speed, and stop when the
clock is paused.

The tile panel's **Climate** section now shows the climate baseline, the
weather offset and the resulting local °C. The new **Weather** section shows
the condition, cloud cover, physical rate, visual density, snow fraction, wind
speed and model version; the no-tile line summarises the camera's weather.

#### Debug controls

Launch with `ANYM_WEATHER_FIXTURE=1` to add session-only controls. Nothing is
saved; reload returns to `AUTO` and the start site.

- **WEATHER +6H / +1D / +30D / +180D** jump game time. The existing time
  button still cycles pause and accelerated speeds, which also animates the
  clouds and particles.
- **WEATHER FORCE: AUTO/CLEAR/CLOUDY/RAIN/SNOW** overrides presentation only.
  The sampled temperature and weather offset stay visible in the climate
  section, and a forced sample is marked in the weather section.
- **WEATHER SITE: START/NORTH POLE/EQUATOR/SOUTH POLE/X SEAM** teleports the
  orbit camera to a region centre. `X SEAM` stands at tile X 0.5. Wait for
  streaming after a jump.

```bash
python3 generate_assets.py
cmake --build build --target app world_logic_tests lod_render_smoke \
  shape_benchmark --parallel 6
ctest --test-dir build -R 'weather_' --output-on-failure --no-tests=error -j 4
ctest --test-dir build -L gameplay -j 6 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang -R 'weather_' \
  --output-on-failure --no-tests=error -j 4
```

The Release app, logic tests, smoke harness and shape benchmark built. All 6
weather cases and all 435 gameplay cases passed. The same 6 cases passed with
ASan and UBSan. The shaders were also compiled directly through `shaderc`
with a checked exit status; `assets/definitions.json` gained `weather`.
`weather_field_reproducible_and_seeded`, `weather_x_seam_is_continuous` and
`weather_time_is_continuous` check determinism, the wrapped X seam and day,
month and year boundaries. `weather_condition_thresholds` checks the four
conditions and the freezing band; `weather_temperature_and_rate_bounds`
checks the rate, offset and visual bounds; `weather_opposite_seasons` checks
that the weather result keeps the opposite annual curves at the two poles.

Visual presentation is unverified in the sandbox: Vulkan could not open a
window here. The app and the device smoke target built, so Playcheck C is
ready for the maintainer.

#### Playcheck C · Weather and seasons

From the repository root:

```bash
ANYM_WEATHER_FIXTURE=1 ./build/src/app/app
```

Load a world with known seed and date, or create a new one. The weather
buttons are in the two rows above the snow controls. The HUD time button
shows D/M/Y and speed; **WEATHER SITE** prints the chosen site in the action
line.

1. At **START**, read the winter sample in **Climate** and **Weather**. Click
   **WEATHER +180D** and compare the same region in summer. The baseline
   should swing by roughly twice the latitude's seasonal amplitude, while the
   weather offset stays within ±8 °C. The daily range should remain 8 °C at
   the same hour.
2. Cycle **WEATHER SITE** through **NORTH POLE**, **EQUATOR** and
   **SOUTH POLE**. Wait for streaming after each jump. At one date the poles
   show opposite seasons and the equator shows little seasonal swing. Snow is
   expected at the winter pole even when the same date rains at the equator.
3. Cycle to **X SEAM**, then orbit or walk across tile X 0. Watch a cloud band
   cross the seam without a jump in cover or precipitation. Travel into the
   next region and confirm the field blends rather than switching.
4. Use **WEATHER +6H** and **+1D** to follow a band. Find a dry cloudy period,
   rain, and a rain-to-snow transition near 0 °C. **WEATHER FORCE** can preview
   each presentation immediately; set it back to **AUTO** to inspect the real
   sample. Rain falls as thin downward streaks; snow falls as larger white
   flakes. Both are world-space and should be hidden by blocks in front of
   them. Watch the reported wind move the clouds and slant the particle drift.
5. Pause with the time button, note the date and both HUD sections, then quit
   and load the same slot. The same date, position and seed must produce the
   same cloud cover, precipitation rate and offset. Forced presentation must
   be back at **AUTO**.

Expected result: cloud cover visibly follows the reported cloudiness; near
precipitation is visible only for the reported rain or snow; the HUD reports
date, baseline, weather offset, resulting °C and physical rate; the wrapped
X seam and region hand-over show no step.

#### Deviations

Cloud cover and near precipitation are presentation layers driven by the
regional sample. They do not yet stop under a roof, accumulate snow or melt
it; exposure, dated accumulation and outdoor melt remain step 10. The debug
force is deliberately session-only and never changes the sampled ledger.

#### Follow-up · 2026-11-26

The first device check reported four presentation problems: rain and snow
moved upward, particles read as a flat screen overlay, clouds tore into
curtains near the skyline, and the palette did not match the cel-shaded
reference. The maintainer accepted the sampling, HUD, controls and station
behaviour otherwise.

The fix keeps the weather sample but replaces the presentation. Rain now uses
world-space instanced billboards whose centres fall with time; the previous
screen-space grid was stepped the wrong way. Snow uses the same world-space
path, so both are depth-tested against blocks and drift with wind. Clouds are
sampled from a three-dimensional shell around the view direction instead of a
radial sky plane. Wind now translates that shell along the ground plane, so
clouds slide horizontally instead of spinning around the dome, and the
mapping no longer tears into curtains at the horizon. Cloud and near-particle
drift accumulate wind over time on the CPU; multiplying a freshly sampled wind
direction by the whole elapsed time was what made a ten-metre walk shift the
sky. Presentation uses a separate visual clock capped at 5× game speed so
debug time acceleration cannot make the sky spin. The palette uses three
flat tones
with a crisp body edge and a sun-facing crown in the style of the reference
images in `/tmp/clouds-style`. The sample gained surface wind: direction and
speed come from two more seeded fields, clouds drift from the wind, and
particle drift follows the same vector. Wind speed is reported in the Weather
section.

```bash
python3 generate_assets.py
cmake --build build --target app world_logic_tests lod_render_smoke \
  shape_benchmark --parallel 6
ctest --test-dir build -R 'weather_' --output-on-failure --no-tests=error -j 4
ctest --test-dir build -L gameplay -j 6 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang -R 'weather_' \
  --output-on-failure --no-tests=error -j 4
```

The app and device targets built; all 6 weather cases and all 435 gameplay
cases passed, and the 6 weather cases passed with ASan and UBSan. The shaders
were compiled with checked `shaderc` exit status; `weather_particles` replaced
the old `weather` overlay program. The maintainer confirmed the retest and
accepted Playcheck C on 2026-11-26.

### Step 10 · 2026-11-26

[Precipitation](../../src/game/world/precipitation.hpp) traces downward to
the first intercepting solid. The sky heightmap skips the known overhead
air. Ready slabs also contribute any solids above that bound, including
zero-opacity pieces and fresh edits. Saved edit slabs can conservatively
raise the bound while their geometry is pending. Loaded cells use actual
vertical intervals, so a glass pane intercepts like stone and a ray through a
shape opening continues downward. Near particles use the same query at
their position and are discarded if their lower end would fall through the
surface. Particles in the open beyond a doorway are still visible from
inside.

The tree fixture follows the world representation. Voxelised trunk and thick
limb cells intercept precipitation. Instanced leaves and thin branches pass
it through. Snow settling off a ledge or through the side of a shelter
continues through the step 06 gravity solver.

Active weather samples a 9 × 9 footprint square around the camera focus
once per real second. Each square metre uses four equal-area rays. Roof and
floor deposits at different support cells can coexist in one column. The
gravity solve considers supports within two horizontal tile steps of each
input. Before a build tool changes a roof, the current interval is applied to
its old exposure. Removing a load-bearing piece can then drop its existing
snow separately. Pausing contributes zero game time. Accelerated intervals
use up to four chronological midpoint samples per update. The inspection
fixture's date jumps skip physical simulation. Unloaded history belongs to
step 11.

The snow rate is the liquid-equivalent precipitation rate multiplied by the
local snow fraction. A fixed density of 200 kg/m³ converts 1 mm of snow water
equivalent (SWE) to 5 mm of settled snow on flat ground. Depth over a slope
comes from the clipped volume. Outdoor melt uses 0.5 mm SWE per positive °C
per game hour, scaled by the exposed area. One cubic centimetre of bulk snow
becomes 0.2 cm³ of liquid. Meltwater stays at the former support in a ledger
for step 18. Weather leaves the original grass and plant data in place.

The world saves deposited snow, pending transfers and meltwater atomically
to `snow.dat` (SNOW v2). Earlier saves start with an empty ledger. The
projection is rebuilt as the supporting geometry streams in. For constant
rates and unchanged exposure, rounding each contribution to the nearest
bulk cubic centimetre allows a difference of up to 6 cm³ per receiver
between ten intervals and one combined interval. A separate melt phase has
the same tolerance. Different sampling dates, a changing snow line or a
different transfer path can redistribute the same volume.

#### Fixtures

The headless scene uses a one-metre footprint, a floor at Y 2 and an optional
hatch at Y 7. The roof case adds an optically clear elevated pane and a tall
cave gap. The edge case puts a half-width roof at Y 6 above a ramp at Y 3,
with two rays through the opening at wrapped X. These fixtures are
deterministic and use no seed or save slot.

For an optional in-game check, copy the step 01 seed 2024 comparison save
into a disposable slot or use the snow fixture on any world.

```bash
ANYM_WEATHER_DEPOSITION_FIXTURE=1 ANYM_SNOW_FIXTURE=1 ./build/src/app/app
```

Select **Snow site**, let geometry stream in, then pause the clock. Cycle
**WEATHER FORCE** to **SNOW** and press **WEATHER APPLY 30M**. This supplies
2 mm SWE, or 1 cm of equivalent depth on a one-square-metre pad before it
spreads. The **Snow** panel reports interception height, bulk volume,
settled level, SWE and stored meltwater. For a hatch, build a thin horizontal
piece above an exposed pad between two 30-minute intervals. The first
interval reaches the floor and the second reaches the hatch. Removing the
hatch may drop snow it has already caught. **RAIN** supplies liquid rain
with zero snow input. **CLEAR** runs a +5 °C dry melt, uncovering the prior
surface and adding to the meltwater total. Fixture forcing resets on reload.

#### Execution checks

```bash
cmake --build build --target app world_logic_tests --parallel 6
ctest --test-dir build -L gameplay -j 6 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 3
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang \
  -R 'weather_|snow_settlement|snow_cover' --output-on-failure --no-tests=error -j 4
```

The Release app and test target built. All 440 gameplay tests passed,
including the five new exposure, hatch, accounting and vegetation cases.
The 26 focused weather and snow cases passed with ASan and UBSan. Validation
ran headlessly. The near-particle change has no device capture for this
step, and the handoff schedules no maintainer playcheck until step 11.
