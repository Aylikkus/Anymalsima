# Rivers, lakes, and fluid simulation

Ship rivers and lakes from voxel detail to the far terrain LOD before turning
on fluid simulation. Use a regional water-body graph for standing water,
directed reaches for rivers, and a bounded cell simulation for active water.
All three need to answer the same questions about where water is and how high
it reaches. A change in level must be visible from a distance and must be
queryable in territory that has never been loaded.

Steps 1–3 establish generation and the visual handoff. The first interactive
river release also includes the scoped work in steps 4–5 so players can dam
a river, see water pool and spill, or expose a spring and create a new river.

## What the game already does

| Area | Current behavior and consequence |
| --- | --- |
| World map | A regional priority flood records candidate basins, catchments and outlet elevations. The preview displays the candidates. River polylines supply candidate routes and sampled bed elevations. |
| Chunk generation | `TerrainSampler` supplies ground, soils and a generated water surface to `LocationGenerator`. The sea surface is Y=385. Inland lakes have their own spill-capped levels and face-connected footprints. Earlier generator versions retain the original sea fill. |
| Detailed rendering | `ChunkMesher` emits opaque horizontal water tops at tile Y+1 when the tile above is known and empty. The detailed draw also depends on a usable light volume. Fractional tops and waterfall faces are planned for later steps. |
| Distant rendering | Stepped and smooth patches use the same water query, including terrain edit summaries. Smooth patches clip water to sampled wet coverage and the patch triangles. Water bounds and slab readiness drive the proxy handoff. Camera submersion uses a loaded tile when available and the shared surface otherwise. |
| Save and edits | `RIVR` stores the candidate polylines. The saved generator version selects the terrain-edit baseline. `water.dat` stores runtime volumes, river holds and local credits. Chunk v5 reprojects water owned by that ledger when a slab loads. |

The create-world UI currently generates 512 × 300 regions. A region is
128 × 128 horizontal tiles and a voxel chunk is 16³. At the proposed
2,000 × 1,200 regions, the world would be 256,000 × 153,600 tiles, about
39.3 billion surface columns. Use region-scale preprocessing and refine
shorelines, routes, and sills on demand. Water coordinates follow
`WorldWrap`, with canonical X and bounded Z. Current distant LOD wraps only
for widths aligned to its 16-region roots and within its view-distance
constraint. The proposed 2,000-region width meets the alignment rule. Water
must use the same patch wrapping policy as terrain.

## Shared data and ownership

- Define one CPU sampling interface over immutable hydrology snapshots.
  A surface query returns final ground, wet footprint at the requested
  coordinate, surface height, liquid kind, body or reach identity, and flow
  direction where applicable. Distinguish the top solid tile Y from the
  geometric ground surface at Y+1. Start with water surfaces at integer
  boundary heights and fill tiles strictly below them and above solid ground.
  Leave room for fractional surfaces when finite-volume cells arrive.
- Keep generated hydrology and runtime changes distinct. Generated data
  includes region membership, candidate flood extents, channel geometry and
  baseline terrain. Runtime state owns standing body storage, confirmed
  connectivity edits, and sparse local fluid overrides. On-demand queries
  apply saved terrain and hydrology overrides and work without voxel
  streaming. Loaded chunks project that same state into voxel cells. Use
  loaded voxels for exact water contact and underground cavities when present.
- Give standing bodies stable saved IDs and elevation-to-storage curves.
  A region mask is a search index. Admission beneath a surface requires a
  connected wet footprint at that elevation. Dry depressions behind ridges
  and air under overhangs need separate connectivity evidence. Directed
  river reaches have their own bed, downstream surface profile and outlet.
  River direction and rendered speed can come from the reach before a
  cellular solver exists.
- Include spring outlets, confluences, spill points, and receiving basins
  in the graph. Springs are sources. Reaches carry water from those sources
  to sinks such as lakes and seas. A reach can receive water from upstream
  and supply downstream reaches. Keep that routing usable while the
  intervening terrain is unloaded.
