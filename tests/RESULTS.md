# Initial gameplay test results

Tested against the working tree on 2026-11-15, including pre-existing edits in
`src/game/world/tile.hpp` and `sunlight_propagator.hpp`. No gameplay fixes were
made as part of adding the suite. Failures remain enabled as regression tests.

Debug (Clang 22) and Release (GCC 16): **33 passed, 4 failed** in each build,
37 discovered cases, about 0.15 and 0.08 seconds respectively for execution.
Both builds report the same four failures. `BUILD_TESTING=OFF` configuration was
also checked: it omits the `world_logic_tests` target.

| Failing CTest case | Expected | Observed |
| --- | --- | --- |
| `game.world.sunlight_opaque_roof` | Sunlight 0 below an opaque roof at `(8,23,8)` | 15 |
| `game.world.sunlight_horizontal` | Positive sunlight at open tunnel entrance `(8,20,8)` | 0 |
| `game.world.sunlight_generated_ocean` | Sunlight 7 at `(8,383,8)`, after entering water at sea level 384 | 15 |
| `game.items.block_explicit_opacity` | Parse `properties.opacity = 3` | Throws: key `transparent` not found |

## Reproductions and code findings

**Opaque roof:** Two air chunks, Y=0..31, retain the actual `Tile` default
exposure. An opaque plane covers Y=24. After lighting, the roof and air above
receive 15, but air below also remains at 15. `TileLightning::sun_exposure`
defaults to 15. `SunLightPropagator::sweepSlabReadOnly` skips cells once their
incoming light reaches zero, and the all-dark early exit skips lower slabs.
Those untouched cells retain their initial exposure. The initial-lighting path
does not clear them as the dirty-column path does.

**Tunnel entrance:** Air starts dark. A roof at Y=21 covers X=8..15 in one chunk
column. The adjacent shaft at `(7,20,8)` receives sunlight 15, but the air at
`(8,20,8)` remains dark. The call to `propagateHorizontalSunlight` in
`SunLightPropagator::submitColumn` is currently commented out. The test requires
positive lateral light without specifying its exact falloff.

**Shallow ocean:** This integration case uses real generation and default tile
lighting, with seabed Y=382 and sea level Y=384. It produces water at Y=383 and
Y=384. With water opacity 8, the incoming sequence should be 15, 7, 0 at Y=384,
383, 382 respectively. `ChunkManager::registerChunk` records column top from
`region.land_height_m`, and the sunlight propagator starts at that height's slab
(23), below the top water slab (24). Thus attenuation through the top water cell
is skipped. This is separate from the default-light shadow problem; adjusting
the start height alone would not fix untouched dark cells below opaque terrain.

**Block opacity:** The minimal input is:

```json
{"name":"glass","material":"glass","properties":{"opacity":3}}
```

`from_json(BlockProperties&)` checks `contains("opacity")` but then reads
`at("transparent")`. This prevents explicit opacity definitions from loading.
Lighting fixtures use typed block definitions so this parser error does not
prevent testing the propagation rules independently.

## Validation limits

Address/undefined-behavior sanitizer instrumentation compiled, but linking failed:
the local GCC installation references `/usr/lib64/libasan.so.8.0.0`, which is
absent. No sanitizer execution results are claimed. The optional sanitizer CMake
switch remains available for a machine with the runtime installed.

Compiler warnings exposed existing unused variables, a pessimizing move in layer
generation, and signed/unsigned comparison in data storage; these were left
unchanged with production code. Tests require no manual game launch. Full
planetary generation, rendering and concurrent teardown remain outside this suite.

Run all cases with the commands in [README.md](README.md), or reproduce any row
with `ctest --test-dir build -R '<case-name>' -V`. The latest full output is also
saved by CTest in `build/Testing/Temporary/LastTest.log`.
