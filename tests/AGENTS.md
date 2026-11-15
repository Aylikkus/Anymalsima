# Testing Guidelines

See the [repository guidelines](../AGENTS.md) and [CTest skill](../.agents/skills/ctest-game-testing/SKILL.md).

## Location & Organization

Keep all project-owned automated tests in this top-level tree. Organize unit tests by the production module they cover and put cross-module scenarios in integration tests. The intended layout is:

```text
tests/
  CMakeLists.txt
  unit/game/world/    # Tiles, coordinates, raycasting, meshing, lighting
  unit/game/items/    # Block/material definitions and validation
  unit/engine/        # Settings, serialization, signals, math utilities
  integration/       # World pipelines, streaming, save/load, cancellation
  fixtures/          # Small, read-only JSON and compatibility examples
  support/           # Shared builders, fake assets, synchronization helpers
```

Create these files/directories as tests are implemented. Additional modules can add `unit/<module>/`. Do not put tests or fixtures in runtime `assets/`.

## Coverage Priorities

Prioritize crucial gameplay rules: air/solid/liquid classification; coordinate and chunk-edge behavior; raycast hits/misses; sunlight attenuation; visible mesh faces and material selection; seeded generation invariants; stale-result rejection and teardown during jobs. Read dimensions and axis conventions from current code.

Cover supporting engine behavior where gameplay depends on it, particularly settings/save compatibility and data validation. Test inventory, needs, and movement rules as behavior is implemented; avoid tests that merely restate component fields.

## Naming & Execution

Use `*_tests.cpp` files and CTest names such as `game.world.chunk_boundaries`. Label by level and domain, for example `unit;gameplay;world` or `integration;gameplay;threading`. Default logic tests should run without a window, GPU, audio device, or network. Label device-dependent tests separately.

Coding agents usually run inside a sandbox with no display server, GPU, or audio device; for example, `./build/src/app/app` fails there with an X authorization error. Prefer headless tests that exercise production logic directly, and validate changes through CTest instead of launching the app. Opt-in device targets such as `lod_render_smoke` may not run in that environment. When a change needs visual or interactive confirmation, say that it is unverified and give the user manual reproduction steps.

CTest registration and a small Release-safe check/registration helper are implemented in `CMakeLists.txt` and `support/test.hpp`. Add cases to the driver and CMake registration together; each CTest case runs in its own process. See `README.md` for build, filtering, and sanitizer commands. Use deterministic inputs, bounded synchronization, and writable scratch paths under the build tree. No numeric coverage threshold is currently required.