- Build workers from captured inputs. Track the hydrology revisions each
  result sampled alongside terrain edits and chunk identity. Reject outdated
  publication and use the existing owner thread for chunk state and GPU
  uploads. Index affected resident chunks, LOD patches and query records by
  footprint or body. A global change should cost work proportional to the
  affected resident data and changed summaries.

## 1. Make the existing sea agree across all LODs

Route today's sea through the shared sampler first. Keep its current height
and appearance for this step. Feed the sample to `TerrainSampler` column
inputs, `LocationGenerator::isAirChunk`, `SurfaceSampler`, and detailed
meshing. Provide a matching halo sample where meshing already permits a
missing-neighbor fallback, including the slab above a water top. Use actual
water bounds in culling and surface-band readiness. Look at light-volume
readiness as part of the handover.

Replace smooth LOD's unconditional sea-plane clip with water geometry
clipped to the sampled wet footprint and actual patch terrain triangles.
For the sea this can still be a flat plane. For later stages, treat geometry
as a distinct CPU output so water-only updates can replace it independently.
The first version can retain the opaque water material and existing world
draw order. Share sampling at patch edges, use the same shifted X image as
the terrain, and drive any water edge joins by the actual water silhouette.
Build ground joins with ground heights. Check shadow submission with water
separated so terrain and structure shadow casters stay correct.

Reuse `DetailCoverage` hysteresis and the current/drawable distinction, with
water ownership tracked independently where necessary. Suppress the proxy
only over the footprint covered by drawable detail at the matching water
revision. Water above a terrain surface band or visible from a high camera
still needs a proxy until its detailed slab is drawable. On unload restore
the current proxy before dropping its detailed replacement. Keep the last
complete presentation while new meshes upload. For changing levels, publish
both sides of affected handoff edges from the same revision before
switching the visible edge. Bound this coordination to visible neighbors.
Install level changes in simulation immediately and accept a controlled
visual delay while replacements build.

Replace the global sea-height assumption in `u_world_water` depth lighting
with per-surface or spatial water information through the relevant shader
paths. Keep eye submersion distinct from water lighting, sampling actual
fluid height in a loaded cell and querying the same surface used by the
proxy otherwise. Feed the shared surface to sky opacity, generated sky
estimates and edit comparisons. Ground cover and flora placement should
query whether the land is submerged once lakes and rivers can sit above sea
level.

**Acceptance.** Extend the existing `lod_surface_matches_chunks`,
`lod_water_and_bounds`, `lod_loaded_water_mesh` and handoff cases in
`tests/unit/game/world/lod_tests.cpp`. Use a stepped shore spanning chunks,
a water surface exactly on a slab boundary, camera movement across the
X seam and delayed uploads. Check top heights, shoreline coverage, bounds,
and a single visible owner during upload, remesh and eviction. Compare
submersion just above and below the same surface with and without a chunk.

## 2. Generate standing basins and static lakes

Build region-scale connected components and a watershed or priority-flood
pass from generated heights. Record basin catchments, candidate shore
regions, outlets and sill heights. Refine ambiguous boundaries and sills
against actual sampled and carved terrain where needed, with a bounded
cache and conservative connectivity when a connection is uncertain. Match
face-connected gameplay water at narrow passages. A diagonal region contact
and the existing lake size heuristic are insufficient evidence for an
open passage. Give inland lakes a generated storage or level capped by the
spill elevation. Keep the map lake tag as a presentation hint and update
the preview with generated hydrology as the plan matures.

Generate river basins and their baseline beds once at world creation.
Lazy sampling can reconstruct that baseline as chunks and LOD patches are
built. Runtime edits can redirect flow and change which areas are wet.
The simulation does not erode beds or regenerate basins.

Start with a section version for the generated hydrology or a versioned,
deterministic derivation from saved world inputs. Bump
`WorldMeta::GeneratorVersion` when generated chunks change and key the
terrain edit baseline to the saved generator version. Older maps can derive
candidate basins from saved regions and river paths. Keep old worlds on
legacy terrain and standing-water rules until a migration can preserve
saved terrain and water edits. Avoid silently carving a channel through
a player build or producing two terrain baselines at the frontier of
explored territory.

