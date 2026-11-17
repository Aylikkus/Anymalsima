# Making plants and ground covers

This guide explains how to add vegetation to the game with data files only:
grass and moss painted on the ground (**ground covers**), the surfaces plants
are made of (**foliage materials**), and the plants themselves (**flora**).
No code changes are needed. For how the systems work internally, see the
design handoff `docs/handoff/flora.md`.

## Overview

| You want | You write | Where |
| --- | --- | --- |
| Grass, moss, lichen painted on the ground | a ground cover + an overlay material + 2 textures | `assets/data/ground_covers/`, `assets/data/materials/`, `assets/materials/blocks/cover/` |
| A new colour or texture for leaves, petals, fruit, bark | a foliage material (+ textures if new shapes) | `assets/data/foliage_materials/`, `assets/materials/foliage/` |
| A plant (grass tuft, flower, fern, bush, ...) | a flora definition | `assets/data/flora/` |
| A tree | a tree definition + a wood block | see `docs/modding/trees.md` |

Neither grass nor plants are blocks. Ground covers are painted on the faces
of the soil tile they grow on; plants are lightweight instances anchored on a
tile, drawn as meshes built procedurally from your recipe. Every plant of a
species looks different: each instance draws its own values from the ranges
you give.

After adding or renaming any file, run from the repository root:

```bash
python3 tools/generate_foliage_textures.py   # only if you use the placeholder art
python3 generate_assets.py                   # rebuilds assets/definitions.json
cmake --build build --parallel               # copies assets next to the game
```

### Rules for every file

- Each file is a JSON array of definitions. Every definition has a unique
  `name`, prefixed by its type: `ground_covers.`, `foliage.`, `flora.`.
- **Unknown fields are errors.** A typo (`"colour"` instead of `"color"`)
  stops the game at load with the file and field name.
- **Numbers or ranges.** Wherever a field is a measurement, you may write a
  number (`0.4`) or a range (`[0.3, 0.5]`). Each plant picks its own value in
  the range, so ranges are what make a meadow look natural.
- **Colours** are `"#rrggbb"`.
- **Order matters for saves.** Ground covers and plant species are stored
  in saved worlds by their position in load order (files sorted by name,
  definitions in file order). Add new definitions at the **end** of a file,
  or in a new file whose name sorts after the existing ones
  (`flora/flora.json`, then `flora/mymod_flowers.json`). Never reorder or
  delete shipped definitions in a world you want to keep.
- Region climate words (`allowed_land`, `allowed_biomes`, features) are the
  same as for soils: land `Deep Water`, `Water`, `Shore`, `Plains`, `Hills`,
  `Mountains`, `Peak`; biomes `Polar`, `Tundra`, `Temperate`, `Subtropical`,
  `Tropical`, `Scorching`; vegetation `None`, `Grass`, `Shrub`, `Forest`,
  `Rainforest`; features such as `Beach`, `Cliff`, `Swamp`, `Floodplain`,
  `Dense Forest`, `Clearing` (see `src/game/utils/region_enum_maps.hpp`).

## Climate rules

Covers and plants choose where they grow with **placement rules**, the same
objects soils use. A definition matches a region when *any* of its rules
matches. A rule matches when every field it sets matches; fields you leave
out match everything.

| Field | Meaning |
| --- | --- |
| `allowed_land` | list of land types |
| `allowed_biomes` | list of biomes |
| `required_features` / `forbidden_features` | lists of region features |
| `humidity_range` | `[min, max]`, 0 (dry) to 1 (wet) |
| `temperature_range_c` | `[min, max]` in °C |
| `allow_underwater` | ground below sea level allowed (default false) |
| `priority`, `weight` | ground covers only: the highest priority among matching rules wins; equal priorities share by weight |

## Ground covers

A region gets one ground cover, chosen from the covers whose soil, climate
and vegetation rules match it (highest `priority` wins). Cover grows on the
top **topsoil** tile of dry land. Its **density** (0 to 1) varies with noise
and drops on slopes; it fades smoothly between tiles and decides how much of
the overlay texture shows.

