# Weather, temperature, snow and ice

**Steps 01–19 complete · Step 20 pending.**

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
| 11 | Unloaded weather history and elapsed-time reconciliation | Complete | D · Shelter and travel |
| 12 | Regional weather at distance | Complete | E · Weather from afar |
| 13 | Polar baselines, vertical runs and excavation | Complete | |
| 14 | Deep-pack LOD, structures and travel | Complete | F · Arctic shelter |
| 15 | Local heating and cooling from blocks and items | Complete | |
| 16 | Room air, wall heat storage, local melt and setpoint heaters | Complete | G · Heated room |
| 17 | Partial-cell water capacity and connectivity | Complete | |
| 18 | Water–ice transfers, persistence and meltwater | Complete | |
| 19 | Surface ice in still and flowing water | Complete | H · Frozen shore |
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

Use deterministic regional snow-depth baselines to initialize undisturbed packs directly. Evaluate how much per-footprint catch-up this avoids, while preserving dated weather changes and sparse records for edits and shelter.

**Verify.** Generate a 16 m fixture with the pack crossing a slab boundary. Excavate the middle of a column and keep the snow above and below. Remove the roof of a buried room and apply new snowfall. A side entrance under a roof stays sheltered. Validate compact storage, excavation across region borders, save/reload and the old-world policy.

Generator 28 samples 08:00 on the first day. Regional depth uses the annual
temperature and the starting season to estimate retained snow, with humidity
scaling the available stock. Depth blends between region centres across
wrapped X. Dry terrain with a full-cube top and a slope below one metre
per tile can hold the pack. Latitude and slope reduce depth continuously
to dusting. The initial stock is projected into the open space above the
generated ground. Nearby solids retain their own tile data. Structure
coverage and distant silhouettes follow in step 14.

The **16 m** limit is measured from the original pack support, including
after excavation splits the column. The upper surviving run takes exposed
snowfall and melt. Cuts remove the picked cell interval and leave the
remaining runs in place. The initial interval needs no save record. A
changed footprint stores its cap and surviving intervals in `snow.dat`
v5, including an empty record if excavation or thaw removed it all; v6 adds
the footprints that track exact local weather. Loose
snow settles through gravity between exposed loose-snow receivers.
Established packs receive new snow through their exposed upper surface.
Saturation is a closed storage boundary
for each weather receiver. Excess goes to the accounted snow-volume sink
at the interval in which it arrived. At the current density, five cm³ of
sink volume represents one cm³ of water. The sink is a recorded loss from
the simulation, and melt keeps its existing pending water ledger.

In older worlds, previously applied weather, existing loose deposits and
pending transfers retain their saved state. A footprint without its own
weather cursor starts at the recovered saved date with zero initial stock.
Migration happens once and the new date is saved with the cursors.

### 14. Carry snow into distant terrain and structures

Make terrain LOD sample the same dated pack summaries and edited surface intervals. Resolve roofs and snow-covered structures at the structure handover. Update silhouettes, sky occlusion, shadows, flora visibility and walkable surfaces as depth changes. Limit invalidation by changed footprint and quantized geometry changes. Underground voids belong to exact nearby geometry; preserve them in the saved data when a far patch simplifies the exterior.

**Playcheck F.** Find the seeded 16 m pack, cut a side entrance and room across a chunk boundary, and add a roof or structural support. Watch snowfall outside. Leave to distant view, approach again, travel three regions away, and reload. Inspect the silhouette, tunnel headroom, snow above it and grass or rock at the excavated floor.

Repeat the x7200 → x1 performance check on the same save and at the same camera position. Record frame times during acceleration and after returning to x1, alongside the work spent replaying weather and updating snow projections.

### 15. Measure local heat and cold

Add a data-defined thermal source with signed output and an effective range. Expose it from a placed block and from an item at its current world position; use a movable debug item if the inventory path is unavailable. Attach, move and remove sources through their owners' lifecycles. Provide a local °C query usable by snow, ice and future actors. Use an indexed local neighbourhood and measure attenuation with distance and barriers. Provide a dated regional contribution interface for future large fires. Record the handover convention so a source contributes once where coarse and detailed simulation meet.

**Verify.** Use one heater, one cooler and an overlapping pair. Sample at specified distances and behind shelter. Move a carried source across a region boundary, drop or remove it, save and return. Check for residual registered sources and temperature jumps at handover. Document configurable units and ranges alongside the item data.

### 16. Let rooms and walls store heat

Compute connected indoor air in the active area with leakage through openings, exposure to outdoors, and heat exchange with surrounding material. Treat warmed walls as finite stores that can release heat into adjacent air. Use baseline ground temperature and insulation for sheltered caves. Bound connectivity work across unloaded boundaries; record the approximation there and invalidate it on geometry changes. Persist significant stored heat and its timestamp; cooling during absence should match the documented approximation. Feed the same local °C into snowmelt and later phase changes.

**Configurable setpoint heaters.** Step 15's `ThermalEmission` remains an open-loop, fixed offset. Step 16 adds an optional thermostat controller so a block or item can hold a room at a requested temperature such as 21 °C. The controller must use the connected room-air temperature, not a single point sample presented as the whole room:

- Extend the thermal data with a mode (`fixed` by default, or `target`) and, for target mode, `target_c`, `deadband_c`, `gain`, `min_delta_c` and `max_delta_c`.
- Each simulation tick, set the source's effective `delta_c` from `clamp(gain × (target_c - room_temperature), min_delta_c, max_delta_c)`. Support positive and negative output, hysteresis through the deadband, and anti-windup so a door left open cannot accumulate an unbounded commanded output.
- Rooms with little connected air or large leakage need the controller to saturate at `max_delta_c`; that is expected, and the inspection panel should show the target, measured room temperature, current command and saturation.
- Move the source through the same block/item owner lifecycle as step 15, update it at the room simulation cadence rather than every frame, and keep the existing local/regional handover key.
- Persist the thermostat setting and any significant controller state with the room's stored heat. A reload must not silently switch a configured heater back to fixed mode or forget its target.
- A fixed `delta_c` source keeps its step 15 behaviour and requires no migration.

**Playcheck G.** Measure a heater outdoors, then inside the fixture room. Open the door, close it, turn off the heater and observe air and walls cooling at different rates. Remove a roof piece during snowfall and check exposure and heat loss. Repeat with the cooler. Inspect a patch of snow beside the warm wall and another farther away. Reload while the wall is still warm. Also configure a heater for 21 °C, open the door during cold weather and watch its command rise toward saturation, then close the door and watch it settle into the deadband. Compare it with a fixed-offset heater and reload to confirm the target and measured room temperature survive.

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

**Carry-over from step 19.** Seasonal ice outside the generated
`ICE_SHEET`, `GLACIER` and `SEA_ICE` features still lacks the snow cap that
feature ice receives from generated columns. Step 21 must fix snow on that
derived seasonal ice using the same partial-cell occupancy treatment that is
planned for water in partially solid tiles, so the snow sits on the actual
ice surface rather than on a whole-cell approximation.

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
opening. Per-piece water capacity arrives in step 17.

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

### Step 11 · 2026-11-26

[Dated weather summaries](../../src/game/world/weather_history.hpp) sample
the seeded regional field at the centre of each 16 × 16 m cell and each
30-game-minute interval. The results carry the weather model and simulation
versions, physical rate, temperature and presentation fields. A bounded
64-entry cache shares sampled days between footprints. X wraps, and cells at
the Z edge use the last in-bounds row. Reading a summary needs only the
world map, with no voxel requests.

The active 9 × 9 m window replays completed intervals. The same replay
handles first visits and returns. It uses four precipitation rays against
the current raw solids, then applies melt and snowfall in date order at
each receiving support. The support elevation adjusts the sampled
temperature. A catch-up batch processes up to seven game days per
footprint. The owner attempts at most eight successful batches per
update, checks a 2 ms time budget between footprints and retries up to
4,096 settlement work units for transfers. Automatic catch-up attempts
run at most ten times per real second while behind and once per second
when current. Missing receiving geometry or water information leaves the
cursor unchanged. Pausing stops new weather time, while replay of
already elapsed time can continue.

Player placement and removal consume history through the exact edit date
with the pre-edit exposure. If catch-up is still pending, the tool
prioritizes that footprint for subsequent updates before accepting the
edit. New or migrated worlds start the ledger at the saved world date.
The sparse progress map stores only footprints that have applied weather,
including dry sites. Unvisited footprints use the epoch by default.
`snow.dat` (SNOW v3) stores the epoch, clock checkpoint, versioned
cursors and dated physical fixture overrides along with snow, pending
transfers and meltwater. Resting snow and its applied date commit together
in one file replacement. SNOW v1 and v2 retain their saved material
ledger. A physical force closes on load and the dated period remains
available for unfinished replay. The ordinary presentation override
continues to affect presentation alone. Debug date jumps now advance the
physical history as well.

The owner projects one dirty chunk column at a time from resident
geometry and the committed ledger, at most eight times per real second
during automatic updates. Pending changes to a column coalesce, and
untouched slabs keep their immutable environment snapshots. A mesh
outside the dirty slab is invalidated only when snow changes at its
shared face. Chunk meshes use the existing topology revision checks.
Unloading drops the projection for the released chunk column while
preserving the material ledger. A support that is missing or mismatches
the recorded geometry is left out of the projection until it can be
resolved. Projection heights use the clipping planes' one-micrometre
tolerance. Settlement keeps the original volume precision.

#### Accuracy and limits

- With fixed exposure and an isolated support, replay agrees with the
  same dated interval sequence applied one interval at a time. Bulk
  additions and removals round to the nearest cubic centimetre, at most
  0.5 cm³ of each per interval and receiver. Splitting an interval at an
  edit or a fixture control adds another rounding. The shelter and
  full-year headless cases measured zero difference at matching dates.
- The summaries approximate the continuous weather field using 30-minute
  midpoint samples on a 16 m grid. Potential precipitation error is bounded
  by the model's 16 mm SWE/h rate cap. For a mapped footprint, the
  temperature error at equal elevation is at most
  `3.2 °C + (8/128) × (Dx + Dz)`, where `Dx` and `Dz` bound adjacent
  regional climate differences along each axis at the same elevation and
  date. The 3.2 °C covers the spatial weather offset, the fastest weather
  wave, and the diurnal and annual temperature curves. The corresponding
  bound on potential melt rate is half the temperature bound in mm SWE/h
  per square metre of exposed area.
