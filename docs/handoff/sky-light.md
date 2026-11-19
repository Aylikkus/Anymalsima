# Handoff: sky light

Status: **step 1 done** (heightmap service, see Progress); step 2 next.
Agreed with the maintainer on 2026-11-19.
This replaces the way sky light reaches the ground today: a top-down sweep
through every generated chunk slab. Instead, a saved sky heightmap that
includes structures is the source of truth. Related: `structures.md` (light
hand-over, sky occlusion at distance), `light-query.md` (gameplay queries),
`trees.md`.

## Decisions (2026-11-19)

1. **The sky heightmap is the source of truth for sky light.** It counts
   terrain, structures (generated and built) and edits. Every other form of
   sky light is derived from it at some resolution: voxel light in loaded
   chunks, the distant occlusion map, gameplay queries for unloaded places.
2. **Sky light comes from straight overhead.** It answers "does this tile
   see the sky, and how much": plant growth and anything else that needs to
   know whether a tile has direct sunlight. With day and night, the time of
   day only scales its brightness.
3. **Sun direction belongs to rendering only.** With day and night, the sun's
   direction drives the sun's shadow maps and the shading of normal maps. It
   never changes the sky light field or gameplay. A tile can be in the sun's
   shadow on screen in the evening and still count as sky-lit for plants.
   This is intended.

## Why (measured 2026-11-19)

Elder approach in the smoke harness (`SMOKE_TERRAIN=2 SMOKE_FOREST=elder
SMOKE_ELDER_SPACING=3000 SMOKE_RADIUS=8 SMOKE_APPROACH="1200 0.5 40"`),
counted at the arrival point over ±32 chunk columns:

- Under the elder, 347 columns need 5398 slabs between ground and wood top
  generated before their ground can be lit. That is 59% of the 9127 loaded
  chunks, generated only because sunlight needs them.
- Frames from a ground chunk becoming ready to its full light, for ground
  chunks within 128 tiles of the walking camera:

  | | p50 | p90 | max | never lit during the run |
  | --- | --- | --- | --- | --- |
  | Under the elder | 193 | 335 | 583 | 182 of 340 |
  | Elsewhere | 46 | 144 | 574 | 163 of 1190 |

  A frame is about 13 ms here, so the elder's ground waits 2.5 s (median) to
  4.4 s (p90), and ordinary ground still waits 0.6 s to 1.9 s.

Computing the light is cheap: a sweep is 4096 cells per slab, and a spread
is a flood fill over 44³ cells on a worker. The time goes into dependencies:

- **Vertical:** `SunLightPropagator::sweep` needs every slab from the column
  top (`ChunkManager::getColumnTopY`, raised by tree wood) down to the
  chunk. Missing ones are requested as sky prerequisites
  (`requestLighting`). Each is a full generation job with tree stamping.
  Cost is structure height times footprint. Sky islands at Y≈2048 would
  multiply it.
- **Horizontal:** a chunk's spread needs the direct light of its 26
  neighbours, and it waits while any neighbour is queued
  (`hasQueuedNeighbor`). The rim of the loaded area always waits.
- **Main thread:** `snapshotHalo` copies the opacity of 27 chunks on the
  main thread for every spread job. At most 8 light jobs run at once, with
  2 ms of dispatch per frame.

This is also why a compute shader is not the answer now. It would speed up
the cheap part, leave the waiting untouched, and its results would have to
be read back for the CPU users (plants, foliage, queries). Revisit it only if
spread throughput becomes the limit after step 3 (measure first). If it does,
use it only for rendering: a near-field pass over a camera-centred volume.

## The sky light field

The definition stays the voxel rule used today, so existing tests and the
look carry over:

- **Direct:** light falls straight down at 15 and loses each tile's opacity
  (`tileOpacity`: block opacity, water 8) as it passes.
- **Indirect:** from every lit cell, light spreads to the 6 neighbours,
  losing `max(1, opacity)` per step. A cell's value is the maximum over all
  paths. A positive value travels at most 14 tiles.
- **Under an opaque top:** the value at height y is `15 - d`, where d is the
  4-neighbour distance to the nearest column open to the sky down to y,
  unless walls on the way block or lengthen the path. `SkyOcclusionMap`
  approximates exactly this at distance.

What changes is where the direct part comes from. It is no longer swept
through generated slabs; it is read from the heightmap.

## The sky heightmap

Per tile column (`sky::Column`, `world/sky_column.hpp`):

- `opaque`: the highest tile with opacity 15 or more.
- `spans`: runs of translucent tiles above it (glass, water), top-down, at
  most `sky::MaxSpans` (8). A column with more is marked `overflow`: the
  lowest kept run's bottom counts as opaque (darker than the truth, never
  brighter), and step 2 sweeps such columns through their loaded tiles.
- `top()`: the highest attenuating tile. Above it the direct light is 15.
- `direct(y)`: 15 minus the opacity of every tile above y, the
  propagator's sweep rule. This attenuated level is gameplay's "direct
  sunlight" (decided 2026-11-19): each rule picks its threshold, so a glass
  greenhouse (opacity 3: 12) can count as lit.

