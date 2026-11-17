# Making trees

This guide explains how to add tree species with data files only. Read
`docs/modding/plants.md` first: trees reuse its foliage materials, climate
rules and placement fields. For how the system works internally, see the
design handoff `docs/handoff/trees.md`.

> **Status:** worlds generate trees; trunks and thick limbs are blocks,
> trees are drawn in full up to 600 m away and as billboards up to 2 km
> (massive trees in full as far as you can see).
> Trees grow while you play, can be cut and regrow, and can be planted with
> the debug **PLANT TREE** tool. Seasons, seeding and wood items are future
> work (see the handoff's *Progress*).

## How trees differ from plants

| | Plants (`flora`) | Trees (`trees`) |
| --- | --- | --- |
| Shape | a recipe of parts (stems, clumps, petals) | a **form** (a growth archetype in code) tuned by numbers |
| Blocks | never | trunks and branches thicker than 0.5 m radius are **wood blocks** you can break and build on; thinner twigs and leaves are meshes |
| Size | up to a few metres | from a 3 m mountain pine to a 120 m landmark with a trunk 128 m wide |
| Life | days to a few years | years to millennia; when a tree dies its wood stays |

A tree's size is continuous: a sapling of any species starts as a thin mesh
and gains wood blocks as its trunk and limbs thicken past 0.5 m radius.

### Size classes

The class comes from the largest `trunk_radius_m` you allow. It is only a
label (for budgets and this guide); behaviour follows the numbers.

| Class | Trunk radius (m) | Example | Notes |
| --- | --- | --- | --- |
| thin | below 0.5 | `trees.mountain_pine` | never forms blocks |
| small | 0.5 to 1.5 | `trees.spruce`, `trees.palm` | a trunk one to three blocks wide |
| medium | 1.5 to 4.5 | `trees.oak` | |
| large | 4.5 to 15.5 | `trees.redwood` | a trunk can fill a whole chunk (16 m) |
| massive | 15.5 to 64 | `trees.elder` | a **landmark**: a handful per continent, spanning several chunks |

## Files

| You want | You write | Where |
| --- | --- | --- |
| A tree species | a tree definition | `assets/data/trees/` |
| Its living wood | a block with `"natural_growth": true` | `assets/data/blocks/` (a new file sorting after `trees.json`, or the end of it) |
| The wood's texture | a terrain material + 2 textures (128x128, coloured) | `assets/data/materials/`, `assets/materials/blocks/wood/` |
| Leaves, needles, fronds, twig bark | foliage materials | `assets/data/foliage_materials/` (see the plants guide) |

The same rules as for plants apply: unknown fields are errors, numbers may
be ranges, and **order matters for saves** (tree species and blocks are
saved by load position: add new definitions at the end or in files that
sort later). After changes, run `python3 generate_assets.py` and rebuild.

### Living wood blocks

```json
{
    "name": "blocks.oak_wood",
    "material": "materials.oak_wood",
    "render_category": "Vegetation",
    "properties": { "hardness": 4, "flammable": true, "opacity": 15,
                    "natural_growth": true }
}
```

`natural_growth` marks wood that only trees grow. Worlds regenerate trees
from the seed, so this wood never counts as a player edit and is not drawn
as a building at a distance. Logs or planks that players place must be
**different blocks** without the flag, and a tree's `wood.block` must have
it (checked at load).

Wood blocks use the terrain material array: their textures are coloured
128x128 images like soil and stone (no palette). The placeholder barks come
from `tools/generate_foliage_textures.py` (`assets/materials/blocks/wood/`).

## A tree definition

```jsonc
{
    "name": "trees.oak",
    "form": "decurrent",
    "size": { "height_m": [15, 26], "trunk_radius_m": [1.2, 2.6],
              "crown_radius_m": [6, 10] },
    "growth": { "maturity_days": [900, 1400],
                "lifespan_days": [100000, 200000], "curve": 1.8 },
    "form_params": { "branch_spacing_m": [3, 5], "branch_angle_deg": [35, 55],
                     "branches_per_node": [2, 3], "crown_base": [0.25, 0.4],
                     "droop": 0.1, "taper": 0.7, "gnarl": 0.35,
                     "root_flare": 0.5, "branch_orders": 3 },
    "wood": { "block": "blocks.oak_wood", "bark": "foliage.bark_oak" },
    "foliage": { "type": "leaf_clump", "material": "foliage.oak_leaf",
                 "clump_size_m": [1.4, 2.2], "density": 1.2 },
    "placement": {
        "climate": [ { "allowed_land": ["Plains", "Hills"],
                       "temperature_range_c": [3, 24],
                       "humidity_range": [0.35, 1.0] } ],
        "vegetation": ["Shrub", "Forest"],
        "max_slope": 0.9,
        "density_per_100m2": [0.4, 1.0],
        "cluster": { "scale_m": 64, "threshold": 0.45 },
        "spacing_m": [7, 10],
        "priority": 15
    },
    "cut": { "stump_resprouts": true, "resprout_days": [20, 40] }
}
```

### `form`

| Form | Shape | Uses |
| --- | --- | --- |
| `excurrent` | one straight leader with whorls of side branches, narrowing to the top | spruce, fir, redwood |
| `decurrent` | the trunk forks into several leaders and a broad, rounded crown | oak, maple, most broadleaves |
| `palm` | a curved trunk without branches and a crown of fronds | palms; `branches_per_node` is the number of fronds |
| `stunted` | short, leaning, gnarled | mountain pines, krummholz |
| `giant` | a buttressed bole splitting low into a few huge limbs with tiered crowns | massive landmarks; the limbs are thick enough to walk and build on |

Beyond 600 m trees are billboards: the crown silhouette comes from the
foliage material `foliage.billboard_conifer` (excurrent),
`foliage.billboard_palm` (palm), `foliage.billboard_pine` (stunted) or
`foliage.billboard_broadleaf` (decurrent, giant), coloured with the
species' own leaf palette. Replace those textures to restyle distant
forests.

### `size`

| Field | Limits | Meaning |
| --- | --- | --- |
| `height_m` | required, 1 to 128 | full-grown height. A tree never grows into a ceiling or past the build limit: with less room it spreads wider instead |
| `trunk_radius_m` | required, 0.02 to 64 | full-grown trunk radius at the base (above the root flare) |
| `crown_radius_m` | 0.2 to 192 (default 3) | horizontal reach of branches and foliage |

**Reach limit.** For every tree except massive ones, the widest extent
(crown, or flared roots if wider) plus `spacing_m` must stay within 96 m:
worlds decide where trees stand chunk by chunk and only look that far
around. Massive trees are placed from a world-wide list and may extend up to
192 m from their trunk.

### `growth`

| Field | Default | Meaning |
| --- | --- | --- |
| `maturity_days` | 365 | game days from germination to full size |
| `lifespan_days` | 36500 | game days until the tree dies; must be at least `maturity_days` |
| `curve` | 2 | growth shape: size fraction `1 - (1 - age/maturity)^curve`; higher grows fast while young and slows toward maturity |

A game day is 72 real minutes, so growth is compressed by class: small trees
mature in about a game year, medium ones in a few years, large ones in tens
of years and massive ones in centuries (they are effectively ancient).
Generated forests start with trees of mixed ages. When a tree dies, its
leaves wither and fall but all its wood stays.

### `form_params`

All optional. How each is used depends on the form.

| Field | Default | Meaning |
| --- | --- | --- |
| `branch_spacing_m` | 1 | distance between whorls (`excurrent`), forks (`decurrent`) or limb tiers (`giant`) |
| `branch_angle_deg` | 60 | angle of branches from their parent (0 = straight up along it, 90 = horizontal) |
| `branches_per_node` | 3 | branches per whorl or fork; fronds for `palm` |
| `crown_base` | 0.3 | fraction of the height kept free of branches |
| `lean_deg` | 0 | lean of the trunk from vertical |
| `droop` | 0 | -1 to 1: branch tips bend down (positive) or up |
| `taper` | 0.85 | 0 to 1: how much the trunk thins toward the top (1: to a point) |
| `gnarl` | 0.1 | 0 to 1: random bending of trunk and branches |
| `root_flare` | 0 | 0 to 4: extra trunk radius at the ground, as a multiple of the trunk radius (buttresses) |
| `branch_orders` | 2 | 1 to 4: branches of branches |
| `limbs` | 0 | `giant` only: main limbs splitting from the bole |
| `bole_height` | 0.3 | `giant` only: where the limbs split, as a fraction of the height |

### `wood`

| Field | Meaning |
| --- | --- |
| `block` | the living wood block (must have `natural_growth`) for parts thicker than 0.5 m radius |
| `bark` | a foliage material for the thinner branches drawn as meshes; match its palette to the block texture |

### `foliage`

| Field | Default | Meaning |
| --- | --- | --- |
| `type` | required | `needle_clump`, `leaf_clump` or `frond` |
| `material` | required | a foliage material (needles: card `foliage.needles`; broad leaves: `foliage.leaf_broad`; palms: `foliage.palm_frond`) |
| `clump_size_m` | 1 | size of one clump (frond length for `frond`) |
| `density` | 1 | 0 to 4: clumps per metre of foliage-bearing twig |

### `placement`

Exactly the plant `placement` fields (`soils`, `ground_cover`, `climate`,
`vegetation`, `max_slope`, `underwater`, `density_per_100m2`, `cluster`,
`priority`, `persistent_sites`; see the plants guide), except `companions`,
plus:

| Field | Default | Meaning |
| --- | --- | --- |
| `spacing_m` | 4 | minimum distance from this tree's trunk to other trees; when trees compete, the higher `priority` (then the larger tree) wins |

Realistic densities: dense conifer forest 2 to 4 per 100 m², broadleaf
woods 0.4 to 1, giant groves 0.05 to 0.2. A landmark species uses a tiny
density (`trees.elder`: 0.0000005, about one per 200 km² of suitable land)
and `persistent_sites`.

### `cut`

Cutting through a trunk or branch removes everything above the cut (until
physics exists to fell it), and the tree regrows from the cut. Thin parts
(up to 2 m radius) are cut by breaking any of their wood; thicker ones only
once their whole cross-section at that height is broken, so felling a
giant takes work. Cuts and planted trees are saved in the world's
`trees.dat`.

| Field | Default | Meaning |
| --- | --- | --- |
| `stump_resprouts` | false | when cut at the root: the stump grows a new trunk (`true`, like oak) or the tree dies and the stump stays (`false`, like spruce and palm) |
| `resprout_days` | 30 | days before new growth starts at a cut |

## Shipped species

| Species | Class | Where |
| --- | --- | --- |
| `trees.spruce` | small | cold regions (tundra, cold temperate) with shrubs or forest |
| `trees.mountain_pine` | thin | hills, mountains and peaks |
| `trees.oak` | medium | temperate plains and hills with shrubs or forest |
| `trees.palm` | small | subtropical and tropical shores and plains |
| `trees.redwood` | large | groves in humid temperate forests |
| `trees.elder` | massive | rare landmarks in humid temperate lowlands |

## Checking your work

- **Load errors** name the file and field: unknown fields, ranges out of
  limits, reach beyond 96 m (or 192 m for massive trees), missing blocks,
  wood without `natural_growth`, missing foliage materials.
- **Automated checks**: `ctest --test-dir build -L trees` loads every
  shipped definition and generates forests.
- **Headless preview**: `SMOKE_FOREST` turns the smoke scene into forest:
  `1` (temperate: oaks, redwoods), `taiga` (spruces), `tropical` (palms),
  `mountain` (mountain pines) or `elder` (many elder landmarks; add
  `SMOKE_RADIUS=8` to load a whole one):

  ```bash
  SMOKE_FOREST=1 SMOKE_CAMERA="5440 6208 0.8 -0.3 45 20" \
      build/tests/lod_render_smoke /tmp/forest 500 lights
  ```
