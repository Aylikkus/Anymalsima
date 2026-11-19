# Handoff: gameplay light queries (`LightQuery`)

Status: **planned, not started.** Implement only after a real day/night cycle
exists: the sky part of a sample must follow the time of day, not the debug
SKY slider.

## Why

Gameplay needs to ask "how much light, and of what colour, reaches this
position?" for any place on the map, loaded or not. Examples:

- An NPC hates red light: if their room is lit red, they get angry, and they
  can name the lamp responsible.
- A strong light placed in our settlement irritates a nearby settlement
  (light pollution), so its people ask us to remove it. This must work while
  that settlement is unloaded and only simulated in the background.

## What exists today (and why tiles are not enough)

- `Tile::lightLevel.sun_exposure` and `artificial_light` (0–15) plus the
  per-chunk chroma side buffer (`BlockLightPropagator::chroma`) hold only the
  **voxel near field**: sky light, and artificial light within
  `LightEmission::NearRange` (15) blocks of each emitter. For far lights
  (radius > 15) this near field even fades out over 11–15 blocks, because the
  rest is handed to the GPU.
- The **far term** (radius up to 8192) exists only per pixel on the GPU
  (`shaders/world_surface.sh`, `surfaceFarLight`), occluded by cube shadow
  maps (`LightShadows`). Reading shadow maps back to the CPU is slow and
  frame-delayed: gameplay must not depend on them.
- Light is derived and only computed for **loaded** chunks. Unloaded areas
  have no light values at all.
- `LightSourceRegistry` (`src/game/world/light_sources.hpp`) indexes **every**
  emissive tile, small and far, in 128-tile buckets: position, block id,
  `LightEmission` (radius, falloff, intensity, color, cone), direction and
  `openFaces` (`sealed()`). It is persisted in `light_sources.dat` and
  reconciled when chunks load, so it is valid for unloaded areas. Entity
  lights (carried torches) are reserved in the same registry.

So a query cannot just read tiles; it evaluates lights from the registry on
the CPU, with the same formula the shaders use.

## Proposed API

A main-thread service, e.g. `src/game/world/light_query.{hpp,cpp}`, built on
the registry plus read access to `ChunkManager` (loaded voxels) and the
distant-terrain data (`TerrainEdits`, surface heights):

```cpp
enum class Occlusion {
    None,     // every light in range counts (area questions, cheapest)
    Voxels,   // straight-line check through loaded chunks (rooms)
    Terrain,  // terrain height + recorded edits (unloaded areas)
};
struct LightContribution {
    LightSourceRegistry::Id id;
    std::array<float, 3> rgb;
};
struct LightSample {
    std::array<float, 3> sky;         // sky light x current daylight
    std::array<float, 3> artificial;  // sum of contributions
    std::array<float, 3> total;       // what a surface there would receive
    std::vector<LightContribution> sources;  // strongest first
};
LightSample sample(std::array<double, 3> position, Occlusion mode) const;
// Mean over an area (settlement light pollution), sources merged.
LightSample sampleArea(std::array<double, 3> centre, double radius,
                       Occlusion mode) const;
```

Notes for the implementation:

- **Per-light term** must match `surfaceFarLight`: `intensity *
  (1 - d/radius)^falloff * color`, the cone factor
  (`LightEmission::coneFactor` with `LightSource::direction`), skip
  `sealed()` lights. Do **not** apply the near-field fade: the CPU sample has
  no voxel near field to hand over to. Use `LightEmission::attenuation` so
  CPU and data stay one definition.
- **Candidates**: `registry.forEachNear(x, z, maxRadius, periodX, ...)`.
  Far lights can have huge radii; far lights are rare, so either scan them
  separately (as `visibleFarLights` does) or bucket by radius class.
- **X wrapping**: use `WorldWrap::deltaf` for the light's copy nearest the
  query position, like `visibleFarLights` and `LodLightMap`.
- **Voxel occlusion**: reuse the Amanatides–Woo traversal in
  `block_light_propagator.cpp` (`lineOfSight`): opaque tiles block,
  translucent tiles add `opacity - 1` to the effective distance, edge
  crossings block only if every grazed cell blocks. Read tiles through
  `ChunkManager::withChunkLocked` / `tileAt` on the main thread. Skip the
  emitter's own tile. Fall back to `Terrain` occlusion where the ray leaves
  loaded chunks.
- **Terrain occlusion**: march the segment in 2D against ground heights
  (generated surface via `ChunkManager::generatedSurfaceY` /
  `SurfaceSampler`, overridden by `TerrainEdits` ground and structure
  voxels). Coarse is fine: it answers "is there a mountain in between?".
- **Sky part**: from the sky light service planned in `sky-light.md`
  (`skyAt`: the voxel value when loaded, the sky heightmap's rule
  otherwise, structures included), never from `Tile::lightLevel` directly.
  Scale by the day/night cycle's daylight (today
  `LightRenderer::setSkyDarkness`, debug only). Sun direction does not
  enter it: it only drives rendering (shadows, normal maps).
- **Colour**: return linear RGB; gameplay helpers such as "is it mostly
  red?" (hue/saturation of `artificial`) belong to the NPC side.
- **Caching**: results depend on the registry (`version()`), edited chunks
  and the time of day. NPC checks should run on events (entering a room,
  a light placed nearby) or every few seconds, not every frame; settlement
  checks on the background simulation tick. Consider caching per chunk
  column keyed by registry version.

## Cost

One sample = the lights whose radius reaches the position (usually a handful)
plus, in `Voxels` mode, one short ray each (at most `radius` cells). Bucket
lookups keep thousands of torches cheap. `sampleArea` should sample a small
fixed pattern (e.g. 5–9 points) rather than every tile.

## Tests to add (headless, `tests/`)

- A white lamp at known distance: `artificial` equals `attenuation(d)`; a
  red lamp gives a red-dominant sample; contributions sorted, ids correct.
- A wall between lamp and position: `Voxels` returns zero, `None` does not.
- A spotlight: inside vs outside the cone.
- Sealed light contributes nothing.
- Unloaded position: `Terrain` mode blocks a light behind a generated ridge.
- X-wrap: a light across the seam reaches a position near x = 0.
- Day/night: the sky part follows the cycle's daylight.

## Related documentation

- Lighting overview: `src/game/AGENTS.md` (Lighting, Persistence).
- Lore for great lights and limits on them: `IDEA.md` ("Light, night, and
  great beacons", Open Decisions).
