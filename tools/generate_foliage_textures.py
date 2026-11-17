#!/usr/bin/env python3
"""Placeholder flora textures (docs/handoff/flora.md, section 6).

1. Ground-cover overlays in assets/materials/blocks/cover/:

  <name>.png         RGB: light, nearly grey detail (the cover's tint gradient
                     supplies the colour). A: coverage order. block.frag shows
                     a texel once the local density exceeds 1 - alpha, so
                     blade cores (high alpha) appear first and gaps fill last,
                     which gives painted, ragged density edges.
  <name>_normal.png  OpenGL-style tangent-space normals (+green = up).

   Every overlay is 128x128 (the material array size) and tiles seamlessly.

2. Plant cards in assets/materials/foliage/ (keys "foliage.<name>"), read by
   FoliageMaterial "albedo"/"normal":

  <name>.png         256x256. RGB: light grey detail and shading (the foliage
                     palette supplies colour). A: cutout mask with
                     anti-aliased edges. Cards are authored root down: the
                     bottom centre of the image is where the part attaches.
  <name>_normal.png  OpenGL-style tangent-space normals of the card's relief.

   Cards: leaf_broad (cluster of broad leaves for bush clumps), blade (a tuft
   of grass blades), frond (fern frond), petal (one petal), berry (a small
   cluster of berries).

Output is deterministic; keys stay stable so real art can replace the files
one by one. Run from the repository root, then `python3 generate_assets.py`.
"""
import os
import sys

import numpy as np
from PIL import Image

SIZE = 128
OUT_DIR = os.path.join("assets", "materials", "blocks", "cover")
CARD_DIR = os.path.join("assets", "materials", "foliage")
CARD = 256


def periodic_noise(rng, size, scale):
    """Tileable smooth noise in [0, 1]: white noise low-passed in frequency."""
    white = rng.standard_normal((size, size))
    f = np.fft.fftfreq(size)
    fx, fy = np.meshgrid(f, f)
    radius = np.sqrt(fx * fx + fy * fy)
    spectrum = np.fft.fft2(white) * np.exp(-((radius * scale) ** 2))
    field = np.real(np.fft.ifft2(spectrum))
    field -= field.min()
    return field / max(field.max(), 1e-9)


def blades(rng, count, length, width, lean):
    """Height field of short strokes leaning roughly one way, wrapped."""
    height = np.zeros((SIZE, SIZE))
    ys, xs = np.mgrid[0:SIZE, 0:SIZE]
    for _ in range(count):
        x0, y0 = rng.uniform(0, SIZE, 2)
        angle = lean + rng.normal(0, 0.6)
        n = rng.uniform(0.5, 1.0) * length
        dx, dy = np.cos(angle), np.sin(angle)
        # Distance from every texel to the segment, with wrap-around.
        px = (xs - x0 + SIZE / 2) % SIZE - SIZE / 2
        py = (ys - y0 + SIZE / 2) % SIZE - SIZE / 2
        t = np.clip(px * dx + py * dy, 0, n)
        d = np.hypot(px - t * dx, py - t * dy)
        taper = 1 - t / n * 0.7
        stroke = np.clip(1 - d / (width * taper), 0, 1) * (0.55 + 0.45 * t / n)
        height = np.maximum(height, stroke * rng.uniform(0.6, 1.0))
    return height


def normals(height, strength):
    gx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    gy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
    # Image rows go down; +green points up the image (OpenGL style).
    n = np.dstack((-gx * strength, gy * strength, np.ones_like(height)))
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return np.uint8(np.round((n * 0.5 + 0.5) * 255))


