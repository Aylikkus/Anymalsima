# Game Guidelines

See the [repository guidelines](../../AGENTS.md) for shared conventions.

## Responsibilities & Structure

`game.hpp/.cpp` owns the active state and defers state changes through the frame lifecycle. `states/` implements menus, world creation, and gameplay. `world/` contains procedural generation, chunks, streaming, meshing, sunlight, and raycasting. `components/` defines entity data; `items/` defines blocks, rocks, and materials; `utils/` maps enums to localized labels.

## Making Changes

Implement screen behavior through `IGameState` and the existing `Game::changeState` mechanism. Keep reusable widgets in `../ui/`. Coordinate item or material schema changes with JSON data in `../../assets/data/` and resource loading.

World generation and chunk processing run asynchronously. Review cancellation, synchronization, captured object lifetimes, and result ownership before changing worker tasks. Keep GPU submission and resource disposal coordinated with the renderer. For voxel changes, check coordinate conversions, chunk dimensions in `world/consts.hpp`, neighbor boundaries, mesh visibility, and sunlight propagation together.

## Persistence

Each world is a folder `saves/<seed>_<YYYY-MM-DD_HH-MM-SS>/` (see `world/save/save_catalog.hpp`):

- `world.json`: `WorldMeta` (format/generator versions, master seed, timestamps, camera).
- `world_map.dat`: `world_map_codec` sections `GENS`, `RGNS`, `RIVR`. Preview meshes are not stored.
- `regions/r.<x>.<z>.dat`: `region_file` sections `CHNK` (one `chunk_codec` blob per chunk, local index `slabY*64 + cz*8 + cx`) and `TOPS` (column ceilings).

Every sub-seed derives from `WorldGenSettings::seed` through `WorldGenSettings::fromSeed`. Bump `WorldMeta::GeneratorVersion` when chunk generation output changes.

`LocationManager` owns region residency. With a `RegionStore` it opens a region asynchronously and registers its chunks with their saved blobs; `ChunkManager` decodes those chunks instead of generating them. Unloading snapshots chunks whose `persistDirty` flag is set and queues a write. `RegionStore` runs at most one job per region at a time and keeps recently released regions warm in RAM. Callers of `ChunkManager::request` never see where data came from.

- Lighting is derived and never saved. `SunLightPropagator` writes must not set `persistDirty`.
- Gameplay edits to tiles must go through `withChunkLocked` followed by `ChunkManager::markModified`, so the chunk is saved and remeshed.
- `GameplayState` autosaves one region per frame every 300 s. Its destructor saves every region, flushes the store and writes `world.json`. Keep `regionStore` declared before the chunk systems that use it.
- New persistent data (units, factions, history) goes in a new fourcc section in the owning file. Use `world_map.dat` or a new root file for world-scope data, and region files for region-scope data. Older readers keep unknown sections untouched and write them back. Add round-trip and malformed-input tests under `tests/unit/game/world/save_codec_tests.cpp` or `tests/integration/region_persistence_tests.cpp`.

## Build & Validation

From the repository root, run `cmake --build build --target app --parallel`. Create a world, enter gameplay, move across chunk boundaries, and return through the affected states. For persistence changes, also travel 3+ regions away and back, leave with Escape, continue the world from LOAD WORLD, and check `saves/` for leftover `.tmp` or `.corrupt` files. For generation changes, record seed and settings so failures can be reproduced. Check cancellation and exit during background work, and inspect lighting and mesh seams for voxel changes.
