# Handoff: generated structures (megastructures)

Status: steps 1–5 done (see Progress). Trees (`docs/handoff/trees.md`) are the first
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

For an NPC settlement that adds a watchtower and retires a castle wing, the
settlement owns the castle revision. It publishes a new source under the
castle's existing key, with `changedSince` covering the tower and wing. The
landmark mesh and shade can update while the region is unloaded, and newly
generated chunks use that revision. Reconciliation for existing chunks needs
the previously applied revision, the new source, and the voxel edits. Tiles
that still match the previous baseline can follow the new revision. Conflicts
with damage or player construction preserve the actual voxel when it differs
from the previous baseline. Step 5 stores that baseline with the saved slab
so the comparison also works after loading.

## Known limits found while scaling elders (2026-11-18)

- **Fixed:** chunk mesh upload crashed (`bgfx::update` on handle `0xffff`) once bgfx ran out of dynamic buffer handles (4096). Every chunk, LOD patch, structure slab and tree-wood slab holds one. Limits are now 16384 (root `CMakeLists.txt`); failed creations are skipped and retried, and `IRenderer::failedDynamicMeshes` counts them.
- **Fixed:** landmark lookups were hard-coded to two regions (about 256 tiles of reach); `TreeMaxLandmarkReach` is now 1024.
- **Fixed (step 1):** a whole tree was rasterized at one level, chosen by the distance to its nearest point, so a 1 km elder near the player was full-resolution end to end. Levels are now planned per slab.
- **Fixed:** seen from high up (an elder's crown), its real chunks were loaded but never drawn, so the slabs stayed and edits did not show. A column counts as detailed only while its ground surface is within the vertical detail range (`DetailCoverage`), and both the chunk pass and the slab hand-over required that. `TerrainLodManager::drawsChunk` now also accepts any chunk above its column's surface band, where distant terrain draws nothing.
- **Fixed (step 2):** distant terrain ignored structures for sky light, so shade appeared only when chunks loaded. Surfaces without voxel light now read the sky occlusion map.
- **Fixed (2026-11-19):** walking up to an elder, whole chunks of open ground turned black and stayed so for seconds. See "Light hand-over when chunks load" below.

## Light hand-over when chunks load (2026-11-19)

How a chunk gets its sky light, in order:

1. **Estimate** (`ChunkManager::estimateSkyLight`, when generation publishes the chunk): full sky down to the first solid tile, for chunks above the column's *terrain* surface. It knows nothing about structures.
2. **Direct sweep** (`SunLightPropagator::sweep`, one job per chunk column): sunlight falls from the column top (`ChunkManager::getColumnTopY`) down through every slab. This used a conservative tree column top before the sky heightmap and shared structure projection were introduced.
3. **Spread** (indirect light, one job per chunk, needs the direct light of its 26 neighbours): the final value.

The bug: a sweep that reaches a slab that is not generated yet does not invent sky through it. It used to publish zero for every chunk below it as their "lower bound". Under an elder the column top is up to about 1000, so a column needs up to 60 slabs above the ground, generated as sky prerequisites (`requestLighting`). Until the last one arrived, the ground chunks were black, and the spread jobs of their neighbours used those zeros too.

Now:

- Chunks below a missing slab are **blind** (`State::blind`). Their tiles keep what they showed (the estimate, or earlier full light), neighbours spread from those values, and they get no spread job until a complete sweep. When the slab arrives its column is swept again (`sweepPending` keeps the blind chunk queued until the sweep is dispatched).
- A chunk that has shown the estimate keeps it through its first complete sweep, until its spread result lands. The direct lower bound has no bounce, so it is black under a wide limb.
- `SunLightPropagator::isEstimated`: never fully lit and showing the estimate. `LightVolumeManager::bind` sets `u_light_chunk.w = 2` for these chunks, and `block.frag` multiplies their sky light by the structure sky occlusion map (step 2), as on distant terrain. Shade under limbs therefore goes distant terrain (map), then estimated chunk (estimate times map), then swept chunk (voxel light). The map matches voxel light within one level, so each step changes little.
- Test: `sunlight_unknown_upper_slab_keeps_estimate`. Smoke: the elder approach (`SMOKE_APPROACH`) logs `approach_black columns=` (loaded columns within 128 tiles whose open ground reads no sky at four probes) and `black_frames` / `max_black` in `approach_done`. Measured (`SMOKE_TERRAIN=2 SMOKE_FOREST=elder SMOKE_ELDER_SPACING=3000 SMOKE_RADIUS=8 SMOKE_APPROACH="1200 0.5 40"`): before, black columns in 1541 of 2555 frames, at most 18 at once; after, none in 2590 frames. Mean frame 12.8 ms against 13.3 ms before.

The blind/estimate path is a stopgap. The structural fix is the sky
heightmap in `sky-light.md`: structures report their sky tops through a
`Source` query, and sunlight no longer needs the slabs above the ground
(step 2 there, done: with a heightmap, chunks are never blind, and only
headless fixtures without a world map still use this path).

### Bottlenecks the structure API must respect

These come from the light pipeline and chunk loading. They decide how far a structure kind can push height, footprint and edit rate.

- **Column top and sky prerequisites.** The sky heightmap seeds sunlight without requesting overhead slabs (see `sky-light.md` step 2). When gameplay requests those slabs, the column projects its actual voxel tops and uses `Source::occupancy` to skip terrain generation and structure rasterization in known air (step 3).
- **Tree sets before slabs.** A slab under a landmark is generated only once every region tree set it needs is built (`treesReady`, landmark reach up to `TreeMaxLandmarkReach`). Kinds with world-wide reach should have their per-region data cheap and ready before chunk generation asks for it.
- **The estimate ignores structures.** Until the sweep lands, structure shade comes only from the sky occlusion map. The map holds wood only (no crowns), at 4-tile texels, within 2 km. Its footprint takes about 3 s per elder, and until a footprint exists an estimated chunk under it is fully lit, so the shade appears later. Sky light step 4 now projects wood directly from its source and replaces the footprint input.
- **Edits in a blind column.** Opacity edits to a chunk whose column is still blind do not change its sky light until the missing slabs arrive (the chunk keeps its estimate or old light). This is harmless at walking pace. A structure that edits itself quickly near the player (a growing settlement, collapse) needs its columns' slabs generated first.
- **Spread waits for neighbours.** A chunk's final light needs the direct light of its 26 neighbours. A structure that dirties many columns at once (a large revision) relights a wide area: one sweep per column and one spread per chunk, at most 8 jobs in flight and 2 ms of dispatch per frame. Report a tight `changedSince` box, which shrinks the dirtied area.
- **Light volume uploads.** At most 64 volume rebuilds per frame (1.5 ms budget, `LightVolumeManager`). A completed column under an elder republishes many chunks at once; the rest catch up over the next frames, nearest first.

## Plan

1. **Voxel source API.** Replace `TreeWoodSource` with a general `StructureVoxelSource` (key, voxel bounds, revision, `rasterize(slab, level)` returning a slab with per-cell block ids, not one block). Trees implement it with the existing rasterizer. `StructureLod` picks the level **per slab** by distance (not per tree) and merges slabs into larger meshes (for example 4x4x4 slabs per mesh) to cut handle counts and draw calls.
2. **Sky occlusion at distance.** Structures write a camera-centred occlusion map (like `LodLightMap`: top-down cells holding the occluder's height and coverage), built from coarse rasterizations of the same voxel sources. `terrain_lod.frag`, joins and structure slabs darken sky light below an occluder. The hand-over has to match chunk sunlight.
3. **Chunk stamping.** One path for every kind's instances overlapping a chunk (generalizing `TreeColumn` / `placeTreeWood` / column tops), with the reach radius per kind.
4. **Generated versus edited.** Edit detection (`TerrainEditTracker`, `ChunkManager::isEdited`) compares with terrain plus generated structures, so a generated castle is not stored as edits and its damage is.
5. **Persistence.** Per-kind override records (removed, damaged, rebuilt, planted) in one structure file; `trees.dat` becomes the tree section of it, keeping the v2 reader.

Billboards, crowns and growth stay in the tree code.

## Progress

### Step 1: voxel source API (done)

Done:

- `world/tile_box.hpp`: `TileBox` (tiles `[x0, x1)` per axis); `TreeTileBox` is an alias.
- `world/structure_voxels.{hpp,cpp}` (pure, in the headless test library):
  - `Source`: `key` (stable across revisions), `revision`, `bounds` (unwrapped X), `rasterize(level, sx, sy, sz)` returning a `Slab` or nothing. Implementations must be thread-safe; a changed structure is a new source with the same key.
  - `Slab`: bits plus one `block`, or per-cell ids in `blocks` for structures of several blocks; `merge` keeps the first structure's cells where two overlap.
  - `plan(bounds, eye, wrap, previous)`: covers the box exactly once with slabs of levels 0-2 by distance (`Limits` 192 / 640 tiles, `Margin` 64 of hysteresis against the previous plan), nearest first. The chunk draw box plus a two-slab margin always uses level 0 so the proxies can hand over to exact chunk meshes. `keyOf`, `rasterizeAll`, `markEnclosed`.
- Trees: `tree_wood_lod::voxels(source)` (a `TreeVoxels` source: key = `TreeInstance::key`, revision from age, skeleton and block); `TreeRenderer::landmarkVoxels` lists them; `TerrainLodManager::setStructures` / `StructureLod::setStructures` take any sources.
- `StructureLod`: per structure, the plan is redone when the eye crosses a 32-tile cell or a source or build changes; only planned slabs not yet rasterized for the current revision go to a job (two jobs, nearest structure first). Kept slabs are not remeshed; replaced and new ones (and their face neighbours) are.
- `Source::occupancy()`: the slabs (per level) that may hold voxels, computed once per source on the pool; plans visit only those. A sparse structure in a huge box (a massive tree's limbs) is otherwise planned and rasterized slab by empty slab. Towns and castles should report their occupied slabs too.
- Jobs merge their slabs with the kept ones on the worker (`StructureLod::Built`: the map and the changed keys); the main thread only swaps pointers.
- A proxy that leaves the plan because the level changed is retired: drawn until all its replacements are drawable (or their real chunks are drawn), and the replacements under it wait, so level swaps show neither holes nor two levels. Level 0 slabs drawn as real chunks well inside the detail box get no proxy mesh (`setChunkDrawn`, `DrawnMargin`). `TerrainLodManager::structureGaps` counts slabs that draw nothing (debug, smoke harness).
- `Source::changedSince(older)`: where a new revision may differ (a box), or everywhere. `StructureLod` then rebuilds only planned slabs touching the box and keeps the rest; a rebuilt slab that comes back empty is dropped. Trees diff their skeleton segments: a branch cut on the 1000 m elder rebuilds 2 slabs instead of all of them. Damage to towns and castles should report the edited box the same way.
- Tests: `structure_voxels_plan_levels`, `_plan_hysteresis`, `_plan_wraps`, `_slab_merge`, `tree_wood_lod_cut_is_local`; `tree_wood_lod_slabs` still checks level 0 against the rasterizer.
- Measured, elder smoke scene (`SMOKE_FOREST=elder SMOKE_RADIUS=8`, 18 elders at 450-500 m, 100x shipped density): 4604 voxel slabs (1555 / 1050 / 1999 at levels 0 / 1 / 2), down from 5574; no failed meshes. The scene still never settles: the voxel budget (4x the structure budget) is full and the remaining slabs wait for room. At shipped density (0-2 elders in view) this does not occur.

- Coarse slabs (levels 1 and 2, which never hand over to chunks) are drawn merged: `StructureLod::GroupSide`^3 = 64 slabs of one level share one mesh (`Group`, keyed by `groupOf`). Each coarse slab keeps its CPU mesh (`Proxy::merged`); `upload` concatenates a group's members into one dynamic mesh when its members or their meshes change, within the per-frame upload limit. Members are the drawable wanted slabs not under a retired proxy, plus the retired coarse slabs. `upload` (no longer `draw`) releases retired proxies; when one goes, every changed group is rebuilt in the same frame regardless of the limit, so a level swap still shows neither a hole nor both levels. Level 0 slabs stay single, so each hides exactly where its chunk is drawn.
- Measured, elder smoke scene (`SMOKE_FOREST=elder SMOKE_RADIUS=8`, 6000 frames): 5433 coarse slabs drawn as 707 meshes. Elder approach walk (`SMOKE_APPROACH="1200 0.5 40"`), against the commit before: mean frame 13.1 ms (13.2), `lod_render` mean 1.6 ms (1.5), no holes within 128 tiles (none), frames with holes within 512 tiles 306 of 2612, at most 3 slabs (214 of 2551, at most 4). Those far holes predate the groups (slabs still waiting for a mesh); not investigated.
- Costs: coarse meshes are held twice (CPU copy for regrouping, GPU group mesh); a group rebuild copies up to 64 slab meshes on the main thread.

### Step 2: sky occlusion at distance (done)

Sky light step 4 replaced the footprint-based input described below.
`SkyHeightMap` now publishes the map from generated terrain, projected
landmark wood, edits and saved overrides. The renderer still samples the
same texture layout. See [Sky light](sky-light.md#step-4-far-and-gameplay).

Voxel sunlight (`SunLightPropagator`) falls straight down at 15 until something opaque stops it, then spreads sideways losing one level per tile. So under an occluder a point at height y gets `15 - d`, d being the 4-neighbour distance in tiles to the nearest column open to the sky down to y, however high the occluder is. Distant surfaces reproduce that from a top-down map; any structure kind gets it from its `Source` alone: a tree's limbs, a castle's overhangs and covered yards, a bridge. Wood only, no crown shade (agreed 2026-11-18).

- `structure_voxels::footprint(source, occupancy, previous, changed)` (pure): per column of level-2 cells (4x4 tiles), the top of the highest solid cell, from the level-2 rasterization (cells solid when a quarter of their tiles are, so a roof one tile thick casts, a limb under about two tiles does not; its voxel shade is one light level). With an earlier footprint and `Source::changedSince`, only the slab columns under the changed box are rasterized again. `StructureLod` computes one per source on the pool after its occupancy and caches them by key (`footprintCache`, 64), so a structure coming back into view with the same revision reuses its footprint.
- `SkyOcclusionMap` (`world/sky_occlusion_map.{hpp,cpp}`, pure): 1024² texels of 4 tiles (4 km) around the camera, recentred in steps of 512 tiles like `LodLightMap`. Footprints are placed at their copy nearest the centre (X wraps). Each texel holds 4 thresholds: for k = 0..3, the lowest occluder top within k texels (k passes of a cross-shaped min filter; open sky = `SkyOcclusionMap::Open`). A point at height y is k texels from open sky for the smallest k whose threshold is at most y (4 or more: about 0), and gets `skyAtDistance(k)` = `(15 - (4k - 1.5)) / 15`, the voxel light at the texel centre. Because the thresholds depend on height, the flared base of a trunk does not darken the trunk above it, while the ground under a wide roof is dark. Thresholds are tops of coarse cells, so a point counts as above one from 8 tiles below it (fully from 4 below): an occluder's own upper faces stay lit.
- `StructureLod::skyOcclusion()` rebuilds it on the pool when the set of footprints changes or the eye recentres it (null when nothing casts). `LightRenderer::setSkyOcclusion` uploads it (RGBA16F, point sampled) and `appendUniforms` binds `u_sky_occlusion` (block stage 6, terrain_lod stage 7) and `u_sky_occlusion_origin`. `surfaceStructureSky` in `shaders/world_surface.sh` samples half a cell in front of the face (the air that lights it, like the light volumes) and blends the four nearest texels; `terrain_lod.frag` and the block program's draws without a light volume (joins, structure slabs) multiply their sky light by it. Constants shared by both sides: `TilesPerTexel`, `Open`, `Fade`, `skyAtDistance` (keep the shader's `SURFACE_*` defines in sync).
- Hand-over: loaded chunks keep their voxel sunlight; the map agrees with it within one light level at texel centres (tests below). In the smoke scene the shade of the limbs continues from detailed chunks into distant terrain without a visible seam.
- Tests: `structure_footprint_tops`, `structure_footprint_incremental`, `sky_occlusion_matches_sunlight` (a roof against `15 - d`), `sky_occlusion_by_height` (two roof heights; a tower on a plinth), `sky_occlusion_wraps`. Visual check: `SMOKE_CAMERA="5857 6484 1.5708 -0.5 1000 0" SMOKE_TERRAIN=2 SMOKE_FOREST=elder SMOKE_RADIUS=4 build/tests/lod_render_smoke <prefix> 3000 lights` (elder from 1 km), with `SMOKE_NO_OCCLUSION=1` for the image without the map.
- Measured (`TREE_BENCH_PLAN_ONLY=1 build/tests/tree_benchmark`, Release): an elder's footprint takes about 2.8 s on one pool thread (it rasterizes every occupied level-2 slab at full resolution through the tree rasterizer), the map build 10 ms. Each rebuild uploads 8 MiB.

Known limits:

- Foliage takes its sky light from the CPU (`foliage_light`). Sky light step 4 added heightmap and coarse sky estimates for crowns and billboards beyond the loaded slabs.
- The map covers 2 km around the camera; beyond it structures cast nothing (view distance is up to 8 km on High).
- Footprint cost: a source could compute its footprint directly (trees: project segments top-down) instead of through `rasterize`, if 3 s per elder per revision shows up in play. `Source::skyTops` (sky heightmap, `sky-light.md`) now does exactly that at full resolution in about 0.1 s per elder; step 4 there now builds the occlusion map from it. A tree's growth step (a new revision without `changedSince`) recomputes the whole footprint.
- The upload could be limited to the changed rectangle.

### Step 3: chunk stamping (done)

- `structure_voxels::Column` combines the level-zero sources in one chunk column. It resolves their X images, merges overlaps in ascending source-key order, and stamps the resulting block IDs into air. Terrain and liquid take precedence. Structure LOD uses the same key order for overlapping slabs.
- `structure_voxels::Kind` indexes a kind's instances by the region holding their anchors. Give it the kind's horizontal reach in tiles. `ChunkManager::setGeneratedStructureKinds` installs these indexes for new kinds. Trees continue to obtain their instances from the existing asynchronous region tree sets, with separate reach for ordinary trees and landmarks. Both routes feed the same column stamping path. Marking an indexed instance as a landmark also supplies its source to distant rendering and sky occlusion within view distance.
- Workers project and cache each column's exact voxel tops with `Source::skyTops`. The streamed sky heightmap shares that projection. The projection and conservative slab occupancy let the dispatcher publish known-air chunks without a job once the column is prepared. Without an occupancy index, the source's bounds and projected top bound the work. As in step 2, projected structure tops represent opaque voxels for sunlight.
- Replacing a source snapshot while generation runs discards and retries its result. Ready chunks and saved tiles retain their contents. Legacy saves still get their one-time tree stamp, and tree growth and cut catch-up still use their existing tree records. Steps 4 and 5 cover generated-versus-edited comparisons and generalized persistence.
- Tests cover sparse multi-block stamping across the X seam, per-kind reach, overlap priority, exact sky tops, the known-air shortcut, stale worker results, and saved tile precedence.

### Step 4: generated versus edited (done)

- Chunk jobs compare saved voxel material IDs against the expected structure
  slab above the generated terrain and water. The terrain comparison still
  uses solidity. Tree wood uses the existing tree cut and growth records.
- The edit tracker compares loaded slabs with the baseline they were prepared
  against. It indexes only added or replaced blocks and the positions where
  an expected structure block was suppressed. Restoring the baseline removes
  the derived edit. Unloaded slabs keep their last known differences.
- Landmark proxies apply the suppression masks at full resolution and
  recalculate affected coarse voxel cells. The streamed and far sky
  projections look for the next intact occluder below a suppressed top.
- `terrain_edits.dat` stores suppression masks in EDIT v2. EDIT v1 still
  loads with additions only. Region CHNK data supplies the actual tiles for
  the reconciliation in step 5.


### Step 5: structure persistence (done)

- `structures.dat` stores the authoritative `TREE` and `VOXL` sections.
  `TREE` writes v4 and reads v1 through v4. The save catalog imports
  `trees.dat` when `structures.dat` is absent. It leaves the legacy file
  intact and prefers the new file after the first successful write. Unknown
  sections survive migration and subsequent saves.
- `VOXL` v1 groups records by stable kind ID and source key. A record can
  remove a source, patch its voxels (block ID zero clears a cell), or supply
  all voxels of a rebuilt or newly planted source. Patch records apply
  after their kind registers the generated source. Replacement records can
  introduce a source and set its landmark role. Register procedural
  instances with the same kind ID on each load and advance the override's
  revision when editing it. Use `ChunkManager::setStructureOverrides` on
  the main thread to publish the new records.
- Region `STRB` v1 stores the previous generated structure baseline with
  each saved slab. An empty payload records a known empty baseline.
  Reconciliation compares old generated blocks, actual tiles, and the new
  generated blocks. An unchanged tile follows the source revision. Player
  damage, replacement blocks and occupied cells survive where they differ
  from the old baseline. Legacy chunks without `STRB` keep their saved
  tiles on first load. Tree wood still uses `TreeStamp` for growth and cuts.
- A revised source updates the landmark and shade without streaming the
  region. Resident slabs reconcile in bounded jobs, and saved slabs
  reconcile on request. The world file is written atomically, with
  failed writes retried while the records are dirty.
- Tests cover TREE v2 migration and file precedence, preserved unknown
  sections, override round trips, a planted model across the wrap seam,
  and the watchtower revision with one slab resident and one saved.