**Acceptance.** Use a synthetic map with two depressions at different
elevations, a low sill, a diagonal-only touch, a dry bowl below sea level,
and a coast across X wrap. Assert stable IDs, expected spill heights and
dry cells, and matching lake surface in an unloaded sampler and a newly
generated chunk. Include a lake whose top is above the old air-chunk cutoff.

Generator version 14 derives the standing-water baseline from the saved
region heights, terrain features, seeds, and river paths in `world_map.dat`.
Earlier saves continue to use the original sea fill for new chunks and edit
comparisons. Lake IDs come from the canonical seed region. The map preview
shows regional candidates, including elevated basins. The tile sampler gates
water on face-connected paths over the generated ground.

The sea pass certifies region-center paths to open Z edges and checks mixed
shorelines within a three-by-three region window. Lake refinement searches
reachable tiles up to the regional spill height. A verified lower exit
lowers that cap. A search that reaches its 24-region budget keeps the lower
levels it fully checked. Shared shore regions are partitioned by proximity to
the candidate basins, and wet masks are held in a bounded shared cache.
Distant roots first draw terrain and certified open sea. The manager later
refines any root still on screen in a single background slot while smaller
patches load. Smooth meshes use up to eight water subdivisions per terrain
cell, and stepped meshes keep tile-wide shorelines.
The same footprints feed new slabs, terrain-edit comparisons, sky estimates,
vegetation and terrain LOD. River beds record the sampled elevation along
the saved paths and their receiving catchments. Channel shaping and river
surfaces begin in Step 3.

CTest covers the regional and sampled sills, a dry sub-sea bowl, the
wrapped coast, a diagonal-only contact, and agreement between an unloaded
query, a newly generated water-only slab and both terrain LOD mesh paths
after save and reload.

## 3. Carve rivers and render flow at every distance

Generated rivers begin at aquifer-bearing rock exposed to the surface.
Use mapped aquifer sites so the origin is reproducible and can be found
through terrain edits later. Existing preview paths are routing candidates.
Anchor each accepted headwater to a spring site. Rock supplies water as a
game rule. Leave aquifer storage, depletion, recharge and pressure-dependent
discharge out of the model. The graph needs only a bounded amount of water
per simulation step from each open spring site for volume accounting.
Snowmelt is outside the planned river source model.

Index wrapped river segments by region including channel width in the index
bounds. Convert preview coordinates once against the terrain sampling
convention. Build connected reaches with deterministic confluences,
deduplicated paths, a descending profile, and explicit outlets into lakes
or the sea. Define how a reach ends at a closed basin or a plateau. Refine
the bed and banks from terrain samples, then apply the same carve before
soils, vegetation placement, terrain edit baselines and both terrain mesh
paths. Give each generated reach a water surface profile and a bed profile
so the sampler can report depth at a column. Depth is the water surface
height minus the geometric bed height. Vary it along and across the reach,
from one block of water in shallow channels to a proposed maximum of
32 metres (32 blocks) in deep channels. Keep this depth cap distinct from
the maximum terrain cut, which measures how far the bed is lowered from
the original terrain. Use caps on width, cut depth, and profile adjustments.
A preview route that cannot be carved within those limits needs repair or
an explicit termination.

### Waterfalls

Give generated rivers an explicit waterfall feature. Where a reach crosses
steep terrain, use the sampled slope and available elevation drop to select
candidate falls, including reaches that cross region boundaries. During
world creation, the river carve can concentrate a gradual descent into a
near-vertical drop for a more dramatic silhouette. Shape a stable upstream
lip, a steep face, and a downstream landing channel or pool. Preserve the
reach's total elevation drop and outlet connection. Keep selection
deterministic and budget the height and width of the fall and the extent of
terrain reshaping. Tune how often these falls occur and how steep a reach
must be through visual playtests. The waterfall's drop height is separate
from the depth of the water in the channel.

