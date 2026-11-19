# Handoff: tree roots and forest renewal

Status: stages 1 and 2 implemented. This updates the death, seeding and
decay direction in [the tree handoff](trees.md), whose Progress section
records what already works. Each stage has its own verification. Roots and
wood materials can land independently of forest renewal. The loaded mesh
simulation provides the reference for offscreen work and voxel renewal.

## 1. Protect elders and record a performance baseline

| Trees | Natural lifecycle | Cutting and planting |
| --- | --- | --- |
| Mesh trees and bounded voxel trees, including redwood | Mature trees can seed nearby sites. They can die and decay. Redwood renewal ships within a measured work budget. | Keep pruning and regrowth from a viable stump as separate events from seeding. Players can plant them. |
| Elders and trees above the autonomous size limit | Immortal and sterile at every age, including as saplings. The primordial beings planted the original elders at world creation. | Keep branch cuts and the elder's trunk settling. Removing its entire base never triggers whole-tree deletion. A player can establish one with a seed and very rare items carrying special energy. |

Use the maximum mature footprint and estimated occupied wood and root slabs
to decide eligibility. Include vertical extent and the cost of applying a
delta, including lighting and mesh work. The existing trunk-radius landmark
class automatically excludes elders, and a broad or deep modded species can
exceed the work limit independently. Establish the numerical limit with the
redwood benchmark. If redwood exceeds the first budget, make its work more
incremental before enabling autonomous renewal. The rule applies to the
species at maturity, so planting an elder sapling or cutting its crown cannot
change its life policy.

Use one lifecycle rule for growth age, foliage withering, billboards, cutting
and support loss. The current lifespan clamp, root-cut death flag and
last-support removal are distinct code paths. Test all of them for elders.
Leave the resulting player cuts and excavations in place. The absence of an
automatic death event keeps even very large trees outside the decay queue.

