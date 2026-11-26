# Gameplay logic tests

The `world_logic_tests` target runs production generation, meshing, chunk
management, raycasting, and sunlight code. It does not link the game/UI/engine
or bgfx libraries, initialize devices, open a window, or load runtime assets.
Normal project configuration still requires vendored dependencies and
Intl/Gettext. Synthetic asset definitions live in `support/test_assets.hpp`.

From the repository root:

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target world_logic_tests --parallel 4
ctest --test-dir build -L gameplay --output-on-failure --no-tests=error
```

There are 103 independently registered cases. Checks remain active in Release.
Failures exit nonzero and report failed conditions or expected/actual values,
including voxel coordinates for lighting. See [RESULTS.md](RESULTS.md) for the
latest verification and the original failures that motivated these regressions.

Run lighting, one case, or the last failed cases:

```bash
ctest --test-dir build -R sunlight --output-on-failure --no-tests=error
ctest --test-dir build -R '^game.world.sunlight_cross_three_axis_corner$' -V
ctest --test-dir build --rerun-failed --output-on-failure
```

Run an individual case in a debugger:

```bash
gdb --args build/tests/world_logic_tests sunlight_cross_barrier_edit
```

Build Release separately:

```bash
cmake -S . -B build/tests-release -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/tests-release --target world_logic_tests --parallel 4
ctest --test-dir build/tests-release -L gameplay --output-on-failure --no-tests=error
```

Build with both address and undefined-behavior sanitizers (GCC or Clang, with
both runtimes installed):

```bash
cmake -S . -B build/tests-sanitized -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DANYMALSIMA_TEST_SANITIZERS=ON
cmake --build build/tests-sanitized --target world_logic_tests --parallel 4
ctest --test-dir build/tests-sanitized -L gameplay --output-on-failure --no-tests=error
```

The sanitizer option instruments the executable and production algorithms
compiled into it. Vendor libraries are not instrumented. UBSan errors terminate
the test instead of merely printing a recoverable warning. LeakSanitizer needs an
execution environment that permits process inspection; it cannot run under the
restricted sandbox used by some coding tools. Set `BUILD_TESTING=OFF` to omit
project tests.

## Coverage

- Tile classification, terrain/rock-layer transitions, sea-level water filling,
  generation seams, reused chunks, repeatability, and layer depth/error rules.
- Chunk lookup boundaries, negative/missing coordinates, shared ownership after
  unregister, and monotonic top-height bounds.
- Nearest ray hits, range limits, water, negative directions, chunk crossings,
  and normalized camera directions.
- Empty/isolated meshes, greedy merging and area, neighbor face culling, world
  positions, material/light tint, and triangle winding.
- Direct sunlight, opaque roofs including stale initial light, partial opacity,
  water, slab seams, late chunks, raised bounds, and generated shallow oceans.
- Exact indirect attenuation across both directions of all three chunk axes,
  bends crossing three axes, opaque barriers, partial opacity, water, competing
  sources, maximum range, and passage closure/reopening.
- Neighbor arrival, unknown upper slabs, source unload/reload, replacement at the
  same coordinates, stale results, worker/load order, and pending-work teardown.
- Default and explicit block opacity deserialization.

## Lighting contract and fixtures

Direct sunlight starts at 15 and crosses air vertically without distance loss.
A cell receives incoming light before its opacity reduces transmission through
it. Indirect sunlight spreads through all six face neighbors, transmitting
`max(0, incoming - max(1, current_cell_opacity))`. An opaque cell receives light
on its surface but transmits none. The strongest contribution wins. Water
opacity is up to 8 according to fill. Combined block/liquid opacity is their
maximum.

Direct sources are separate from final sunlight. Indirect jobs solve a target
chunk and a 14-tile halo from fresh direct sources, and publish only that target.
This permits removal of obsolete light without neighboring jobs overwriting one
another. Unknown chunks are closed boundaries, not sky sources. Missing required
registered slabs are requested and consumed through normal chunk generation.

Synthetic lighting fixtures map local Y=0 to sea level, keeping their explicitly
carved air and roofs above the generated ocean bound. `WorldFixture` retains
absolute coordinates for generation and ocean integration tests. Darkness tests
can preload 15 deliberately to check replacement of stale values.

Workers use owned snapshots. Main-thread publication validates revisions and
invalidates neighbors on geometry/source changes, arrival, and removal. The
scheduler permits 1–8 lighting jobs in flight based on worker count, with a
2 ms dispatch/snapshot budget checked between jobs. Fixture pumping uses `isIdle()`
with a five-second deadline and three-second pool waits rather than arbitrary
update counts or sleeps. CTest also applies per-process timeouts.

These tests do not establish full planetary-generation correctness, race-detector
coverage, every data catalog/seed, or GPU appearance. Add a `TEST(case_name)`
function under `unit/` or `integration/` and register its name in CMake.

## Device targets (GPU and display)

`lod_render_smoke` renders real frames through bgfx/Vulkan. It needs a GPU and
a display session, so it is built only on request (`EXCLUDE_FROM_ALL`) and is
never registered in CTest. Agents and CI without a display must not run it;
headless checks of the same logic belong in `world_logic_tests` or
`tree_benchmark`. Any future device test must stay out of the default suite
the same way, or carry a `device` CTest label and be excluded from default runs.

```bash
cmake --build build --target lod_render_smoke
build/tests/lod_render_smoke /tmp/capture 900 lights
```

The `shapes` scene sets a grassy corner and ramp run against bare ramps
across an X chunk face. Check `/tmp/shapes.ppm` for continuous geometry at
the boundary and grass that follows the incline without spilling onto the
bare side.

```bash
build/tests/lod_render_smoke /tmp/shapes 1200 shapes
```

The river flyover starts at the mouth, crosses the X seam, approaches the
waterfall during a remesh, and finishes below the landing's water surface.
Compare the PPM captures for missing or doubled water, submerged banks,
visible seams during handoff, and underwater fog.

```bash
for preset in 0 1 2; do
  SMOKE_TERRAIN="$preset" build/tests/lod_render_smoke "/tmp/river-$preset" 5000 river
