# Handoff: trees

Status: **steps 1 to 7 done** (data; skeletons and rasterizer; world
generation; crown rendering; growth and life; cutting and regrowth; distant
trees). Step 8 (seasons, seeding, wood items, physics, canopy shade, decay,
growth accelerators) is future work. See [Progress](#progress). How to make tree
species as a modder: `docs/modding/trees.md`. Trees reuse the flora
pipeline (`docs/handoff/flora.md`): placement rules, foliage materials,
the foliage program and the world clock.


## Why

Flora steps 1–9 are done (ground cover, plants, world clock, growth, seeding,
harvest; `docs/handoff/flora.md`). Trees come next. They were left out of the
flora plan because they do not fit its model: a tree can be a 4 m mountain
pine or a 120 m landmark whose trunk is 128 blocks wide and crosses region
borders. Large trees must behave like terrain: they can be broken and built
on (elves living in them later). They grow over centuries, adapt to the
headroom they have (indoors, or under the build limit on peaks), regrow from
where they were cut, and stay standing when they die.

Decisions from the author (2026-11-17):

| Question | Decision |
| --- | --- |
| Radius classes | **Trunk** radius: thin < 0.5 (no blocks), small ≈ 1-block trunk, medium 1–4, large 5–15, massive 16–64. The crown spreads further. |
| Height | Up to ~120 m, limited by headroom. A tree never grows into a ceiling or past the build limit; it spreads instead. |
| What is a block | Trunks and branches thick enough (radius ≥ 0.5) are real wood blocks. They thicken over time and "overgrow into blocks". Leaves and thin twigs stay foliage meshes. |
| Growth speed | Compressed by class: small trees mature in ~1 game year (365 days), medium in a few years, large in tens of years, massive in centuries (ancient landmarks). |
| Massive trees | Rare landmarks with persistent sites, a handful per continent. Large trees form old forests in humid regions. |
| Cutting | Cutting a trunk or branch removes everything above it (no physics yet), and the tree regrows from the cut. At the root, a per-species flag decides: resprout from the stump, or die and leave the stump. |
| Death | All blocks stay; leaves wither and drop. |
| Species to ship | spruce (tundra), palm (subtropics), broadleaf (temperate plains), thin mountain pine (mountains), plus one large and one massive rare species. |

All trees share one system with a continuous size. A sapling of a massive
species starts thin (mesh only) and gains blocks as its parts pass radius
0.5. "Class" comes from the species' mature trunk radius and is used for
docs, budgets and validation.

Proportions of massive trees: a 64-radius trunk only 120 m tall would read as
a mesa. The `giant` form therefore keeps the solid bole short (buttress roots
flaring out to the full radius at the ground, tapering fast) and splits low
into several voxel-thick limbs that carry tiered crowns far outward. The
silhouette stays tree-like within the height cap, and the limbs are where
elves would build.

## Architecture

### Data (`Tree`, `assets/data/trees/*.json`)

- A new data type, registered after `Flora`. Ids are u8 and saved raw, so new
  files go last.
- It reuses `flora_json` (`ValueRange`, `Color3`, strict keys) and the `Flora`
  placement parser (soils, cover, climate, slope, density, cluster,
  `persistent_sites`, priority).
- Parse limits differ from flora: height ≤ 128, trunk radius ≤ 64.

```jsonc
{
  "name": "trees.spruce",
  "form": "excurrent",            // excurrent | decurrent | palm | stunted | giant
  "size": { "height_m": [18, 32], "trunk_radius_m": [0.5, 1.2],
            "crown_radius_m": [3, 5] },
  "growth": { "maturity_days": [300, 450], "lifespan_days": [20000, 40000],
              "curve": 2.2 },
  "form_params": { "whorl_spacing_m": [0.6, 1.0], "branch_angle_deg": [70, 95],
                   "branch_length": [0.25, 0.4], "droop": 0.3, "taper": 0.9,
                   "lean": [0, 4], "root_flare": 0.2 },
  "wood": { "block": "blocks.spruce_wood", "bark": "foliage.bark_spruce" },
  "foliage": { "type": "needle_clump", "material": "foliage.spruce_needles",
               "clump_size_m": [0.8, 1.4], "density": 1.0 },
  "placement": { /* flora placement */ "spacing_m": [5, 8] },
  "cut": { "stump_resprouts": false, "resprout_days": [20, 40] }
}
```

Forms are code archetypes tuned by data:

- `excurrent`: conifer with a single leader and whorls of branches.
- `decurrent`: broadleaf with a forking crown.
- `palm`: curved, unbranched trunk with a frond crown.
- `stunted`: mountain krummholz, wind-bent and thin.
- `giant`: buttresses, limbs and tiered crowns.

Blocks and materials:

- New block property `"natural_growth": true` (`BlockProperties`,
  `src/game/items/block.hpp`). At load, it builds a `std::bitset<256>` of
  grown-wood ids, shared with workers as `shared_ptr<const>`. Do not reuse
  `render_category`: no code reads it.
- Living wood gets its own block per species in `blocks/trees.json`, which
  sorts after `test_light.json`. Logs and planks a player can place later must
  be **different ids**.
- Wood textures for voxel faces are terrain `Material`s (128² entries in
  `materials/*.json`), because the terrain array only takes those. Foliage
  bark materials texture only the twig tubes. Placeholder textures come from
  `tools/generate_foliage_textures.py`.

### Skeleton (pure, deterministic)

`world/tree_skeleton.{hpp,cpp}`: `buildTreeSkeleton(species, treeSeed,
anchor, heightCap, terrain, cuts)` returns an immutable `TreeSkeleton`.

- **Contents:** segments (capsule start/end, full-age radius, parent, birth
  age), leaf and frond attachment points with birth ages, a segment AABB grid,
  and a `nextVoxelChangeAge` query.
- **Growth:** positions are fixed offsets from the anchor and never move. Age
  only decides which segments exist and their radius:
  `radius(age) = r_full · g((age − birth)/maturity)`. Voxels are therefore
  only ever added by growth.
- **`heightCap`:** leaders stop below it, and a blocked leader releases
  lateral growth.
  - Generated trees use `buildCeilingY(layer)` minus the anchor height. This
    is a function, not a hard-coded 1024, because sky islands and the
    underworld are planned.
  - Planted trees use the clearance measured from live tiles at planting.
- **Terrain pruning:** segments whose end lies below the generated surface at
  their xz are dropped, so no branches are buried in cliffs and no leaves
  stick out of rock.
- **Cuts** are `(segment, t, day)`. A cut prunes the subtree. A sprout subtree
  is generated from `(treeSeed, cut index)` at the cut, born at
  `day + resprout_days`. A root cut on a non-resprouting species marks the
  tree dead instead.
- **`TreeVersion` constant:** stored per chunk next to `treeDay`. Reconcile is
  skipped on a mismatch, so changing the algorithm or the JSON never grows
  new-shape wood next to old wood.
- **`TreeSkeletonCache`:** a mutex LRU of `shared_ptr<const TreeSkeleton>`
  keyed by (tree key, override revision), used by workers.

`world/tree_rasterizer.{hpp,cpp}`:

- Wood goes on tiles inside segments with radius ≥ 0.5. The 1-block
  small-trunk case is the column containing the axis.
- Wood never replaces a solid tile.
- Modes: full at an age, delta between two ages (growth), and subtree (cuts).

### Placement and generation

`world/tree_generator.{hpp,cpp}` follows `generateFlora`:

- **Candidates:** a jittered world grid per species, X-periodic
  (`SoilGenerator::PeriodicAxis`).
  - Validity comes from `TerrainSampler` (surface, slope, soil, cover, region
    climate) with level ground under the root disc.
  - Conflicts use local outranking (bigger exclusion, then priority, then
    hash).
  - Crown reach plus conflict reach must stay ≤ 128 tiles: the
    `TerrainNeighborhood` halo is valid only for the centre region ±1.
- **Massive landmarks:** a world-scope list computed once per world from
  coarse `Region` data (biome, humidity, land), with a deterministic seed. It
  is stored in `trees.dat` as derived data and recomputable. It is not part
  of per-chunk candidates.
- **No storage for untouched trees:** they are never epoch-resampled.
  Generated birth days are spread into the past; massive trees are centuries
  old.
- **`TreeColumn` cache:** a list of tree references per chunk column (trees
  whose xz bounds hit it, with Y ranges). It is built in `prepareColumns`
  (`chunk_manager.cpp` ~660) on the main thread next to `columnInputs`,
  refcounted by registration and erased in `unregister`. It is the source for
  `isAirChunk`, the job inputs, column tops and the `TreeIndex`. Chunk jobs do
  **not** return tree ids, because air chunks never run a job.
- **Chunk job** (`chunk_manager.cpp` ~450–546):
  - `isAirChunk` takes the column's wood top.
  - The terrain neighbourhood is captured when the slab meets a tree.
  - The job captures the tree list and the overrides snapshot.
  - Fresh chunks rasterize after `generateChunk`; decoded chunks reconcile
    after decode.
  - Results are marked for saving like `floraRegrown`.
- **Column tops:** `raiseColumnTopY(cx, cz, woodTop)` is called when the
  `TreeColumn` is built, before any sweep. Otherwise missing upper slabs are
  treated as open sky and light the trunk.
- **Flora:** candidates inside any tree's 2D trunk/root disc are excluded.
  This is pure, so border agreement holds. Live seeding already refuses
  non-air headroom.
- **Edits:** grown wood counts as air in the saved-chunk edit check
  (`chunk_manager.cpp:510`) and in the `TerrainEditTracker` structure loop
  (`terrain_edit_tracker.cpp:136–141`). `SurfaceSampler` and `StructureLod`
  inherit this. `GeneratorVersion` becomes 8; `SurfaceVersion` stays.
- **Old saves (v7 chunks):** a missing `treeDay` means "before birth", so
  reconcile grows whole trees into air. There are no half trees at borders
  between stored and fresh chunks.

### Tree state and persistence

- **`TreeIndex` (main thread):** trees near the loaded area with their
  skeletons, in 128-tile buckets like `LightSourceRegistry`, built from the
  `TreeColumn` caches. The renderer, editor and simulation use it.
- **`TreeOverrides`:** the authoritative record of trees that differ from
  generation.
  - It holds cuts, planted trees (anchor, species, seed, birth, `heightCap`),
    death by cutting, and reserved growth and lifespan modifiers for future
    fertilizers and elves.
  - It is copy-on-write (`shared_ptr<const>` plus a revision) and captured by
    chunk jobs.
  - In `publishGenerated`, a revision change since dispatch re-runs reconcile
    for that chunk.
- **`trees.dat` (fourcc `TREE`, world scope):** loaded before the first
  region registers.
  - Unlike `terrain_edits.dat`, it is truth, not derived data. It is written
    (tmp file, then rename) whenever it is dirty and a region is saved or
    unloaded (`LocationManager::saveRegion`/`unloadRegion`), not only at the
    end of the autosave cycle.
  - Backstop: reconcile adds a segment's new voxels only while the tile at its
    parent's end is still wood. This also self-heals hollowed trunks.
- **`treeDay` and `treeVersion`:** kept in `ChunkHandle`/`GeneratedChunk`
  and carried in `RegionSnapshot`. They are stored in the FLOR blob header as
  separate encode/decode arguments (`flora_codec` v3; v2 decodes as "before
  birth"). They are not stored in `FloraLayer`, because `setFlora` nulls
  empty layers and the editor rebuilds them.

### Growth, death, cutting

- **Reconcile:** for each tree in the chunk,
  - add `raster(today) − raster(treeDay)` into air tiles;
  - remove grown-wood tiles of any subtree cut after `treeDay`;
  - wounds the player made inside the older volume stay.

  It runs in the chunk job on load, and in the `FloraSimulation` sweep for a
  second list of chunks with trees (`reconcileTrees`). The sweep skips chunks
  until the earliest `nextVoxelChangeAge` of their trees. Writes go through
  the editor's grown-wood path.
- **Death** is analytic at the end of the lifespan. Blocks stay; the foliage
  withers, then disappears. In v1, dead trees stand forever.
- **`WorldEditor` grown-wood mode:** a bulk path that skips the per-tile plant
  scans. It keeps remesh, column-top, sunlight and `blockChanged` coalescing.
- **Cutting:** removing a `natural_growth` block in `applyOne` finds the tree
  through `TreeColumn` and the segment through the skeleton.
  - The segment counts as severed when either:
    - its radius is ≤ 2 and the removed block is in the core, or
    - no wood remains in the rasterizer's slice of the segment at that height
      (counted with `withChunkLocked` per chunk).
  - A severed segment records a cut, bulk-removes the wood above it and
    updates the overrides. The sprout grows on later reconciles.
- **Blocks placed inside a crown** hide the leaf clumps in that cell (per
  piece).
- **Fix an existing bug** in `WorldEditor::commit`
  (`world_editor.cpp:197`): it builds an empty `FloraLayer` and loses
  `resolvedDay`, so any edit after day 365 re-samples the whole chunk's
  plants, wiping harvests and possibly regrowing the removed plant. Copy the
  old layer and filter its instances. Add a regression test in
  `world_editor_tests.cpp`. The smoke gallery (`lod_render_smoke.cpp` ~487)
  has the same pattern.

### Rendering (`TreeRenderer`, separate from `FloraRenderer`)

- **Pieces:** `TreeFoliageBuilder`, run on workers, builds chunk-aligned
  pieces keyed by `ChunkCoord`. They hold bark tubes for thin branches,
  needle and leaf clumps (the existing clump code), and palm fronds.
  - Pieces are rebuilt when a tree's growth bucket changes (~32 buckets over
    maturity), when it is cut, or when the chunk changes.
  - Indices are 32-bit: add the `uint32_t` `MeshHandle::initStatic` overload
    (`renderer_api.hpp:213`).
- **Shader:** the foliage program gets a `TREE` variant.
  - `FoliageVertex.height` carries the sway weight.
  - `u_foliage.z` switches wind to a weight-only amplitude.
  - It samples the piece chunk's light volume (bound at stage 2, as in
    `block.frag`), so it gets per-pixel canopy light and lamps, and
    `lightChanged` invalidates per chunk.