def cover(name, seed, count, length, width, lean, clump_scale, floor,
          brightness):
    rng = np.random.default_rng(seed)
    height = blades(rng, count, length, width, lean)
    clumps = periodic_noise(rng, SIZE, clump_scale)
    fine = periodic_noise(rng, SIZE, 2.0)
    # Coverage order: blades first, then clumpy fill; never fully zero so
    # density 1 covers the soil completely.
    order = 0.6 * height + 0.3 * clumps + 0.1 * fine
    # Equalize: the covered fraction then grows linearly with density.
    rank = np.argsort(np.argsort(order, axis=None)).reshape(order.shape)
    alpha = floor + (1 - floor) * rank / (rank.size - 1)
    value = brightness[0] + (brightness[1] - brightness[0]) * (
        0.65 * height + 0.35 * fine)
    rgb = np.dstack((value * 0.97, value, value * 0.95))
    albedo = np.dstack((rgb, alpha))
    os.makedirs(OUT_DIR, exist_ok=True)
    Image.fromarray(np.uint8(np.round(np.clip(albedo, 0, 1) * 255)), "RGBA").save(
        os.path.join(OUT_DIR, name + ".png"))
    Image.fromarray(normals(height + 0.3 * fine, 3.0), "RGB").save(
        os.path.join(OUT_DIR, name + "_normal.png"))


class Card:
    """Max-composited card layers: alpha, relief height and grey value."""

    def __init__(self):
        self.alpha = np.zeros((CARD, CARD))
        self.height = np.zeros((CARD, CARD))
        self.value = np.zeros((CARD, CARD))
        ys, xs = np.mgrid[0:CARD, 0:CARD]
        # Isotropic card space: x in [-0.5, 0.5] left to right, y in [0, 1]
        # bottom to top.
        self.x = (xs + 0.5) / CARD - 0.5
        self.y = 1 - (ys + 0.5) / CARD
        self.pixel = 1.0 / CARD

    def put(self, alpha, height, value):
        """Draws over earlier shapes where this one is opaque."""
        over = alpha > 0.5
        self.height = np.where(over, height, np.maximum(self.height, height * alpha))
        self.value = np.where(over, value, np.where(self.alpha > 0, self.value, value))
        self.alpha = np.maximum(self.alpha, alpha)

    def leaf(self, base, angle, length, width, bend=0.0, round_tip=False,
             veins=True, shade=0.0):
        """A leaf from base (x, y) pointing at angle (radians, 0 = up)."""
        dx, dy = np.sin(angle), np.cos(angle)
        px, py = self.x - base[0], self.y - base[1]
        u = (px * dx + py * dy) / length  # along the leaf, 0..1
        v = px * dy - py * dx  # across
        v = v - bend * length * np.clip(u, 0, 1) ** 2  # curls sideways
        uc = np.clip(u, 0, 1)
        profile = np.sin(np.pi * (uc ** 0.7 if round_tip else uc)) ** 0.8
        half = np.maximum(width * profile * (1 - 0.25 * uc), 1e-6)
        edge = (np.abs(v) - half) / self.pixel
        inside = np.clip(0.5 - edge, 0, 1) * ((u > 0) & (u < 1))
        across = np.clip(np.abs(v) / half, 0, 1)
        height = (1 - across ** 2) * 0.8
        value = 0.78 + 0.18 * (1 - across) - 0.12 * uc * shade
        if veins:
            rib = np.exp(-(v / (self.pixel * 1.5)) ** 2)
            height -= 0.25 * rib
            value += 0.12 * rib
            side = np.sin((uc * 9 - across * 2.5) * np.pi)
            vein = np.clip(side * 4 - 3.2, 0, 1) * (across < 0.9)
            value += 0.07 * vein
            height += 0.08 * vein
        self.put(inside, height, np.clip(value, 0, 1))

    def disc(self, centre, radius):
        px, py = self.x - centre[0], self.y - centre[1]
        r = np.sqrt(px * px + py * py) / radius
        alpha = np.clip((1 - r) * radius / self.pixel, 0, 1)
        z = np.sqrt(np.clip(1 - r * r, 0, 1))
        # Lit from the upper left, with a small highlight.
        light = np.clip(0.55 + 0.45 * (-px * 0.5 + py * 0.6) / radius + 0.4 * z, 0, 1.2)
        highlight = np.exp(-(((px + radius * 0.35) ** 2 +
                              (py - radius * 0.4) ** 2) / (radius * 0.18) ** 2))
        self.put(alpha, z, np.clip(0.55 * light + 0.45 * highlight, 0, 1))

    def save(self, name, normal_strength=4.0):
        os.makedirs(CARD_DIR, exist_ok=True)
        rgba = np.dstack((self.value * 0.98, self.value, self.value * 0.96,
                          self.alpha))
        Image.fromarray(np.uint8(np.round(np.clip(rgba, 0, 1) * 255)),
                        "RGBA").save(os.path.join(CARD_DIR, name + ".png"))
        Image.fromarray(normals(self.height, normal_strength), "RGB").save(
            os.path.join(CARD_DIR, name + "_normal.png"))


