# Handoff: generated structures (megastructures)

Status: step 1 in progress (see Progress). Trees (`docs/handoff/trees.md`) are the first
generated structure. Towns, castles and other megastructures will reuse what
trees built, without billboards. This plan names the pieces that are
tree-specific today and the general API they move into.

## What trees already solve

| Concern | Trees today | General form |
| --- | --- | --- |
| Placement | Ordinary trees per region (`generateRegionTrees`); landmarks from a world-wide list (`placeTreeLandmarks`) | A structure kind places sites per region or world-wide |
| Reach | Ordinary: one region. Landmarks: `ChunkManager::landmarkRadius`, from the widest species | Radius per kind, from its declared extent |
| Voxels in chunks | `TreeColumn`, `placeTreeWood` writes only into air, raises column tops before sunlight | One stamping path for every kind's instances overlapping a chunk |
| Voxels at distance | `TreeWoodSource` -> `StructureLod::setTrees` -> `tree_wood_lod::rasterize` (levels 0-2) | A voxel source any kind implements: `rasterize(slab, level)` |
| Edit detection | Living wood is `natural_growth`, treated as air | Generated structure voxels are part of "generated", not edits |
| Persistence | `TreeOverrideMap` in `trees.dat` (cuts, planted, removals) | Per-kind override records in one structure file |
| Far rendering extras | Billboards, crowns (`TreeRenderer`) | Kind-specific; not part of the core |

## Living structures and landmarks (agreed 2026-11-18)

- Structures are **living**: they change after generation. NPC settlements grow, decay and get rebuilt; an elder may later be inhabited (elves); the player damages and builds. The API treats every structure as a sequence of revisions, not a one-time stamp. A voxel source has a revision, and far proxies and shade follow it.
- **Landmark is a role, not a kind or a size class.** Today "landmark" means a massive tree from the world-wide list (`Tree::landmark()`, `TreeSizeClass::Massive`). In general a landmark is any structure in the world-wide index: known and drawn to the view distance, kept while its regions are unloaded, and given its own reach radius. Generated or built, tree or town, any structure can hold the role.
- **Open (maintainer):** whether and how player-built or NPC-built structures become landmarks (size, significance, fame, a lore rule). The API only needs promote/demote to exist; the rule comes later.

## Known limits found while scaling elders (2026-11-18)