Columns are packed per chunk column (`sky::Tile`, 16x16) and built by
`sky::build` from:

- **Terrain:** the generated surface (`ChunkColumnInputs::surface_y`,
  through `ChunkManager::columnSources`): solid up to it, sea water (opacity
  8) above it up to `SEA_LEVEL_Y`. No chunk is generated for it.
- **Structures:** their highest voxel per column, treated as opaque.
  - Trees: `treeWoodTops` (`tree_rasterizer.hpp`) projects the skeleton at
    `ChunkManager::woodDay`, for every tree of the column's `TreeColumn`
    (landmarks included), as `placeTreeWood` would place it. It solves each
    sample sphere for its top in closed form and checks the result with
    `markTreeWood`'s own test, so it matches the rasterizer tile for tile
    without scanning any volume.
  - Any structure kind: `structure_voxels::Source::skyTops(columns)`. The
    default rasterizes level-0 slabs from the top down (slow for tall
    structures); trees override it with `treeWoodTops`. Kinds added later
    feed the heightmap through it (not wired yet: only trees exist).
- **Edits:** `TerrainEdits` already records every edited column world-wide
  and saves it (`terrain_edits.dat`): the ground override (topmost solid
  tile at or below the generated surface), water above it, and every solid
  tile above the generated surface (block ids, so glass keeps its
  opacity). This answers "a top block removed above unloaded slabs": the
  tracker recomputes the column, and the map rebuilds from it. A
  translucent ground block counts the tile under it as opaque.
- **Overrides:** what none of the above records, chiefly holes dug into
  tree wood (`TerrainEdits` treats living wood as air, and a hole that
  severs nothing is no cut). After every opacity change from `WorldEditor`,
  the column is scanned through its loaded tiles (unloaded slabs keep what
  the map knew there). Where the result differs from the sources it is kept
  as an override; where they agree again (the tracker caught up, a tree
  rebuilt), the override is dropped. Overrides are re-checked when a chunk
  of their column arrives (tree catch-up on load).

Storage (decided 2026-11-19): only overrides are saved, in `sky.dat`
(`save/sky_codec.hpp`, section `SKYO`), written with the autosave cycle and
on exit. Everything else is rebuilt on load from the sources, which are
saved already (seed, `terrain_edits.dat`, `trees.dat`). There is no region
file change. Holes dug into wood before `sky.dat` existed are not known
until that column is edited again.

Consistency: the map must agree with the loaded tiles. Tests check that it
matches generated forest columns tile for tile, and the elder smoke scene
checks it against the voxel sunlight of fully lit chunks (see Progress).

### Layers

The overworld (0–1024) and the planned sky islands (around Y 2048) get
separate heightmaps. Sky islands are lit from their own map and do not
shadow the overworld (IDEA.md). The underworld has no sky: no map, and its
sky light is 0. Code must not assume one Y range starting at 0.

## Consumers, by resolution

| Where | What it reads | Notes |
| --- | --- | --- |
| Loaded chunks (rendering) | Voxel field per chunk (tiles today), from heightmap plus local spread | Light volumes, unchanged on the GPU side |
| Distant terrain, joins, structure slabs | `SkyOcclusionMap`, built from the heightmap | Replaces per-source footprints |
| Chunks not lit yet | Estimate × occlusion map (`u_light_chunk.w = 2`) | Kept as a fallback only (see `structures.md`) |
| Instanced foliage | `foliage_light`: tiles when loaded, heightmap rule otherwise | Today it sees only loaded tiles |
| Plant growth (`flora_simulation`) | `directSky(x, y, z)`: y above `top`, so exact everywhere | Replaces `sun_exposure >= 10` and `== 15` checks |
| `LightQuery` sky part | Loaded: voxel value. Unloaded: the heightmap rule. Times daylight | Same rule as the occlusion map, so loaded and unloaded agree |

`SkyHeightMap` (`world/sky_height_map.hpp`) owns the maps and answers
`top(x, z)` and `directSky(x, y, z)`; `skyAt(x, y, z)` (the voxel value when
loaded, else the heightmap rule) comes with step 4. Gameplay uses only this
service, never `Tile::lightLevel` directly.

## Plan

1. **Heightmap service (done).** See Progress.
2. **Direct light from the heightmap.** `SunLightPropagator` computes each
   slab's direct light from the heightmap plus its own tiles, independent of
   other slabs.
   - Remove sky-prerequisite generation (`requestLighting`, beneficiaries,
     missing-slab handling). Upper slabs then generate only when something
     needs them: drawing, edits, simulation.
   - Tree column tops stop driving sunlight.
   - Columns over the span bound keep today's sweep.
   - The blind/estimate path stays for columns whose heightmap is not ready
     yet.
   - Measure: no slabs generated for sunlight alone; latency under the elder
     about equal to elsewhere.
3. **Spread without waiting.**
   - A missing neighbour's halo is filled from the heightmap (15 above
     `top`, opaque below) instead of being waited for; its arrival refines
     the result (`invalidateAround` already does).
   - Halo snapshots move off the main thread, using immutable per-chunk
     opacity snapshots shared with the workers, as `BlockLightPropagator`
     does.
   - Measure: ready-to-lit latency, aiming at one generation job plus one
     spread job.