Store the lip, landing, width, and drop with the reach. Use that geometry
to continue the flowing water surface down the face and into the landing.
Preserve significant falls in coarse LOD even where terrain sampling would
smooth away the cliff or miss the narrow channel. Include the full drop in
mesh bounds and share endpoints across chunk and patch edges. Carry flow
animation down the face and into the downstream channel. Foam and spray
can be added near the landing after the silhouette and handoff work.

During play, water can fall over an existing ledge, a player-dug drop, or
a dam crest. Use the same rendering path for those falls. Feed the landing
through the existing flow or reach transfer so water crosses the drop
once and keeps moving downstream. Generated cliff shaping is a
world-creation rule. A runtime waterfall follows the existing terrain.

At voxel detail draw tops and exposed drop faces. At spacing 1 derive the
same geometry from the terrain and water samplers. At coarser spacing use
a reach ribbon clipped to the shared channel footprint and tessellated at
patch boundaries. A narrow river can cross a coarse cell without touching
any of its terrain sample points. Use the segment index to retain that reach
and preserve its connections, and verify that coarse terrain does not bury
its surface. Refine the terrain locally around the channel or use a
channel-aware coarse mesh where the silhouette requires it. Lakes and
oceans can use clipped polygons or a patch-local wet mask, with resolution
driven by shoreline complexity and a bounded patch budget. Clip overlaps at
confluences and mouths. Preserve the same world-space flow direction and
animation phase across LODs and wrapping copies. Animate normals or color
along the reach to convey downstream flow. Gameplay current can use the
same direction with a separate strength curve when movement supports it.

**Acceptance.** Use a river narrower than the far grid spacing, a
confluence, a waterfall, and a mouth crossing a patch or wrap seam. Check
continuous coverage, surface levels, direction, exposed faces, and bounds
in CPU meshes. Use a steep slope spanning two regions that the generator
reshapes into a single drop. Check that chunk and patch build order leave
the same lip and landing, that coarse LOD retains the drop, and that the
landing feeds the downstream reach. Include one-block shallows and a
32-block channel. Check that the sampler, detailed cells and LOD geometry
agree on bed and surface heights at the handoff. Extend the opt-in
`lod_render_smoke` fixture for a short
shore-to-river flyover at each terrain preset. Check for disappearing water,
double surfaces, drowned banks, popping during upload, and underwater fog.
This is the visual gate before live levels and simulation.

Generator version 15 derives the river graph from the saved paths and
terrain seeds. Paths are processed longest first, with saved order breaking
ties. A route joins an established reach where its centerline meets it and
ends at the first verified lake or sea footprint it can reach. A closed
route ends in a level channel pool. A route that exceeds the carve limits or
would need to climb into its receiver terminates at the last valid column.
Version 14 saves keep their standing basins and original terrain.

Each accepted headwater maps a spring to its exposed rock tile. Reach spans
carry a water level, bed depth, and direction, with banks indexed in wrapped
region and 16-tile buckets. The profile limits river depth to 32 tiles and
rock cuts to 96 tiles. Generated waterfalls concentrate an existing descent
of 8 to 32 tiles within a 32-tile profile window. The lower water level
defines the bottom of the visible sheet, which can extend over the exposed
cliff below the lip's channel bed. The voxel and adaptive distant meshes
use the same tile footprint and flow animation. Distant terrain subdivides
around the indexed channel, including in fallback patches.

Generator version 16 rounds river turns, adds seeded bends and width
variation, and leaves shallow shelves at the channel edges. A reach keeps
at most one waterfall. The waterfall needs a natural drop of at least
18 blocks over eight sampled stations. Other declines are graded upstream.
Version 15 saves continue to use their original river beds and waterfalls.

Initial smooth patches use a sampled coastal proxy and reuse already
surveyed lake footprints. The proxy can temporarily fill a low coastal
pocket while its exact connectivity is pending. A separate low-priority
job replaces the water mesh and depth map without rebuilding the ground.
Outlet lakes keep their exact masks within a bounded memory budget. At
coarse spacing the terrain uses fewer subdivisions along channels, while
merged water quads preserve narrow streams and significant drop faces.