- **Fixed:** chunk mesh upload crashed (`bgfx::update` on handle `0xffff`) once bgfx ran out of dynamic buffer handles (4096). Every chunk, LOD patch, structure slab and tree-wood slab holds one. Limits are now 16384 (root `CMakeLists.txt`); failed creations are skipped and retried, and `IRenderer::failedDynamicMeshes` counts them.
- **Fixed:** landmark lookups were hard-coded to two regions (about 256 tiles of reach); `TreeMaxLandmarkReach` is now 1024.
- **Fixed (step 1):** a whole tree was rasterized at one level, chosen by the distance to its nearest point, so a 1 km elder near the player was full-resolution end to end. Levels are now planned per slab.
- **Fixed:** seen from high up (an elder's crown), its real chunks were loaded but never drawn, so the slabs stayed and edits did not show. A column counts as detailed only while its ground surface is within the vertical detail range (`DetailCoverage`), and both the chunk pass and the slab hand-over required that. `TerrainLodManager::drawsChunk` now also accepts any chunk above its column's surface band, where distant terrain draws nothing.
- **Open:** distant terrain ignores structures for sky light (`terrain_lod.frag` uses full sky above water). Loaded chunks are shaded by wood blocks through `SunLightPropagator`, so shade appears only when chunks load.

## Plan

1. **Voxel source API.** Replace `TreeWoodSource` with a general `StructureVoxelSource` (key, voxel bounds, revision, `rasterize(slab, level)` returning a slab with per-cell block ids, not one block). Trees implement it with the existing rasterizer. `StructureLod` picks the level **per slab** by distance (not per tree) and merges slabs into larger meshes (for example 4x4x4 slabs per mesh) to cut handle counts and draw calls.
2. **Sky occlusion at distance.** Structures write a camera-centred occlusion map (like `LodLightMap`: top-down cells holding the occluder's height and coverage), built from coarse rasterizations of the same voxel sources. `terrain_lod.frag`, joins and structure slabs darken sky light below an occluder. The hand-over has to match chunk sunlight.
3. **Chunk stamping.** One path for every kind's instances overlapping a chunk (generalizing `TreeColumn` / `placeTreeWood` / column tops), with the reach radius per kind.
4. **Generated versus edited.** Edit detection (`TerrainEditTracker`, `ChunkManager::isEdited`) compares with terrain plus generated structures, so a generated castle is not stored as edits and its damage is.
5. **Persistence.** Per-kind override records (removed, damaged, rebuilt, planted) in one structure file; `trees.dat` becomes the tree section of it, keeping the v2 reader.

Billboards, crowns and growth stay in the tree code.

## Progress

### Step 1: voxel source API (in progress)

Done:

- `world/tile_box.hpp`: `TileBox` (tiles `[x0, x1)` per axis); `TreeTileBox` is an alias.
- `world/structure_voxels.{hpp,cpp}` (pure, in the headless test library):
  - `Source`: `key` (stable across revisions), `revision`, `bounds` (unwrapped X), `rasterize(level, sx, sy, sz)` returning a `Slab` or nothing. Implementations must be thread-safe; a changed structure is a new source with the same key.
  - `Slab`: bits plus one `block`, or per-cell ids in `blocks` for structures of several blocks; `merge` keeps the first structure's cells where two overlap.
  - `plan(bounds, eye, wrap, previous)`: covers the box exactly once with slabs of levels 0-2 by distance (`Limits` 512 / 1536 tiles, `Margin` 64 of hysteresis against the previous plan), nearest first. `keyOf`, `rasterizeAll`, `markEnclosed`.
- Trees: `tree_wood_lod::voxels(source)` (a `TreeVoxels` source: key = `TreeInstance::key`, revision from age, skeleton and block); `TreeRenderer::landmarkVoxels` lists them; `TerrainLodManager::setStructures` / `StructureLod::setStructures` take any sources.
- `StructureLod`: per structure, the plan is redone when the eye crosses a 32-tile cell or a source or build changes; only planned slabs not yet rasterized for the current revision go to a job (two jobs, nearest structure first). Kept slabs are not remeshed; replaced and new ones (and their face neighbours) are.
- `Source::changedSince(older)`: where a new revision may differ (a box), or everywhere. `StructureLod` then rebuilds only planned slabs touching the box and keeps the rest; a rebuilt slab that comes back empty is dropped. Trees diff their skeleton segments: a branch cut on the 1000 m elder rebuilds 2 slabs instead of all of them. Damage to towns and castles should report the edited box the same way.
- Tests: `structure_voxels_plan_levels`, `_plan_hysteresis`, `_plan_wraps`, `_slab_merge`, `tree_wood_lod_cut_is_local`; `tree_wood_lod_slabs` still checks level 0 against the rasterizer.
- Measured, elder smoke scene (`SMOKE_FOREST=elder SMOKE_RADIUS=8`, 18 elders at 450-500 m, 100x shipped density): 4604 voxel slabs (1555 / 1050 / 1999 at levels 0 / 1 / 2), down from 5574; no failed meshes. The scene still never settles: the voxel budget (4x the structure budget) is full and the remaining slabs wait for room. At shipped density (0-2 elders in view) this does not occur.

Next:

- Merge coarse slabs (levels 1 and 2, which never hand over to chunks) into meshes of several slabs, to cut handles and draw calls. Level 0 slabs stay single so they hide exactly where their chunk is drawn.
- Then step 2 (sky occlusion at distance, wood only, agreed 2026-11-18).
