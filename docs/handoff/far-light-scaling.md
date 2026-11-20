# Handoff: scaling far lights to many lighthouses

The shadow requirement below is implemented. Revisit the remaining work when
worlds start holding many far-reaching lights, such as generated settlements
with lighthouses.

## Target scenario

A large map with many settlements, many of them with their own lighthouse
(e.g. radius 512). They are placed carefully, so no more than about 4 lie
within 512 tiles of each other, but the world holds a lot of them (around
100 or more).

## How the current system selects far lights

Far light = `LightEmission::isFar()` (radius > 15). Each frame:

1. `visibleFarLights` (`src/game/world/light_sources.cpp`) scans **the whole
   registry**, keeps unsealed far lights within the **view distance**
   (`camera.viewDistance`: 4096 / 6144 / 8192 for Low / Medium / High), and
   ranks them by `max(0, distance - radius)`: lights whose radius covers the
   camera first, then by how close their light comes to it.
2. `LightShadows::assign` gives cube shadow maps to the first
   `Budgets[quality].lights` (**4 / 8 / 16**) of that ranking; a light keeps
   its slot while it ranks within 1.5 x the budget (`Keep`).
3. `LightRenderer::fillLightUniforms` sends only lights with ready maps to
   `u_lights`. Voxel terrain, distant terrain and foliage evaluate those
   lights per pixel. Lights waiting for a map or outside the budget still
   contribute to the voxel near field (up to 15 tiles).
4. Glare (`LightRenderer::drawGlare`) is separate: every unsealed light
   within the view distance, nearest first, up to `MaxGlares` (4096), one
   instanced draw.

## How the scenario fares

- **Near the player: correct.** At most ~4 lighthouses can cover any point,
  so the ones lighting the player's position always rank first and fit even
  the Low shadow budget (4). Medium and High also shadow the lighthouses
  lighting the terrain a few hundred tiles around.
- **The ranking only looks at the camera position.** It does not know whether
  a light's area is on screen: a lighthouse behind the player, or one whose
  lit area is hidden behind a ridge, can take a shadow slot.

## Problems to expect

1. **Only lights with shadow slots illuminate distant surfaces.** With an
   8 km view, many lighthouses can be in range while only 16 are shadowed.
   Distant lit patches can pop in and out as the ranking changes.
2. **The per-frame scan touches every registered light,** including small
   torches (`registry.forEach` in `visibleFarLights`). Fine for hundreds or
   a few thousand; with tens of thousands of torches it becomes measurable
   frame time.

## Proposed changes (in order of value)

1. **Approximate shadows for lights outside the atlas budget.** A terrain
   heightmap test or a per-light horizon map could let exposed lighthouses
   light nearby terrain while keeping hills in shadow.
2. **Rank by visible contribution.** Skip lights whose sphere is outside the
   view frustum, and weight the rest by how much on-screen area they light
   (e.g. projected sphere size times attenuation at its nearest visible
   point). Shadow slots and the 32 per-pixel entries then go to lights the
   player actually sees.
3. **Separate far-light index.** Keep far lights in their own small list (or
   buckets by radius class) inside `LightSourceRegistry`, so the per-frame
   selection never iterates over torches.
4. **Only if still needed:** raise `MaxLights` (keep
   `SURFACE_MAX_FAR_LIGHTS` in `world_surface.sh` in sync; `u_lights` is a
   vec4 array of 4 entries per light) or move to clustered/tiled light lists
   if dense regions exceed it.

## How to verify

- Extend `lod_render_smoke lights` (see `tests/LOD.md`) with a phase that
  scatters ~100 entity lights of radius 512 over the scene with the 4-per-512
  spacing rule, on High (`SMOKE_QUALITY=2`); report far lights in range,
  per-pixel count, shadowed count, and per-frame CPU time of the selection.
- Visual check from a ridge at night: no lit patches on slopes facing away
  from distant lighthouses; no popping while flying across the map.
- Unit tests for the ranking: a light behind the camera ranks below a
  visible one at the same distance; the far-light index returns the same set
  as a full scan.

## Related

- Lighting overview: `src/game/AGENTS.md` (Lighting).
- Shadow budgets and atlas: `src/game/world/light_shadows.hpp`.
- Gameplay light queries (separate, planned after day/night):
  `docs/handoff/light-query.md`.
- Lore limits on great lights: `IDEA.md` ("Light, night, and great beacons").