- **Distance:** pieces fade at the detail radius.
- **Known limit:** leaves do not reduce `sun_exposure`, so forest floors are
  fully sky-lit. Canopy shade is a later step.

### Distant trees (step 7)

- Ordinary trees are impostors or simplified meshes per LOD patch, from the
  same deterministic placement.
- Massive trees use a coarse voxel proxy through a `StructureLod`-like path
  (done: see "Landmark voxel wood at distance" under Progress):
  their downsampled raster goes to the existing mesher job. Their crowns use
  simplified clump meshes. Billboards would look wrong at 100 m widths.

## Steps (each builds, runs and is tested on its own)

1. **Data.**
   - Build: the `Tree` type and validation, the `natural_growth` flag and
     bitset, wood blocks and materials, foliage materials, placeholder
     textures, the six species, and the manifest.
   - Fix the `resolvedDay` bug, with a regression test.
   - Docs: `docs/modding/trees.md`, plus `docs/handoff/trees.md` (this plan
     as the roadmap, with a Progress section).
2. **Skeleton and rasterizer.**
   - Build: the five forms, height cap, terrain pruning, monotonic growth,
     cuts and sprouts, and the version.
   - Tests: determinism, raster(a) ⊆ raster(b) for a < b, the cap is
     respected, trunk radius per class, no wood in terrain.