def leaf_broad(rng):
    card = Card()
    # Overlapping leaves around the card centre, back to front, pointing
    # outward, so a bush clump made of these cards reads as a dense mass.
    for i in range(18):
        a = rng.uniform(-np.pi, np.pi)
        r = rng.uniform(0.0, 0.14)
        base = (r * np.sin(a), 0.5 + r * np.cos(a))
        card.leaf(base, a + rng.normal(0, 0.3), rng.uniform(0.24, 0.32),
                  rng.uniform(0.07, 0.09), bend=rng.normal(0, 0.15), shade=1)
    card.save("leaf_broad")


def blade(rng):
    card = Card()
    for i in range(11):
        lean = rng.normal(0, 0.22)
        card.leaf((rng.normal(0, 0.03), 0.0), lean, rng.uniform(0.7, 0.98),
                  rng.uniform(0.014, 0.022), bend=rng.normal(0, 0.6),
                  veins=False, shade=-1)
    card.save("blade", normal_strength=2.5)


def frond(rng):
    card = Card()
    pinnae = 15
    # Rachis: a thin, slightly bent stem from the bottom to the tip.
    card.leaf((0, 0), 0.0, 0.98, 0.008, veins=False)
    for i in range(pinnae):
        t = (i + 0.5) / pinnae
        y = 0.08 + 0.86 * t
        length = 0.36 * (1 - t) ** 0.8 + 0.04
        for side in (-1, 1):
            card.leaf((0, y), side * (1.25 - 0.45 * t), length,
                      0.028 * (1.2 - t), bend=-side * 0.1, veins=False, shade=1)
    card.save("frond", normal_strength=3.0)


def petal(rng):
    card = Card()
    card.leaf((0, 0.02), 0, 0.95, 0.24, round_tip=True, shade=-1)
    card.save("petal", normal_strength=2.0)


def berry(rng):
    card = Card()
    centres = [(-0.13, 0.3), (0.12, 0.28), (0.0, 0.5), (-0.02, 0.15),
               (0.17, 0.52), (-0.17, 0.55)]
    for c in sorted(centres, key=lambda c: -c[1]):
        card.disc(c, rng.uniform(0.11, 0.13))
    card.save("berry", normal_strength=3.0)


def main():
    if not os.path.isdir("assets"):
        sys.exit("Run from the repository root")
    cover("meadow_grass", 11, count=150, length=14, width=1.6, lean=-1.3,
          clump_scale=18, floor=0.08, brightness=(0.62, 1.0))
    cover("steppe_grass", 23, count=95, length=11, width=1.2, lean=-1.1,
          clump_scale=12, floor=0.05, brightness=(0.68, 1.0))
    cover("tundra_moss", 37, count=260, length=4, width=1.8, lean=0.0,
          clump_scale=8, floor=0.1, brightness=(0.58, 0.95))
    leaf_broad(np.random.default_rng(101))
    blade(np.random.default_rng(103))
    frond(np.random.default_rng(107))
    petal(np.random.default_rng(109))
    berry(np.random.default_rng(113))


if __name__ == "__main__":
    main()