done
```

The headless benchmark reports the build time and mesh size for the two
fallback patches that meet at the same seam.

```bash
cmake --build build --target lod_benchmark
build/tests/lod_benchmark --river
```

To profile a real location, point the smoke test at a save. This reads the
world map and starts at its saved camera. It generates nearby chunks from
the map for repeatability, without writing to the save.

```bash
SMOKE_SAVE=/path/to/save SMOKE_TERRAIN=2   build/tests/lod_render_smoke /tmp/saved-view 850 saved
```

## Weather comparison

The weather handoff's pre-weather fixture uses the normal chunk streaming,
editing, lighting, meshing and region save paths. Run the Vulkan capture
from the repository root on a device that supports the smoke harness.

```bash
cmake --build build --target app world_logic_tests lod_render_smoke shape_benchmark --parallel 4
mkdir -p build/weather-baseline
SMOKE_TERRAIN=0 build/tests/lod_render_smoke build/weather-baseline/weather-walk 2800 weather_walk
SMOKE_TERRAIN=0 build/tests/lod_render_smoke build/weather-baseline/weather-seam 1600 weather_seam
build/tests/shape_benchmark --legacy
build/tests/shape_benchmark
```

`weather` runs the ordinary chunk-face view alone. `weather_walk` measures 60
frames after the view settles, moves the camera for 500 frames, then captures
the settled destination. Each step moves 2 m west and 1.5 m south, following
terrain at 24 m above the surface. `weather_seam` repeats the stationary view
at the world wrap. The camera and clock are printed at startup. Preset 0 is
Low with a four-chunk radius. Leave other `SMOKE_*` settings unset for
comparison.

The run writes captures and a dated save under the output prefix. It decodes
the saved ramp, plant layer, planted oak record and water after the checkpoints.
To inspect the save in the app, copy the dated folder into `saves/` and open
the copy through Load World. The smoke harness treats the quartzite roof as
fully light-transmitting. It has the regular opaque quartzite texture, and the
app copy uses the shipped block definition.

Frame times are wall-clock update, draw submission and flush time. The
measurement excludes the harness sleep, the stationary warm-up capture and
checkpoint writes. The queue peak is sampled immediately before submission and
after flush. The shape benchmark measures isolated CPU mesh builds with air
neighbors. The [weather handoff](../docs/handoff/weather.md#execution-log)
records the fixture coordinates and reference measurements.

### Regional weather fixture

Step 09 adds a session-only inspection fixture to the app. It does not change
the world or save file.

```bash
ANYM_WEATHER_FIXTURE=1 ./build/src/app/app
```

The two rows above the snow controls gain **WEATHER +6H**, **+1D**, **+30D**
and **+180D**, a **WEATHER FORCE** cycle (AUTO, CLEAR, CLOUDY, RAIN, SNOW) and
**WEATHER SITE** (START, NORTH POLE, EQUATOR, SOUTH POLE, X SEAM). The force
changes presentation only; the climate panel keeps the sampled temperature.
The tile panel shows the date, climate baseline, weather offset, resulting °C,
condition, cloud cover, physical precipitation rate, visual density, snow
fraction and wind speed. Rain and snow are near-camera world-space particles;
clouds drift with the sampled wind. Wait for streaming after a site jump. The
expected results and the step 09 replay are in the
[weather handoff](../docs/handoff/weather.md#step-09--2026-11-26).

## Natural shape budget

`shape_benchmark` reports natural ramp coverage, column and mesh time,
resident sparse shape-table bytes, encoded chunk size and the dense
worst case (every cell shaped). It uses the scene in
`tests/support/lod_scene.hpp`; compare `--legacy` for the cube-only
baseline. The `--compound` run measures snapshot, edit and mesh costs for a
16 × 16 wall with two materials per tile. A `--stats` pass explains why cells stay whole blocks, and
`--map out.ppm` writes a shape map (red straight ramps, blue outer corners,
green inner corners, yellow diagonal corner ramps).

```bash
cmake --build build --target shape_benchmark
build/tests/shape_benchmark
build/tests/shape_benchmark --legacy
build/tests/shape_benchmark --compound
build/tests/shape_benchmark --stats
build/tests/shape_benchmark --map /tmp/shape-map.ppm
```

The natural device scene generates the hillside with generator 27, finds the
densest ramp cluster in region 45,45, streams chunks and draws near plus LOD
terrain. Use `SMOKE_CAMERA` for close-ups of a junction.

```bash
build/tests/lod_render_smoke /tmp/natural 900 natural
SMOKE_CAMERA="5793 5876 0 -0.28 12 4" \
  build/tests/lod_render_smoke /tmp/natural-close 700 natural