3. **Generation.**
   - Build: placement, the landmark list, `TreeColumn`, `isAirChunk`, the
     terrain capture, column tops, the edit exclusion, the flora exclusion,
     `TreeIndex`, and `GeneratorVersion` 8.
   - Tests: chunk-order and border agreement, the X seam, a massive tree
     across a region border, `isAirChunk` and column tops, and a saved chunk
     with wood that is not edited and produces no structure.
4. **Crown rendering.**
   - Build: `TreeRenderer`, the pieces, the `TREE` shader variant, and the
     32-bit index overload.
   - Visual check: `SMOKE_TREE_GALLERY` in `lod_render_smoke` (one tree per
     species and age, spaced by crown diameter).
   - Profile with the harness CPU counters against the flora budget.
5. **Growth and life.**
   - Build: `TreeOverrides` and `trees.dat` (write-through), `treeDay`/
     `treeVersion` and `flora_codec` v3, reconcile in the job and the sweep,
     death, and a debug PLANT TREE tool (cap from live tiles, for the indoor
     test).
   - Tests: codecs v3/v2, `trees.dat` round trip and malformed input,
     reconcile keeps player holes, cuts recorded while unloaded, whole trees
     grown into v7 chunks.
6. **Cutting and regrowth.**
   - Build: the editor's grown-wood mode, the severing test, subtree removal,
     sprouts, and the stump flag.
   - Tests: a root cut per flag, a mid-trunk cut that regrows from the cut,
     and wounds that do not sever.
7. **Distant trees:** impostors and the massive voxel proxy.
8. **Later:** seasons and deciduous trees, seeding and forest renewal, wood
   items, felling physics, canopy shade, dead-wood decay, growth
   accelerators.

## Verification per step

- `cmake --build build --parallel && ctest --test-dir build` (label
  `trees` for tree cases).
- Visual: `build/tests/lod_render_smoke <prefix> 900 lights` with
  `SMOKE_CAMERA` and (from step 4) `SMOKE_TREE_GALLERY`: species and growth
  stages, crown lighting, seams at chunk and region borders and at the X
  seam.
- In game: tundra, temperate plains, a subtropical coast, mountains and a
  landmark; cuts at the root and mid-height with TIME sped up; a tree
  planted in a closed room (height cap); save, travel 3+ regions away,
  reload: wood and cuts persist, no tree columns in `terrain_edits.dat`, no
  `.tmp`/`.corrupt` files.

## Progress

### Step 1: data (done)

- `items/tree.hpp`: `Tree` (`TypeName "trees"`), forms, foliage types,
  `TreeSizeClass` (`treeSizeClass`: thin < 0.5 <= small < 1.5 <= medium <
  4.5 <= large < 15.5 <= massive), `extent()`/`reach()`, limits
  `TreeMaxHeight` 128, `TreeMaxTrunkRadius` 64, `TreeMaxReach` 96 (ordinary
  trees: crown or flared roots plus spacing) and `TreeMaxLandmarkReach` 192
  (massive). Placement reuses `flora_json::placement` (extracted from the
  `Flora` parser) plus `spacing_m`; companions are refused.
- Placement rules gained `vegetation` (region `VegetationType` list), used
  by flora too (`floraSuits`).
- `BlockProperties::natural_growth` and `naturalGrowthBlocks(...)` (a
  `GrownBlockSet` bitset by block id). `validateTreeReferences`
  (`items/flora_validation.hpp`, run by `ResourceManager`): wood blocks
  exist and have `natural_growth`; bark and foliage materials exist; soils
  and covers exist.
- Data: `blocks/trees.json` (six living woods), `materials/wood.json`,
  barks in `assets/materials/blocks/wood/` and cards `foliage.needles`,
  `foliage.palm_frond` (`tools/generate_foliage_textures.py`), twelve
  foliage materials appended, `trees/trees.json`: spruce (small), mountain
  pine, oak and palm (thin), redwood (large), elder (massive).
- Fixed: `WorldEditor::commit` rebuilt the flora layer and lost
  `resolvedDay`, so any edit in a world older than a year re-sampled the
  chunk's plants (regression check in `flora_edits`); the smoke gallery
  kept the live day too.
- Tests: `tests/unit/game/items/tree_data_tests.cpp` (`tree_species_parsing`,
  `tree_size_classes`, `tree_references_validated`, `tree_runtime_content`).

### Step 2: skeleton and rasterizer (done)

- `world/tree_skeleton.{hpp,cpp}`: `buildTreeSkeleton(species, input)`
  with `TreeSkeletonInput{seed, headroom, ground, cuts}` returns an
  immutable `TreeSkeleton` (segments, foliage sites, root flare, sampled
  height/trunk/crown, maturity, lifespan, bounds, `dead`). Randomness is a
  local splitmix64, so skeletons are identical on every platform and run.
  - **Timing:** an axis's tip passes fraction u at
    `birth0 + (done - birth0) * growthTime(u)`; tips extend during the first
    85% of the maturity and thicken until it. `radius(s, t, age)` is the
    full radius times `growth((age - born) / (matureAge - born))`.
  - **Forms:** `excurrent` (leader, golden-angle whorls, conical branch
    lengths, order-2 twigs), `decurrent` and `stunted` (bole to the crown
    base, then recursive forks, area-preserving radii; stunted turns
    branches away from the wind), `palm` (curved stem, ten frond rings,
    each alive while the tip is within two rings of it, so the crown rises
    with the stem), `giant` (buttressed bole with lobed `TreeFlare`, limbs in
    tiers, a central leader, forks). Young trees carry juvenile foliage on
    the stem, shed once the crown forms.
  - **Fit:** the crown (everything above the crown base) is scaled so tips
    reach the sampled crown radius and height; then `clampToHeadroom`
    (also after cuts) keeps wood and clumps under the headroom. A capped
    tree spreads up to 1.4x wider and its trunk shrinks by
    sqrt(height ratio).
  - **Ground:** with `input.ground`, branches (not the trunk) ending more
    than 0.3 m inside the ground are pruned with their subtrees and
    foliage.
  - **Cuts:** segments are never renumbered: pruned segments keep their
    slot (`Pruned`, `cut` = 1-based cut index) and the part above the cut
    is appended as a pruned piece. A root cut (trunk below
    max(1.5 m, half the flare height)) kills species without
    `stump_resprouts`. A trunk cut regrows after `resprout_days` as a
    young tree of the species grafted onto the cut (`Builder::graft`: the
    normal builder with the remaining height as headroom, radii at most 0.7
    of the stump, times shifted to the resprout day, every part a `Sprout`
    of that cut), so spruces regrow whorled cones and oaks forks. Palms
    grow new frond rings; a cut branch grows a `Sprout` axis of its former
    length with forks capped at half the crown radius.
  - **Where a cut lies:** on the removed tile's lower face (the stump's top
    face), not its centre. Cut planes clip every tile whose centre lies
    past them, on the cut segment and on the segments of the same axis
    below whose rounded ends reach past it, so a cut is flat and the
    regrowth starts exactly on the remaining blocks. A cut at a segment's
    end also takes the axis continuing from it.
  - `TreeSkeletonCache`: mutex LRU of shared skeletons (build outside the
    lock; keys must include the cut revision).