- In a multi-day batch, snow spends the thermal calculation at its
  interception support. Gravity distributes surviving snow after the
  batch. On connected slopes and ledges, this can shift the place and
  timing of melt or change depths with visit order. The worst-case
  volume difference at a receiver is bounded by the starting snow plus
  snowfall and incoming transfers available to the connected catchment
  over the replay. The balance sheet conserves calculated snow input
  through resting volume, transit, sink and meltwater.
- Replay uses current generated tree geometry and liquid coverage for
  unloaded dates. Player edits consume their prior exposure before
  changing it. Generated tree growth and shoreline changes have no dated
  geometry history in this step. The four rays sample exposure in
  quarter-square-metre increments, with a worst-case error of one
  square metre for shapes that pass between all four rays. These
  approximations can matter near a changing shoreline or thin opening.

#### Playcheck D · Shelter and travel

The disposable fixture uses the step 01 seed 2024 save, generator 27 and
Low terrain detail. Its two sets of pads sit four regions apart.
The exposed floor is two tiles west of each site centre and the covered
floor is two tiles east. The table gives their coordinates.

| Site | Exposed X | Covered X | Z |
| --- | ---: | ---: | ---: |
| A | 6206 | 6210 | 6208 |
| B | 6718 | 6722 | 6208 |

The HUD reports the floor Y, both floor volumes and the world day applied
to each pad. The fixture starts paused and limits deposition sources to
the four pads, so surrounding terrain does not add snow to the
comparison. Use the same fixture flag when reloading. The repository's
baseline save is copied into the disposable slot.

```bash
mkdir -p build/weather-travel-playcheck/saves
cp -an build/weather-baseline/weather-walk_saves/2024_2026-11-25_22-28-13 \
    build/weather-travel-playcheck/saves/
ln -sfn ../src/app/assets build/weather-travel-playcheck/assets
cd build/weather-travel-playcheck
ANYM_WEATHER_TRAVEL_FIXTURE=1 ../src/app/app
```

1. In **Load World**, select seed 2024 dated 2026-11-25 22:28:13.
   Click **Travel site A** and let chunks stream in. Click **CLEAR SNOW**
   to reset both pads and start the comparison at the current date.
2. Cycle **WEATHER FORCE** to **SNOW** and click **WEATHER +6H**.
   Wait for A's dates to reach the target. The open floor should hold
   about 120,000 cm³ (120 mm) and the covered floor should stay at zero.
   B should still show the date at which you cleared snow.
3. Cycle through **AUTO** to **CLEAR** without advancing time. Click
   **WEATHER +6H** again. A's exposed floor should hold about
   45,000 cm³ (45 mm) after the thaw. Its covered floor stays dry.
4. Click **Toggle catch-up hold**, then **Travel site B**. Let its
   geometry stream in and use **Catch up 1h** eight times. The open pad
   should have about 95,000 cm³ with four hours of thaw still to apply.
   Use Escape, then load the same slot. The fixture resumes automatic
   catch-up at the saved clock date.
5. Once B's dates reach the target, compare the pads. Both covered
   floors should report zero. The open floors should agree within
   12 cm³ (0.012 mm), allowing rounding when the manual steps split
   intervals. Reload once more at the same date and check that both
   volumes stay fixed.

#### Execution checks

```bash
cmake --build build --target app world_logic_tests --parallel 6
ctest --test-dir build -L gameplay -j 6 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 3
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang -j 4 --output-on-failure \
  --no-tests=error -R 'weather_|snow_settlement|snow_cover'
```

The Release app built, all 446 gameplay tests passed, and 32 focused
cases passed under ASan and UBSan. The shelter case yielded 45,000 cm³
on both exposed pads and zero on both covered floors, including a
checkpoint during thaw and a second replay at the same date. The
seed 2024 full-year case used 52 bounded batches per site and produced
identical saved ledgers when reversing visit order and comparing against
daily replay. Additional cases covered an edit at a fractional date, X
wrapping, Z clamping, legacy snow data, and a pending transfer after
reload. The sandbox could not open display `:0`, so the in-game fixture
and visual confirmation remain part of Playcheck D.

#### Accelerated-clock follow-up · 2026-11-26

The maintainer saw snow forming and reported sustained 5–15 FPS after
using x7200 and returning to x1. The old replay loop could run every frame
while behind. An update with any volume change rebuilt snow over all
resident columns and invalidated the affected slabs and all six
neighbours.
Projection now follows dirty column changes and publishes at a bounded
real-time rate. The saved weather cursor still advances in date order
and preserves the remainder across a change of clock speed.

A Release CPU comparison used a 48 × 48 m mixed flat-and-ramp snowfield
with 2,678 projected cells. After one footprint advanced, the old update
reprojected the full snowfield and took 138 ms in the initial run. The
updated path republished one chunk column in roughly 12–13 ms in two
runs. The regression test verifies that an untouched column receives no
geometry queries and keeps its prior snapshot. A second test covers mesh
revision changes on a shared face. The Release app built, all 447 gameplay
tests passed, and 43 focused weather, snow and compound geometry tests
passed under ASan and UBSan. Live frame rate remains unverified.

The maintainer approved marking step 11 complete and reported that FPS
still drops after using x7200. Further optimization is deferred to steps
13–14. Step 13 will evaluate predefined regional snow depths to reduce
per-footprint work, and step 14 will repeat the accelerated-clock
performance check.

### Step 12 · 2026-11-26

Weather presentation selects regions inside the view and the preset's
512 m, 768 m or 1,024 m range. A fixed cache holds the two surrounding
Step 11 dated samples from the weather cell nearest each region's centre.
The installed weather and simulation versions are fixed for that cache's
lifetime. Interpolation over the dated samples keeps changes continuous.
Phase is derived at the representative regional terrain height. This is
a presentation approximation at 128 m resolution. Per-column settlement
still uses its own geometry, weather cells and replay dates.

One instanced draw submits as many as five depth-tested slices per visible
wet region. The shader feathers across 8 m at the region edge, tints rain
with regional cloud cover, and gives snow a lighter, softer veil. The far
pass fades in from 18 m to 26 m horizontally from the eye while the near
particle pass fades out using the complementary weight. Near particles
sample the region they occupy and still check whether solid geometry above
them intercepts precipitation. The far pass takes world-map metadata and
dated weather samples. It has no chunk or voxel access. Both passes use
the camera-relative world depth buffer and the paused visual clock.

#### Playcheck E · Weather from afar

Use the inland seed **2024** fixture at **day 1/3 (08:00)**.
The save generator runs headless. Keep its working directory separate
from regular saves.

```bash
cmake --build build --target app lod_render_smoke --parallel 6
mkdir -p build/weather-distance-v2-playcheck/saves
build/tests/lod_render_smoke build/weather-distance-v2-playcheck/saves 0 weather_distance_save
test -e build/weather-distance-v2-playcheck/assets ||
  ln -s ../src/app/assets build/weather-distance-v2-playcheck/assets
(cd build/weather-distance-v2-playcheck && ANYM_WEATHER_DISTANCE_FIXTURE=1 ../src/app/app)
```

1. Open **Load World** and select the seed 2024 slot. Click **Weather ridge**.
   The eye is at X/Z **172/6242** at 104 m over the generated surface.
   Clear is region (1, 48), heavy rain is (0, 48) to the west, snow is
   (1, 49) ahead to the south, and cloudy/dry is (0, 49). Look left for
   rain under a dark cloud bank and right for lighter snow under its
   cloud bank. The foreground and the sky directly overhead should stay clear.
2. Click **Weather boundary** for X/Z **136/6242**, 8 m above the ground
   and facing west. Hold **W** briefly to move across X 128 into rain,
   then **S** to return. Curtains should give way to nearby streaks
   as you enter, with the foreground clear on the dry side. The debug
   panel shows both instance counts and the voxel-streaming counters.
3. Repeat with **Weather seam** at X/Z **44/6242** and **Weather seam edge**
   at **8/6242**. The west neighbour is now region **(95, 48)**.
   Cross X 0 and check that the rain stays in the west region through
   the wrap. Compare Low with Medium or High by changing terrain detail
   in Settings and reloading the world.
4. The fixture opens paused. Use the clock button to resume and pause again.
   The curtains and particles should stop moving while the camera can still
   cross the boundary. Use Escape and reload the same slot to check its date.
   Select the same named site again to restore the fixture's layout.
   The weather force is session-only and leaves physical accumulation on
   the saved weather model.

#### Initial execution checks

```bash
python3 generate_assets.py
cmake --build build --target app world_logic_tests lod_render_smoke --parallel 6
ctest --test-dir build -L gameplay -j 6 --output-on-failure --no-tests=error
```

Both Vulkan shader stages compiled, the Release app and device harness
built, and all 449 gameplay tests passed. The new tests cover the four
conditions, X wrapping, the Z edges, the near fade, reuse of dated samples
across presets, and deterministic sampling after a time jump and reload.