**Verify.** Advance an elder past its former lifespan, cut a limb, sever
the trunk and remove the entire base. Check nearby trees, billboards and
reloads. Record frame-time tails and region crossing stalls for the
[elder approach and cut scenarios](../../tests/README.md#device-targets-gpu-and-display)
before changing voxel writes.

**Implementation.** The existing mature trunk-radius landmark class (15.5 m
and up) is immortal. Elder branch cuts and trunk settling still work at any
age. The species rule also protects elder saplings and trees that lose their
entire base. Existing saves retain recorded removals. The separate workload
threshold for autonomous renewal is set when redwood decay can be measured
in stage 6.

**Performance baseline.** The original High elder approach run took 2,337
frames and crossed region borders on the walk from 1,200 m to 40 m. Frame
time was 26.95 ms at p99 and 36.74 ms at worst, with four frames over 33 ms.
Chunk update was 11.13 ms at p99 and 19.29 ms at worst. The run reported zero
wood gaps and zero black frames. The headless baseline measured 0.38 ms for
the sparse voxel plan and 1.30 s of worker work for the thick limb cut.

## 2. Grow roots through natural terrain

Start with configurable subsurface roots for elder and redwood. Other
species can opt in once they can form a one-block trunk. The flared trunk
continues into a shallow base of the same width. Tapered roots spread
sideways from that base and fork as the tree matures. The species controls
depth and horizontal reach. Include the subsurface shape in tree bounds,
slab occupancy and chunk generation, using
canonical X and the tree's nearest image when it crosses the wrap seam.

Root growth replaces eligible natural soil and rock. It respects open caves,
water, protected blocks, existing wood and player edits, including excavation
and same-material replacements wherever those edits would otherwise be
indistinguishable from generated terrain. Constrain or skip branches when
they encounter those obstacles. A solid replacement shares the normal
editor, save and mesh paths. Account for roots in tree support checks so
digging around a living redwood has the same effect before and after reload.
Use the existing ground-support rule as the starting point and test what
happens when roots are exposed or severed.

Record or reconstruct the displaced substrate for each owned root voxel.
Restoration should recover the original soil or rock and its relevant
terrain data. Prototype a deterministic substrate lookup against the same
generation version, with sparse per-slab provenance where reconstruction is
ambiguous. Validate this on both fresh and saved chunks before turning on
root replacement. Root wood counts as earth where the edit tracker compares
against generated substrate, so replacing natural rock does not create a
player structure entry. Exposed roots still need the usual mesh and light
updates. Give existing chunks a targeted migration before enabling roots
in old worlds. Preserve legacy geometry, unrelated growth and player
construction.

Start with shallow roots that spread through dry ground. A later refinement can
bias growth toward the existing coarse aquifer saturation data. Sample it
from bounded, immutable terrain data and use a deterministic fallback when
a suitable layer is out of reach. This can precede fluid simulation and
works without loading chunks to find water.

**Verify.** Check root placement and support after excavation, cave
intersections, reloads, region borders and the X seam. Check that exposed
roots remesh and relight, and that the terrain edit tracker leaves
natural root growth out of player structure entries.

**Implementation.** Each tree has its own deterministic root skeleton.
Elder uses up to 16 m depth, 150 m horizontal reach and nine forked branches.
Redwood uses up to 4 m depth, 26 m reach and seven branches. Chunk generation and
the loaded growth sweep use the same bounded, age-dependent raster and
skip ineligible cells. Root cells in the trunk's contact layer support
their owner. Excavating below or around that layer can expose roots
without cutting the crown.

Chunk encoding v4 stores the displaced block and ground cover with the
tree's identity, plus terrain edits, including explicit same-material
replacements. Loading a v3 slab restores substrate owned by the deeper
prototype roots before growing the new shape. Previously saved v1/v2 slabs
retain their original terrain because they have no per-cell edit history.
Newly generated slabs in those
worlds can grow roots. The above-ground tree stamp remains compatible and
root branches use independent indices. Buried roots participate in the
chunk path. The distant wood proxy continues to show above-ground wood.

## 3. Show inner wood on cut faces

Use two living voxel blocks where the wood is thick enough: bark on the
outside and light wood inside. Start with a shell three blocks thick for
elder and one for redwood. Classify cells against the age-dependent,
uncut exterior of the local wood shape. A freshly exposed face should show
the interior that was already there. Handle junctions using the merged wood
shape so overlapping branches do not leave a bark seam inside the trunk.
If growth covers old bark, convert the now-interior voxel only while it
still belongs to the living tree. Keep holes from cuts and excavation.

Mesh-only species need foliage bark material for their twig meshes and can
omit both voxel blocks. Keep the existing inclusive voxel threshold of
radius 0.5 for a one-block trunk, which needs the outer block. Require an
inner block when the maximum trunk radius exceeds 1.0. Shell thickness
determines whether a particular age and cross-section have a core. Validate
both block references as living wood, retain existing numeric IDs, and
teach ownership, support, cutting, daylight, decay and distant wood
occupancy to handle both blocks. Give legacy trunks a compatibility path
for exposed interior wood before changing their stamps.

**Verify.** Saw through elder and redwood at different ages. Check mixed
bark and core for support and cut ownership, including branch joints.
Reload a legacy trunk and confirm that growth preserves player holes.

## 4. Renew mesh forests in loaded regions

Reuse flora's approach of deterministic seed attempts and local crowding.
On a coarse cadence, a mature, living, eligible parent gets a limited number
of seed attempts within its dispersal radius. Evaluate soil, climate, slope,
space, available headroom, existing trees and stumps, and a local population
cap. Reject seeds that land across the Z boundary and wrap X. Consider
compatible neighboring species in spacing checks and tune establishment to
produce renewal and occasional spread under available space.

Use a stable event key derived from world seed, parent identity, epoch and
attempt index. Give each accepted tree an identity that includes its birth
event, so replanting at an old anchor cannot collide with retained cleanup.
The region containing a candidate's anchor owns its acceptance. Resolve
cross-border conflicts with a fixed order and a bounded neighboring-region
snapshot, so job order and duplicate attempts cannot overfill a site.
Persist accepted births and death dates. A tree gets a fixed anchor, shape
seed and birth day when it establishes. Keep those values across region
visits and renderer cache refreshes.

Derive a slow ecological epoch from the saved `WorldClock`. Tree growth
continues to use age in game days. Assign forests a due epoch by anchor region,
then run a bounded queue for loaded regions. Use region cursors and bounded
catch-up batches so a long time jump does not
enqueue a task for every elapsed day or force a global scan in one frame.

Start with region-keyed population records and spatial buckets for site
queries. `TreeSet`, `TreeColumn` and `TreeOverrides` already serve generation
and rendering. Add region-local immutable snapshots and revisions for the
changing population so accepting a seed only updates the affected region
records. Consider EnTT if profiling shows benefits from component
queries shared with other gameplay systems. Persistent keys and region
ownership should work with either representation.

Workers read immutable inputs and return proposed changes. The owning update
validates the world and region revisions before accepting results, including
the revisions of neighboring sites that affect conflicts. Bound in-flight
jobs, tile writes, lighting invalidations and remesh work as well as the
time spent publishing updates. Prioritize chunk streaming and cancel or
discard stale work on unload and world exit.

Keep accepted lifecycle events in versioned persistent state alongside tree
overrides in `structures.dat` or in region records indexed by anchor.
Carry old cuts, anchors and generation versions through migration.

Feed detailed trees, distant meshes and billboards from the same
committed lifecycle state. A standing
tree keeps its anchor in every representation. Treat a genuine birth or
death as an age-driven appearance or fade; avoid rethinning the whole
billboard list when one tree changes. Invalidate the affected region and
crossing footprints, reject stale jobs, and keep the current drawable
representation until its compatible replacement is ready.

**Verify.** Check local crowding, cross-border conflicts, reload, and
fast-forward. Compare the same committed trees across the billboard and
detailed-mesh handover. Births and deaths should appear at their recorded
sites and ages.

## 5. Advance mesh forests offscreen

Add middle-distance updates first and compare them with the loaded
reference. Add very-far summaries once middle distance meets the same
checks. Prioritize approaching and visible forests while giving background
regions a share of the queue to prevent starvation.

| Tier | General world simulation | Trees |
| --- | --- | --- |
| Loaded chunks | Physics, effects, detailed AI, precise local light | Use live blocks, edits, support and local site checks. Apply bounded voxel and light changes. |
| Middle distance | No physics, fewer effects, simplified AI and approximate light | Use terrain columns, saved edits and occupancy summaries to evaluate the same seed and death rules. Existing sky-occlusion and distant light maps provide a starting point where light is relevant. |
| Very far | No effects. Aggregate state, such as an NPC's approximate location and current task | Advance sparse regional counts and age or seed-pressure cohorts when due. Materialize candidate events deterministically from that state as a forest approaches visible range. Preserve accepted births, persistent sites and known edits across tier changes. |

The tiers should produce roughly equivalent outcomes over the same time.
Share event dates, seed generation, site constraints and conflict rules where
the inputs allow it. Limit approximation to unresolved outcomes and inputs
that are expensive offscreen. Once a tree's site has been committed, retain
it across all tiers. Promote a region early enough to prepare a stable visible
snapshot. Near, middle and very-far thresholds can be tuned independently
from rendering distance.

**Verify.** Run paired A/B worlds from the same seed, initial state and
scripted edits.
Keep one region loaded throughout in A. In B, leave it in each unloaded tier
in separate runs, then promote and reload at the same game day. Compare across
many seeds and biomes at 10-year and multigeneration checkpoints. Record
survival, births and deaths by species, stems per hectare, canopy cover,
age distribution, patch spread and time to recover a clearing. Establish
a tolerance for each metric from loaded reference runs and playtests before
accepting the approximation. Rework a tier that exceeds its agreed
tolerances. Committed anchors, player edits, landmark sterility and
cross-border agreement require exact checks. Revisit a forest after ten
game years and check the approach from billboards to detailed trees.

## 6. Enable voxel renewal and decay for redwood

Apply the established renewal rules to redwood and other eligible voxel
trees. Use the stage 1 baseline to set a work budget, then measure mature
redwood wood and root workloads before enabling renewal.

Record a death day and retain tree identity, its last living shape and the
bounds required to reconcile its wood. Losing support and dying of old age
use the same durable lifecycle record. A viable resprout remains alive, and
a branch cut retains the existing cut-specific behavior.

On death, stop adding wood and stop seed production. Wither the canopy and
remove above-ground wood from the top down over a comparatively short
interval, leaving a stump. Then decay roots more slowly from their tips
inward. Replace a decaying root voxel with its recorded substrate only if the
tree still owns that voxel and it still holds the expected root wood. Holes,
foreign wood and player blocks retain their edits. Keep the stump until a
separate stump-removal rule is chosen, and let its remaining footprint
affect seedling placement.

Use deterministic decay stages by date. Queue only affected occupied slabs
and apply a bounded number of changes to loaded chunks per frame. Unloaded
chunks reconcile to the same stage with their saved tree stamp; a chunk
visited after a century can apply the difference directly. Pair the day
stamp with an applied event revision or make same-day replay safe. Keep cleanup
metadata while a saved chunk can still contain old wood, even after a tree
stops appearing in the render sets. In particular, the current last-support
path can skip the tree in generation and leave its wood in already saved,
unloaded chunks. Fix that case before enabling voxel-tree mortality. Retain
enough identity history to keep original generated trees from reappearing.

Key voxel work by affected slab. Save authoritative changes before
checkpointing the corresponding chunk stamps and make replay idempotent
after an interrupted save.

Feed the landmark wood proxy from the same committed state. For voxel trees,
stage crown growth and removal with the corresponding visible wood. Reuse
the existing overlap and dither band and the landmark proxy's retirement
behavior.

**Verify.** Compare local and reload decay for the same committed tree.
Stress a century of catch-up, many simultaneous deaths, interrupted saves
and rapid traversal. Use pre-aged stands and short-lifespan fixtures to
exercise multiple generations while keeping shipped lifespans intact.

Measure frame-time tails, region crossing stalls, peak queue depth, time to
catch up, and voxel, light and mesh work on the same hardware as the
baseline. Use the existing `trees` CTest label for deterministic logic and
save cases, plus device smoke tests for visible handovers. Compare p99 and
worst-frame times against the baseline before increasing the autonomous
size limit.

## 7. Let players establish new landmarks

Keep deterministic generated landmark sites rare. For player-grown landmarks,
validate mature bounds, wrapped spacing from existing and pending landmarks,
and room for the growing trunk before charging the items. Show the mature
footprint and growth time before committing. Reserve the site when planting
succeeds and make the charge and reservation safe across retry and reload.
The current debug planting path is a prototype. At landmark scale, player
planting needs a sparse bounds lookup so outlying chunks and distant wood can
discover an anchor several regions away. Reuse landmark streaming and
incremental growth. The source of the rare items and the energy recipe are
left for the progression design.

**Verify.** Check failed placement, retries, overlapping mature footprints
and recognition across regions at sapling and mature sizes. Update the
modding guide as the schema ships.