- `world/tree_rasterizer.{hpp,cpp}`: `markTreeWood` (mask over a
  `TreeTileBox`), `forEachTreeWood`, `forEachTreeGrowth` (tiles added
  between two ages), `treeWoodBounds`. A tile is wood when, at one of the
  segment's fixed sample points (a quarter radius apart, at least 0.5 m)
  within a window of its *unclamped* projection, the radius is >= 0.5 and
  the centre is within `sqrt(r^2 + 0.25)`. The window and samples do not
  depend on age, so raster(a) is a subset of raster(b). Selection
  `{cut}` rasterizes the pieces a cut removed (full radius).
- Measured (`world_logic_tests tree_costs`, not registered in CTest):
  skeleton build 0.01 ms (oak, palm, pine) to 0.2 ms (elder, about 1600
  segments and 1300 clumps); one chunk at the trunk rasterizes in 0.02 ms
  (spruce) to 0.7-0.9 ms (elder, 4096 wood tiles).
- Known limits: oak crowns turn thin (mesh) after the first fork, so
  medium trees offer little block wood above the bole; giant limbs and the
  bole are where wood is walkable. Juvenile foliage and frond rings use a
  `death` age; the renderer must hide sites outside [birth, death).
- Tests: `tests/unit/game/world/tree_tests.cpp` (`tree_skeleton_deterministic`,
  `tree_sizes_follow_species`, `tree_growth_only_adds_wood`,
  `tree_size_classes_in_blocks`, `tree_height_cap`, `tree_ground_pruning`,
  `tree_cuts_and_regrowth`, `tree_skeleton_cache`).

### Step 3: generation (done)

- `world/tree_generator.{hpp,cpp}`:
  - `TreeSpeciesTable` resolves species (placement through
    `floraPlacementRule`, shared with flora; soils through its own
    `FloraSpeciesTable`, which now builds soil tables even without plant
    species), wood block ids, the `natural_growth` set, and the widest
    ordinary extent and spacing.
  - `placeTreeLandmarks`: massive species, once per world, from region data
    (land height, vegetation, climate rules), one candidate per region with
    probability density x region area, inside a 24-tile margin; spacing is
    a global greedy pass in hash order. Recomputed at load (not saved).
  - `generateRegionTrees`: the trees **anchored** in one region, built by
    that region's job only, so every skeleton has a single source and
    neighbours never disagree. Ordinary species: one jittered candidate per
    cell (cell from density, at least the minimum spacing), kept by density
    and cluster mask, valid by `floraSuits` at the anchor and a level root
    disc (8 samples at the flared trunk radius, capped at 12 m; the tree
    stands on the lowest sample). Rivals within spacing are evaluated
    `maxSpacing()` around the region (local outranking: priority, then
    larger extent, rank, hash). Landmarks are always built in their region
    and exclude ordinary trunks from their own trunk disc. Generated ages:
    landmarks 1.5-2.5 maturities old, others `u^0.6` of
    min(0.8 lifespan, 3 maturities). Skeleton headroom is the build limit,
    ground pruning reads the region's terrain sampler.
  - `makeTreeColumn`: trees whose bounds (wood or foliage) meet a chunk
    column, with anchors moved to the copy nearest the column (X seam),
    trunk discs within `TreeDiscMargin` (32) for plants, and the column's
    wood top from block-thick segments crossing it.
  - `placeTreeWood`: rasterizes the column's trees into the air tiles of a
    chunk (key order, so overlapping trees agree).
- `ChunkManager`:
  - Region tree sets are worker jobs (`regionTrees`, the neighbourhood is
    captured on the worker). `prepareTrees` builds a column's `TreeColumn`
    once the sets of the 3x3 regions around it (and of regions within two
    holding a landmark) are ready; meanwhile its chunks stay queued.
    Building it raises the column top to the wood (`raiseColumnTopY`), so
    sunlight never treats a tree's slabs as open sky. Sets are released
    when no registered region lies within two regions.
  - `isAirChunk` takes the column's wood top. Fresh chunks get wood after
    `generateChunk`; plants avoid trunk discs (`FloraGenerationInput::
    treeDiscs`, also in `regenerateFlora`).
  - Per-chunk `TreeStamp` (`ChunkHandle::trees`): day and `TreeVersion` of
    its wood, saved in the FLOR blob header (`flora_codec` v3; v1/v2 decode
    as `Never`) through `RegionSnapshot::trees`. A saved chunk without a
    stamp (saved before trees) gets whole trees once, into its air, and is
    marked for saving.
  - Edit detection treats grown wood as air (saved-chunk check and
    `TerrainEditTracker` ground and structure scans), so forests are never
    edits or distant structures.
- `GeneratorVersion` 8. `SMOKE_FOREST=1` turns the smoke scene into humid
  temperate forest (oaks, redwood groves): for example
  `SMOKE_FOREST=1 SMOKE_CAMERA="5440 6208 0.8 -0.3 45 20"
  build/tests/lod_render_smoke /tmp/forest 500 lights`.
- Known limits: trunks disappear beyond the detail radius (distant trees
  are step 7); block branches cast vertical sun shadows, foliage does not
  (step 4 draws crowns, canopy shade is later); trees are not reconciled
  as they grow (step 5).
- Tests (`tree_tests.cpp`): `tree_region_sets` (determinism, anchors,
  spacing across a region border), `tree_column_wraps_seam`,
  `tree_chunks_order_independent`, `tree_chunks_air_and_tops`,
  `tree_saved_chunks_not_edits` (stamps, no edits, no structures, whole
  trees for chunks saved before trees), `tree_flora_avoids_trunks`,
  `tree_landmarks`; codec v3 in `flora_codec_roundtrip`.

### Step 4: crown rendering (done)

- Shared shapes: ordinary generated trees take one of
  `TreeSpeciesTable::TreeVariants` (8) skeletons per species
  (`TreeSpeciesTable::variant`, cached, built with the species seed and
  the variant), turned by 0-3 quarter turns (`TreeSkeletonInput::rotation`,
  `rotateTreeSkeleton`; exact on the tile grid, so the turned wood equals
  the turned raster). A tree gets a skeleton of its own (`variant` -1) when
  the shared one would push a branch into the ground or rise above the
  headroom; landmarks always have their own. About 90% of forest trees
  share shapes, so crown meshes are shared too.
- Landmarks stand on the lowest ground under their whole flared base
  (rings of samples out to the flared trunk radius), so buttresses never
  float; ordinary trees keep the level root-disc rule.
- The crown fit scales every branch point (drooping branches below the
  crown base too) and may shrink a crown to 5%.
- `world/tree_foliage_builder.{hpp,cpp}` (`tree_foliage::build`): a
  `FloraMesh` in tree-local space, 16-bit indices (at most 65535
  vertices): bark tubes (two crossed strips) for branch parts thinner than
  a block, needle/leaf clumps (3-5 cards, spherical normals, centre-to-edge
  gradient) at each live foliage site, arching drooping strips for fronds.
  Vertex `height` is the wind weight. `Detail::Far` keeps half the clumps
  (larger, 3 cards) and only main branches, plus any branch carrying a kept
  clump. Growth is drawn in 32 buckets of the current growth phase (from
  `TreeSkeleton::growthPhaseStart`, the latest regrowth, to maturity), each
  built at its bucket's start so a mesh never runs ahead of the wood
  blocks (`bucket`, `bucketAge`, `tubeSpan`); dead trees (age past their
  lifespan) keep only twigs. Decurrent crowns now carry a larger clump at
  every twig tip and leaves along the last two fork levels.
