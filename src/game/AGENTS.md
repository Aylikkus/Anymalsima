# Game Guidelines

See the [repository guidelines](../../AGENTS.md) for shared conventions.

## Responsibilities & Structure

`game.hpp/.cpp` owns the active state and defers state changes through the frame lifecycle. `states/` implements menus, world creation, and gameplay. `world/` contains procedural generation, chunks, streaming, meshing, sunlight, and raycasting. `components/` defines entity data; `items/` defines blocks, rocks, and materials; `utils/` maps enums to localized labels.

## Making Changes

Implement screen behavior through `IGameState` and the existing `Game::changeState` mechanism. Keep reusable widgets in `../ui/`. Coordinate item or material schema changes with JSON data in `../../assets/data/` and resource loading.

World generation and chunk processing run asynchronously. Review cancellation, synchronization, captured object lifetimes, and result ownership before changing worker tasks. Keep GPU submission and resource disposal coordinated with the renderer. For voxel changes, check coordinate conversions, chunk dimensions in `world/consts.hpp`, neighbor boundaries, mesh visibility, and sunlight propagation together.

## Build & Validation

From the repository root, run `cmake --build build --target app --parallel`. Create a world, enter gameplay, move across chunk boundaries, and return through the affected states. For generation changes, record seed and settings so failures can be reproduced. Check cancellation and exit during background work, and inspect lighting and mesh seams for voxel changes.