The [Vulkan comparison](../../tests/README.md#weather-at-distance) used seed
2024 at day 1/3 with a 1280 × 720 capture. The stationary eye was at X/Z
36/6242. The crossing moved it west one metre per frame to X −27, while
descending toward the surface. Both phases stayed paused. Frame time
includes terrain LOD work, draw submission and flush, excluding the
harness sleep and capture. Weather CPU time includes the near exposure
queries against the generated surface in this harness.

| Preset | Phase | Summary rebuilds | Far draws / frames | Peak wet regions / near particles | Frame p50 / p95 (ms) | Weather CPU p95 (ms) |
|---|---|---:|---:|---:|---:|---:|
| Low | Stationary | 0 | 32 / 32 | 2 / 0 | 0.58 / 0.68 | 0.013 |
| Low | Crossing | 3 | 64 / 64 | 2 / 1,204 | 0.93 / 2.01 | 0.627 |
| Medium | Stationary | 0 | 32 / 32 | 2 / 0 | 0.46 / 0.62 | 0.016 |
| Medium | Crossing | 6 | 64 / 64 | 2 / 1,204 | 0.60 / 1.10 | 0.581 |
| High | Stationary | 0 | 32 / 32 | 2 / 0 | 1.06 / 1.14 | 0.022 |
| High | Crossing | 5 | 64 / 64 | 2 / 1,204 | 1.10 / 2.32 | 0.549 |

All three runs reported zero voxel generation work. The dry control
submitted zero precipitation draws and particles. The paused ridge
capture shows the rain bands to the west and the lighter snow veil
to the south. The post-wrap capture shows near rain streaks on the wet
side. Captures and logs are under `build/weather-distance-check/`.
Medium needed more than 2,000 frames for terrain LOD to settle after
the crossing. The documented command allows 6,000 frames.

#### Playcheck E follow-up · 2026-11-26

The first playcheck found that the coastal fixture stalled after opening the
save. A main-thread backtrace showed the first frame waiting for a connected
ocean survey while checking whether the camera was underwater. Unloaded
camera cells now use published hydrology previews and resident tiles still
supply exact water. The replacement fixture uses an inland map to keep the
weather comparison away from ocean survey costs.

The first pass drew repeated sheets with visible ridges, and the camera's
local cloud cover could leave blue sky above a neighbouring downpour. The
sky, distant precipitation and near particles now read the same dated
weather grid. The cloud shader projects weather at its world position and
varies the cloud base with the regional terrain. The precipitation pass
accumulates light attenuation through depth-tested samples without placing
a curtain at the centre of each region. Natural weather interpolates across
the grid. The four-condition fixture keeps marked edges to make it easy to
see where the clear side ends.

Weather model 2 uses moving storm systems with a main scale of 3.6 km and
weaker detail at 1.45 km. The same system controls both cloud cover and
physical precipitation. Medium and heavy precipitation form broad areas,
with cloudy/dry edges and clear gaps between systems. Terrain detail selects
a precipitation and cloud draw range of 1,024 m, 1,536 m or 2,048 m,
capped by the visible terrain range and half the world circumference.
Existing snow saves retain their deposits and replay the original weather
model through the last saved interval if it was partially applied. The
switch to the new model is recorded in the snow checkpoint.

The Release app rebuilt with both shader programs, and all 451 gameplay
tests passed. The new coverage checks broad wet systems, legacy weather
replay after migration, and preview water before and after an exact
hydrology result is published. In the app, the inland slot loaded through
**Load World**, accepted the ridge jump, saved on Escape, and reopened at
08:00. The old coastal copy could enter gameplay again and accept the ridge
jump. Its exit did not finish within the test window.

The follow-up Vulkan runs sampled 32 stationary and 64 crossing frames at
1280 × 720. The paused eye started at X/Z 44/6242 and crossed X 0 to −19.
The harness draws distant terrain through the weather range plus 512 m.
Each wet run used one distant weather draw per frame, showed two forced
wet regions, and peaked at 1,001 near particles on the wet side. The dry
control retained the same clouds and drew zero precipitation. All runs
reported zero voxel generation work.

| Preset | Summary rebuilds, stationary / crossing | Frame p95, stationary / crossing (ms) | Weather CPU p95, stationary / crossing (ms) |
|---|---:|---:|---:|
| Low | 0 / 19 | 0.66 / 1.91 | 0.194 / 0.559 |
| Medium | 0 / 27 | 1.01 / 1.95 | 0.204 / 0.739 |
| High | 0 / 35 | 1.92 / 6.01 | 0.258 / 0.654 |

The natural capture uses the same seed and date from X/Z 9800/10600,
facing east into a broad storm. A final Low repeat after smoothing the
cloud detail and wrapping the rain texture passed the seam crossing. Its
stationary and crossing p95 frame times were 0.85 ms and 1.77 ms.
Captures and logs are under `build/weather-distance-v2-check/`.

The maintainer approved the revised result for Playcheck E and confirmed
that step 12 is complete.

### Step 13 · 2026-11-26

The headless pack fixture uses seed 424242 and a uniform cold, humid
starting climate. The 16 m run spans Y 12–28, crosses the slab seam at
Y 16, and covers both sides of the X 127/128 region border and the
X 255/0 world seam. The storage check projects 1,024 untouched footprints
with no change to the save payload. Four excavated footprints use 29 bytes
each for the key, original cap and two surviving intervals. The editor
check uses a raycast, the versioned piece-removal path and the player
ground probe on the same projected geometry.

The weather check cuts a side entrance and keeps both room roofs. Snow
landing on a saturated roof goes to the sink while the floor stays dry.
Opening the first roof lets 5 cm of new snow reach that floor. A thaw lowers
the cap to 3.75 cm and credits 2,500 cm³ of meltwater. The adjacent roofed
floor stays sheltered. The save checks also reopen the edited intervals,
reject a truncated payload and confirm that an older world's existing
deposit survives while an unseen footprint starts at the saved date.

A Release CPU comparison resolved 256 undisturbed footprints in about
0.03 ms without evaluating a weather interval. Reconstructing the same
16 m per footprint from constant 10 mm/h snowfall took about 12.5 ms and
163,840 half-hour samples. This measures creation of the initial stock.
Weather from the start of play still replays in date order, and geometry
projection is outside this comparison.

```sh
cmake --build build --target app world_logic_tests --parallel 5
ctest --test-dir build --output-on-failure --no-tests=error -j 6
build/tests/world_logic_tests polar_pack_direct_baseline_work
cmake --build build/tests-san-clang --target world_logic_tests --parallel 5
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/tests-san-clang \
  --output-on-failure --no-tests=error -j 4 \
  -R 'polar_pack|snow|weather|environment_snow|compound'
```

The Release app built and all 461 tests passed. The focused ASan and UBSan
run passed all 46 cases. LeakSanitizer could not attach in this sandbox,
so the sanitizer run disabled leak detection.

#### In-game fixture

Run from the repository root with `ANYM_POLAR_SNOW_FIXTURE=1 ./build/src/app/app`.
Create a separate generator 28 world, then select **Polar pack**. The scene
uses a 6 × 6 m base that straddles the east border of the selected starting
region. Its Z coordinate is the centre of the region. The debug panel shows
the border X, floor Y, first Z and pack top. The base begins four metres
below a slab boundary and the world starts paused.

Use **Remove Piece** to cut a two-metre opening at floor Y + 2 and + 3.
Cut across the region border and inspect the remaining floor and roof.
Cycle **Weather Force** to **Snow** and use **Weather Apply 30M**. With
the roof intact, the tile inspector reports cover over the floor. Open
a shaft down to the floor and apply another 30 minutes to see the exposed
snow cap change. Save with Escape and reopen under the same launch flag
to see the cuts persist. Layered lighting and distant terrain remain
part of step 14. The live fixture was not exercised here because the
sandbox could not access the display.


### Step 14 · 2026-11-26

Terrain LOD samples the generator 28 starting pack and the latest published
weather or excavation caps from the same snow ledger as the detailed chunks.
An empty edited interval suppresses the starting pack. Distant terrain draws
the exterior as a height field, so a side tunnel can disappear at distance
while its roof remains visible. The saved runs retain the exact tunnel and
its floor. The chunk handover includes the height range from the underlying
terrain through the snow cap.

Loose deposits on roofs and shaped surfaces use the structure proxy path.
The proxy clips snow against the terrain and the structure and preserves its
layer thickness as the horizontal footprint coarsens. After unload it keeps
the measured cap. Immediately after reload, an unloaded loose deposit uses
its saved support height and volume over one square metre until the exact
geometry is available. Detailed collision, picking, and sunlight use the
clipped cell geometry. Fully buried plants are hidden. Snow opacity also
feeds local block light and distant sky occlusion. The existing mesh caster
paths include the resulting snow surfaces.

Replaying weather at 7200× can leave days of work when the clock returns
to 1×. The simulation keeps that work in date order in the existing ledger.
Gameplay attempts at most eight advancing footprints per update and at most
two days per footprint. It checks the 2 ms replay budget between footprints.
Catch-up runs at most every 100 ms at fast speed and every 500 ms after
returning to 1×. Nearby updates project only dirty footprints in one chunk
column per periodic publication. Streaming and explicit edits may request
other columns. Distant snow geometries use 5 cm height steps, while nearby
clipping and the save ledger keep the settled height.

#### Playcheck F · Arctic shelter

1. Start a disposable generator 28 world with
   `ANYM_POLAR_SNOW_FIXTURE=1 ANYM_WEATHER_FIXTURE=1
   ANYM_WEATHER_DEPOSITION_FIXTURE=1 ./build/src/app/app`. Select
   **Polar pack**. The debug panel shows the region border **X**, floor **Y**,
   first row **Z**, and cap height. The fixture runs from X − 3 through X + 2
   and Z through Z + 5. It opens with the clock paused.
2. Use **Remove Piece** on the side at Z and cut an entrance at Y + 2 and
   Y + 3. Extend the room along X on both sides of the border. Place a roof
   or support, cycle **Weather Force** to **Snow**, and use
   **Weather Apply 30M**. Check that the room still has headroom and that
   the floor and upper pack remain distinct.
3. Fly back beyond detailed chunks while looking at the fixture. Check that
   the cap, roof and supporting exterior are visible. Approach and inspect
   the original cuts. Travel at least 384 m, return, exit with Escape, and
   reopen with the same flags. Check the floor, headroom, snow above the
   room and any exposed grass or rock again.

#### Fixed-camera performance procedure

From the repository root, build the device harness, create a disposable
seed-2024 save, and run against the dated folder it prints.

```sh
cmake --build build --target app world_logic_tests lod_render_smoke --parallel 6
build/tests/lod_render_smoke /tmp/anym-weather-clock 0 weather_clock_save
ANYM_POLAR_SNOW_FIXTURE=1 build/tests/lod_render_smoke \
  /tmp/anym-weather-clock/<dated-save-folder> 0 weather_clock
```

Use a GPU and an accessible display. The camera stays at X/Y/Z
1664/550/1215 in a generator 28 world, with the Low detail preset and a
1280 × 720 window. The harness warms up for 25 seconds, samples 1× for
10 seconds, switches to 7200× for 15 seconds, then measures two consecutive
15-second windows back at 1×. It prints end-to-end median and p95 frame
times, the CPU time for snow replay and projection, the number of replay
interval lookups and projected chunk columns, and a breakdown for water,
flora, trees, the seasonal-snow publication, distant terrain and chunk
streaming. `SMOKE_CAPTURE=<prefix>` writes one TGA per phase;
`SMOKE_NO_ACCEL=1` omits the 7200× phase as a control. It writes back to the
disposable save on exit.


#### Execution results

The Release app and device harness built successfully, and all 465 CTest
cases passed. All 69 focused cases passed with AddressSanitizer and
UndefinedBehaviorSanitizer. The sanitizer run used
`ASAN_OPTIONS=detect_leaks=0`, as in step 13.

```sh
ctest --test-dir build --output-on-failure --no-tests=error -j 6
cmake --build build/tests-san-clang --target world_logic_tests --parallel 5
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/tests-san-clang \
  --output-on-failure --no-tests=error -j 4 \
  -R 'polar_pack|snow|weather|sky_|compound|lod_detail|lod_surface'
```

The Vulkan run used the fresh `2024_2026-11-26_20-01-28` fixture on a
Radeon Navi 23 GPU with X11. The camera stayed fixed through all phases.
The save ended at elapsed day 25.3312 with no leftover temporary or corrupt
files. CPU times below are totals for each sampling window.

| Phase | Frames | Frame p50 / p95 (ms) | Replay CPU (ms) | Interval lookups | Projection CPU (ms) | Chunk columns projected |
|---|---:|---:|---:|---:|---:|---:|
| 1× baseline | 962 | 10.23 / 14.01 | 19.6 | 146 | 25.1 | 35 |
| 7200× | 1,354 | 10.54 / 17.39 | 130.2 | 93,906 | 168.1 | 90 |
| First 15 s back at 1× | 1,567 | 9.00 / 12.66 | 34.3 | 3,842 | 42.6 | 57 |
| Next 15 s back at 1× | 1,563 | 8.99 / 11.93 | 34.6 | 219 | 42.4 | 56 |

The first window back at 1× still replayed weather accumulated at 7200×.
Frame p95 stayed below the recorded 1× baseline in both return windows.
The interactive tunnel, roof and travel walkthrough remains Playcheck F.

#### Generated-world follow-up

The maintainer reported that snow disappeared on approach and that steep
mountains showed tall, isolated snow columns. The High detail reproduction
used generator 28 seed 5574655516549108312, saved at day 40.4385 and
X/Y/Z 52875.17/493/38273.49.

A new detailed column now waits until its streamed snow has been projected
and the current meshes have uploaded. Already drawable columns keep their
previous meshes during a refresh. Streaming processes the nearest pending
column first, with at most one publication every 40 ms while nearby
columns are waiting. Ordinary weather projection retains its 125 ms
cadence.

Untouched polar packs use the steepest grade in the neighbouring footprints
to limit starting depth and later accumulation. Broad flat terrain can
still hold a 16 m pack. Small mountain shelves carry less snow as the
surrounding grade increases. Existing saved cuts, explicit pack runs and
loose deposits keep their stored values.

The reported save contained 41 loose deposits whose clipped ramp support
height had rounded just below zero. New support heights are clamped to
the cell range. The v5 reader accepts and clamps roundoff within one
millionth of a metre so those deposits load with their original volumes.

The 96 m forward approach used a disposable copy of this save with the
clock paused. During the approach the ground mesh at the saved viewpoint
uploaded while snow was still pending. The terrain proxy retained coverage
until the snow geometry uploaded. The Vulkan capture showed snow covering
the detailed foreground after handover. The full Release suite passed
468 cases, including the streaming gate, the ramp save roundoff and
agreement between exact and distant pack generation. The focused ASan
and UBSan suite passed 72 cases.

On a fresh copy of the same save, the fixed-camera benchmark used High
detail with `SMOKE_TERRAIN=2`. Warm-up projected 328 chunk columns. In the
measured windows the local replay advanced cursors without publishing new
snow geometry. Replay CPU times are totals for each window.

| Phase | Frame p50 / p95 (ms) | Replay CPU (ms) | Interval lookups |
|---|---:|---:|---:|
| 1× baseline | 4.77 / 7.41 | 3.0 | 15,360 |
| 7200× | 8.17 / 10.84 | 19.6 | 111,360 |
| First 15 s back at 1× | 7.71 / 9.42 | 3.9 | 22,272 |
| Next 15 s back at 1× | 6.29 / 9.25 | 4.2 | 23,040 |

Frame times improved after the switch back to 1× and stayed above their
initial baseline during this run. Benchmark and visual runs used copied
saves.


#### Generated-world follow-up 2 · 2026-11-26

The maintainer reported three problems on a new generator 28 world: frame
time still degraded through 1x -> 7200x -> 1x, new snow built up only in a
band beside the camera, snow borders between regions were straight instead
of blended, and removing pieces eventually stopped working until reload.
The reproduction save is `1613076668213318756_2026-11-26_20-30-45` at day
111.41 and X/Y/Z 14115.55/493/7391.78.

**Dated seasonal stock.** Untouched polar terrain no longer rebuilds its
pack from a weather cursor. `snow::polar::Baselines::depth` combines the
generated annual temperature with the current annual curve and adds a
fine snowline variation at a 48-tile scale. A permanent component rises
with the annual mean and latitude; a seasonal layer appears below about
3 C and saturates below about -14 C. The old hard eligibility at
`|latitude| > 0.72` is gone: a smooth latitude ramp starts at 0.55 and
reaches full strength at 0.91. Region-centre temperature, humidity and the
latitude curve are blended bilinearly, so adjacent footprints differ by
less than 20 cm even where region rows have opposite seasons. Regions
that are saturated at both seasonal extremes are skipped when the date
changes. The stock remains continuous through the wrapped X seam.

Exact weather still governs footprints with local history: roofs, tunnels,
excavations and debug deposits. The first edit records the footprint's date;
replay stays within two days of the current date, at most two footprints per
update and only within 33x33 m of the camera. Replay is skipped entirely
while the clock runs faster than 1x, so abandoning an accelerated clock
cannot leave a snow backlog. The ordinary terrain's seasonal change is a
15-game-day step (about 18 real minutes at 1x), published one chunk column
per 125 ms. Chunk snow is projected before the detailed mesh is allowed to
hide the terrain proxy, and each newly published column also refreshes the
distant LOD patch that contains it.

**Edits after travel.** Ordinary piece edits no longer wait for a weather
catch-up (`prepareSnowEdit` gates only the travel fixture). Removing a
loose snow piece is now routed to `SnowSettlement::excavateLoose`, which
removes exactly the free-space interval in the picked cell and books it as
an accounted sink. Previously only packed snow had a removal path, so
clicking loose snow returned "invalid" and looked like dead input.
`trackLocalWeather` does not create an empty pack record any more; a later
storm starts the pack with a support-relative ceiling, so its saved runs
round-trip.

#### Final fixed-camera run

A disposable copy of the reproduction save ran with High detail on a
Radeon Navi 23 through Wayland (X11's window mapping was unresponsive in
this session). The saved camera and its full old snow ledger were kept.
The harness used `SMOKE_TERRAIN=2`; startup projected 357 chunk columns.
CPU columns are totals for each window.

| Phase | Frame p50 / p95 (ms) | Snow replay (ms) | Interval lookups | Projection (ms) | Columns |
|---|---:|---:|---:|---:|---:|
| 1x baseline | 15.94 / 17.50 | 2.3 | 0 | 0.2 | 0 |
| 7200x | 19.65 / 25.28 | 0.05 | 0 | 4.1 | 12 |
| First 15 s back at 1x | 18.80 / 24.17 | 6.2 | 0 | 96.8 | 109 |
| Next 15 s back at 1x | 18.22 / 24.81 | 4.7 | 0 | 74.9 | 110 |

A no-acceleration control at the same camera measured 15.48 / 17.33,
16.36 / 19.87 and 15.94 / 17.14 in its three windows. Snow replay is gone
at 7200x (0.05 ms total), and the two return windows publish the season
step rather than backlogged weather. With the snow season disabled for
comparison, the same run returned to 17.75 / 20.58 and 17.33 / 24.0, so
most of the residual accelerated frame cost comes from the time-accelerated
flora, water and presentation updates, not from snow. The old behavior
(weather replay plus full snowfield reprojection) is no longer on that
path.

Existing v5 ledgers keep their loose deposits and pack edits. On the
reproduction save, 8,782 loose columns and 569 pack runs were written by the
old per-frame replay around the camera; the new seasonal stock governs only
footprints without an explicit record. A fresh world, a copy without
`snow.dat`, or an approved one-time rebase is needed to see the clean
seasonal surface on that save. Nothing is deleted silently.

The full Release suite passed 473 cases, including seasonal continuity,
the old latitude cutoff, a deep pack with no saved stock, local-weather
save version 6, weather-created packs above the floor, and loose-snow
removal through `WorldEditor`. The focused ASan and UBSan run passed 77
cases. The visual check used a copy of the save without `snow.dat`, so the
current seasonal stock regenerated from the same world map; a wide capture
showed the pack following terrain and the slope rule rather than a line at
the old latitude cutoff. The remaining visible stripes in that capture are
the generator's one-metre ramps shedding their snow, which is the intended
45-degree behavior.


#### Generated-world follow-up 3 · 2026-11-26

The maintainer reported four remaining issues after the 7200x fix:

- distant snow did not follow the same seasonal state as streamed chunks;
- ramps stayed bare in otherwise snow-covered terrain;
- snow in a tunnel appeared in the ledger well before its blocks rendered;
- thin snow steps produced dark one-pixel side-face noise.

**Season and LOD.** The dated seasonal step is now **5 game days**. A
season change invalidates only LOD patches whose region sample crosses the
snowline, and the surface snapshot carries both the pinned pack tops and the
tops of **ground-resting loose snow**. `SurfaceSampler::packTop` returns the
larger of the seasonal/pinned pack and the loose top, so local weather snow
does not vanish when its chunk unloads. Ground loose columns are no longer
duplicated in `SnowCover`'s roof proxy; the distant height field owns them,
while the proxy keeps roofs and other elevated supports. A newly streamed
column is added to the next surface publication, so the height field catches
it within a second.

**Tunnel snow latency.** Local weather replay now also runs while the clock
is accelerated. It stays bounded: only footprints with local history, at most
two footprints per update, and at most two days per update. Applied changes
publish immediately (`rebuildSnow(2)`), and an edit that changes a covered
surface republishes its column at once instead of waiting for the periodic
tick. Untouched terrain still never replays.

**Ramps.** A seasonal stock of at least **5 cm** fills the ramp cell up to the
adjacent snow surface (the same top a cube receives), so a deep pack buries
the 45-degree face instead of leaving a square hole. A thinner stock still
produces only the toe wedge. The stepped LOD skips `shapedColumn` when the
column has a snow top and draws the same flat snow cap as the nearby chunk.
The far LOD uses the same pack top, so the hand-over no longer changes the
ramp silhouette.

**Side-face noise.** The near renderer snaps snow tops to **2 cm** for
geometry only. The settlement ledger keeps the exact height. Footprints whose
exact tops differ by millimetres therefore share a face and stop producing
a dark side pixel at every cell boundary. The block and `terrain_lod`
programs take `u_material_soft` (snow albedo layer and a side strength):
vertical snow faces mix toward the top-facing sky and get a small light
floor. This keeps thin steps from reading as dark contour lines in daylight
and at night.

A newly streamed column is added to the next surface publication, so its
ground loose tops reach the height field before the chunk can unload again.

The final device run on a copy of the reproduction save measured
15.18 / 16.67 ms at 1x, 18.77 / 26.19 ms at 7200x, and 18.51 / 25.24 then
17.69 / 25.57 ms in the two return windows. The return windows still settle
that save's 57 old pending transfers and 8,782 loose columns; a copy without
`snow.dat` returned to 15.5 / 16.6 ms. The release build does not crash on
the initial surface publication (a null previous snapshot is handled).

The Release suite passed 474 cases, including the new 5-day season, the ramp
fill and the existing LOD hand-over, pack-round-trip and loose-removal
checks. The focused ASan/UBSan run passed 78 cases. A wide capture on a copy
of the reproduction save without `snow.dat` showed snow continuous over the
ramps and snow-covered hills; a near capture showed the 2 cm snapping removed
the previous field of dark specks. The existing v5 depreciation caveat still
applies: old explicit deposits are preserved, so the reproduction save keeps
its replay-era patches until a new world, a copy without `snow.dat`, or an
approved rebase.


#### Generated-world follow-up 4 · 2026-11-27

The maintainer checked the tunnel fix (snow now appears after returning to
1x) and reported four remaining visual problems on the latest save.

**Night side light.** The previous snow-side fix added a constant light
floor, which made side faces glow at night. Snow side faces now take the
brightness of their own top-facing sky: they match the top by day and stay
dark at night. The block shader also samples the air cell one step above a
thin snow side so the side is not lit by the ground cell behind it.

**Ramp columns.** A seasonal ramp no longer fills to a flat one-metre column.
The near geometry adds a **draped layer above the ramp's upward facets**,
parallel to the profile and clipped by the cell, using the seasonal depth
(capped at one cell). The stepped LOD keeps the ramp profile and changes only
its material to snow, so near and far silhouettes agree and no tower stands
above the surrounding snow. A shallow stock (< 5 cm) still leaves only a toe
wedge.

**Ridges.** The near renderer keeps the 2 cm top snapping, and the snow-side
sky mix now uses the top-facing exposure. The smooth snowfield has only
faint material transitions; the remaining long lines in a high wide view are
the stepped terrain LOD's real one-metre terraces, not snow depth contours.

**Snow from nowhere.** Two mismatches caused it. The LOD rejected a pack when
the *rendered* water/reservoir projection was wet, while a chunk starts its
pack from the generated water; near a flooded shore or a tundra lake the LOD
was bare and the loaded chunk snowy. It now uses the generated water. A
shallow pack also kept the ground-cover overlay in the LOD, so grass showed
through snow that the chunk hid; any settled snow now hides cover.

The old automatic loose snow is rebased on load for every ledger version:
all loose columns in a chunk column **with no player edit** are dropped and
their volume is removed from the accounted input. Columns with a terrain
edit keep every deposit, so tunnels, roofs and built floors survive; packs
and explicit cuts are not touched. The reproduction save dropped its 8,782
old loose columns; a stderr line reports the count. The rebase uses the
generated surface from the world map, so it does not need loaded chunks and
a stacked deposit cannot reappear after a later chunk load.

The Release suite passed 475 cases; the focused ASan/UBSan run passed 57.
A night capture shows snow tops and sides equally dark, and a day capture
shows the 2 cm snapping removing the dark speck field.

#### Maintainer sign-off · 2026-11-27

The maintainer confirmed the tunnel snow, night lighting, ramp, ridge and
far/near snow fixes and signed Playcheck F off. Step 14 is complete.

### Step 15 · 2026-11-27

`items/thermal_emission.hpp` defines a data-driven signed source:
`delta_c` is the °C offset at the source centre, `radius` is its effective
range in metres and `falloff` is the exponent of `(1 - d / radius)`. The same
object is accepted by blocks and by the new `ThermalItem` data type.
`assets/data/blocks/weather.json` appends `blocks.heater` (+20 °C, 6 m) and
`blocks.cooler` (−15 °C, 5 m) after the existing block files, preserving all
saved numeric block IDs. `assets/data/items/thermal.json` defines the movable
debug heater and cooler. The schema, units and ranges are documented in
[`docs/modding/temperature.md`](../../docs/modding/temperature.md).

`world/thermal_sources.hpp` indexes block and item sources in 32-tile
buckets with X wrapping. `world/thermal_source_tracker.hpp` scans chunks when
they become ready, reconciles one tile on `WorldEditor::blockChanged`, and
removes a chunk's sources on unload. `world/local_temperature.hpp` combines
the dated outdoor weather sample with nearby sources, adds overlapping
sources linearly, and attenuates each path by the length of solid material it
crosses. The V1 barrier is an exponential e-folding length of 5 cm, so a wall
is effectively opaque while a doorway or thin pane passes some heat. The
query also exposes a dated `RegionalHeat` list for future large fires. A
detailed source and its regional summary share a stable `key`; while the
detailed source is registered the regional entry with that key is skipped.
That is the recorded handover convention, and identical definitions produce
the same open-air value when the detailed copy is removed.

Item sources persist in `thermal_sources.dat` (`thermal_sources_codec` section
`HEAT` v1: position, item definition name, key and carried flag). Block
sources are derived from loaded tiles, like emissive tiles. The gameplay
fixture also tracks a carried item at the player position, drops it, picks it
up, clears item sources, and reports ambient, source, regional and local °C in
the tile panel.

```bash
python3 generate_assets.py
cmake --build build --target app world_logic_tests --parallel 6
ctest --test-dir build -R 'thermal_|game.items.thermal' \
  --output-on-failure --no-tests=error -j 4
ctest --test-dir build -L gameplay -j 6 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang -R 'thermal_|game.items.thermal' \
  --output-on-failure --no-tests=error -j 4
```

Asset generation added the `items` data type and preserved the existing block
file order. The Release app and logic tests built. All 13 focused cases and
all 483 gameplay cases passed. The focused ASan/UBSan run also passed all 13.
The first sanitizer run exposed a test-fixture destruction order that removed
the editor before the tracker; the fixture now declares the tracker after the
editor, matching the production ownership order.

The headless cases cover one heater, one cooler, their overlapping sum,
distance falloff, a wrapped-X source, a full wall and a doorway created from
the same wall, the inactive edges of a dated regional entry, the detailed
source winning the handover, removal leaving no registered source, and a
carried item moving and round-tripping through `thermal_sources.dat`. The
block tracker registers a placed heater, replaces it with a cooler, removes
it and clears the registry when its chunk unloads. No maintainer playcheck is
scheduled for this step.

The optional in-game scene is `ANYM_THERMAL_FIXTURE=1 ./build/src/app/app`.
Select **Heat site** for a stone pad with a heater at X−2, a cooler at X+2 and
a shelter wall at Z+2 relative to the reported site. **HEATER ITEM** and
**COOLER ITEM** spawn a carried source, **THERMAL DROP** leaves it in place,
**THERMAL TAKE** carries it again, and **CLEAR HEAT ITEMS** removes item
sources. Walking across the wrapped X seam moves the carried source with the
player; reloading restores dropped and carried sources. The local °C panel is
the acceptance readout.

### Step 16 · 2026-11-27

`items/thermal_emission.hpp` now carries an optional thermostat. `mode` is
`fixed` (default) or `target`; target mode adds `target_c`, `deadband_c`,
`gain`, `min_delta_c` and `max_delta_c`. The room owner recalculates the
effective `delta_c` as
`clamp(gain * (target_c - room_temperature), min_delta_c, max_delta_c)`, with
a zero command inside the deadband for hysteresis and the clamp as the
anti-windup limit. Fixed sources keep their step 15 open-loop offset. The
schema and effective constants are documented in
[`docs/modding/temperature.md`](../../docs/modding/temperature.md).

`world/room_air.hpp` detects connected indoor air in a bounded window around
the camera. A cell belongs to a room when it is non-solid and has a solid
ceiling within twelve metres. Outdoor, liquid and unloaded faces become
leakage; solid faces exchange heat with one finite wall store. A flood fill
stops at its work budget; the remaining boundary is counted as an
approximate leak and the panel reports it. Geometry changes invalidate the
scan, and rescans are coalesced to the room cadence. Inputs are the dated
outdoor sample, the region's annual ground mean at local elevation, and the
nearby fixed or target sources. Air relaxes toward `outdoor + source offset`
through a source coupling, and the wall exchanges heat with the air and the
insulated ground baseline. Thermostat commands are recalculated at every
substep, so an accelerated clock cannot leave a step wide enough to make the
controller oscillate.

`world/local_temperature.hpp` gained a room provider. Inside a connected room
the query returns the simulated room air instead of outdoor plus detailed
sources, so a source is not counted twice. The tile panel shows room air,
wall and ground temperature, air volume, wall and leak faces, the bounded
scan flag, the target command and saturation. The same local °C is passed
into `precipitation::reconcile` as an additive offset, so indoor snowmelts
against room air and outdoor snow near a heater sees its contribution.
Historical replay uses the current room state; that is the recorded bounded
approximation.

`thermal_sources.dat` gained a `ROOM` v1 section beside `HEAT` v1. It stores
the representative cell, air and wall °C, and the last-applied day for rooms
that differ from the ground baseline. A room at the baseline is omitted and
starts there. Thermostat mode and target are data, so they survive a reload;
the runtime command and hysteresis are recomputed on the next tick.

`assets/data/blocks/weather.json` appends `blocks.thermostat` (+21 °C, gain
60, max +60, −30 … +60 clamp) after the existing blocks, preserving saved
numeric block IDs. `assets/data/items/thermal.json` appends `items.thermostat`
with the same settings. `generate_assets.py` ran with the configured shaderc
path; no texture or material manifest changed. The asset copy was refreshed by
the app build.

```bash
python3 generate_assets.py
cmake --build build --target app world_logic_tests --parallel 5
ctest --test-dir build -L gameplay -j 6 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 3
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang -j 4 --output-on-failure \
  --no-tests=error -R 'room_air|thermal_|weather_melt_uses_local'
```

The headless cases build synthetic one-chunk rooms and use the deterministic
weather map; no seed or save is required. The in-game room is generated around
the loaded world's start region and its coordinates are reported by the HUD.
The Release app and logic tests built. All 490 gameplay cases passed. The 20
focused room, thermal and local-melt cases passed under ASan and UBSan with
leak detection disabled for the sandbox. The new cases cover thermostat
parsing and clamping, deadband hysteresis, saturation, a sealed room, a
doorway leak, a removed roof cell, wall heat remaining above the ground
baseline after the source is removed, faster air loss through an open door,
room-query handover without double counting, thermostat recovery after a
door closes, room-record round-trip, malformed room records, and the extra
melt from a local temperature offset. Acceptance ran headlessly; the visual
and interactive result is Playcheck G.

#### Deviations

- V1 uses one lumped air temperature and one lumped wall temperature per
  connected room, not a per-cell airflow field. The wall face count sets its
  capacity and conductance.
- A room is limited to cells with a ceiling within twelve metres. Taller
  halls are outside V1.
- The bounded scan treats an unloaded boundary as an approximate leak. Rooms
  wholly outside the active window are not simulated; a loaded record cools
  toward the documented approximation when it is rediscovered.
- Historical snow replay uses the current room temperature rather than a
  dated room-temperature history.
- Controller state is runtime-only; the configured mode, target and measured
  room/wall heat are saved. A reload starts the controller on its next tick.

#### Playcheck G · Heated room

From the repository root:

```bash
ANYM_THERMAL_FIXTURE=1 ANYM_WEATHER_DEPOSITION_FIXTURE=1 \
  ./build/src/app/app
```

Load or create a world, click **Heat site**, and let the room and the
surrounding chunks stream in. The HUD prints `Thermal site [x, y, z]`. The
room is the 3 x 3 interior around that air cell, with the floor one metre
below and the roof three metres above. Relative to the site:

| Feature | Offset (X, Y, Z) |
| --- | --- |
| Room centre / panel sample | 0, 0, 0 |
| Doorway (lower / upper) | 0, 0, −2 / 0, +1, −2 |
| Roof test cell | 0, +3, 0 |
| Fixed heater | −2, 0, 0 |
| Thermostat | +2, 0, 0 |
| Cooler | 0, 0, +2 |

1. Start with **FIXED HEAT** on, **TARGET HEAT** and **COOLER** off, and the
   door and roof closed. Hover the room centre. After the room cadence has
   run, the **Temperature** section reports a connected room, a wall
   temperature above the ground baseline and the fixed source command. Read
   the local °C at the same camera position with the door and roof open; it
   should be much closer to ambient.
2. Close the door and roof and resume time. Watch the room air and wall warm.
   Turn **FIXED HEAT** off. The air falls first; the wall stays above the
   ground baseline and releases heat. Open **ROOM DOOR** and confirm the leak
   face count rises and the air cools faster. Remove the roof cell during a
   forced **SNOW** period and confirm exposure and additional heat loss.
3. Repeat with **COOLER** on and the heater off. The room air should fall
   below ambient and the wall should follow more slowly on removal.
4. With a warm wall, press Escape and reload the same slot. The panel should
   restore a warm room/wall; the room continues toward the ground baseline
   using the stored last-applied day.
5. Turn **FIXED HEAT** off and **TARGET HEAT** on. With the door closed, the
   room should settle around **21 °C** and the command should leave saturation.
   Open **ROOM DOOR** during cold weather: the command should rise to
   `max_delta_c` and the panel should mark it saturated. Close the door and
   confirm the measured room returns to the deadband. Compare the fixed
   heater, which keeps its open-loop offset and does not track the target.
   Reload and confirm the target, measured room and wall survive.
6. Inspect a snow patch beside the warm wall and another farther away. The
   nearer patch should melt faster through the same local °C. Use the
   **Snow** panel's deposit and meltwater readouts.

Expected result: the sealed room warms and cools more slowly than open air,
the wall outlasts the air, an open door shows leakage and cools the air faster,
a removed roof exposes the room, snow melts against the measured local
temperature, and a 21 °C thermostat saturates through an open door and settles
inside the deadband after it closes.

#### Follow-up · 2026-11-27

The first in-game check reported that selecting the heater jumped the panel to
about 16 °C immediately, while opening the roof or closing the door appeared
not to change the temperature. Three things caused it.

- The tile panel sampled the picked solid cell itself. A heater or wall tile
  is not room air, so the query fell back to `outdoor + fixed offset`, which
  looks like an instant jump. It now samples the air on the picked face and,
  when that is outdoors, searches the six neighbouring cells for a connected
  room. The no-tile panel and the tile panel also report the nearest scanned
  room (within 24 m and 16 m) when the camera or picked tile is outside one.
- The room's source coupling was applied even with no contributing source.
  It pulled the air toward `outdoor + 0` at the same conductance as a heater,
  masking wall storage and doorway leakage. With no source (or a thermostat
  commanding zero inside the deadband) there is now no such coupling.
- The fixture's interior was one square metre wide, so removing its only roof
  cell deleted the whole room. It is now a 3 x 3 interior, matching the
  playcheck table. Removing one roof cell leaves a covered ring around the
  hole.

The room constants were also made observable in play: `60 kJ/K` air per
cubic metre, `120 kJ/K` wall per square metre, `25 W/K` air-to-wall,
`1500 W/K` per open face, `5 W/K` to deep ground and a `120 s` source
coupling. Commands and air/wall states are integrated in at most `5 s` game
steps (capped at 4,096 per update), with source geometry and barriers cached
once per room update.

```bash
cmake --build build --target app world_logic_tests --parallel 5
ctest --test-dir build -L gameplay -j 6 --output-on-failure --no-tests=error
cmake --build build/tests-san-clang --target world_logic_tests --parallel 3
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang -j 4 --output-on-failure \
  --no-tests=error -R 'room_air|thermal_|weather_melt_uses_local'
```

The Release app and logic tests built. All 490 gameplay cases passed; the 20
focused cases passed under ASan and UBSan. The door test now uses a synthetic
valley with a colder outdoor sample, so an open door measurably cools the room
instead of matching the ground baseline. The sign-off below covers the
corrected 3 x 3-room Playcheck G.

#### Maintainer sign-off · 2026-11-27

The maintainer confirmed the corrected behaviour in walk mode: inside the
room, opening the door lowers the air temperature and closing it raises the
temperature again, as expected. The earlier orbit-camera reading was taken
next to the doorway, where the panel correctly sampled the outside air; the
fixture itself was not inverted. Step 16 is complete. Playcheck G is signed
off. Step 17 follows below.

### Step 17 · 2026-11-27

[FluidCell](../../src/game/world/fluid_cell.hpp) derives sub-cell water
capacity from the same `CellGeometry` used by meshing, collision and picking.
A profile stores cumulative free volume in 64 height samples, the lowest
full-width horizontal solid (the sealed ceiling of the bottom free run), and
the open area and lowest point of each cell face. `freeVolumeBelow` is exact
for slabs, layers, ramps and compound pieces. Free volume is converted to the
ledger's units as **255 units per cubic metre**, so a half-free cell tops out
at 128 rather than 255. Old full-cell water keeps its saved 0–255 values.

Local flow now compares free volume and water level instead of raw fill.
Transfers are limited so unequal capacities cannot overshoot the equal level,
and every horizontal or vertical move checks the shared face opening. A
closed face holds its volume; a side gap passes only water above the gap; a
bottom slab cannot feed the cell below. The same capacity and opening checks
apply to reservoir cut-filling and river-intake filling.

`ChunkManager` installs a profile query on the reservoir ledger. A partially
solid cell is `Open` only when its actual geometry has free volume, and the
cell's solid geometry is queried through `solidCellAt`, so pieces and
environmental solids share the same occupancy rule. Chunk projection sets
`liquidLevel` from the geometry level, not from a whole-cell fraction.
Snow-filled cells have no water capacity. The near mesher clips exposed water
side quads against both cells' boundary openings, and camera submersion tests
the actual solid geometry.

`water.dat` (`RSVR`) is v10 with an explicit partial-cell flag. A v9 or older
ledger restores with the flag off: every partial solid remains a whole-cell
barrier, preserving an old dam or reservoir wall that would otherwise drain
through a newly recognised gap. New worlds start with the flag on. A
player-placed partial solid inside projected water also stays a barrier until
it carries a local credit, which keeps existing dam edits and the old
whole-cell edit rule intact.

#### Fixtures and checks

The profile fixtures are synthetic one-cell geometries: a bottom slab, an
elevated lid, a sealed middle slab, a full cube, a 25 cm wall and a ramp. The
local-flow fixtures use explicit profile maps, so they are deterministic and
need no seed or save. The projection fixture places a slab in a loaded chunk,
credits local water and checks the reported level; the same chunk places a
whole cube and verifies that it accepts no local water.

```bash
cmake --build build --target app world_logic_tests --parallel 6
ctest --test-dir build -L water -j 6 --output-on-failure --no-tests=error
ctest --test-dir build -L gameplay -j 8 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang -j 4 --output-on-failure \
  --no-tests=error -R 'fluid_|hydrology_|snow|weather|compound'
```

The Release app and logic tests built. All 58 water cases and all 501 gameplay
cases passed. The focused sanitizer run passed 112 cases with leak detection
disabled for the sandbox. The first sanitizer pass exposed a dangling pointer
in the per-operation profile cache; the cache now returns the stored optional
rather than a local one. Existing river, lake, drawdown, dam and save/load
cases remain green, including the unchanged full-cube river barrier.

#### Deviations

- V1 tracks one water body per cell and fills the bottom free run. Water that
  would sit in a sealed upper pocket, entering from a side opening above a
  full-width slab, is not represented until a later step. The profile still
  reports the pocket as sealed so a bottom-fed cell cannot fill it.
- Face connectivity uses open area and the lowest opening, not a full
  horizontal polygon intersection. Complementary pieces with the same area
  but disjoint extents can pass a conservative overlap test.
- `ReservoirSnapshot::projectLocal` and distant local-water queries still use
  the cell top and a volume fraction for partial cells because the ledger
  snapshot does not carry chunk geometry. Loaded chunk projection is exact.
  Distant weather and ice summaries do not consume this path yet.
- The water top quad in a partially solid cell is not clipped to the free
  cross-section at the waterline; side faces are clipped. Ramp shorelines
  therefore draw a full-footprint top at the measured level. Ice and the
  visual shoreline pass in step 19 will refine this.
- Old ledgers keep the whole-cell barrier rule. Rebasing a pre-step-17 world
  onto partial-cell water is a deliberate migration, not automatic.

### Step 18 · 2026-11-27

[Reservoirs](../../src/game/world/reservoirs.hpp) now owns the water phase
ledger beside the liquid it changes. A `PhaseKey` identifies a standing body
or one local cell. Each `PhaseState` stores frozen water equivalent in the
same 255-per-cubic-metre units as liquid, signed residual phase progress and
the last applied day. `freeze`, `thaw`, `freezeLocal` and `thawLocal` debit
the liquid and credit the ice in one immutable snapshot, so a partial write
cannot split the two sides. Thaw clamps to the receiver's capacity; surplus
becomes an accounted outflow. `setPhaseProgress` carries the step 19 progress
without changing the liquid. `clearLocal` routes excavation volume to the
disposal and removed-ice ledgers, and `adoptLocal` gives legacy independent
water one ledger owner before it can freeze.

`RSVR` v11 appends the exact conversion remainder, removed-ice total, phase
entries and dated input cursors after the v10 partial-cell flag. v10 and older
ledgers restore with no ice and no cursors, keeping their liquid and barrier
rules unchanged. The phase map is part of the immutable `ReservoirSnapshot`.
Projected ice is not written into chunks or ordinary solid edits; step 19 adds
its geometry and distant summaries.

`Reservoirs::applyInput` is the weather boundary. A `WaterInput` carries the
receiver (local cell, body or reach), a kind, a dated once-only key and an
amount in water units. A repeated key for the same receiver is ignored, so a
remote catch-up replay and local detail cannot both apply a transfer. A full
body keeps its surplus in `pendingInflow`, a reach keeps it in its delivery
hold, and an unresolved or local overflow becomes `Disposal`. `addMeltwater`
converts bulk snow centimetres at 0.2 liquid cm³ per bulk cm³ and `addRain`
converts liquid centimetres; both use `InputRemainderDenominator = 5,000,000`
so small events accumulate instead of rounding to zero. The remainder and all
cursors are saved in the same atomic `water.dat` replacement.

`precipitation::reconcile` gained an optional rain callback that fires only
for exposure rays that hit existing liquid, using the same measured
temperature as the snow fraction. `GameplayState` aggregates those amounts per
cell and weather interval and sends them through `addRain`. It also routes
`SnowSettlement::meltwater` through `addMeltwater` after each bounded replay.
Meltwater waits while its receiver chunk is unloaded; melt at the world top is
booked as outflow. The meltwater date key is the exact applied day in
microseconds, so two partial catch-up batches ending inside one half-hour are
distinct while a reload of the same batch remains a duplicate.

#### Verification

The new headless cases use synthetic constant-area bodies and local cells; no
seed or save slot is required. They cover a full freeze/thaw cycle, local
conversion and excavation, legacy ownership adoption, a closed-basin audit,
sub-unit melt and rain remainders, reload with frozen state, duplicate
catch-up, a full destination, a changed body head, two receivers from one
source, a river reach, a v10 ledger without ice, a combined snow-water-
equivalent audit across the meltwater boundary, and rain reaching that
boundary only on existing liquid.

```bash
cmake --build build --target app world_logic_tests --parallel 6
ctest --test-dir build -R 'hydrology_water_|weather_(rain|snowmelt)' \
  --output-on-failure --no-tests=error -j 4
ctest --test-dir build -L gameplay -j 8 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang \
  -R 'hydrology_water_|weather_(rain|snowmelt)' \
  --output-on-failure --no-tests=error -j 4
```

The Release app and logic tests built. All 8 focused water-phase and
rain/melt boundary cases passed. All 508 gameplay cases passed. The same 8
cases passed with ASan and UBSan, with leak detection disabled for the
sandbox. Acceptance ran headlessly; step 18 schedules no maintainer playcheck.

#### Deviations

- V1 stores ice as water equivalent only. The 0.917 ice-expansion factor,
  visible 5/10/25/50/75/100 cm stages and the projected ice surface belong to
  step 19.
- Rain is routed only where an exposure ray already hits liquid. Rain on bare
  ground and its surface-runoff path remain future consumers.
- The audit helper `liquidTotal` sums defined body curves and local cells. A
  lazily untouched generated body is not forced into the curve cache.
- The exact cm³ remainder is ledger-wide rather than per receiver. It
  conserves total volume; the eventual whole water unit is credited to the
  receiver that crosses the boundary.
- The input cursor is a high-water key per receiver and kind. Replaying an
  older key is ignored rather than tracked as a set, which keeps saves bounded
  and matches the date-ordered replay order of step 11.
- Save order in `GameplayState` writes `water.dat` before `snow.dat`; a crash
  between them leaves the snow entry to be replayed into the deduplicating
  water cursors, while the opposite order cannot occur.

### Step 19 · 2026-11-27

[Surface ice](../../src/game/world/surface_ice.hpp) is a per-water-column
phase layer. `Reservoirs` gained `SurfaceIceState`: the liquid owner (body,
reach or local cell), the top water cell, frozen water equivalent, signed
sub-stage progress, the captured mean waterline, visible thickness, stage,
freeze-through and stranded flags. `freezeSurface`, `thawSurface` and
`setSurfaceIce` debit or credit the owner and publish the ice state in one
immutable snapshot. Reach ice is supplied by flow and booked as an accounted
`Input`; thawing it reverses that entry. `RSVR` v12 appends the surface-ice
records; v11 and older ledgers load with no surface ice.

Visible stages are **5, 10, 25, 50, 75 and 100 cm**. A column accumulates
signed progress in millionths of a water unit per square metre. Still water
gains 2 mm of ice per game hour per °C below zero; flowing reaches use one
quarter of that rate. Thaw removes 4 mm per hour per °C. Ice starts at the
fractional mean waterline (`WaterSurface::height`, which excludes the wave
displacement), grows down into the available water, and stops at the bed or
the 1 m stage. A column shallower than its next stage freezes through first.
Progress at the cap is clamped so a long cold spell cannot make the later
thaw take years. Frozen-through columns remain in the ledger when their
liquid surface disappears, so they thaw back into the same local cell.

`IceCover` projects ice through the shared `CellGeometry`, using
`freeSpaceBetween` in each affected cell and a dedicated `blocks.ice` block.
The resulting solids join the snow environment in `GameplayState`; unchanged
chunk slabs share their immutable pointers through a merge cache. Chunk
meshes skip a water top face where the environment already covers it and mark
covered flowing faces as non-animated, so a lid suppresses the river wave.
Walking, picking and sunlight use the environment geometry. The tile panel
reports the ice stage, thickness, mean waterline, remaining liquid depth and
frozen water equivalent.

For distant terrain the same ledger publishes ice tops into
`snow::polar::SurfaceSnapshot::ice`. `SurfaceSampler` takes the higher of
snow/loose/ice tops, uses `materials.ice` for the column, and hides the water
surface under the lid. Ice on unloaded columns therefore keeps its broad
footprint without requesting voxels. Ice is not written into ordinary chunk
saves; only the water ledger owns it.

Assets added `blocks.ice` after the existing weather blocks and
`materials.ice` with a cool-tinted albedo and normal derived from the snow
textures. `generate_assets.py` ran with the configured `shaderc` path; the
existing block file order is unchanged.

#### Fixture and checks

```bash
python3 generate_assets.py
cmake --build build --target app world_logic_tests --parallel 6
ctest --test-dir build -R 'surface_ice_' --output-on-failure --no-tests=error -j 4
ctest --test-dir build -L gameplay -j 6 --output-on-failure --no-tests=error

cmake --build build/tests-san-clang --target world_logic_tests --parallel 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tests-san-clang -R 'surface_ice_' \
  --output-on-failure --no-tests=error -j 4
```

The Release app and logic tests built and all 516 gameplay cases passed.
The headless fixtures are synthetic one-column waters and need no seed or
save. `surface_ice_stage_progression_and_balance` checks successive stages,
the initial debit and the liquid + ice balance. The shallow-bank case checks
freeze-through before the deep pool reaches 5 cm and thaw back into the
frozen-through cell. The thaw case checks sub-stage reversal, reload and a
full body thaw. The reach case checks the quarter freezing rate. The
projection case checks a fractional waterline, the distant top and the
clipped ice geometry. The sanitizer run is recorded below.

The focused ASan/UBSan run passed the six surface-ice cases together with
the three water phase and legacy-ledger cases. Leak detection
was disabled for the sandbox. The full Release CTest run passed 521 cases
(516 gameplay, including the feature-target, ledger-free projection and generated-block cases).

#### Playcheck H · Frozen shore

Use a disposable world. Launch from the repository root with the ice and
weather fixtures so the same scene has time controls:

```bash
ANYM_ICE_FIXTURE=1 ANYM_WEATHER_FIXTURE=1 ./build/src/app/app
```

Create or load a world, then click **ICE SITE**. The fixture builds a stone
basin at the northern pole region (world X/Z near 4/4). The camera moves
above it. **ICE COLD** clamps the sampled temperature of nearby surface
water to −12 °C for the session; it is not saved. **ICE CLEAR** removes the
local water and ice; **WEATHER +6H** and **WEATHER +1D** advance the same
clock.

| Feature | Position relative to the printed site |
| --- | --- |
| Deep pool | X 0–2, Z 0–5, floor +1, top cell half full |
| Divider wall | X 3 |
| Shallow bank | X 4–6, Z 0–5, 2 cm deep |
| Basin walls | X −1 and +7, Z −1 and +6 |

1. Click **ICE SITE** and let chunks stream. Click **ICE COLD**, then
   **WEATHER +6H** twice. The shallow bank should freeze through first; the
   deep pool should form its first lid at the fractional mean waterline.
   Inspect both from above and from a low orbit angle.
2. Watch the deep pool grow through 5, 10, 25, 50, 75 and 100 cm stages.
   The liquid below the lid should remain visible at the edges and the
   reported frozen/liquid balance should conserve water equivalent.
3. Click **ICE CLEAR**, refill with **ICE SITE**, then compare the still
   pool with a moving reach. Keep **ICE COLD** on and visit any generated
   river reach in the same world (or the step 01 seed-2024 river scene).
   A reach forms ice at one quarter of the still rate on the same clock.
4. While the deep pool is partially frozen, toggle **ICE COLD** off and use
   **WEATHER +1D** to warm it. Reload midway through the thaw. The saved
   thickness, sub-stage progress and remaining liquid should resume.
5. Fly far enough that the basin becomes distant terrain, then return. The
   distant height field should show the same lid footprint and hide the
   water surface; near chunks should hand back to the clipped ice layer
   without a double surface. Inspect the opening after **ICE CLEAR** or a
   full thaw and confirm the reported liquid/ice volume balance.

Expected result: the shallow bank freezes through before the deeper pool,
the first lid sits at a non-integer waterline, cold water across the
streamed view shares the broad lid, still water grows faster than a reach,
a reload preserves the partial lid, and the near ice and distant ice
footprint agree. The 100 cm stage is the planned V1 maximum; later growth
stops there rather than stacking another layer.

#### Deviations

- V1 models the surface lid per column and uses the mean stage; it does not
  model ice rafting, pressure ridges or a separate ice strength rule (step
  20).
- Reach ice is supplied by flowing water and booked as an accounted input,
  not debited from a reach store. A closed reach therefore cannot freeze.
- The distant summary combines the ledger's current frozen columns with the
  five-day feature/seasonal target grid. New territory gets the broad
  footprint (up to 16 m for ice sheets and 1 m for sea ice) immediately;
  local sub-stage history takes over once its columns are loaded and seeded.
- Ice is opaque in V1. Sub-surface liquid is visible around the lid and at
  the shore, not through the ice material.

#### Startup crash follow-up · 2026-11-27

The first fixture launch crashed in the `GameplayState` constructor before
the world appeared. The constructor moves its `world` parameter into the
member, but the initial surface-ice rebuild read the moved-from parameter
(`world->reservoirs`). It now reads `this->world`. The Release app starts and
survives the fixture launch window; the interactive **ICE SITE** click remains
part of Playcheck H. All 516 gameplay cases still pass.

#### Breadth and accelerated-clock follow-up · 2026-11-27

The first in-game check found that ice stayed only in the camera chunk and
that returning from 7200x to 1x left the frame rate low.

- Ice work per update is now bounded. The detailed simulation consumes at
  most 1.2 game hours (`0.05` days) per 0.5 s tick, so a fast clock cannot
  queue hours of stage growth for the return to 1x. Excess accelerated time
  is deliberately not replayed; this V1 approximation is recorded here.
- Sub-stage progress is held in the simulation and written to `water.dat`
  only when a visible stage changes, the ice loses/gains support, or the
  progress moves by 1 cm. This removes the per-tick reservoir snapshot
  churn that cleared LOD water caches every half second.
- `IceCover` now rebuilds only the chunk columns that changed, reusing every
  other environment slab. The merged snow/ice map is likewise updated per
  changed chunk instead of rebuilding the whole resident map.
- Loaded chunk columns are seeded from the current regional temperature one
  column per tick: each cold water column gets a starting lid of 2 cm per
  degree below zero, capped at 1 m. The detailed local simulation then
  refines it. This makes cold water across the streamed view freeze, not
  only the column under the camera.
- `SurfaceSampler` gives unloaded cold water the same broad fallback lid
  from the dated regional weather, so distant terrain shows the frozen
  footprint without requesting its chunks. Explicit ledger ice always wins.
- Ice temperature is sampled on a 4 m lattice per update, so one weather and
  source query serves a small patch of columns. Step 19 seeds lakes, reaches
  and local pools; the sea is left to a later regional summary because
  per-column ocean records would dominate the save.

The Release and debug apps start with the fixture, and all 516 gameplay
cases pass. The remaining visual check is the
**ICE SITE** walkthrough below.

#### Feature-driven seasonal ice · 2026-11-27

The 60x stall came from running the per-column thermal refinement over a
large visited area every half second. The broad ice field is now driven by
the generated region features and a five-day seasonal target:

- `surface_ice::Regional` samples the region grid and produces a target ice
  thickness in metres. `ICE_SHEET` and `GLACIER` regions target **16 m**;
  `SEA_ICE` targets **1 m**; ordinary water uses the seasonal temperature
  and the existing 1 m cap. Targets are blended bilinearly across region
  centres, so an ice sheet hands over smoothly to one-metre sea ice instead
  of stepping at a region border.
- Targets are recomputed only when the five-game-day step changes, and
  only at 1x. At 60x/7200x a five-day step would arrive every few frames
  and force a full LOD refresh, so target changes are deferred until the
  clock returns to 1x.
- `ICE_SHEET`, `GLACIER` and `SEA_ICE` regions write ice and a snow block
  directly into generated chunk columns. The ice is an ordinary solid block
  from the first frame, so there is no water flash. Old saved chunks are
  migrated on publish and marked dirty for saving. Ordinary seasonal lake
  and river ice still uses the target-derived environment projection.
- Dirty projection chunks are processed nearest-camera first, up to four per
  tick, so a teleport fills visible ordinary ice first and a long fast run
  leaves a bounded queue for later ticks. Feature chunks carry blocks and do
  not need that scan.
- The detailed thermal pass still runs at `1x`, but only for local/fixture
  targets and existing legacy ledger states. It skips all regionally
  targeted columns; at 60x and above it is disabled entirely.
- The target grid is part of `SurfaceSnapshot`, so distant LOD water reads
  the same target without a weather query or chunk request. Feature targets
  carry a matching snow-cap top. A five-day target change refreshes the
  distant patches; SEA_ICE and ICE_SHEET regions are drawn as continuous
  cover rather than per-column noise.
- Feature sea ice is booked as an accounted input where a legacy record is
  thawed, rather than a debit on the global sea body, so a 16 m sheet cannot
  drain the ocean.

Release and debug builds start with the fixture, and all **516 gameplay**
cases (**521** total) pass, including the regional feature-target case.

#### Snow-on-ice, picking and streaming follow-up · 2026-11-27

- Feature chunks now contain a generated snow block above the ice, so the
  cap is present immediately. `ChunkSnowTerrain` also appends legacy ice
  environment to its support query, so precipitation and `SnowSettlement`
  see any remaining projected lid as ground.
- Near ice geometry changes no longer invalidate distant LOD patches. LOD
  already reads the seasonal target grid; moving across an ice region only
  remeshes the affected near chunks. The environment change also skips the
  full-chunk local-water refresh when no local credit or reservoir cell is
  involved.
- `Remove Piece` removes generated feature ice as an ordinary block edit,
  which persists through the chunk save. Legacy projected ice still uses the
  session-local cut path. Step 20 owns the ice-strength rule, refreeze and
  the final cut persistence.
- Dirty regional chunks are processed nearest-camera first and remain
  bounded, so crossing many ice chunks does not rebuild the whole resident
  set in one frame.

Release and debug builds start with the fixture; all **516 gameplay** cases
(**521** total) pass, including the cut-survival and generated-block cases.

#### Generated feature columns · 2026-11-27

`LocationGenerator` now writes feature ice and a one-block snow cap directly
into `ICE_SHEET`, `GLACIER` and `SEA_ICE` chunk columns (generator **29**).
The first generated mesh therefore contains ice and snow; there is no water
frame or asynchronous environment fill. `ChunkManager` applies the same
conversion when an older saved chunk is published and marks it for saving,
so existing worlds migrate as their columns load.

The regional environment projection is now limited to ordinary seasonal
lake/river targets. Feature chunks carry no per-column ice ledger, and their
chunk loads no longer enqueue regional projection work. `Remove Piece`
removes the generated ice as a normal block edit; the existing cut summary
still handles legacy projected ice. A removed generated block is skipped by
the migration pass because it is no longer liquid water.

Snow on generated feature ice is present immediately and is also visible to
`SnowSettlement`. Weather snow thereafter settles on the same block cap.
Release and debug builds start with the fixture; all **516 gameplay** cases
(**521** total) pass, including the generated ice/snow column fixture.

#### Maintainer sign-off · 2026-11-27

The maintainer accepted the in-game result and marked step 19 complete.
Snow on seasonal ice outside the generated ice features is recorded as a
step 21 carry-over; it is to be fixed with the same partial-cell treatment
as water in partially solid tiles.