4. **Far and gameplay.**
   - Build `SkyOcclusionMap` from the heightmap.
   - Move plant growth, `foliage_light` and the `LightQuery` sky part onto
     the service.
   - Unloaded plants then grow by the same rule as loaded ones.
5. **Optional: light out of `Tile`.** Sky light is derived data and is never
   saved. A lighting-owned buffer per chunk would publish by pointer swap
   instead of writing tiles under the chunk lock, and could be dropped for
   chunks nobody draws. Do this only if it pays for itself after step 3.

With day and night (separately): the sun's direction feeds only the sun's
shadow maps and normal-map shading. Daylight scales the sky term
everywhere.

## Progress

### Step 1: heightmap service (done 2026-11-19)

- `world/sky_column.{hpp,cpp}` (pure, headless test library): `sky::Column`,
  `sky::scanColumn`, `sky::Tile`, `sky::build`, `sky::Overrides`.
- `treeWoodTops` (`tree_rasterizer.{hpp,cpp}`) and
  `structure_voxels::Source::skyTops` (default plus the tree override in
  `tree_wood_lod.cpp`).
- `ChunkManager`: `columnSources(cx, cz)` (terrain inputs and trees of a
  registered column, prepared on demand), `woodDay()`, and the signals
  `columnTreesChanged` (a column's tree list was rebuilt) and
  `columnReleased` (its last chunk was unregistered).
- `SkyHeightMap` (`world/sky_height_map.{hpp,cpp}`), main thread:
  - `request(cx, cz)` keeps a chunk column built while it is registered.
    `GameplayState` requests every column with a ready chunk.
  - Builds run on the pool, nearest first, at most 4 at once. A column waits
    until its terrain and tree sets are ready.
  - Rebuilds when `TerrainEdits` changes a column, when its tree list
    changes, and for columns with trees when the wood day changes.
  - `tile`, `column`, `top`, `directSky`; `changed` signals a rebuilt chunk
    column (for step 2).
  - Overrides: observed after `WorldEditor` changes, re-checked on chunk
    arrival, dropped when the sources agree; `dirty` for saving.
- `save/sky_codec.{hpp,cpp}`, `save_catalog::readSky`/`writeSky` (`sky.dat`).
  `GameplayState` loads it, saves it with the autosave cycle and on exit,
  and shows "sky top" and "direct" for the aimed-at tile in the tile debug
  text.
- Nothing reads the map yet: sunlight still sweeps (step 2).
- Tests: `sky_column_scan_and_direct`, `sky_tile_build_sources` (ground,
  sea, wood, edits with a pit, pond, glass and stone, a glass floor),
  `sky_tile_overrides`, `sky_codec_roundtrip` (and malformed input),
  `tree_wood_tops_match_raster` (every shipped species at two ages, a
  clipped box, perpendicular and sawn cuts, the elder at its trunk and
  under its widest limb), `sky_heightmap_matches_chunks` (16 generated
  forest columns tile for tile; a placed block handled through
  `TerrainEdits`; a hole dug into wood kept as an override and restored
  from saved overrides).
- Smoke: `SMOKE_APPROACH` prints `sky_check` at the end: over fully lit
  chunks, cells where voxel sunlight is 15 but the map's direct light is
  not (or the reverse), or where voxel light is below it.
- Measured (2026-11-19):
  - Elder approach smoke (`SMOKE_TERRAIN=2 SMOKE_FOREST=elder
    SMOKE_ELDER_SPACING=3000 SMOKE_RADIUS=8 SMOKE_APPROACH="1200 0.5 40"`):
    `sky_check cells=5828608 wrong=0` (1423 fully lit chunks, 442 mapped
    chunk columns, elder included). Mean frame 12.9 ms, against 12.8 ms
    without the map.
  - `TREE_BENCH_PLAN_ONLY=1 build/tests/tree_benchmark` (Release, the
    1507x589x1749 elder): `skyTops` for the whole tree 103 ms (129409 wood
    columns), against 2969 ms for its level-2 footprint. Per chunk column,
    as `SkyHeightMap` builds them: mean 0.34 ms, max 1.25 ms. Each call
    still visits every segment's samples; cull segments by box first if
    this ever matters.

## Open questions

- **Layers:** the map covers one band (0 to `WORLD_HEIGHT_Y`). Sky islands
  need their own band when they exist; nothing in `sky::Column` assumes the
  band starts at 0 except the codec's range check.
- **Canopy shade:** tree crowns are not tiles, so they are not in the
  heightmap. Plants under a crown count as sky-lit. Canopy shade for plants
  is trees step 8 (`trees.md`), as a separate input, not opacity.
- **Span list bound:** how many spans per column, and whether built
  translucent roofs (greenhouses) need more.
- **Time-varying structures:** a growing tree changes its revision every
  growth step. The query must stay cheap enough (projection, not
  rasterization) to rerun per revision.
