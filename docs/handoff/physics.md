# Handoff: physics (Jolt)

Status: **planned, not started.** Nothing in the code depends on a physics
engine yet. Pick this up when the first feature needs it (likely the
character slice or tree felling).
Engine chosen by the maintainer on 2026-11-19. Related: `trees.md`
(cutting, felling), `structures.md` (living structures, overrides),
`sky-light.md` (edits must keep the sky heightmap in sync).

## Decision (2026-11-19)

**Jolt Physics** (<https://github.com/jrouwe/JoltPhysics>, MIT) is the
physics engine. Alternatives considered:

| Engine | Why not |
| --- | --- |
| PhysX 5 (BSD-3) | Builds through its own scripts and the packman downloader, not `add_subdirectory`; Linux presets are clang-oriented. Its GPU path needs CUDA, so AMD and Intel users get nothing on a low-end target. Heavy setup API. No double precision. |
| Bullet3 (zlib) | In maintenance mode, documentation is from about 2015 and focused on pybullet, solver is mostly single-threaded. |
| Box3D (MIT) | Alpha (v0.1.0, June 2026), pull requests closed, manual in progress. No ragdoll or skeleton support, and no ball or swing-twist joint at the time of review. Re-evaluate around its 1.0, especially if it adds custom shapes. |

Jolt was chosen for one-line CMake integration, good docs and heavily
commented headers (which suit agent-written code), multithreaded CPU
performance, a custom shape API (voxel collision), ragdolls, a mature
character controller, double precision and an MIT license.

## Principles

1. **Physics never owns the world.** Voxels, trees and structures stay in
   their current systems (`ChunkManager`, `TerrainEdits`, tree records and
   structure overrides). Physics holds disposable collision copies and
   temporary dynamic bodies, and reports events. Every lasting change, such as
   a broken block, a dent or a settled log, is written back through the
   normal edit path so saves, meshing, lighting and the sky heightmap stay
   correct.
2. **Physics is behind a thin game-side interface** (`src/game/physics/`,
   its own CMake target). Bodies, shape building, queries and contact
   events go through it, and no Jolt header leaks into other modules.
   Swapping engines later should cost only that layer.
3. **The engine does not do voxel destruction.** Craters, dents and broken
   blocks are game code driven by impact data the engine reports.
4. **Collision exists only where it is needed:** around active dynamic
   bodies and characters. Do not add every loaded chunk to the physics
   world.
5. **No physics in unloaded or distant areas.** Out there, approximate:
   ballistic fall, then a stamped result (see Felling).
6. **Fixed timestep.** Physics steps at a fixed rate (start with 60 Hz,
   accumulator in the gameplay update). The day/night clock and time
   abilities do not change the step: time abilities are perception
   (IDEA.md, *World time, reaction speed, and perception*). If a real
   world-time scale is ever added, it scales the accumulator input, not the
   step length.
7. **Gameplay rules stay out of the engine.** Damage, energy cost and
   what breaks are game rules computed from engine data (contact points,
   relative velocity, impulse, mass). Keep them headless-testable.

## Integration

### Vendoring and CMake

- Vendor a release as `external/JoltPhysics-<version>` (5.6.0 was current
  at the time of the decision). Add it to the dependency list in `README.md`.
- In the root `CMakeLists.txt`, next to the other dependencies:

  ```cmake
  set(USE_AVX2 OFF CACHE BOOL "" FORCE)    # low-end baseline, see below
  set(USE_AVX512 OFF CACHE BOOL "" FORCE)
  set(USE_LZCNT OFF CACHE BOOL "" FORCE)
  set(USE_TZCNT OFF CACHE BOOL "" FORCE)
  set(USE_F16C OFF CACHE BOOL "" FORCE)
  set(USE_FMADD OFF CACHE BOOL "" FORCE)
  set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
  set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
  set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
  set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
  set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
  set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_SOURCE_DIR}/external/JoltPhysics-<version>/Build)
  ```

  Check the option names against the vendored `Build/CMakeLists.txt`.
  They change between releases.
- **CPU baseline:** Jolt picks SIMD at compile time and defaults to AVX2.
  Low-end hardware is a pillar, so build with SSE4.2 (all `USE_*` above
  off) unless profiling on target hardware shows a real loss.
- **Matching defines:** the game and the Jolt library must agree on
  every `JPH_*` define (`JPH_DOUBLE_PRECISION`, `JPH_ENABLE_ASSERTS`,
  `JPH_FLOATING_POINT_EXCEPTIONS_ENABLED`, `JPH_PROFILE_ENABLED`,
  `JPH_DEBUG_RENDERER`, `JPH_OBJECT_STREAM`). A mismatch compiles fine and
  crashes at runtime. Link the `Jolt` target, which exports them, and never
  add them by hand to only one side.
- Link `Jolt` privately from the physics target only.
- Jolt's profiler macros can forward to Tracy. Hook them up when profiling
  physics (see the Tracy skill).

### Startup and threading

- Call `RegisterDefaultAllocator`, create `Factory::sInstance`, then
  `RegisterTypes()` once at startup. Tear down in reverse order after the
  last `PhysicsSystem` is destroyed.
- **Job system:** Jolt needs a `JobSystem`. Do not start
  `JobSystemThreadPool` beside the existing BS::thread_pool workers: two
  pools fight over cores. Implement a `JobSystemWithBarrier` adapter that
  submits to the shared pool, or give physics a fixed share of threads.
  Measure this. Chunk generation jobs must not starve the physics step.
  See the BS::thread_pool skill.
- `PhysicsSystem::Update` runs on the main thread once per fixed step.
  Contact callbacks run on worker threads: they only record events into a
  thread-safe buffer, and the game handles them after the step.

### Coordinates

- `DOUBLE_PRECISION ON` from the start. The world spans the underworld
  (below Y 0), the overworld (0–1024) and sky islands at about Y 2048, and is
  wide horizontally. Use `RVec3` for positions everywhere in the interface.
  Never assume a single 0-based Y range (IDEA.md, *Vertical layers*).
- One unit is one tile (one metre).

### Layers and filtering

Start with these object layers: `Static` (voxel collision, settled
structures), `Moving` (falling wood, debris, loose objects), `Character`
(kinematic characters), `Ragdoll` (limb bodies), `Sensor` (auras, triggers).
Map them to broad-phase layers `NonMoving`, `Moving` and `Sensor`. Ragdoll
limbs of one body ignore each other via a group filter.

## Voxel collision

The hardest part of the integration, and the first thing to prototype.

- **Prefer a custom shape** that reads solid tiles directly from chunk data
  (Jolt `Shape` subclass plus registered collide and cast functions). It
  needs no meshing, has no build cost after an edit, and has no ghost
  edges between tiles. Reference: Jolt's `Samples` has custom shape examples;
  check the vendored version.
- **Fallback:** one `StaticCompoundShape` of merged boxes per chunk
  (greedy box merging over solid tiles). Rebuild it on a worker thread after
  an edit and swap it in on the main thread. Do not use triangle meshes for
  terrain: they are slower to build and produce internal-edge contacts.
- **Which chunks get collision:** those within a small radius (start
  with 2 chunks) of every awake dynamic body, character and ragdoll. Add
  and remove them in batches (`BodyInterface::AddBodiesPrepare` and
  `AddBodiesFinalize`) as chunks stream.
- **Thread safety:** a custom shape reading live chunk data must never
  see a chunk that is being rewritten or unloaded during a physics step.
  Either pin chunk storage for the step or give the shape a read-only
  snapshot. Decide this with the chunk job owners (see `src/game/AGENTS.md`).
- **Leaves and thin twigs are not solid** (trees.md): they get no
  collision, or a sensor at most (for slowing or rustling).
- **Materials:** friction, restitution and hardness come from block
  definitions (`assets/data/blocks/`), looked up per contact through a
  sub-shape ID or a material table.

## Scenarios

### 1. Tree and branch felling

Replaces today's rule "cutting removes everything above the cut"
(trees.md, *Cutting*). IDEA.md: "Once physics exists, a cut tree should
fall instead."

1. A cut separates a piece of wood (trunk part or limb) from the tree
   skeleton. The tree system already knows which voxels are cut off.
2. Remove those voxels from the world through the normal edit path and
   build one dynamic body from them:
   - shape: a compound of boxes greedy-merged from the cut voxels (exact
     enough and cheap), or a convex hull for small limbs;
   - mass and inertia: from voxel count times wood density per species
     (species data), set with `MassProperties`, not Jolt's default density;
   - initial motion: zero, or a small push away from the cut side.
3. Render it as the cut voxels' mesh following the body's transform. It
   should use the same wood material as the tree renderer.
4. **Impact:** on contact, read the relative normal velocity and the
   contact points, compute impact energy (`0.5 m v²` along the normal) and
   pass it to the voxel damage rules. They break or dent blocks whose
   hardness is below the local energy and can damage structures and houses.
   The body loses the spent energy (reduce velocity) so it doesn't plough
   endlessly.
5. **Settling:** when the body sleeps, or after a timeout, turn it back
   into voxels at its final pose: rasterise the rotated voxel set into the
   grid, write it through terrain edits, mark it as fallen dead wood (it
   stays, per IDEA.md *Death leaves the wood*), and remove the body.
6. **Huge pieces** (world trees, trunks up to 128 m wide): mass ratios
   against ordinary bodies break the solver, and the impact radius is
   kilometre-scale. Use a scripted fall instead: rotate about the hinge at the
   cut, sweep the shape to find the ground contact, stamp a precomputed
   crater and settle. Only small debris gets real bodies.
7. **Unloaded destination:** if the fall reaches chunks that are not
   loaded, run the same analytic fall (hinge rotation or ballistic) and
   store the result as a pending edit for those chunks, applied when they
   load. This matches how tree cuts already settle for chunks loaded later.

Open: species-specific fall behaviour (splitting, branches breaking off
mid-fall), and whether settled wood keeps its tree identity for regrowth.

### 2. Aura bursts (energy release around the user)

- Find everything in range with a sphere overlap query
  (`NarrowPhaseQuery::CollideShape` or the broad phase `CollideSphere`).
- Push dynamic bodies, characters and ragdolls outward with
  `AddImpulse`, scaled by distance falloff and the target's mass and
  defense stats (game rules).
- Voxel damage is game code: a crater or ring of broken blocks from the
  burst energy against block hardness, written through terrain edits.
  Optionally spawn a few debris bodies for show.
- A continuous aura (IDEA.md levels 100+) is a `Sensor` body following the
  character. Its enter and leave events drive buffs and pressure effects.

### 3. Explosives

- The crater is voxel code (energy against hardness, line of sight from
  the centre), not physics.
- Physics adds knockback impulses (as with an aura burst, with an impulse
  falling off with distance and occlusion via ray casts) and short-lived
  debris bodies built from some removed voxels. Cap the debris count, and
  turn debris that sleeps back into loose items or blocks.

### 4. Combat and limb hits

- Every combat-capable body has a ragdoll (Jolt `RagdollSettings`) with one
  body per limb and swing-twist and hinge constraints. While the actor is
  alive it is **driven by animation** (keyframed or motor-driven to match the
  animation pose). On death or a strong hit it goes limp or blends to
  physics. Use `SkeletonMapper` to map between the animation skeleton
  (`src/engine/animation/`) and the ragdoll.
- **Weapon hits:** sweep the weapon shape between the previous and the
  current pose every step (`NarrowPhaseQuery::CastShape`). Do not use
  per-frame overlap: very fast actors (IDEA.md: reactions in fractions of a
  millisecond, fast strikes) will tunnel through. The hit gives the limb,
  contact point and normal. Damage comes from the blade's velocity at the
  contact point (relative to the target limb), the weapon's mass and the
  edge type: a game rule.
- Hit reactions: an impulse on the struck limb, then motors pull it back
  toward the animated pose.
- Projectiles (arrows, bullets) are ray or shape casts per step, or
  bodies with `EMotionQuality::LinearCast` when they should bounce or stick.

### 5. Characters and gravity

- Use `CharacterVirtual` (kinematic) for the player, NPCs and animals,
  not dynamic rigid bodies. It handles steps, slopes, ground detection
  and pushing dynamic bodies. Size the shape (capsule) from species
  anatomy.
- Gravity uses one world value, multiplied per character by species and
  ability factors (game data). Fall damage comes from vertical speed on
  landing, scaled by body stats.
- Voxel worlds need step-up for one-tile ledges (`ExtendedUpdate` with
  stair walking), tuned per species.
- Distant or unloaded NPCs do not use physics: the world simulation moves
  them abstractly (IDEA.md, *A living world with consequences*).

### 6. Flying and movement abilities

- Flight is game logic setting the `CharacterVirtual` velocity each step;
  the controller resolves collisions. Model wings as lift and drag
  (lift scales with speed and wing area, drag with speed squared), impulse jumps
  as instant velocity changes, and hovering as cancelled gravity with an
  energy or stamina cost. All of it is headless-testable without the engine.
- Short-range teleport (aspect example, IDEA.md) is a shape cast or
  overlap check at the destination before moving the character.
- The 1024–2048 gap to the celestial layer is crossed by these abilities
  (or creatures, or phenomena). The gap is unstored and empty, so there is
  nothing to collide with. Physics must not request chunks there.

### 7. Other later uses

- Loose items and dropped goods: small bodies that become static items
  once asleep.
- Carried cargo and falling cargo (IDEA.md, *Causal events*).
- Thin ice breaking under weight (IDEA.md, *Temperature, fluids*): contact
  force from the character's mass against ice strength, as game code.
- Swimming and buoyancy: Jolt has a buoyancy helper for bodies. Characters
  need their own model. Depends on the fluid model, which is undecided.
- Doors, rope bridges, hanging lamps: constraints between bodies.

## Suggested order

1. Vendor Jolt, add the physics target, start up and shut down with an empty
   `PhysicsSystem`. Build check on Windows and Linux.
2. Voxel collision prototype (custom shape against box compound) with one
   falling box. Measure build time after edits and step time. Headless test:
   a box dropped on flat terrain rests at the right height.
3. Character controller on voxel terrain (with stair walking).
4. Tree felling for small trees and limbs (impact energy, settling back to
   voxels), then the scripted fall for huge trees.
5. Aura and explosive impulses.
6. Ragdolls and weapon sweeps, together with the combat prototype.

## Testing

- Physics tests are headless (Jolt needs no device) and belong in
  `tests/unit/game/physics/`. Use deterministic setups: fixed step, fixed
  job count or single-threaded, fixed seeds.
- Priorities: resting height on voxels; no tunnelling for fast sweeps;
  edits that remove collision under a sleeping body wake it up; settling
  writes the expected voxels; impact energy maps to the expected broken
  blocks; save and load mid-fall (decide: settle on save, or persist the
  bodies).
- Debug drawing (`JPH_DEBUG_RENDERER`) only in debug builds, through a
  bgfx debug line pass.

## Open questions

- Persist in-flight bodies in saves, or force-settle them on save?
- Custom voxel shape or box compounds: decide after step 2 measurements.
- Thread split between chunk jobs and physics jobs.
- Exact impact-to-damage rules and block hardness data.
- Whether structures (houses) collapse when their support breaks
  (structural integrity is a separate system that would feed physics).
