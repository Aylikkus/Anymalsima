# Handoff: sky light

Status: **steps 1 to 4 implemented** (see Progress).
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
  (`tileOpacity`: block opacity, water up to 8 according to fill) as it
  passes.
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

- **Terrain:** the generated surface and projected water from
  `ChunkManager::columnSources`. Ground is opaque and water attenuates
  according to its fill. No chunk is generated for it.
- **Structures:** `structure_voxels::Column` shares its exact voxel tops with
  chunk generation. The highest structure voxel of each tile column counts
  as opaque. The default `Source::skyTops` rasterizes occupied level-zero
  slabs from the top down. Trees override it with `treeWoodTops`, which
  projects the skeleton at `ChunkManager::woodDay` and checks each top with
  `markTreeWood`'s voxel test.
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
| Instanced foliage | `foliage_light`: published voxel light or heightmap estimate | Distant crowns and billboards can use the coarse map |
| Plant growth (`flora_simulation`) | `directSky(x, y, z)`: attenuated overhead light | Germination and catch-up need at least 10. Cover regrowth needs 15. |
| `LightQuery` sky part | Published voxel light or heightmap estimate, scaled by daylight | The sky API is available for the planned `LightQuery` |

`SkyHeightMap` (`world/sky_height_map.hpp`) owns the maps and exposes
`top(x, z)`, `directSky(x, y, z)` and `skyAt(x, y, z)`. Gameplay reads
sky light through this service.

## Plan

1. **Heightmap service (done).** See Progress.
2. **Direct light from the heightmap (done).** See Progress.
3. **Spread without waiting (done).** See Progress.
4. **Far and gameplay (done).**
   - Build `SkyOcclusionMap` from the heightmap.
   - Move plant growth, `foliage_light` and the `LightQuery` sky part onto
     the service.
   - Unloaded plants then grow by the same rule as loaded ones.
