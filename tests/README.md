# Gameplay logic tests

The `world_logic_tests` target compiles production chunk generation and meshing
code and exercises the production chunk manager, layer generator, raycaster and
sunlight propagator. It does not link the game/UI/engine targets or bgfx library,
initialize a renderer, open a window, load runtime assets or require a device.
Normal project configuration still requires the vendored dependencies and
Intl/Gettext. Synthetic block/rock definitions live in `support/world_fixture.hpp`.

From the repository root:

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target world_logic_tests --parallel 4
ctest --test-dir build -L gameplay --output-on-failure --no-tests=error
```

There are 37 separately registered cases. A local run takes less than a second
after building. Checks remain active with `NDEBUG`/Release. Failures exit nonzero
and report the failed condition or expected/actual values with coordinates where
applicable. Known bugs are ordinary failing tests; they are not disabled or marked
as expected failures. See [RESULTS.md](RESULTS.md) for the initial findings.

Run just lighting, one failing case, or the cases that failed on the last run:

```bash
ctest --test-dir build -R sunlight --output-on-failure --no-tests=error
ctest --test-dir build -R '^game.world.sunlight_opaque_roof$' -V
ctest --test-dir build --rerun-failed --output-on-failure
```

The executable also accepts an individual case name, useful with a debugger:

```bash
gdb --args build/tests/world_logic_tests sunlight_horizontal
```

Build Release with address/undefined-behavior sanitizers (GCC or Clang):

```bash
cmake -S . -B build/tests-release -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Release -DANYMALSIMA_TEST_SANITIZERS=ON
cmake --build build/tests-release --target world_logic_tests --parallel 4
ctest --test-dir build/tests-release -L gameplay --output-on-failure --no-tests=error
```

The sanitizer option instruments the test executable and production algorithms
compiled into it. Vendor libraries are not instrumented. Set `BUILD_TESTING=OFF`
to omit project test targets and registration.

## Coverage and fixture rules

- Tile air/solid/liquid classification.
- Terrain surface and rock-layer transitions, sea-level water filling, vertical
  chunk seams, reused chunk contents and repeatable location generation.
- Rock-layer depth coverage, depth limits and unavailable base-rock errors.
- Chunk lookups at each axis boundary, missing chunks, shared ownership after
  unregistering, and monotonic top-height bounds.
- Nearest ray hits, range limits, water hits, negative directions, chunk crossings
  and normalized camera directions.
- Empty/isolated meshes, greedy prism merging and exposed area, cross-chunk face
  culling, world positions, material/light tint and triangle winding.
- Direct sunlight, opaque shadows, partial block opacity, water attenuation,
  slab boundaries, late slab arrival, lateral propagation, relighting after edits
  and rejection of completed results from an older generation.
- Actual chunk generation followed by sunlight in a shallow ocean.
- Default and explicit block opacity deserialization.

Most lighting fixtures start with zero exposure so tests measure computed light.
The opaque-roof and generated-ocean regressions retain production defaults to
catch initialization errors. Opacity semantics follow the current vertical pass:
a tile receives incoming light, then its opacity reduces light for the next tile.
Opaque surfaces can therefore receive 15 while tiles behind them receive 0.
The horizontal test requires positive light at an open tunnel entrance without
imposing a particular future lateral falloff model.

Worker tests use one real pool worker, explicit completion waits with three-second
bounds, and CTest process timeouts. They do not use scheduling sleeps. Fixtures
wait for workers before destroying captured owners, and mutate chunks only after
workers have finished. The stale-result case waits for computation, edits the
column before consuming the result, then verifies the old light is discarded.

These tests do not cover full planetary generation, every rock catalog/seed,
concurrent unload while work is running, detached-job exception recovery,
world-generation cancellation or GPU rendering. Passing cases establish the
specific rules above, not correctness of all world logic.

Add a `TEST(case_name)` function under `unit/` or `integration/` and register the
same name in `CMakeLists.txt`. No external assertion framework is needed.