```jsonc
{
  "name": "ground_covers.meadow_grass",
  "overlay_material": "materials.cover_meadow_grass",
  "faces": { "top": true, "sides": true, "side_fringe_m": [0.9, 1.0] },
  "soils": { "fertility": [0.45, 1.0], "water_retention": [0.3, 1.0] },
  "placement": [
    { "allowed_land": ["Shore", "Plains", "Hills", "Mountains"],
      "humidity_range": [0.3, 1.0], "temperature_range_c": [2, 40] }
  ],
  "vegetation": ["Grass", "Shrub"],
  "density_noise": { "scale_m": 32, "threshold": 0.0, "contrast": 3.5 },
  "max_slope": 1.6,
  "tint_gradient": {
    "humid_warm": "#5a9a3e", "humid_cold": "#467a4c",
    "dry_warm": "#a3a24a", "dry_cold": "#86895a",
    "temperature_c": [0, 30]
  }
}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `overlay_material` | required | a material (`assets/data/materials/`) whose textures are the overlay |
| `faces.top`, `faces.sides` | true | where the cover shows (never on undersides) |
| `faces.side_fringe_m` | `[0.25, 0.45]` | how far it hangs down a side face, up to `1` (the whole side) |
| `soils.names` | any | soils it grows on (`soils.loam`, ...) |
| `soils.fertility`, `soils.water_retention` | `[0, 1]` | ranges of the topsoil's properties |
| `placement` | required | climate rules (above) |
| `vegetation` | any but `None` | region vegetation types |
| `density_noise.scale_m` | 24 | size of dense and sparse patches, in metres |
| `density_noise.threshold` | 0.35 | noise below this is bare ground (0 = almost never bare) |
| `density_noise.contrast` | 1.5 | how fast density reaches full above the threshold |
| `max_slope` | 0.8 | slope (metres per metre) at which density reaches 0 |
| `tint_gradient` | greens | colour by local climate: humid/dry × warm/cold, blended; `temperature_c` is the cold-to-warm span |

Dense meadows: `threshold` near 0 and `contrast` 3 or more. Patchy steppe:
`threshold` 0.2 to 0.4.

**Overlay material** (in `assets/data/materials/`, e.g. a new
`materials/mymod_covers.json`):

```json
{ "name": "materials.cover_meadow_grass",
  "texture_albedo": "blocks.cover.meadow_grass",
  "texture_normal": "blocks.cover.meadow_grass_normal" }
```

**Overlay textures** (`assets/materials/blocks/cover/<name>.png` and
`<name>_normal.png`): 128×128, tiling seamlessly, one metre per tile.

- RGB: light, nearly grey detail (the tint gradient supplies the colour).
- Alpha: the **order** in which texels appear as density grows. A texel
  shows once density exceeds `1 - alpha`: give blade cores high alpha and
  the gaps between them low alpha, never exactly 0 if full density should
  cover the soil. Spread alpha evenly over 0..1 so coverage follows density.
- Normal map: tangent space, OpenGL style (green up).

## Foliage materials

A foliage material is the surface of plant parts: a texture **card** for the
shape, and a **palette** for the colour, so one card serves many species.

```jsonc
{
  "name": "foliage.broadleaf",
  "albedo": "foliage.leaf_broad",
  "normal": "foliage.leaf_broad_normal",
  "palette": { "base": "#4c7d33", "shade": "#2c4a26", "tip": "#9bbf4a", "dry": "#8a7a3a" },
  "gradient": "center_to_edge",
  "hue_jitter": 0.04, "value_jitter": 0.12,
  "translucency": 0.35,
  "withered": ["#8a7a3a", "#5e4a2a"]
}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `albedo` | required | texture key of the card (`foliage.<file>`) |
| `normal` | flat | texture key of its normal map |
| `palette.base` | green | the main colour (most of the plant) |
| `palette.shade` | dark green | deep inside clumps, low on stems, at the root |
| `palette.tip` | light green | tips and the tops of clumps |
| `palette.dry` | brown | used when `withered` is absent |
| `gradient` | `root_to_tip` | `uniform`, `root_to_tip` (by height), `center_to_edge` (inside to top of clumps) |
| `hue_jitter` | 0.04 | per-plant hue variation (0 to 0.5) |
| `value_jitter` | 0.12 | per-plant brightness variation (0 to 1) |
| `translucency` | 0.3 | how much sunlight shines through (0 to 1); leaves 0.3 to 0.5, fruit and bark low |
| `withered` | `[dry]` | colours a dying plant fades to |