```

The walking variant halves the chosen terrain budget, moves the saved
camera through new regions for 500 frames and checks that the stationary
camera regains tile-spacing terrain and the terrain queue drains. Use
`saved_walk_full` for the same route with the full preset budget.

```bash
SMOKE_SAVE=/path/to/save SMOKE_TERRAIN=2   build/tests/lod_render_smoke /tmp/lod-budget-walk 2800 saved_walk
```

Massive tree approach (`docs/handoff/trees.md`, "Approaching massive trees"):
one elder in view, the game's chunk radius, the High terrain preset; it walks
from 1200 m to 40 m and prints per-section frame times and holes in the
distant tree wood.

```bash
SMOKE_TERRAIN=2 SMOKE_FOREST=elder SMOKE_ELDER_SPACING=3000 SMOKE_RADIUS=8 \
  SMOKE_APPROACH="1200 0.5 40" build/tests/lod_render_smoke /tmp/approach 9000 lights
```

Sunlight under a sawn elder limb: `SMOKE_LIMB_CUT=1` (same variables, no
`SMOKE_APPROACH`) settles on the thickest sideways limb, saws it, then logs
`limb_cut` sunlight samples on the ground under it; `SMOKE_LIMB_CUT=walk`
loads that ground first and stays at the cut.

Distant shade under structures (`docs/handoff/structures.md`, step 2): an
elder seen from 1 km, its limbs' shadows on distant terrain. Add
`SMOKE_NO_OCCLUSION=1` for the same frame without the sky occlusion map.

```bash
SMOKE_CAMERA="5857 6484 1.5708 -0.5 1000 0" SMOKE_TERRAIN=2 SMOKE_FOREST=elder \
  SMOKE_RADIUS=4 build/tests/lod_render_smoke /tmp/shade 3000 lights
