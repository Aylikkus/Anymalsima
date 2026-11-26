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

## Setpoint mode (planned for step 16)

The step 15 definition is open-loop: `delta_c` is a fixed offset. Step 16 will
add an optional thermostat mode for blocks and items that hold a connected
room at a requested temperature. The planned schema adds a `mode` (`fixed` by
default, or `target`) plus `target_c`, `deadband_c`, `gain`, `min_delta_c` and
`max_delta_c`. The effective `delta_c` is recalculated from the measured room
air temperature, with hysteresis and saturation. Fixed-mode sources keep the
existing behaviour and need no migration.

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
