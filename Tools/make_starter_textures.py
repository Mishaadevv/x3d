"""Generates the Starter Content textures and color grading LUTs into Engine/Content/StarterContent.

    python Tools/make_starter_textures.py

Textures/T_Bricks_{Color,Height,Normal}.png  - a running-bond brick wall (relief for Parallax Occlusion)
Textures/T_Cobble_{Color,Height,Normal}.png  - rounded cobblestones
LUTs/LUT_*.png                                - 256 x 16 color grading tables (Unreal layout: 16 slices of 16 x 16),
                                                with .zmeta import settings: no compression
All procedural (numpy + Pillow), free to use like the rest of the engine.
"""

import json
import os
import uuid

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'Engine', 'Content', 'StarterContent')
SIZE = 512
rng = np.random.default_rng(7)


def value_noise(size, cells, seed):
    """Tiling smooth noise in 0..1."""
    r = np.random.default_rng(seed).random((cells, cells))
    x = np.arange(size) / size * cells
    i0 = np.floor(x).astype(int) % cells
    i1 = (i0 + 1) % cells
    f = x - np.floor(x)
    f = f * f * (3 - 2 * f)
    a = r[np.ix_(i0, i0)] * (1 - f)[None, :] + r[np.ix_(i0, i1)] * f[None, :]
    b = r[np.ix_(i1, i0)] * (1 - f)[None, :] + r[np.ix_(i1, i1)] * f[None, :]
    return a * (1 - f)[:, None] + b * f[:, None]


def fbm(size, seed, base=4, octaves=5):
    out = np.zeros((size, size))
    amp, total = 1.0, 0.0
    for o in range(octaves):
        out += value_noise(size, base * 2 ** o, seed + o) * amp
        total += amp
        amp *= 0.5
    return out / total