- `world/tree_renderer.{hpp,cpp}` (`TreeRenderer`, in `GameplayState` and
  the smoke harness): gathers trees from `ChunkManager::treeColumn` of the
  columns within `FadeEnd` (128 m) of the eye every 120 frames or when the
  eye changes column; groups them by mesh key (species, variant, bucket,
  detail, bare; unique trees by their key) and draws one instanced
  `foliage` draw per mesh, instance yaw = quarter turns. Light per tree is
  the brightest of four air tiles at crown height; ground colour from the
  cover under the trunk; withering over the last 3% of the lifespan. The
  near mesh is used within `NearDistance` (40 m) plus half the crown.
  Meshes are built on the main thread (8 per frame), released after 600
  unused frames.
- Foliage program: `u_foliage.z = 1` is tree mode (sway = 0.3 x weight,
  no root darkening).
- Data tuning after visual review: oak clumps 1.8-2.6 m at density 2.2,
  elder 5-8 m at 1.0, oak and redwood root flares 0.25 and 0.5, palm trunk
  radius 0.1-0.35 m and oak 0.2-0.45 m (both mesh-only, including the
  root flare), palm lean
  3-15 degrees; rasterizer slack 0.4 (thin leaning trunks stay
  face-connected).
- Smoke harness: `SMOKE_FOREST=taiga|tropical|mountain|elder|1` (elder
  raises landmark density and aims at one), `SMOKE_RADIUS` (chunk request
  radius, default 3; use 8 for a massive tree). Measured (llvmpipe/RADV
  harness): tree CPU per frame below 0.05 ms once meshes exist; a mesh
  build costs up to about 1.5 ms, the elder's about 7 ms once.
- Known limits: GPU cost of dense forests was not measured; crowns end at
  128 m (distant trees are step 7); sky light is one value per tree
  and block (lamp) light two, sampled by the trunk near the ground and in
  the crown and blended by height in `foliage.frag` (packed 4+4 bits in
  `i_data3.y` in tree mode), so a lamp at a tree's foot lights its trunk
  and lower branches; there is no per-pixel canopy shading; a landmark mesh build is a one-time
  main-thread hitch.
- Tests: `tree_rotation_exact` (quarter turns, shared shapes in
  generation), `tree_foliage_meshes` (all species, both details, buckets,
  dead trees).

### Fixes after the first in-game test

- Leaves on wood that has grown into blocks are not drawn (juvenile
  leaves on a thickened stem looked like ivy): `woodAt` in the foliage
  builder.
- Palms keep only their top frond ring (a ring dies when the stem passes
  the next one), and fronds start at the surface of a block stem.
- Crown light: samples at crown height and above the crown; all-zero
  samples (slabs not lit yet) count as unknown (full sky) and every gather
  samples again. Black crowns came from light read once before sunlight.

### Steps 5 and 6: growth, overrides, cutting (done)

- `world/tree_overrides.hpp`: `TreeRecord` (planted trees: species,
  seed, anchor, birth day, headroom; any tree: its cuts; generated trees:
  a persistent removal marker) in a
  `TreeOverrideMap`, the truth for trees that differ from generation.
  `save/trees_codec` writes it to `trees.dat` (fourcc `TREE` v3, reads v1
  and v2; v3 adds `TreeCut::plane`, see "Approaching massive trees");
  `save_catalog::readTrees`/`writeTrees`. `GameplayState` loads it before
  regions register and writes it as soon as it changes (and on exit).
- Cuts on existing skeletons: `applyTreeCuts(skeleton, species, cuts,
  first)`; `buildTreeSkeleton` applies `input.cuts` the same way after the
  rotation, so a tree cut in play and one rebuilt from `trees.dat` are
  identical (`tree_cuts_incremental`). Skeletons keep `seed`, `headroom`,
  `clumpSize`, `windAzimuth` for that. The part below a cut gets
  `TreeSegment::CutEnd`: its wood stops flat at the cut plane.
- Generation: region jobs apply saved cuts to generated trees (their
  skeleton becomes their own) and build planted trees anchored in the
  region (`buildPlantedTree`, no terrain pruning so play and reload
  agree). `TreeInstance::cuts`; `finishTreeInstance` makes wood and bounds
  include the wood cuts removed, so chunks loaded later still find the
  tree. A tree job built with outdated overrides is redone.
- `ChunkManager`: `setTreeOverrides`, `updateTree(tree, oldBounds)`
  (replaces the tree in its region set and rebuilds the columns it covers
  at once, raising their tops), `treeRevision`, `setTreeStamp`. A chunk job
  whose tree column was replaced meanwhile is redone instead of published.
  Saved chunks catch up on load: `treeWoodChanges(since, today)` adds wood
  grown since their stamp (into air only) and clears natural wood of cuts
  made after it; player holes in older wood stay (`tree_reload_catches_up`).
  Wood of a different `TreeVersion` is frozen.
- `TreeInstance::growthAge`: growth stops at the end of the lifespan, or at
  a root cut of a species that does not resprout (`skeleton->dead`).
- `world/tree_simulation.{hpp,cpp}` (`TreeSimulation`, in `GameplayState`):
  - Growth sweep: chunks of the built tree columns in turn (4 per frame);
    a chunk whose stamp is older than today gets `treeWoodChanges` through
    `WorldEditor` (Replace requests: remesh, sunlight, plants in the way)
    and a new stamp.
  - Cutting: on `blockChanged`, removed living wood finds its tree and
    segment (`owner`); thin parts (radius <= 2) are cut at once, thicker
    ones once their cross-section at that height holds no wood (unloaded
    tiles count as wood). Removing the top of a stump (nothing above) cuts
    nothing. A cut is recorded, the tree rebuilt, and the wood above the
    cut cleared in loaded chunks; it regrows from the cut later.
  - `plant(ground, species, today)`: a sapling with the free height above
    it as headroom (`tree_planted_indoors`).
  - `describe(tile, today)` for the debug panel.
- Debug UI: **PLANT TREE: <species>** (row above the toolbar; click again
  while armed for the next species); the tile panel shows the tree under
  the cursor (species, age, maturity, height, cuts, dead, planted).
- Tests: `trees_codec_roundtrip`, `tree_cuts_incremental`,
  `tree_cut_and_regrowth` (saw through a voxel-wood oak test fixture, nothing
  above, regrowth, identical reload from `trees.dat`), `tree_reload_catches_up`,
  `tree_planted_indoors`.

### Step 7: distant trees (done, simple form)

- `tree_foliage::Detail::Distant`: a sixth of the clumps (2.2x, two
  cards) and every visible branch part as a six-sided bark prism (block
  wood 0.6 m thinner than the blocks, so voxels win where both exist).
- `TreeRenderer` gathers from all built region tree sets
  (`ChunkManager::forEachTree`, the regions within two of a loaded one) and
  from `ChunkManager::landmarkTrees` (a cache of built landmarks that is
  never released; missing ones get region jobs, two per call; landmark
  regions built without their tree are not asked again). Trees anchored in
  the detailed chunks around the camera target draw near/far crowns next
  to their blocks; others are distant meshes. Ordinary trees end at
  `DistantEnd` (600 m), landmarks at the view distance (dithered fades).
- Measured on the forest smoke view: 330 trees, 204 instanced draws, 0.35
  ms CPU per frame, up to about 3 ms in frames that build meshes.
- Not done (step 8 or later): canopy darkening of distant terrain, mesh
  builds on workers.