Generator version 17 closes exposed dry sides of generated river
columns with solid banks up to the local waterline. Downstream channels
and verified standing receivers remain open. The carve also fills deep
holes under a reach to keep the water depth within its channel limit.
These checks operate on the baseline before chunk generation or LOD
sampling, including at chunk and wrap edges. Existing worlds retain their
saved generator's terrain.

Generator version 18 uses local soil and sand on riverbeds and banks.
Soil depth follows the terrain grade before the river cut, and submerged
sediment has no vegetation or topsoil. Springs still emerge from rock.
Chunks and terrain LODs use the same sediment layers.

Generator version 19 follows saved river routes through surveyed lakes.
The inlet ends at the lake and an outgoing reach starts at the last wet
tile on the far shore. Each lake can supply one generated outlet. The
outlet starts at the lake's water level and must reach a downstream body
or channel within the existing cut and grade limits. A lake-fed reach
draws from the lake and has no spring source.

Generator version 20 checks low coastal basins for face-connected paths
to surveyed seawater. A connected basin fills to sea level across region
boundaries, and the same footprint supplies chunks and terrain LODs.
The survey uses the natural terrain before river cuts.

Generator version 21 allows a map region to contain both a coastal shelf
and an inland lake. Seawater follows submerged faces across the region
border. The lake keeps its own level up to the point where it can spill
into the surveyed sea.

The spring record supplies a bounded per-step input for the fluid solver.
Water introduced by edits can render exposed faces. Opening new aquifer
outlets, redirecting flow, and transferring water over runtime drops
depend on the fluid solver in steps 4–5.