```

## Camera priority and streaming

Gameplay updates both schedulers' focus from the current camera position every
frame. Pending work is ordered by distance to chunk bounds, then request age and
coordinates. Every sixteenth dispatch favors the oldest pending work. Existing
worker jobs finish normally; unsent requests are immediately reranked when the
camera moves. Camera movement does not invalidate lighting data.

Indirect lighting waits only for its ready halo's direct-light dependencies,
not for unrelated distant columns. Direct light is published progressively.
Readiness notifications preserve existing valid light; changed direct sources
invalidate their dependent halos. Missing sky slabs inherit nearby beneficiary
priority, while a camera moving into the slab itself can raise its priority.

Chunk requests enqueue demand. `ChunkManager::update()` collects results and,
within a 2 ms main-thread budget, keeps up to `clamp(threads * 2, 4, 32)`
generation and `clamp(threads, 2, 16)` mesh jobs in flight. Chunks that
`LocationGenerator::isAirChunk` proves empty are published without a job, and
chunks with no visible geometry (all air, or solid and covered by solid
neighbour layers) finish meshing without one. Indirect sunlight is skipped
when it cannot change the direct light (all 15, or an opaque shell) and waits
while a neighbour is still queued for generation. Workers share a
priority-enabled pool: direct sunlight sweeps, generation prerequisites, and
urgent remeshes use the high tier; indirect spreads run at normal priority. Submitted jobs own snapshots, and identities
and revisions reject obsolete generation/mesh results after edits or replacement.
The existing uploaded mesh stays visible while its replacement is computed.

Priority regressions use a single controlled worker where exact ordering matters,
plus promise-based gates for worker queues. They cover near-first publication,
teleports, vertical movement, continuous arrivals, background progress, inherited
sky priority, caps, stable age, generation replacement, and mesh ordering.

```bash
ctest --test-dir build -L priority --output-on-failure --no-tests=error
```

## Lighting backlog benchmark

```bash
cmake --build build/tests-release --target lighting_benchmark --parallel 4
./build/tests-release/tests/lighting_benchmark
./build/tests-release/tests/lighting_benchmark teleport
```

The optimized benchmark preloads a radius-eight cube (4,913 chunks), overwrites it
with explicit air, and isolates the production lighting scheduler with eight
workers and no mesher. Startup measures the central chunk. Teleport moves focus
to a corner at update eight and measures response from that point. Both report
first direct light, completed indirect light, and full backlog completion.

Each update has a worker completion barrier; there is no artificial frame delay.
Reported frames are scheduler update opportunities, not measured rendering frames.
Target intervals start at zero for the initial dispatch or camera jump.
Elapsed times exclude preload/generation, rendering, mesh uploads, and user input.
The 2 ms dispatch budget makes counts hardware/load dependent. This benchmark is
manual and has no timing pass/fail threshold in CTest. Source supports
`LIGHTING_BASELINE` when compiled against saved pre-priority sources; it does not
contain a duplicate lighting algorithm. See `RESULTS.md` for the comparison.

## Distant terrain

The default suite includes surface/material agreement, deterministic geology,
stepped terrain, mixed-resolution seams, stepped-to-smooth joins, water,
finite-world bounds, coverage transactions and stale completions. See
[LOD.md](LOD.md) for architecture, tuning, benchmarks and optional Vulkan captures.