**Cards** (`assets/materials/foliage/<name>.png`, key `foliage.<name>`):

- 256×256 RGBA.
- RGB: light grey detail and shading (0.6 to 1.0 grey); the palette colours it.
- Alpha: the cutout. Pixels with alpha under 50% are not drawn; keep edges
  antialiased.
- Draw the part **root down**: the bottom centre of the image is where it
  attaches to the plant (for clumps and fruit the centre is used).
- Optional `<name>_normal.png`: tangent-space normals, OpenGL style.

Shipped cards: `leaf_broad` (a cluster of leaves, for bushes), `blade` (a
grass tuft), `frond` (a fern frond), `petal` (one petal, also usable as a
single leaf), `berry` (a few berries). The placeholder generator
`tools/generate_foliage_textures.py` draws them; replace any file with real
art under the same name.

Stems are never textured; they take the material's palette.

## Plants (flora)

A plant species is a recipe: where it grows, how it grows over time, and the
**parts** its mesh is built from.

```jsonc
{
  "name": "flora.round_bush",
  "kind": "bush",
  "footprint": { "tiles": [2, 2], "blocks_placement": true, "collision": "soft",
                 "min_spacing_m": 1.5 },
  "placement": {
    "soils": { "fertility": [0.3, 1], "water_retention": [0.25, 1] },
    "ground_cover": { "required": ["ground_covers.meadow_grass"] },
    "climate": [ { "allowed_land": ["Plains", "Hills"], "humidity_range": [0.35, 1.0] } ],
    "max_slope": 0.6,
    "density_per_100m2": [0.5, 2],
    "cluster": { "scale_m": 18, "threshold": 0.55 },
    "companions": [ { "species": "flora.fern", "radius_m": 3, "chance": 0.4 } ],
    "priority": 10
  },
  "growth": {
    "stages": { "seedling": [3, 6], "young": [10, 20], "mature": [40, 80],
                "flowering": [8, 14], "fruiting": [10, 20], "withering": [6, 10] },
    "perennial": true,
    "lifespan_days": [400, 900],
    "spread": { "radius_m": 6, "chance_per_cycle": 0.8, "seeds": [3, 6],
                "germination": 0.6, "stage": "fruiting" },
    "regrow_after_harvest_days": [10, 20]
  },
  "parts": [
    { "type": "stem", "count": [3, 5], "height_m": [0.35, 0.6], "width_m": [0.025, 0.04],
      "bend": [0.25, 0.45], "material": "foliage.bark" },
    { "type": "leaf_clump", "count": [3, 5],
      "shell": { "radii_m": [[0.7, 1.0], [0.5, 0.8], [0.7, 1.0]], "lift_m": [0.0, 0.12] },
      "cards_per_clump": [18, 30], "card_size_m": [0.35, 0.55],
      "material": "foliage.broadleaf", "color_variation": 0.12,
      "stages": { "seedling": 0.2, "young": 0.6, "mature": 1.0, "flowering": 1.0,
                  "fruiting": 1.0, "withering": 0.9 } },
    { "type": "fruit_cluster", "count": [2, 6], "material": "foliage.red_berry",
      "attach": "leaf_clump_surface", "stages": { "fruiting": 1.0 } }
  ],
  "forage": [ { "item": "items.red_berry", "count": [2, 6], "stage": "fruiting" } ]
}
```

### `kind`

One of `grass_tuft`, `flower`, `fern`, `herb`, `bush`, `shrub`, `succulent`.
It is descriptive (future systems such as animals use it); the look comes
from the parts.

### `footprint`