### Far billboards (done)

- `regionBillboards` (`tree_generator`): placement only
  (`RegionTreesInput::lightweight`: every tree takes its shared shape, no
  ground fit, own skeletons, landmarks or overrides), so the positions,
  species and ages equal full generation (`tree_billboards`). Each
  `TreeBillboard` carries the anchor, the shape's height and crown radius,
  growth timing and a hash; a region's list is sorted by hash. About 10-15
  ms per region.
- `ChunkManager`: billboard sets per region, cached until
  `releaseBillboards`; `wantBillboards` takes the renderer's nearest-first
  wish list and `update` keeps `MaxBillboardJobs` (2) low-priority jobs
  busy; `billboardRevision` tells the renderer new sets arrived.
- Textures: `foliage.billboard_conifer|broadleaf|palm|pine` (crown
  silhouettes, `tools/generate_foliage_textures.py`) and foliage materials
  of the same names, chosen by form (giant and decurrent use broadleaf).
  `tree_foliage::buildBillboard`: one normalized mesh per species (three
  crossed crown cards coloured from the species' leaf palette, two bark
  quads for the trunk, proportions from its variant-0 shape); instances
  scale it by each tree's crown and height times its growth.
- `TreeRenderer`: regions from 0.8 `DistantEnd` to `BillboardEnd` (2048 m,
  or the view distance), one instanced draw per species. A region at
  distance d draws the first (600/d)^2 of its trees (at least 10%), crowns
  up to 1.8x larger to keep the cover. Foliage instances gained a fade-in
  distance (`i_data3.w`, `v_fade`): billboards dither in over exactly the
  band where distant meshes dither out (480-600 m). Dead and very young
  trees are skipped; landmarks stay meshes.
- Measured (smoke harness, taiga from 250 m up): 6665 billboards, 0.4 ms
  tree CPU per frame.
- Known limits: billboards end at 2 km; one shape per species (variants
  differ only in size); no per-tree light or ground tint at that distance.

### Playtest fixes: support, cuts, placement and LOD

- A tree that loses its last ground support or its voxel trunk base vanishes
  immediately. Loaded natural wood is cleared through `WorldEditor`. A
  removal record prevents a generated tree from returning after reload;
  a removed planted tree is erased from the overrides. Natural aging is
  separate and can leave standing wood.
- The cut plane clips the surviving trunk chain, not just the last segment.
  Thin trees sit on their own column with room above it, and fronds and
  juvenile foliage stay attached to their parent segments. Palm and oak
  are mesh-only at every age; cutting tests use a separate voxel-wood fixture.
- Rendering requests tree sets beyond the voxel streaming radius. Billboards
  cover a region until its tree set arrives and account for cuts, planting,
  removals and landmark exclusion. Distant meshes keep the branches that
  carry visible foliage and supply bark where voxel wood is not yet uploaded.
- Wood hand-over: a tree in the detailed chunks keeps its distant mesh (bark
  prisms) until every chunk of its wood bounds is actually drawn
  (`TreeRenderer::setChunkDrawn`; in the game `GameplayState::detailChunk`
  plus an uploaded mesh, the same rule as the chunk pass). It used to switch
  as soon as no chunk was still meshing, while the terrain detail coverage
  (enter radius 6, whole column uploaded) had not yet handed the column to
  blocks, so big trees such as redwoods vanished for a few frames.


### Landmark voxel wood at distance (done)

The massive-tree voxel proxy from the plan: elder trunks and limbs keep
their block look out to the view distance (4-8 km), and parts of a landmark
outside the loaded chunks are no longer missing.

- `tree_wood_lod` (pure): `rasterize(source, level, period)` turns a tree's
  wood at an age into 16^3 slabs keyed like chunks. Level 0 is the real
  rasterizer, tile for tile; levels 1 and 2 use cells of 2 and 4 tiles
  (wood when at least a quarter of the cell is), with the level in the high
  bits of the key's y. Full slabs surrounded by full slabs are marked
  `enclosed` and get no proxy. Test: `tree_wood_lod_slabs` (level 0 equals
  the rasterizer, coarse cells meet the threshold, keys wrap on X).
- `StructureLod::setStructures` takes the landmarks in view as structure
  voxel sources (`TreeRenderer::landmarkVoxels`, whose age is quantized to
  the crown growth buckets, so a tree is re-rasterized a few dozen times over
  its maturity). Since the structure voxel API (`docs/handoff/structures.md`)
  the level is chosen per slab, not per tree; the paragraph below describes
  the original per-tree version.
  Each tree is rasterized on the pool (two jobs, nearest tree first) at a
  level chosen by its distance: full voxels within 512 m, 2-tile cells to
  1536 m and 4-tile cells beyond, with 64 m of hysteresis. The old slabs stay
  drawn until the new ones are ready. The merged wood fills the air of the
  proxy slabs and the meshes are built like structure slabs
  (`ChunkMesher`, greedy). Coarse proxies are meshed in cell units and
  drawn scaled. Level 0 slabs hide where the real chunk is drawn, exactly
  like structures, so the hand-over is seamless.
- Tree wood has its own GPU cap (`treeBudget`, 4x the structure budget:
  32 MiB at Low). In the smoke harness's elder scene (18 elders in view,
  about 100x the shipped density), wood uses about 1100 slabs and fills in
  ~30 s. With the shipped density (0-2 in view) it takes a few seconds.
- `TreeRenderer::setVoxelLandmarks`: landmarks beyond the detailed chunks
  draw `Detail::DistantCrown` (distant clumps, tubes for thin branches, no
  bark prisms). Ordinary trees are unchanged.
- Landmark wood shades distant terrain and structure slabs like voxel
  sunlight through the sky occlusion map (`docs/handoff/structures.md`,
  step 2); crowns cast no shade there, as in the chunks.
- Known limits: player wounds in a landmark that do not sever it (holes
  dug into the trunk) are not shown at distance until the cut is recorded;
  the wood comes from the skeleton, not from saved chunks. Coarse cells are
  textured as 2x or 4x larger blocks.
### Massive tree performance (the elder sample)

The elder is the optimization sample for big trees: `tree_benchmark`
(`cmake --build build --target tree_benchmark`, then
`build/tests/tree_benchmark [crown_m height_m trunk_m]`) streams one elder
and times a trunk-surface break, a thin-branch cut, its crown meshes and
its distant voxels. Measured with a 1000 m crown, 500 m tall, 64 m trunk
(1752 segments, 24k foliage sites, wood box 1507x589x1749):

| Work | Before | Now |
| --- | --- | --- |
| Break without a cut (`TreeSimulation`) | 0.07 ms | 0.07 ms |
| Thin-branch cut (`TreeSimulation`) | 3.3 ms | 3.3 ms (mostly `applyTreeCuts`) |
| Clearing the cut wood | in the same frame | queued per chunk, nearest first, 2 ms per update |
| Removing a dead tree's wood | one mask of the whole wood box (1.5 GB here) | queued per chunk, one chunk box at a time |
| Near crown mesh | 14 ms on the main thread, truncated at 65532 vertices | 29 ms on the pool, 3 parts, 143640 vertices |
| Distant voxels after a cut | every slab of the tree (11 s of pool time at level 0) | the slabs near the cut (2 here) |

- `TreeSimulation::lastBreak()` gives the stage times of the last removal;
  `pendingClears()` / `finishClears()` expose the clearing queue. Cuts keep
  an immutable snapshot of the cut tree, so column rebuilding, later cuts
  and tree death cannot lose the pending cut's wood.
