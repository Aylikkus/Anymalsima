# Temperature sources

Blocks and items can warm or cool nearby space. The values are data, and the
same schema is used by both definitions.

## Block definition

Add an optional `thermal` object to a file in `assets/data/blocks/`:

```json
{
    "name": "blocks.heater",
    "material": "materials.quartzite",
    "thermal": {
        "enabled": true,
        "delta_c": 20.0,
        "radius": 6.0,
        "falloff": 1.5
    }
}
```

| Field | Unit | Default | Range | Meaning |
| --- | --- | --- | --- | --- |
| `enabled` | — | `true` | — | `false` removes the source |
| `delta_c` | °C | `10` | −500 … 500, non-zero | signed offset added at the source centre |
| `radius` | metres | `6` | (0, 512] | distance at which the contribution reaches zero |
| `falloff` | — | `1.5` | (0, 16] | exponent of `(1 - d / radius)` |

`delta_c` is an offset, not an absolute temperature. The local query adds it
to the dated outdoor weather sample. Positive values warm and negative values
cool. Two sources add linearly where their ranges overlap, so a `+20 °C`
heater and a `-10 °C` cooler at the same point produce `+10 °C` above the
ambient temperature.

The contribution at distance `d` is:

```text
delta_c * (1 - d / radius) ^ falloff
```

A solid between the source and the query point attenuates the contribution.
The current local barrier uses an exponential e-folding length of 5 cm of
solid material, so a one-metre wall is effectively opaque while a thin pane
or a partly open doorway can pass some heat. Air and liquid are not barriers.

## Item definition

Movable sources use the same object in `assets/data/items/`:

```json
{
    "name": "items.heater",
    "thermal": {
        "enabled": true,
        "delta_c": 20.0,
        "radius": 6.0,
        "falloff": 1.5
    }
}
```

An item source has a world position. The debug fixture carries one at the
player position, drops it, picks it up again or removes it. Dropped and
carried item sources are saved in `thermal_sources.dat`; the emission itself
is read from this definition when the world loads.

## Setpoint mode

A source is open-loop by default: `delta_c` is a fixed offset. Add
`"mode": "target"` to make it a thermostat that holds its connected room at a
requested air temperature:

```json
{
    "name": "blocks.thermostat",
    "material": "materials.quartzite",
    "thermal": {
        "enabled": true,
        "delta_c": 0.0,
        "radius": 8.0,
        "falloff": 1.2,
        "mode": "target",
        "target_c": 21.0,
        "deadband_c": 0.5,
        "gain": 60.0,
        "min_delta_c": -30.0,
        "max_delta_c": 60.0
    }
}
```

| Field | Unit | Default | Range | Meaning |
| --- | --- | --- | --- | --- |
| `mode` | — | `fixed` | `fixed`, `target` | thermostat when `target` |
| `target_c` | °C | `21` | −200 … 200 | requested room air temperature |
| `deadband_c` | °C | `0.5` | 0 … 20 | hysteresis half-width around the target |
| `gain` | °C/°C | `3` | (0, 100] | command per degree of error |
| `min_delta_c` | °C | `-20` | ±500 | lower command clamp |
| `max_delta_c` | °C | `40` | ±500 | upper command clamp |

Each room simulation step measures the connected room air temperature and
sets the source's effective offset to:

```text
command = 0                              when |room - target| <= deadband
command = clamp(gain * (target - room),
                min_delta_c, max_delta_c) otherwise
```

The zero-command deadband is the hysteresis. The clamp is the anti-windup
limit: an open door can make the controller saturate at `max_delta_c`, but it
cannot accumulate an unbounded command. `min_delta_c` allows a cooling
thermostat to work symmetrically. Fixed-mode sources keep their configured
`delta_c` and need no migration.

The loop is proportional, so a large fabric loss leaves a steady-state droop.
Increase `gain` when a room must hold its target against cold weather, and use
`max_delta_c` for the maximum heating or cooling effort. The shipped
`blocks.thermostat` uses `gain: 60` and a `-30 … 60 °C` clamp.

