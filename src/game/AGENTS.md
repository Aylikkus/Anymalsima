# Game Guidelines

See the [repository guidelines](../../AGENTS.md) for shared conventions.

## Responsibilities & Structure

`game.hpp/.cpp` owns the active state and defers state changes through the frame lifecycle. `states/` implements menus, world creation, and gameplay. `world/` contains procedural generation, chunks, streaming, meshing, sunlight, and raycasting. `components/` defines entity data; `items/` defines blocks, rocks, and materials; `utils/` maps enums to localized labels.

## Making Changes

Implement screen behavior through `IGameState` and the existing `Game::changeState` mechanism. Keep reusable widgets in `../ui/`. Coordinate item or material schema changes with JSON data in `../../assets/data/` and resource loading.

World generation and chunk processing run asynchronously. Review cancellation, synchronization, captured object lifetimes, and result ownership before changing worker tasks. Keep GPU submission and resource disposal coordinated with the renderer. For voxel changes, check coordinate conversions, chunk dimensions in `world/consts.hpp`, neighbor boundaries, mesh visibility, and sunlight propagation together.

### X wrapping

The world wraps on X (the world map is generated tileable): tile `x` and `x + W` are the same column, where `W = world.width * 128`. Z ends at the map edges. Use `WorldWrap` (`world/world_wrap.hpp`, from `World::wrap()` or `ChunkManager::wrap()`). Never clamp or skip X at `0`/`W`.

- Stored coordinates (`ChunkCoord`, regions, edit keys, saved camera) are canonical, in `[0, W)`. Wrap X before any lookup or neighbor step (`wrapTile`/`wrapChunk`/`wrapRegion`). Map-less fixtures (`width == 0`) disable wrapping.
- Distances and draws use the copy nearest the camera (`deltaChunk`, `imageX`). Chunks are drawn at `camCX + deltaChunk(coord.x, camCX)`, and `LightVolumeManager::bind` gets that draw origin, so `block.vert` light lookups match. LOD patches stay canonical and are drawn shifted by 0 or ±W through `u_lod_shift` and `u_lod_patch`.
- Terrain noise must repeat every `W` tiles: pass `TerrainNeighborhood::periodX()` to `SoilGenerator::field`/`sample` and to `gradientField`. A lattice that cannot be periodic (the rotated soil-choice field) blends across the seam.
- Distant terrain wraps only when `TerrainLod::wrapsExactly(W, viewDistance)` holds: the width is a multiple of 16 regions (one LOD root), and the view distance plus two roots is at most half of `W`. New worlds are 512 regions wide. Other widths keep distant terrain bounded on X; their voxels still wrap.

### Lighting

Tiles carry two 4-bit light levels: `sun_exposure` (`SunLightPropagator`) and `artificial_light` (`BlockLightPropagator`). `LightVolumeManager` encodes both into RGBA8 volumes (R sun, G block light, BA block-light chroma) sampled by `shaders/block/block.frag`. Light changes go through `ChunkManager::markLightChanged` and never remesh.

- Emission is data: an optional `"light"` object on a block (`items/light_emission.hpp`: `enabled`, `radius` up to 8192, `falloff` exponent of `1 - d/radius`, `intensity`, `color`, `cone`). Items and entities must reuse `LightEmission`; a placed or carried emitter is a `LightSource` (`world/light_sources.hpp`) with a world position and a free direction.
- Near field: within `LightEmission::NearRange` (15) blocks, `BlockLightPropagator` flood-fills each emitter on workers from immutable per-chunk opacity snapshots. The effective distance is the straight line plus the path's detour and translucency excess, so open space is round and walls cast soft shadows. A chunk depends only on emitters in its 3x3x3 neighborhood. Colored light stores chroma in a side buffer only for chunks a colored light reaches.
- Far field: lights with a radius above 15 are also kept in `LightSourceRegistry`. `LightRenderer` sends up to 32 of them to `u_lights` for `block` and `terrain_lod` (per pixel, scaled by sky exposure, no voxel occlusion), and draws depth-tested glare sprites in `DrawLayer::GAME_GLOW`. Every program that includes `shaders/world_surface.sh` must bind `LightRenderer::appendUniforms`; `TerrainLodManager::setExtraUniforms` does this for distant terrain.
- `u_light_info.y` darkens sky light (the debug SKY slider); it is not saved.

