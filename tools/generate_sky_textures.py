#!/usr/bin/env python3
"""Generate the sun and moon's straight-alpha RGBA sprites."""

import argparse
from pathlib import Path

import numpy as np
from PIL import Image


SIZE = 256
DEFAULT_OUTPUT = Path(__file__).resolve().parents[1] / "assets/textures/sky"
AXIS = (np.arange(SIZE) + 0.5) * (2 / SIZE) - 1
X, Y = np.meshgrid(AXIS, AXIS)
RADIUS = np.hypot(X, Y)


def smoothstep(low, high, value):
    t = np.clip((value - low) / (high - low), 0, 1)
    return t * t * (3 - 2 * t)


def noise(rng, cells):
    """Interpolate a seeded lattice across the sprite."""
    position = (AXIS + 1) * (cells / 2)
    cell = position.astype(int)
    weight = smoothstep(0, 1, position - cell)
    lattice = rng.random((cells + 1, cells + 1))
    rows = lattice[:, cell] * (1 - weight) + lattice[:, cell + 1] * weight
    weight = weight[:, None]
    return rows[cell] * (1 - weight) + rows[cell + 1] * weight


def sprite(disc, radius, halo_color, halo):
    edge = 1.5 / SIZE
    coverage = 1 - smoothstep(radius - edge, radius + edge, RADIUS)
    halo *= (1 - coverage) * (1 - smoothstep(0.72, 0.99, RADIUS))
    alpha = coverage + halo
    premultiplied = disc * coverage[..., None] + halo_color * halo[..., None]
    color = np.divide(premultiplied, alpha[..., None], out=premultiplied,
                      where=alpha[..., None] > 0)
    rgba = np.dstack((color, alpha))
    return Image.fromarray(np.rint(np.clip(rgba, 0, 1) * 255).astype(np.uint8))


def sun():
    rng = np.random.default_rng(2927)
    radius = 0.30
    limb = np.sqrt(np.maximum(0, 1 - (RADIUS / radius) ** 2))
    granules = noise(rng, 71) * 0.05 + noise(rng, 24) * 0.04
    warmth = np.clip(limb * 0.63 + 0.29 + granules, 0, 1)
    disc = np.dstack((np.ones_like(RADIUS), 0.67 + 0.31 * warmth,
                     0.30 + 0.61 * warmth))
    distance = np.maximum(RADIUS - radius, 0)
    halo = 0.35 * np.exp(-distance / 0.065) + 0.22 * np.exp(-distance / 0.22)
    return sprite(disc, radius, (1.0, 0.66, 0.27), halo)


def moon():
    rng = np.random.default_rng(5833)
    radius = 0.245
    x, y = X / radius, Y / radius
    z = np.sqrt(np.maximum(0, 1 - x * x - y * y))
    terrain = 0.56 * noise(rng, 22) + 0.30 * noise(rng, 55)
    terrain += 0.14 * noise(rng, 120)
    # Irregular maria and a field of overlapping crater basins.
    maria = 1 - smoothstep(0.41, 0.60, terrain)
    ground = 0.76 + terrain * 0.22 - maria * 0.23
    relief = np.zeros_like(ground)
    for _ in range(90):
        cx, cy = rng.uniform(-1, 1, 2)
        crater_radius = rng.uniform(0.16, 0.65) ** 2
        distance = np.hypot(x - cx, y - cy) / crater_radius
        bowl = 1 - smoothstep(0.13, 1.0, distance)
        rim = np.exp(-((distance - 0.93) / 0.17) ** 2)
        relief += crater_radius * (rim * 0.022 - bowl * 0.047)
        ground += 0.08 * rim - 0.07 * bowl
    dy, dx = np.gradient(relief, 2 / (SIZE * radius))
    light = np.clip(0.68 + 0.30 * z - 0.10 * x - 0.11 * y
                    + 1.2 * dx + 1.2 * dy, 0.44, 1.04)
    disc = (ground * light)[..., None] * np.array([1.02, 1.04, 1.10])
    halo = 0.17 * np.exp(-np.maximum(RADIUS - radius, 0) / 0.105)
    return sprite(disc, radius, (0.62, 0.76, 1.0), halo)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    sun().save(args.output_dir / "sun.png")
    moon().save(args.output_dir / "moon.png")


if __name__ == "__main__":
    main()