5. **Optional: light out of `Tile` (deferred after profiling).** Sky light is derived data and is never
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
- `ChunkManager`: `columnSources(cx, cz)` (terrain inputs and structure sources of a
  registered column, prepared on demand), `woodDay()`, and the signals
  `columnStructuresChanged` (a column's structure sources changed) and
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

### Step 2: direct light from the heightmap (done 2026-11-19)

- `SunLightPropagator` takes an optional `SkyHeightMap*` (`GameplayState`
  and the smoke harness pass it; `SMOKE_LEGACY_SUN=1` leaves it out for A/B
  runs). With it:
  - A column sweeps once its map is built (`snapshotColumn` returns nothing
    before that). Each run of loaded slabs starts from
    `sky::Tile::direct` at the top tile of its highest slab and continues
    down through its own tiles. A slab whose slab above is loaded continues
    from that slab, so edits relight at once and overflow columns are exact
    wherever the slabs above are loaded.
  - Nothing is missing, so nothing is requested: no slab is generated for
    sunlight alone, and chunks are never blind.
  - A heightmap change (`SkyHeightMap::changed`) sweeps the column again;
    spreads rerun only where direct light changed.
  - Pending sweeps are dispatched nearest first (`dispatchSweeps`), using
    at most half the job slots (otherwise chunk arrivals, each queueing a
    column sweep, starve the spreads). A spread can still start its own
    column's sweep, as before.
  - A chunk that was never fully lit keeps its generation estimate until
    its spread lands (`isEstimated` is true until then, so light volumes
    shade it with the occlusion map). The direct lower bound has no bounce
    and is black under a wide limb, so it is no longer shown.
- The legacy sweep used in headless fixtures starts at the column top.
  `ChunkManager::requestLighting` asks for missing slabs, and blind chunks
  keep their light while those requests are pending.
- Test: `sky_sunlight_without_upper_slabs`. A forest lit from slabs 20 to
  39 with the heightmap matches, cell for cell, one lit the old way with
  every slab up to the sky loaded. The top slab, which light from unloaded
  slabs above reaches, is excluded. Over a million cells, tree shade
  included; nothing above slab 39 is registered.
- Measured (elder approach smoke, same binary, `SMOKE_LEGACY_SUN=1` for the
  old sweep), frames from ready to full light for ground chunks near the
  walking eye:

  | | Old sweep | Heightmap |
  | --- | --- | --- |
  | Under the elder: p50 / p90 / max | 204 / 318 / 592 | 67 / 134 / 216 |
  | Under the elder: unlit at the end | 215 of 340 | 84 of 340 |
  | Elsewhere: p50 / p90 / max | 46 / 144 / 534 | 50 / 140 / 501 |
  | Elsewhere: unlit at the end | 170 of 1190 | 111 of 1190 |
  | Loaded chunks at the end | 8847 | 7514 |
  | Mean frame | 15.1 ms | 14.7 ms |

  `sky_check`: 14.3M cells of fully lit chunks, no mismatch. Ground under
  the elder now lights about as fast as anywhere else. What remains (median
  50-70 frames, unlit chunks at the rim) is the spread waiting for queued
  neighbours (step 3, which found the real limit elsewhere).

### Step 3: spread without waiting (done 2026-11-19)

- Halos are assembled on the workers. `State::opacity` is the chunk's
  opacity as of its last sweep snapshot, shared as an immutable array; every
  opacity change goes through `markColumnDirty` (`WorldEditor`) and sweeps
  the column again before any spread may use it. `IndirectInput` carries,
  per neighbour, its opacity and a copy of its direct light, or (sky mode,
  neighbour not loaded) its column's heightmap tile. `spread()` builds the
  44³ halo from those. A missing neighbour's cells are exact from the sky
  down to its column's highest opaque tile (open sky, translucent runs)
  and opaque padding below. `spreadIsIdentity` reads the snapshots too and
  refuses while a face neighbour's sweep is pending.
- Sweeps reuse each chunk's snapshot unless it is stale (new chunk, edit:
  `State::opacityStale`), so a column sweep reads only new or edited
  chunks (sky mode; the old path still scans every slab for its column
  top).
- Waiting: with a heightmap, a chunk not lit yet spreads at once, even next
  to neighbours still being generated. A lit chunk keeps its light until
  its queued neighbours have arrived, so it respreads once rather than once
  per arrival. A spread result made outdated by an arrival is still shown
  for a chunk never lit (it is queued again anyway), so heavy streaming
  cannot outdate every attempt.