The target is data, so it survives a reload even though the runtime command is
recomputed. A source outside any connected room uses the outdoor weather at its
position as its measured temperature. The inspection panel shows the target,
measured room air, current command and whether the command is saturated.

## Room air and wall heat storage

A room is a connected set of non-solid cells that each have a solid ceiling
within twelve metres. Exposed air, liquid and unloaded cells are not part of
the room. The faces between room air and those cells become leakage; solid
faces become wall contact. If a flood fill reaches the scan budget, the
remaining boundary is counted as an approximate leak. Geometry changes
invalidate the rooms and the active scan is bounded to the window around the
camera.

Each room has a lumped air temperature and one lumped wall temperature. The
wall is a finite store, so it heats and cools more slowly than the air:

- air capacity: `60 kJ/K` per cubic metre (including furnishings);
- wall capacity: `120 kJ/K` per square metre of solid face;
- air-to-wall conductance: `25 W/K` per square metre;
- open-face leakage: `1500 W/K` per square metre;
- wall-to-deep-ground conductance: `5 W/K` per square metre;
- source coupling time: `120 s`.

The deep-ground baseline is the region's annual mean at the local elevation.
That is the insulated baseline for sheltered caves; the small ground
conductance is why a sealed wall can stay warm for a long time. A thermostat
or fixed source supplies an effective offset, and the air relaxes toward
`outdoor + offset` through that source coupling. With no contributing source
(or a thermostat commanding zero inside its deadband) the coupling is absent,
so removing a heater lets the air settle with its walls instead of continuing
to ventilate toward the outdoor sample.

Rooms are integrated at the room cadence (several times per real second), not
every frame. Time jumps and unloaded rooms are subdivided into bounded steps,
and a room's elapsed gap is integrated on rediscovery. Significant air and
wall heat, with the room key and last-applied day, are saved in
`thermal_sources.dat` (`ROOM` v1). A room that has returned to the ground
baseline is omitted and starts there on load. Rows and columns outside the
loaded area are preserved; the local approximation only covers the active
scan.

The same measured local temperature is used by snowmelt: an indoor snow
receiver melts against its room air, and an outdoor receiver near a source
uses that source's contribution. Historical replay uses the owner's current
room state, which is the recorded bounded approximation.

## Debug fixture

Launch with `ANYM_THERMAL_FIXTURE=1` and click **Heat site**. The scene places
a sealed stone room on a pad:

- **ROOM DOOR** opens or closes the two-block doorway on the near wall.
- **ROOM ROOF** removes or replaces one roof cell.
- **FIXED HEAT** places or removes the fixed `+20 °C` heater on the west wall.
- **TARGET HEAT** places or removes the `21 °C` thermostat on the east wall.
- **COOLER** places or removes the `−15 °C` cooler on the back wall.
- **HEATER ITEM** / **COOLER ITEM**, **THERMAL DROP**, **THERMAL TAKE** and
  **CLEAR HEAT ITEMS** control the movable debug items.

The tile panel reports the room air and wall temperatures, volume, wall and
leak faces, whether the scan was bounded, and the source command, target and
saturation. It samples the air on the picked face and falls back to the
nearest room, so hovering a room boundary from outside is useful. The no-tile
panel reports the camera position, which may be outside the building; use walk
mode to read the true indoor value when standing in the room.

## Dated regional contributions

`thermal::RegionalHeat` represents a source too large or too far away for
detailed voxel sampling, such as a future large fire. It has a position,
`delta_c`, `radius`, `falloff`, a start day and an end day. The local query
adds active entries only.

Every producer that has both representations must use the same stable `key`
for its detailed `ThermalSource` and its regional entry. While the detailed
source is registered, the regional entry with that key is skipped. This is
the handover convention: one source contributes once, whether the engine is
simulating it in detail or through its dated summary. The two representations
should use the same emission values so an open-air sample does not jump when
the detailed copy unloads.

## Debug fixture

Launch with `ANYM_THERMAL_FIXTURE=1` and click **Heat site**. The scene places
a heater block, a cooler block and a shelter wall on a stone pad. The buttons
spawn a carried heater or cooler item, drop it, pick it up and clear item
sources. The tile inspection panel reports ambient, source and regional
contributions and the resulting local °C.