def normal_from_height(h, strength):
    dx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * strength
    dy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * strength
    n = np.stack([-dx, dy, np.ones_like(h)], axis=-1)  # OpenGL convention (green up)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def save(img, *path):
    full = os.path.join(OUT, *path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    arr = np.clip(img * 255.0 + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(arr).save(full)
    print(path[-1])


def bricks():
    rows, cols = 8, 4  # bricks per texture
    y, x = np.mgrid[0:SIZE, 0:SIZE] / SIZE
    row = np.floor(y * rows)
    xs = x * cols + (row % 2) * 0.5  # running bond
    col = np.floor(xs)
    fx, fy = xs - col, y * rows - row
    mortar = 0.06
    edge = np.minimum(np.minimum(fx, 1 - fx) * 2.0, np.minimum(fy, 1 - fy) * 1.0)  # brick is 2:1
    face = np.clip((edge - mortar * 0.5) / 0.06, 0, 1)  # 0 in the mortar, 1 on the face, bevelled
    face = np.sqrt(face)
    grain = fbm(SIZE, 11, 8)
    chips = np.clip((fbm(SIZE, 23, 16) - 0.62) * 6, 0, 1)
    height = face * (0.85 + 0.15 * grain) - chips * 0.25 * face
    # Per-brick colour variation.
    seed = (row * 31 + col % cols * 7).astype(int) % 97
    tint = np.random.default_rng(3).random(97)[seed]
    brick = np.stack([0.48 + 0.12 * tint, 0.22 + 0.06 * tint, 0.15 + 0.04 * tint], -1) * (0.75 + 0.35 * grain[..., None])
    mortar_col = np.array([0.55, 0.52, 0.48]) * (0.8 + 0.3 * fbm(SIZE, 5, 16)[..., None])
    color = brick * face[..., None] + mortar_col * (1 - face[..., None])
    save(color, 'Textures', 'T_Bricks_Color.png')
    save(np.repeat(height[..., None], 3, -1), 'Textures', 'T_Bricks_Height.png')
    save(normal_from_height(height, 12.0), 'Textures', 'T_Bricks_Normal.png')


def cobbles():
    # Rounded stones: distance to jittered cell centres (tiling Worley).
    cells = 6
    pts = rng.random((cells, cells, 2))
    y, x = np.mgrid[0:SIZE, 0:SIZE] / SIZE * cells
    best = np.full((SIZE, SIZE), 9.0)
    second = np.full((SIZE, SIZE), 9.0)
    stone_id = np.zeros((SIZE, SIZE))
    for oy in range(-1, 2):
        for ox in range(-1, 2):
            cy = np.floor(y) + oy
            cx = np.floor(x) + ox
            p = pts[(cy.astype(int)) % cells, (cx.astype(int)) % cells]
            d = np.hypot(cx + p[..., 0] - x, cy + p[..., 1] - y)
            closer = d < best
            second = np.where(closer, best, np.minimum(second, d))
            stone_id = np.where(closer, (cy % cells) * cells + (cx % cells), stone_id)
            best = np.minimum(best, d)
    gap = np.clip((second - best) / 0.25, 0, 1)  # 0 at the joints
    dome = np.sqrt(np.clip(gap, 0, 1))
    height = dome * (0.85 + 0.15 * fbm(SIZE, 41, 8))
    tint = np.random.default_rng(9).random(cells * cells)[stone_id.astype(int)]
    stone = np.stack([0.42 + 0.1 * tint, 0.4 + 0.08 * tint, 0.37 + 0.06 * tint], -1) * (0.75 + 0.3 * fbm(SIZE, 13, 16)[..., None])
    dirt = np.array([0.18, 0.15, 0.11])
    color = stone * dome[..., None] + dirt * (1 - dome[..., None])
    save(color, 'Textures', 'T_Cobble_Color.png')
    save(np.repeat(height[..., None], 3, -1), 'Textures', 'T_Cobble_Height.png')
    save(normal_from_height(height, 10.0), 'Textures', 'T_Cobble_Normal.png')


def luts():
    s = 16
    b, g, r = np.meshgrid(np.arange(s), np.arange(s), np.arange(s), indexing='ij')  # b = slice, g = row, r = column
    rgb = np.stack([r, g, b], -1) / (s - 1.0)  # (b, g, r, 3)

    def to_image(c):
        img = np.zeros((s, s * s, 3))
        for k in range(s):
            img[:, k * s:(k + 1) * s] = c[k]
        return img

    def luma(c):
        return (c @ np.array([0.2126, 0.7152, 0.0722]))[..., None]

    def smooth(x):
        return x * x * (3 - 2 * x)

    looks = {
        'LUT_Neutral': rgb,
        'LUT_Warm': np.clip(rgb * np.array([1.08, 1.0, 0.86]) + np.array([0.02, 0.01, 0.0]), 0, 1),
        'LUT_Cold': np.clip(rgb * np.array([0.88, 0.98, 1.1]), 0, 1),
        # Teal shadows, orange highlights, a gentle S-curve.
        'LUT_Cinematic': np.clip(smooth(rgb) * 0.4 + rgb * 0.6 + (np.array([-0.04, 0.02, 0.06]) * (1 - luma(rgb)) +
                                                              np.array([0.06, 0.02, -0.06]) * luma(rgb)), 0, 1),
        'LUT_BlackWhite': np.repeat(np.clip(smooth(luma(rgb)) * 0.3 + luma(rgb) * 0.7, 0, 1), 3, -1),
        # Faded blacks, warm mids, desaturated.
        'LUT_Vintage': np.clip((luma(rgb) * 0.35 + rgb * 0.65) * np.array([1.05, 0.98, 0.85]) * 0.88 + 0.08, 0, 1),
    }
    for name, c in looks.items():
        save(to_image(c), 'LUTs', name + '.png')
        # Import settings next to it: uncompressed (block compression shifts the colours of a lookup table).
        meta = {'guid': str(uuid.uuid5(uuid.NAMESPACE_URL, 'zeqoux3d/starter/' + name)), 'importer': 'Texture',
                'settings': {'srgb': True, 'normalMap': False, 'compression': 'None'}}
        with open(os.path.join(OUT, 'LUTs', name + '.png.zmeta'), 'w', encoding='utf-8', newline='\n') as f:
            json.dump(meta, f, indent=2)


if __name__ == '__main__':
    bricks()
    cobbles()
    luts()