## Persistence

Each world is a folder `saves/<seed>_<YYYY-MM-DD_HH-MM-SS>/` (see `world/save/save_catalog.hpp`):

- `world.json`: `WorldMeta` (format/generator versions, master seed, timestamps, camera).
- `world_map.dat`: `world_map_codec` sections `GENS`, `RGNS`, `RIVR`. Preview meshes are not stored.
- `regions/r.<x>.<z>.dat`: `region_file` sections `CHNK` (one `chunk_codec` blob per chunk, local index `slabY*64 + cz*8 + cx`) and `TOPS` (column ceilings).
- `terrain_edits.dat`: `terrain_edits_codec` section `EDIT`, the world-wide index of edited chunk columns that distant terrain shows (`world/terrain_edits.hpp`). It is derived data: region files hold the truth.
- `light_sources.dat`: `light_sources_codec` section `LITE`, the world-wide index of far-reaching emissive tiles (see Lighting). Also derived.

Every sub-seed derives from `WorldGenSettings::seed` through `WorldGenSettings::fromSeed`. Bump `WorldMeta::GeneratorVersion` when chunk generation output changes.

`LocationManager` owns region residency. With a `RegionStore` it opens a region asynchronously and registers its chunks with their saved blobs; `ChunkManager` decodes those chunks instead of generating them. Unloading snapshots chunks whose `persistDirty` flag is set and queues a write. `RegionStore` runs at most one job per region at a time and keeps recently released regions warm in RAM. Callers of `ChunkManager::request` never see where data came from.

- Distant terrain shows edits through `TerrainEdits`. Generated tiles are solid exactly up to the generated surface, so each edited column stores a ground override (topmost solid tile at or below that surface) and structure voxels (every solid tile above it). `TerrainEditTracker` recomputes columns from `WorldEditor::blockChanged` and from saved chunks that differ from generation (`ChunkManager::isEdited`, enabled only when the save's `generatorVersion` is at least `WorldMeta::SurfaceVersion`, the oldest generator with today's surface heights; bump `SurfaceVersion` only when surface heights change). Edits that bypass `WorldEditor` are not shown at distance until their chunk is reloaded. `GameplayState` writes `terrain_edits.dat` with each autosave cycle and on exit.
- Lighting is derived and never saved. `SunLightPropagator` and `BlockLightPropagator` writes must not set `persistDirty`. The one exception is `light_sources.dat` (`light_sources_codec`, section `LITE`): a derived world-wide index of far-reaching emissive tiles (position + block id), so lighthouses glow while their regions are unloaded. Emission values are re-read from block definitions on load, and chunk loads reconcile the index. `GameplayState` writes it with `terrain_edits.dat`.
- Block ids are assigned in data-file order and saved raw, so new block files must sort after existing ones (e.g. `blocks/test_light.json` after `blocks/soils.json`) or existing saves remap.
- Gameplay edits to tiles must go through `withChunkLocked` followed by `ChunkManager::markModified`, so the chunk is saved and remeshed. Prefer `WorldEditor` (`world/world_editor.hpp`) for block placement and removal: it also remeshes border neighbors, raises column tops and recomputes sunlight.
- `GameplayState` autosaves one region per frame every 300 s. Its destructor saves every region, flushes the store and writes `world.json`. Keep `regionStore` declared before the chunk systems that use it.
- New persistent data (units, factions, history) goes in a new fourcc section in the owning file. Use `world_map.dat` or a new root file for world-scope data, and region files for region-scope data. Older readers keep unknown sections untouched and write them back. Add round-trip and malformed-input tests under `tests/unit/game/world/save_codec_tests.cpp` or `tests/integration/region_persistence_tests.cpp`.

## Build & Validation

From the repository root, run `cmake --build build --target app --parallel`. Create a world, enter gameplay, move across chunk boundaries, and return through the affected states. For persistence changes, also travel 3+ regions away and back, leave with Escape, continue the world from LOAD WORLD, and check `saves/` for leftover `.tmp` or `.corrupt` files. For generation changes, record seed and settings so failures can be reproduced. Check cancellation and exit during background work, and inspect lighting and mesh seams for voxel changes.