- The real limit, found with `SunLightPropagator::waitingOn` (printed as
  `approach_waits` by the smoke harness): most unlit chunks sat queued, not
  waiting on anything. At most 8 light jobs were in flight, collected once
  per frame, while each arriving chunk queues a spread for itself and its 26
  neighbours. With a heightmap the cap is 3 jobs per pool thread (24 at
  most; sweeps take at most half); spreads of chunks still showing their
  estimate run at high priority, respreads at normal. Heightmap builds run
  at high priority (short, and the column's sweeps wait for them).
- Test: `sky_spread_without_waiting`. A column next to one held in the
  generation queue is fully lit with the heightmap and still waits without
  one.
- Measured (elder approach smoke), frames from ready to full light for
  ground chunks near the walking eye:

  | | Old sweep | Step 2 | Step 3 |
  | --- | --- | --- | --- |
  | Elsewhere: p50 / p90 / max | 46 / 144 / 534 | 50 / 140 / 501 | 13 / 24 / 144 |
  | Elsewhere: unlit at the end | 170 of 1190 | 111 of 1190 | 1 of 1190 |
  | Under the elder: p50 / p90 / max | 204 / 318 / 592 | 67 / 134 / 216 | 20 / 59 / 150 |
  | Under the elder: unlit at the end | 215 of 340 | 84 of 340 | 16 of 340 |

  `sky_check`: 26.2M cells of fully lit chunks, no mismatch. Main-thread
  chunk section (`approach_chunks`) mean 3.7 ms (4.5 ms before snapshot
  reuse); mean frame 12.9 ms. Under the elder the rest is generation: its
  wood chunks arrive slowly, and each arrival sweeps its column again
  (`approach_waits` splits the waits roughly evenly between sweeps,
  neighbours' sweeps and the queue).

### Step 4: far and gameplay

- `SkyHeightMap` publishes the renderer's camera-centred `SkyOcclusionMap`.
  The worker samples generated terrain at 4-tile texel centres, overlays
  exact heightmap tiles, and fills the rest with saved terrain edits,
  overrides and projected landmark wood. Landmark projection uses
  `Source::skyTops` at the voxel wood day. It reuses projections with the
  same source revision and terrain samples shared by successive map
  positions. The coarse map stores opaque heights and uses the existing
  shader's lateral falloff and ocean attenuation. Translucent spans are
  retained for voxel and gameplay queries.
- `requestUnloaded(x, z)` builds the requested column without registering
  or generating voxel slabs. The query cache is bounded and the caller can
  poll `directSky` or `skyAt` for the result. `skyAt` uses published voxel
  light where available. Elsewhere it estimates lateral light from direct
  light in known columns within 14 tiles, using 4-neighbour distance. Request
  adjacent columns when sampling near their edges. Unloaded cavity walls
  can change the true voxel spread. The planned `LightQuery` can scale this
  sky component once daylight is implemented.
- Plant germination and long-unloaded population catch-up use the same
  direct light threshold of 10. Ground cover needs 15. These checks can run
  before the voxel light arrives and across a missing upper slab. Plant
  stages advance analytically. Seed placement needs a loaded target for
  soil, room and population checks.
- Plant instances and tree crowns sample sky light through `foliage_light`.
  Distant crowns and billboards can use the coarse map. Lamp level and
  chroma still come from nearby voxels.
- `sky_queries_and_far_shade_without_chunks` exercises a saved roof and an
  override before any voxel slab is registered, including a wrapped query
  and lateral falloff under the roof. `flora_catchup_uses_direct_sky`
  verifies that an unloaded roof prevents catch-up germination.

### Step 5: publication profile (2026-11-19)

Deferred the separate sunlight buffer. Two Release headless runs used the
LOD smoke scene (height seed 1234, feature seed 5678), four worker threads,
an eight-chunk request radius and a 13 ms update cadence. Each run walked
from 1,200 tiles west of the elder to 40 tiles from its trunk at four tiles
per update. The edit phase alternated placing and removing 25 overhead
blocks every four updates, for 950 successful edits per run. Temporary
timers measured the production `SunLightPropagator::publish` path,
including lock acquisition, the tile writes and change notification.

| Phase | Mean publication per update | p95 publication per update | p95 lock acquisition per update | p95 total CPU update |
| --- | ---: | ---: | ---: | ---: |
| Elder approach | 0.11–0.13 ms | 0.22–0.29 ms | 0.007–0.009 ms | 7.6–8.4 ms |
| Rapid edits | 0.12–0.13 ms | 0.27–0.29 ms | 0.010–0.013 ms | 7.6–8.1 ms |

The largest publication total was 0.61 ms in one approach update. A
separate software Vulkan smoke run collected only 44 frames before a
crash. Its CPU timers recorded up to 0.12 ms per frame for sunlight
publication, 0.36 ms for light-volume cell encoding and 0.37 ms for
border and cover packing. GPU execution time was unavailable.

`Tile` remains six bytes if only the sunlight nibble moves, since the
artificial-light nibble still occupies that byte. A separate buffer would
add 2 KiB per retained chunk when nibble-packed, or 4 KiB at one byte per
voxel. Revisit the split if publication becomes a larger share of slow
updates or another change allows the light byte to be removed entirely.

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