| Field | Meaning |
| --- | --- |
| `radius_m` | **small plants**: their size on the ground. Small plants share tiles with each other and never block anything. |
| `tiles` | **large plants**: `[x, z]` whole tiles (1 to 16). Large plants never overlap each other. |
| `blocks_placement` | large plants only: the player cannot place blocks inside the footprint |
| `collision` | `none`, `soft` or `solid` (reserved for when physics exists) |
| `min_spacing_m` | extra clearance around a large footprint |

Use either `tiles` or `radius_m`, not both. A plant's height (for blocking
and for removing it when a block is placed into it) is worked out from its
parts.

Large plants need **level ground** under their whole footprint, with
nothing rising directly beside it, so they never stand half on a step or
grow into a wall. Their canopy (`leaf_clump` parts) is kept within the
footprint plus a 0.3 m overhang whatever radii you give: pick `tiles` large
enough for the bush you want.

### `placement`

| Field | Default | Meaning |
| --- | --- | --- |
| `soils.names`, `soils.fertility`, `soils.water_retention` | any | as for ground covers; plants always need soil (never bare rock) |
| `ground_cover.required` | none | the tile must have one of these covers, with some density |
| `ground_cover.forbidden` | none | the tile must not have these covers |
| `climate` | required | at least one climate rule |
| `vegetation` | any | list of region vegetation types the plant grows in (for example `["Forest", "Rainforest"]` for a forest-floor fern) |
| `max_slope` | 1 | steepest ground, metres per metre (0.6 is already a steep hill) |
| `underwater` | false | allow ground below sea level |
| `density_per_100m2` | 1 | plants per 100 m² where everything allows them |
| `cluster.scale_m`, `cluster.threshold` | none | grow in patches of about this size; a higher threshold (0 to 1) means fewer, smaller patches |
| `companions` | none | small species that may grow around this one: `species`, `radius_m`, `chance` (large plants only) |
| `priority` | 0 | when large plants compete for space, higher priority wins |
| `persistent_sites` | false | keep the plant's sites when long-unvisited areas are re-drawn (rare and landmark plants) |

Realistic densities: grass tufts 30 to 90, flowers 3 to 15, ferns 1 to 5,
bushes 0.5 to 2 per 100 m².

#### Rare plants

A plant the player has to search for is just a species with strict rules
and a tiny density:

- **Where:** require rare region features (`required_features`:
  `Volcano`, `Crater`, `Oasis`, `Swamp`, `Glacier`, ...), narrow climate
  ranges, specific soils or ground covers.
- **How rare:** `density_per_100m2` goes as low as you like: `0.01` is one
  plant per hectare, `0.0001` one per square kilometre, wherever the rules
  allow. A high `cluster.threshold` adds a second layer: only a few patches
  qualify at all.
- **Where it stays:** set `"persistent_sites": true`. Areas the player has
  not visited for a game year re-draw their plants; ordinary species then
  grow in new spots, but a species with persistent sites comes back to the
  same places (only its age is re-drawn), so "it grows by the crater lake"
  stays true. Harvesting one that does not regrow removes it until its
  seeds or such a re-draw bring it back.
- **How it lives on:** give it `spread` like any plant. Crowding is
  measured over an area that holds about three plants at your density, so a
  lone rare flower can seed a few more around it, and never more than its
  density allows.

```json
"placement": {
    "climate": [{"required_features": ["Crater"], "humidity_range": [0.4, 1.0]}],
    "density_per_100m2": 0.005,
    "persistent_sites": true
}
```

Small plants fade out at 45 to 55 m, so finding one means walking the area.

### `growth`

Time is measured in game days (72 real minutes each at normal speed).