See [the river flyover](../../tests/README.md#device-targets-gpu-and-display)
for the graphics acceptance check and the CPU patch benchmark.

## 4. Drain and refill standing bodies with lazy reconciliation

Store the body volume in wide fixed-point units sized for large maps and
convert it to a level through an elevation-to-storage curve built from
connected area by height. A fixed surface area can serve as a constant-area
fixture. Budget approximation error in production curves, especially near
shallow beaches. Track stored quantity even when its visible level has yet
to move a whole tile. Expose a signed volume transfer operation to both
gameplay and tests. Charge an external removal once. A transfer into a
cavern or isolated pocket debits the reservoir and credits the receiving
local cells in the same operation. Treat explicit weather input, disposal,
or evaporation as recorded sources and sinks.

Credit river deliveries to their receiving bodies. Track water held
upstream or rerouted by a dam, and credit it only when it reaches a sink.
Closing an inlet lowers the receiving body's level when its losses exceed
the remaining inflow. Model a simple background loss in the basin budget
so this can happen before weather exists. Use the storage curve to make
the same missing inflow matter sooner in a lake or small inland sea than
in an open ocean. If the overflow reaches the original basin again, its
inflow recovers and the effect can be temporary.

Save the runtime body state and versioned chunk water provenance. A body
revision is enough for scheduling, while reconciliation needs to know
which cells are generated reservoir water, which are locally stored, and
which are isolated or deliberately edited. Prefer a sparse chunk side
record or reconstruction from the generated baseline plus saved overrides.
Use one reconciliation path for newly generated, restored and affected
ready chunks. Apply rising and falling levels to eligible connected
cells, preserving solids and independent liquids. Check solid edits and
structure stamping before committing changed water. Refresh affected light,
surface bands, proxy meshes, neighbors, culling and camera queries through
one water change path. Do not turn projected reservoir changes into
terrain edits or count projection as a volume transfer.

Tie chunk metadata and the authoritative reservoir ledger to a recoverable
save generation or an idempotent transaction record. Restore the ledger
before reconciling saved chunks. A crash or retry between saves must be
safe in either write order. Use existing versioned save sections and
define how legacy chunks with ambiguous liquid provenance are preserved.

**Acceptance.** Run a small synthetic ocean through the real transfer
operation in CTest using two already generated shore chunks, a saved and
evicted chunk, and a never-generated chunk. Drain enough to cross two
known height thresholds directly through the API, with multiple small
withdrawals crossing the first threshold, then refill. Check the ledger,
changed wet footprint, detailed and coarse mesh heights, and the saved
and new chunk results. Include a newer revision arriving during a job,
a deliberately dry isolated edit, and restart between saves. Assert that
no distant chunks are streamed for this test. No manual ocean-draining
playthrough is needed.

Standing bodies use 255 integer units per block. An owner-thread transfer
returns the accepted signed amount and records its source or sink. A
transfer to a confirmed isolated cell commits the reservoir debit and the
cell credit in the same ledger. Visible standing levels follow whole storage
bands. Withdrawals within a band accumulate until the next level threshold.
Refills stop at the generated waterline, which is the surveyed spill level
for lakes.

Lake curves use the surveyed tile floors and the lowest face-connected path
to the basin seed. The ocean uses weighted region depths in the interior
and a 32-tile sampling grid in surveyed coastal regions. This keeps first-use
cost proportional to region count. The shoreline query checks a bounded
tile survey at the current level. Deep drawdowns can leave a connection
outside the survey unverified. The resulting ocean curve approximates shelf
and beach volume, especially in narrow coastal pockets.

The river budget advances in game seconds. Generated springs feed the reach
graph, confluences combine deliveries, and a full through-lake passes its
surplus to its generated outlet. Background losses match normal delivery
at terminal bodies and use half of normal delivery at lakes with an outlet.
Gameplay can report a reduced delivery rate or supply a confirmed alternate
outlet. Withheld water stays on the reach until it has an open outlet.
Step 5 supplies detection of edited channels, dam crests, and runtime
spill geometry.

The versioned `RSVR` section in `water.dat` is the authoritative record of
body storage, the budget step, reach holds, and local credits. The atomic
file replacement commits both sides of a diversion together. Chunk v5
records reservoir eligibility and omits projected water from the saved tile
palette. Chunk v1–v4 liquids and deliberately dry gaps retain their saved
state as their original ownership is ambiguous. Version 3 terrain edit
summaries carry the cap where a solid or deliberately dry voxel truncates a
projected column. A chunk published after a worker finishes uses the latest
ledger, and changes invalidate intersecting LOD and lighting work.

## 5. Add local finite-volume flow and edited connections

Use an active-cell budget at a fixed simulation step. Start with downward
transfer and leveling between faces, then add limited pressure only when
required for wells, pipes, and aquifers. Reuse the saved 0–255 level for
local fill with defined rounding and an explicit full-cell convention.
Account for any conversion remainder in the transfer ledger so repeated
small transfers retain their volume.
Drive surface height and side faces from that fill. Keep ownership and
ledger updates on the world owner and apply fluid changes through a
batched path sharing the editor's mesh, lighting, and persistence
notifications. Decide what happens to displaced liquid on block placement.
Use deterministic ordering or double-buffered flux commits so the same
transfer cannot be accepted twice.

At the active boundary freeze transfer where there is no evidenced
recipient. Persist pending local work or resume from saved cell ownership
when the neighbor loads. A confirmed reservoir portal can exchange volume
through the ledger without loading all connected voxels. Keep explicit
boundary inputs for open springs and downstream outflow and account for
their sinks or receiving bodies. Add player pump and pipe mechanics after
the transfer API works in tests.

Detect exposed aquifer-bearing rock through the terrain edit path. Register
an outlet when the exposed face can feed surface flow, and close it when
the player seals the opening. Key outlets to stable rock sites and save
their state so opening another face or reloading a chunk cannot duplicate
the source. Route the new flow over the existing terrain, including any
channel the player has dug. Water can pool or spill when the ground blocks
its route. Exposing a spring leaves the bed at its existing elevation.
Promote a continuing surface route to a runtime reach so it renders beyond
the active chunks through the same LOD path as generated rivers.

For the first interactive release, support a dam across a reach in a
bounded area where the downstream and spill routes can be verified.
Blocking the reach reduces or stops downstream delivery. Incoming water
fills the upstream pool and spills through the lowest reachable outlet
once it reaches that height, including over the dam if its crest is the
lowest outlet. The spill can feed the old reach or another sink. Publish
the pool and the changed downstream footprint through the shared sampler.

For dams and canals, update connectivity only around modified passages.
Combine a bounded 3D search with certified connections to the coarse
graph. Running out of the loaded area or the work budget yields unknown
connectivity. Keep the current ownership and defer a split or merge until
there is enough evidence, potentially using a saved portal summary.
An enclosed pocket gets an independent finite store, including its
initial volume. A canal connects at its actual sill and exchanges water
while the heads differ. Recalculate connected storage at a split and
preserve the total volume across a split or join. Start with small
fully verifiable edits. Arbitrary world-scale 3D splits remain a later
capability and require evidence beyond the region heightmap.

**Acceptance.** In headless fixtures test downhill flow, conservation at
a chunk border, placement displacement, isolated-pocket draining,
reconnection at a sill, and an unknown outlet in an unloaded neighbor.
Schedule load, unload and reload around a transfer. Check volume, cell
ownership, and stable mesh handoff after the change.

Add a spring-to-sea CTest fixture with an upstream pool and a known spill
height. Expose aquifer rock and verify that the new downstream reach
follows the existing bed without changing terrain heights. Then dam it
and verify that downstream delivery stops while the pool fills.
Check that overflow resumes or redirects delivery at the expected height,
and that overflow falling over the dam reaches the landing without
duplicating water. Check that closing the spring or reloading cannot
duplicate the source. Apply the same interrupted inflow and ongoing losses
to a small basin and an ocean
fixture. Assert the expected level change for each storage curve and
verify the updated footprints in detailed and distant meshes.

## 6. Publish flood reach for weather and unloaded inhabitants

Give weather dated, replay-safe inputs for basin recharge and loss,
runoff into reaches, and temporary coastal forcing. Coastal surge can
change effective local head and reach without adding permanent ocean storage.
Keep rainfall and runoff volume in the ledger when those processes are
implemented. Compute inundation from the water source, terrain and
known barrier or spill heights. Low ground alone is insufficient.
Track exposure by location, water depth, duration, and revision. Publish
regional flood envelopes plus a point or route query with an uncertainty
state for unresolved barriers.

The current gameplay state has no unloaded settlement evacuation system.
Give the future population simulation a sparse spatial index of settlement
sites, destinations and persistent actor or group locations. On flood
changes enqueue affected records and evaluate passability and dry refuge
against the same inundation query. Advance travel and relocation through
saved world time, with an event or revision cursor so loading an actor
cannot replay relocation. Persist the destination or an unresolved
outcome if no known route is dry. A flood that recedes before an actor
loads may still have affected it, so retain dated exposure or flood events
until their consumers have processed them. Detailed NPCs can later use
the same hazard query in their local navigation.

**Acceptance.** Raise a synthetic sea in a world with an inland bowl, a
dry levee, two settlement sites and a refuge. Keep all their chunks
unloaded. Assert that only the connected site floods, its inhabitants
relocate once via a passable route, and a reload or receding flood preserves
the outcome. Include a blocked-route case and compare rendered shoreline
extent against the flood query.

## Verification and open decisions

Put headless cases in the existing `world_logic_cpu` and
`world_logic_tests` targets and tag them `water` or `lod` as appropriate.
Run the existing LOD suite with `ctest --test-dir build -L lod
--output-on-failure`. Add focused benchmark cases for patch builds,
shoreline rebuilds and the active-cell budget. Record sampled columns,
mesh size, uploads and queue latency at each LOD preset. This lets a small
test world exercise the expensive transitions directly.

Liquids currently have hardcoded IDs for water and lava in `LiquidType`.
Only water has the generated and meshed surface path described here.
Water and lava can remain built-in in the long term. Custom liquids could
later use a data registry, analogous to blocks, trees, and flora, with
stable saved IDs and material and behavior parameters. Keep ownership,
mesh generation, opacity and the codec ready for that extension without
depending on it for the first river. How custom liquids enter world
generation remains open, including the rules for placing sources, forming
basins and mixing with other liquids. Pressure, ice, and phase changes
also remain later gameplay decisions.