- `tree_foliage::buildParts` splits a crown into meshes of at most 65535
  vertices; `build` returns the first part (enough for ordinary trees).
- `TreeRenderer` builds crowns on the pool (up to 8 jobs, 8 starts per
  frame) and draws a tree's previous mesh meanwhile (`drawnMesh`), so cuts,
  growth steps and detail changes never blink a crown out.
- `structure_voxels::Source::changedSince`: trees diff their skeleton
  segments and return the bounds of those that differ (clamped to the
  tree), so `StructureLod` rebuilds only those slabs. A cut resets the
  renderer's quantized age: unchanged mature segments remain reusable
  across that age change, and unborn sprouts contribute no dirty bounds
  (`tree_wood_lod_cut_is_local`).
- Clearing runs on the pool: `treeCutWoodDelta` / `treeWoodRemoval` (pure
  rasterization) per chunk, at most 8 high-priority jobs; the main thread
  applies `filterTreeWood` edits within the 2 ms budget. A cut queues only
  loaded chunks overlapping its removed pieces, found by
  `treeCutWoodBounds`, and processes only that cut of that tree. Empty
  removal masks skip the living-tree rasterization. Settling also compares
  live wood with the day before the cut to find additions (float tree ages:
  1e-3 day rounds away at elder ages). `tree_cut_delta_local` checks both
  falling and settling against the catch-up path across the X seam.
- `TREE_BENCH_CUT_ONLY=1 build/tests/tree_benchmark` measures a thin elder
  cut and exits before full-tree rasterization. On the default fixture the
  cleanup queue shrank from 143 chunks to 2, and drained in about 0.09 ms
  instead of 39 ms, without rendering contention. Use
  `TREE_BENCH_LARGE_CUT=1` to measure the mixed-resolution proxy update of
  the thickest primary limb cut at one quarter of its root segment.
  With the renderer's quantized ages, the small cut rebuilds 2 of 131444
  planned slabs; the primary-limb cut rebuilds 23684 of 144205 in about
  1.9 seconds of worker time. These are CPU fixture measurements, not
  end-to-end in-game latency with meshing, uploads and other pool work.
- A removal tests every segment whose wood holds the tile (`owners`) and
  cuts the thickest one it severs: a flared trunk shares its outer
  tiles with buttresses, which used to win. The thin-part shortcut (any
  block cuts a part up to 2 m) applies to the best match only.
- Thick-branch detection checks transverse voxel-grid slices through the
  removed row, including overlapping capsules at joints. A plane normal to
  a sloping limb incorrectly samples intact wood beside a complete vertical
  slit. The cut parameter now comes from the retained face of the cleared
  row, keeping the stump and regrowth at that face. Unloaded supporting wood
  still prevents a cut; cheap exact cell probes avoid building large masks
  while obvious support remains. Ownership uses the actual azimuth of the
  tile for lobed root flares, and candidates are not capped at four.
  Trunks retain their shared ring test for overlapping flare and buttress
  capsules.
  `tree_cut_angled_branch` checks a 12-block-wide sloping limb with a
  one-tile slit and one last support; `TREE_BENCH_SAW_ONLY=1` runs the elder
  trunk sawing benchmark without full-tree distant rasterization afterward.
- Open: falling branches (physics); withering of massive trees;
  `refreshTreeColumns` after a cut (about 5 ms with the elder's loaded
  columns) and `applyTreeCuts` are the main-thread cost of a cut.

### Settling trunks (`cut.fell`, done)

Repeated cuts rescale surviving children's `attachT` to the shortened
parent segment. This keeps later cuts pruning the correct branches and
their foliage when working back from a limb's tip. Crown mesh cache keys
also include the cut count so reusing a skeleton allocation cannot bring
back a crown from an earlier cut. `tree_repeated_branch_cuts` checks wood,
foliage and replay of a sequence of tip-to-base cuts.

Agreed 2026-11-18: a massive trunk cut through comes down onto its stump
and closes the gap; trees that fall (redwoods) will be hazards later, with
physics, and today still lose the part above.

- Data: `Tree::cut.fell` (`TreeFell::Fall` default, `Settle`); the elder
  settles. Modder docs: `docs/modding/trees.md`, `cut`.
- Skeleton (`applyTreeCuts` -> `Builder::settle`): a trunk cut (order 0) of
  a settling species prunes as usual, then adds a live copy of everything
  it pruned, one tile lower along the cut axis; the piece of the cut
  segment above the cut starts on the stump, one tile shorter; foliage the
  cut pruned comes down with it. No regrowth, never dead. The stump keeps
  its flat cut (`CutEnd`); the cut plane clips only the stump's own axis.
- `TreeSegment::appear`: a settled copy exists from the cut's age on, so
  any day before the cut still has the original. That is what lets the
  usual wood reconciliation (`treeWoodDelta`: wood now but not before is
  added into air, cut wood not wood now is removed) fill the sawn ring in
  loaded chunks and in chunks saved before the cut. `matureAge()` and
  `growthPhaseStart()` include `appear`, so crowns and distant voxels are
  never built for a day before the settle (a settle on an old tree comes
  long after its maturity).
- `TreeSimulation`: a settling trunk is cut only through a whole
  cross-section, however thin; losing its first wood row does not kill it
  (losing the ground does); while its clears are pending, its trunk is not
  cut again (the sawn ring is still air, which would cut it again and again).
- Tests: `tree_settle_closes_gap` (rasterizer: ring wood again, one tile
  shorter, every clump kept, nothing before the cut), `tree_settle_in_world`
  (sawing a ring in a streamed world). Benchmark: `TREE_BENCH_SAW=1
  build/tests/tree_benchmark` saws the elder's trunk (16160 tiles, 0.03 ms
  per block on average; one cut at 6.6 ms; 886 chunk updates done on the
  pool in about 0.3 s; the ring refilled except buttress tiles below the
  cut).
- `TreeVersion` was not bumped (only cuts of settling species change, and a
  bump stops growth in every saved chunk): elders cut before this change
  keep wood without their top in chunks saved then.

### Approaching massive trees (2026-11-19)