| Field | Meaning |
| --- | --- |
| `stages` | duration in days of each stage the species has: `seedling`, `young`, `mature`, `flowering`, `fruiting`, `withering`. Leave out stages it skips. |
| `perennial` | `false` (annual): passes every stage once, then dies. `true`: grows up once, then repeats `mature` → `flowering` → `fruiting` until the end of its lifespan, then withers. |
| `lifespan_days` | perennials: age when withering starts |
| `spread.radius_m` | how far seeds fall from the plant |
| `spread.chance_per_cycle` | chance (0 to 1, default 1) that the plant drops seeds in a seeding cycle (an annual's life, or one perennial cycle) |
| `spread.seeds` | seeds dropped per cycle (number or range, default 3) |
| `spread.germination` | chance (0 to 1, default 0.5) that a seed grows on empty, suitable ground |
| `spread.stage` | the stage at whose end seeds drop (default: `fruiting`, else `flowering`, else the last growing stage; never `withering`) |
| `regrow_after_harvest_days` | after a harvest the plant regrows its flowers or fruit this many days later; without it, harvesting removes the plant |
| `season_mask` | reserved for seasons; only `null` is accepted |

#### How populations live on

Plants never regrow in place: a dead plant leaves an empty spot, and new
plants come only from seeds.

- Each seed lands at a random spot within `radius_m`, a few days after it
  falls it germinates if the ground suits the species (soil, cover, slope,
  climate, open sky, no bush footprint in the way) and its roll beats
  `germination × (1 − crowding)`. **Crowding** is the number of plants of
  the species around the spot divided by the number your
  `density_per_100m2` and `cluster` patches would put there. Gaps fill up;
  full areas get no more plants. Populations settle near the density you
  set: they neither thin into bare patches nor overgrow, and failed seeds
  cost nothing.
- A species must leave enough seedlings to replace itself. Roughly:
  `cycles in a life × chance_per_cycle × average seeds × germination` should
  be **2 or more** (an annual has one cycle; a perennial has
  `(lifespan − seedling − young) / (mature + flowering + fruiting)`).
  Below about 1.5 the species slowly dies out where it grows; much more only
  spends time on seeds that crowding rejects. The automated checks test
  this for shipped species.
- A species without `spread` lives one generation and disappears.
- Areas the player has not visited for a game year or more are not replayed
  seed by seed: their plants are re-drawn at the expected density, with
  ages spread over their lives. The patches (`cluster`) stay where they
  were.

### `parts`

A plant is built from parts. Every part has:

| Field | Default | Meaning |
| --- | --- | --- |
| `type` | required | see below |
| `material` | required | a foliage material |
| `count` | 1 | how many |
| `stages` | all | `{stage: scale}`: the part shows only in the stages listed, scaled (0 to 4). Use it to grow plants from small seedlings and to show flowers or fruit only in their season. |
| `color_variation` | 0 | per-card brightness variation (0 to 1) |
| `attach` | `ground` | `ground`, `stem` (along stems), `stem_tip`, `leaf_clump_surface` |

Part types and the fields that shape them:

| `type` | What it builds | Shape fields |
| --- | --- | --- |
| `stem` | thin, tapered, untextured stalks; other parts attach to them | `height_m`, `width_m`, `bend` (-1 to 1, how much the top leans) |
| `blade` | a grass tuft: crossed cards of the card texture, leaning outward | `length_m`, `spread_angle` (degrees of lean), `bend` |
| `leaf_card` | single leaves, along stems by `phyllotaxis` (degrees between leaves; 137.5 by default) or around the base | `length_m`, `width_m`, `droop` (0 = raised, 1 = hanging) |
| `leaf_clump` | a bush mass: cards on an ellipsoid shell with round shading. Needs `shell`. | `shell.radii_m` (`[x, y, z]` radii, each a number or range), `shell.lift_m` (raise above the ground, e.g. for shrubs on stems), `cards_per_clump`, `card_size_m` |
| `petal_ring` | petals around each stem tip | `length_m`, `width_m`, `spread_angle` (from vertical: 90 = flat open flower) |
| `flower_head` | a small disc facing up at stem tips (flower centres) | `length_m` (its size) |
| `fruit_cluster` | small cards on clump shells or stem tips | `card_size_m` |
| `frond` | arching fern fronds around the base | `length_m`, `width_m`, `spread_angle`, `droop`, `phyllotaxis` |

`normal_mode` (`card`, `spherical`, `cylindrical`) is accepted for future
use; parts already pick fitting normals (clumps spherical, stems
cylindrical).

### `forage`

What harvesting gives, for the items system that will come later:
`{ "item": "items.red_berry", "count": [2, 6], "stage": "fruiting" }`. A plant
can be harvested only in that stage. Item names are not checked yet.

## Walkthrough: a blue meadow flower

1. Add a material for the petals to a new file
   `assets/data/foliage_materials/mymod_flowers.json`. The shipped `petal`
   card works for any colour:

   ```json
   [
       {
           "name": "foliage.blue_petal",
           "albedo": "foliage.petal",
           "normal": "foliage.petal_normal",
           "palette": {"base": "#4a6ad8", "shade": "#2a3a8a", "tip": "#a0b8ff", "dry": "#6a5a7a"},
           "gradient": "root_to_tip",
           "hue_jitter": 0.06,
           "value_jitter": 0.12,
           "translucency": 0.5
       }
   ]
   ```

2. Add the species to a new `assets/data/flora/mymod_flowers.json` (it sorts
   after `flora.json`, so existing worlds keep their species ids):

   ```json
   [
       {
           "name": "flora.blue_bell",
           "kind": "flower",
           "footprint": {"radius_m": [0.1, 0.15]},
           "placement": {
               "ground_cover": {"required": ["ground_covers.meadow_grass"]},
               "climate": [{"humidity_range": [0.5, 1.0], "temperature_range_c": [0, 25]}],
               "max_slope": 0.6,
               "density_per_100m2": [2, 8],
               "cluster": {"scale_m": 10, "threshold": 0.5}
           },
           "growth": {
               "stages": {"seedling": [2, 4], "young": [5, 8], "flowering": [12, 20], "withering": [3, 6]},
               "spread": {"radius_m": 2, "seeds": [4, 8], "germination": 0.6}
           },
           "parts": [
               {"type": "stem", "height_m": [0.2, 0.35], "bend": [0.1, 0.3], "material": "foliage.small_leaf"},
               {"type": "leaf_card", "count": [2, 3], "length_m": [0.06, 0.1], "width_m": 0.03,
                "attach": "stem", "material": "foliage.small_leaf"},
               {"type": "petal_ring", "count": [4, 6], "length_m": 0.03, "width_m": 0.02,
                "spread_angle": [40, 60], "attach": "stem_tip", "material": "foliage.blue_petal",
                "stages": {"flowering": 1.0, "withering": 0.6}}
           ]
       }
   ]
   ```

3. Run `python3 generate_assets.py` and rebuild. Create a **new world** to
   see the flower generated (existing worlds keep the plants they saved, but
   spreading can bring new species into them only from plants already
   there).

## Checking your work

- **Load errors** name the file and field. Unknown fields, reversed ranges
  and missing references (materials, covers, soils, companion species,
  textures) are all reported.
- **Automated checks**: `ctest --test-dir build -L flora` loads every shipped
  definition and builds every species' mesh at every stage. Run it after
  editing shipped files.
- **In game**: the tile panel (top left) shows the cover and the plants on
  the tile under the screen centre, with stage, age and harvest state. The
  **TIME** button speeds up the clock (x60, x720, x7200) to watch plants
  grow, flower, die and spread; **HARVEST** picks the plants on the clicked
  tile and lists what they yield.
- **Headless preview** (no window): `build/tests/lod_render_smoke` renders
  the test scene to `.ppm` images. With `SMOKE_FLORA_GALLERY=1` it plants
  every species in rows by growth stage around the camera;
  `SMOKE_CAMERA="x z yaw pitch distance above"` places the camera:

  ```bash
  cmake --build build --target lod_render_smoke
  SMOKE_FLORA_GALLERY=1 SMOKE_CAMERA="6205 6122 1.57 -0.6 6 0.5" \
      build/tests/lod_render_smoke /tmp/plants 600 lights
  ```

## Tips for a painted look

- Prefer ranges everywhere: sizes, counts, bends, stage lengths.
- Let the palette carry colour and keep cards grey; reuse cards across
  species with different palettes.
- Keep `base` dominant and `tip` subtle: clumps show mostly `base`, with
  `tip` only on top.
- Bushes: 3 to 5 clumps of 18 to 30 cards, `lift_m` near 0 so they sit on
  the ground, and a few `stem` parts as visible twigs.
- Plants lean toward the hue of the grass they stand in, and darken toward
  the ground at the root, so they blend with the meadow.
