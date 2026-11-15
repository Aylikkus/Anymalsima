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
on its surface but transmits none. The strongest contribution wins. Water/liquid
opacity is 8; combined block/liquid opacity is their maximum.

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

Chunk requests enqueue demand. `ChunkManager::update()` collects results and
submits at most four generation jobs and two mesh jobs. Workers share a
priority-enabled pool: lighting, its generation prerequisites, and remeshing
newly lit chunks use the high tier. Submitted jobs own snapshots, and identities
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