Walking up to an elder dropped the frame rate and parts of the tree
vanished and "built themselves". Measured with the smoke harness's
approach mode (needs a GPU and display: `tests/README.md`, "Device
targets"): one elder in view, chunk radius 8, from 1200 m to 40 m at
0.5 m per frame.

| Per frame, near the tree | Before | Now |
| --- | --- | --- |
| `StructureLod::update` (main thread) | 20-30 ms, up to 77 ms | 0.5-1 ms, at most 10 ms |
| Frames over 33 ms | 883 of the walk | 8 (all region unloads, see below) |
| Distant wood showing nothing within 512 m | up to 536 slabs, never recovered at Low | at most 2 slabs, for a few frames |
| Chunk dispatch (`ChunkManager::update`) | 25-50 ms with a full queue | below 8 ms (p99) |

What cost the time, and the fixes:

- **Planning every empty slab.** `structure_voxels::plan` split the whole
  1.5 x 0.6 x 1.7 km wood box (about 131000 slabs, almost all air), and
  every finished voxel job asked for a new plan, so it ran nearly every
  frame (9-20 ms). Sources now report where they may hold voxels
  (`Source::occupancy`; trees mark slabs within reach of their wood
  segments, about 2.7 ms once per source on the pool) and the plan visits
  only those (5000 slabs, 0.9 ms). A structure is not planned again while
  its eye cell, source and built slabs are unchanged. Rasterizing only
  occupied slabs also makes the large-limb cut rebuild 1849 slabs instead
  of 23684 (0.9 s of worker time instead of 1.6 s).
- **Merging on the main thread.** Each finished job copied and merged all
  slabs and re-marked enclosure (2.5-5 ms). The job now returns the merged
  map and the changed keys; one structure's map is shared as is.
- **Holes while levels change.** A coarse proxy was deleted as soon as the
  plan wanted finer slabs, which were then meshed two at a time, nearest
  first, and at Low could not fit in the tree budget at all. Proxies that
  leave the plan are now *retired*: drawn until every replacement is
  drawable or shown by its real chunk, while the replacements under them
  wait, so a level swap never shows a hole or two levels at once. Retired
  bytes do not count against the budget (they would block their own
  replacements). Level 0 slabs whose chunk is drawn at least
  `DrawnMargin` (2) chunks inside the detail box get no proxy mesh, and
  four mesh jobs run instead of two.
- **Tree readiness per queued chunk.** With a landmark in the world a
  chunk reads tree sets up to `landmarkRadius` (about 9) regions away; the
  starvation guard checked 361 regions for each of thousands of queued
  chunks, and chunks waiting for tree sets computed their column inputs
  first (41 ms for a 1024-chunk window). Readiness is now memoized per
  region and update, and checked before the column inputs.
- **Region unloading** (fixed 2026-11-19, see below): crossing a region
  border unloaded about three regions (12000 chunk handles and their
  `removed` listeners) in one frame, 100-140 ms with or without trees.

Why the tree looked like it was building itself: distant wood comes in
three levels (full tiles within 512 m, 2- and 4-tile cells farther). As the
eye moved, slabs switched to a finer level; the old coarse mesh was dropped
before the new one existed, and the whole tree's batch of new slabs had to
be rasterized before any was shown.

Bugs fixed in the same pass:

- **Leaves left in the air after a cut.** A crown mesh job was collected
  only when its own key was drawn again. A key left behind by a cut, a
  growth step or a detail change kept its job slot forever; with all 8
  slots taken no crown was rebuilt and cut trees kept drawing their old
  crown (`drawnMesh`) until a stale key happened to be drawn again.
  `TreeRenderer` now collects every finished job each frame, and an empty
  finished crown draws nothing instead of the previous one.
- **Blocks left beyond a sawn limb.** `TreeSimulation` finds a thick
  branch severed on a tile-grid slice (the row the player sawed), but the
  cut used a plane perpendicular to the branch: on a sloping limb a wedge
  beyond the slit stayed stump wood. `TreeCut::plane` now records the
  grid plane (+-x, +-y, +-z; 0 is the old perpendicular plane, used for
  thin parts, trunks and cuts saved before), stored in `trees.dat` v3, and
  the stump keeps it as `TreeSegment::cutNormal`. A cut's removed wood
  also takes the stump's wood beyond its face and reaches half a sample
  spacing further (the pruned piece samples anew from the cut point; rim
  tiles of the original raster stayed as stray blocks). `TreeVersion` is
  unchanged: existing cuts rasterize as before. Test:
  `tree_cut_angled_branch` (18 blocks were left beyond the slit).

Tests: `tree_wood_lod_occupancy` (occupancy never misses a filled slab at
any level, the elder's is sparse), `trees_codec_roundtrip` (v3 plane, v1/v2
read as perpendicular, bad planes refused). Benchmarks:
`TREE_BENCH_PLAN_ONLY=1 build/tests/tree_benchmark` (occupancy and plan
costs), `TREE_BENCH_LARGE_CUT=1` (now with occupancy).

### Region borders and cut wood in saved chunks (2026-11-19)

- **Freeze at every region border.** Unloading three regions in one frame
  cost 100-140 ms. Most of it was `SunLightPropagator::removed`: each
  removed slab marked its column dirty, walking every remaining slab of the
  column and its neighbours (quadratic). It now only bumps the column's
  revision and recomputes the column once on the next update (an emptied
  column costs nothing). That halved the stall; the rest was per-chunk
  work, so play now unloads gradually: `LocationManager::onPlayerRegionChanged`
  hands distant regions to `update()`, which unregisters whole chunk
  columns within `UnloadBudget` (2 ms) per frame, snapshotting each column
  as it goes (edits to the rest are kept) and storing the region after its
  last column. Loading a region back while it unloads, or `saveAll`,
  finishes it first. `unloadDistant` without `gradual` is unchanged.
  Approach run (elder, High): worst chunk section 130 ms -> 16 ms, frames
  over 33 ms 8 -> 1. Tests: `region_gradual_unload`,
  `region_reload_while_unloading`.
- **Shadow under a sawn limb, far from the cut.** Chunk stamps are whole
  days (`floraDay`), and a saved chunk skipped catching up when its stamp
  day equalled today, and cuts made during the stamp's own day. A limb's
  far chunks saved earlier the same day (a region unloaded before the cut)
  came back with the cut wood: high above the detailed slabs and absent
  from the distant voxels (built from the cut skeleton), so invisible, but
  it shaded the ground (sun 12-13). Catch-up now runs on the stamp's own
  day, and only cuts made before the stamp day began (with float slack)
  are skipped; clearing a cut again is harmless. Test:
  `tree_reload_same_day_cut` (47 cut tiles came back before).
- Sunlight under a sawn limb in loaded or later-loaded chunks was already
  right: `tree_cut_relights_below` (CTest), `TREE_BENCH_LIMB_LIGHT=1
  build/tests/tree_benchmark` (the elder's thickest sideways limb, ground
  0-4 before, 15 after) and the device scenario `SMOKE_LIMB_CUT=1|walk` of
  `lod_render_smoke` keep it covered.

### Slanted and stepped cuts (2026-11-19)

Thick branches were cut only when a whole tile-grid slice (x, y or z
constant) through the removed block was empty, so slanted, turned or
stair-stepped slits never cut, and the cut plane could only be +-x/y/z.

- `TreeSimulation::severedByConnectivity` (after the grid slices fail): the
  limb's axis chain (the owning segment and those continuing it) within a
  window of 3 radii + 4 tiles each way; from the limb's core at the far
  window end, a best-first search toward the base through wood tiles
  (face-connected, within radius + 1.5 of the chain; unloaded tiles count
  as wood, at most 400000 visits). Reaching the base: attached. Otherwise
  the far piece is off: the plane is fitted (least squares) to the hollow
  tiles inside the limb next to it, oriented along the limb, and offset so
  every far tile lies beyond it (midway to the near face when a plane
  separates them, else just before the far piece; a stepped slit may cost
  the stump a few corners). Slits nearly along the limb, or planes missing
  the axis, fall back to a cut across the limb before the far piece.
- Blocks hold by faces: a limb whose last contact across the slit is an
  edge or a corner falls. `tree_cut_angled_branch` now expects the cut when
  the last face bridge goes, not the last block of the slice.
- `TreeCut::normal` (any unit vector, zero = perpendicular) replaces the
  grid code; `trees.dat` `TREE` v4 writes it as three floats, v3 codes and
  older cuts still load. A cut's removed wood also takes the clipped end
  caps of the stump's axis below (as `cutPlanes` clips the live wood).
- Measured (`TREE_BENCH_LIMB_LIGHT=1 TREE_BENCH_SLANT=1
  build/tests/tree_benchmark`: the elder's thickest sideways limb, a slit
  35 degrees off its axis, 1818 blocks): cut at the last block, nothing
  left beyond; 0.45 ms per block on average, p99 1.6 ms; the block that
  cuts costs about 28 ms (18 ms search and fit, 7.5 ms commit).
- Test: `tree_cut_slanted_slit` (x + y = c and x + z = c staircases).

