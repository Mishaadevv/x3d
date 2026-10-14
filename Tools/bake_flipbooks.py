"""Bakes particle flipbooks from a 2D fluid simulation (stable fluids) into Engine/Content/StarterContent/Flipbooks.

    python Tools/bake_flipbooks.py

Each flipbook is an 8 x 8 atlas of 128 px frames (1024 x 1024), played left to right, top to bottom:
    T_Flipbook_Fire.png   r = temperature (flame brightness / colour ramp), a = density
    T_Flipbook_Smoke.png  r = g = b = smoke density lighting term, a = density
The fire is a licking flame (hot gas rising from a flickering source, vortices); the smoke is a billowing puff that
grows and thins out. Frames are taken from the running simulation, so the fire loops reasonably well.
"""

import json
import os
import uuid

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'Engine', 'Content', 'StarterContent', 'Flipbooks')
N = 128          # grid
FRAMES = 64
ATLAS = 8


def sample(field, x, y):
    """Bilinear sample of `field` at fractional cell coordinates (clamped)."""
    x = np.clip(x, 0, N - 1.001)
    y = np.clip(y, 0, N - 1.001)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    a = field[y0, x0] * (1 - fx) + field[y0, x0 + 1] * fx
    b = field[y0 + 1, x0] * (1 - fx) + field[y0 + 1, x0 + 1] * fx
    return a * (1 - fy) + b * fy


YY, XX = np.mgrid[0:N, 0:N].astype(np.float64)


def advect(field, u, v, dt):
    return sample(field, XX - u * dt, YY - v * dt)


def project(u, v, iterations=40):
    div = 0.5 * ((np.roll(u, -1, 1) - np.roll(u, 1, 1)) + (np.roll(v, -1, 0) - np.roll(v, 1, 0)))
    p = np.zeros_like(u)
    for _ in range(iterations):
        p = (np.roll(p, 1, 0) + np.roll(p, -1, 0) + np.roll(p, 1, 1) + np.roll(p, -1, 1) - div) / 4.0
        p[0, :] = p[-1, :] = 0  # open top / bottom
        p[:, 0] = p[:, -1] = 0
    u -= 0.5 * (np.roll(p, -1, 1) - np.roll(p, 1, 1))
    v -= 0.5 * (np.roll(p, -1, 0) - np.roll(p, 1, 0))
    return u, v


def vorticity(u, v, strength, dt):
    w = 0.5 * ((np.roll(v, -1, 1) - np.roll(v, 1, 1)) - (np.roll(u, -1, 0) - np.roll(u, 1, 0)))
    aw = np.abs(w)
    gx = 0.5 * (np.roll(aw, -1, 1) - np.roll(aw, 1, 1))
    gy = 0.5 * (np.roll(aw, -1, 0) - np.roll(aw, 1, 0))
    mag = np.sqrt(gx * gx + gy * gy) + 1e-5
    gx, gy = gx / mag, gy / mag
    u += gy * w * strength * dt
    v += -gx * w * strength * dt
    return u, v


def simulate(kind, seed):
    rng = np.random.default_rng(seed)
    u = np.zeros((N, N))
    v = np.zeros((N, N))  # +y = down in image space; buoyancy pushes towards row 0 (up)
    dens = np.zeros((N, N))
    temp = np.zeros((N, N))
    dt = 1.0
    frames = []
    warmup = 80 if kind == 'fire' else 0
    steps_per_frame = 2
    total = warmup + FRAMES * steps_per_frame
    for step in range(total):
        if kind == 'fire':
            # Flickering source at the bottom centre.
            cx = N / 2 + rng.normal(0, 3)
            r = 18 + rng.normal(0, 2)
            src = np.exp(-(((XX - cx) / r) ** 2 + ((YY - (N - 14)) / 6.0) ** 2))
            temp = np.maximum(temp, src * (0.8 + 0.4 * rng.random()))
            dens += src * 0.35
            v -= src * 1.2
            u += src * rng.normal(0, 0.6)
        elif step < 18:
            # Smoke: a lumpy puff (noise breaks the symmetry), pushed up with random swirls, then left to billow.
            lumps = 0.5 + value_noise_grid(rng)
            src = np.exp(-(((XX - N / 2) / 18.0) ** 2 + ((YY - N * 0.62) / 14.0) ** 2)) * lumps
            dens += src * 0.22
            temp = np.maximum(temp, src * 0.3)
            v -= src * 0.15
            u += src * rng.normal(0, 1.0, (N, N)) * 0.6
            v += src * rng.normal(0, 1.0, (N, N)) * 0.4
        # Buoyancy, vortices, projection, transport.
        v -= temp * (0.35 if kind == 'fire' else 0.04) * dt
        u, v = vorticity(u, v, 0.45 if kind == 'fire' else 0.3, dt)
        u, v = project(u, v)
        u2 = advect(u, u, v, dt)
        v2 = advect(v, u, v, dt)
        u, v = u2 * 0.995, v2 * 0.995
        dens = advect(dens, u, v, dt) * (0.985 if kind == 'fire' else 0.994)
        temp = advect(temp, u, v, dt) * (0.93 if kind == 'fire' else 0.95)
        # Closed, empty border: nothing wraps around (np.roll is periodic).
        for f in (u, v, dens, temp):
            f[:2, :] = 0
            f[-2:, :] = 0
            f[:, :2] = 0
            f[:, -2:] = 0
        if step >= warmup and (step - warmup) % steps_per_frame == 0:
            frames.append((dens.copy(), temp.copy()))
    return frames


def value_noise_grid(rng, cells=8):
    r = rng.random((cells + 1, cells + 1))
    x = XX / N * cells
    y = YY / N * cells
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a = r[y0, x0] * (1 - fx) + r[y0, x0 + 1] * fx
    b = r[y0 + 1, x0] * (1 - fx) + r[y0 + 1, x0 + 1] * fx
    return a * (1 - fy) + b * fy


def blur(f):
    for _ in range(2):
        f = (f + np.roll(f, 1, 0) + np.roll(f, -1, 0) + np.roll(f, 1, 1) + np.roll(f, -1, 1)) / 5.0
    return f


def edge_mask():
    # Fade to zero at the sprite border.
    d = np.maximum(np.abs(XX - N / 2), np.abs(YY - N / 2)) / (N / 2)
    return np.clip((1.0 - d) / 0.12, 0, 1)


def bake(kind):
    frames = simulate(kind, 3 if kind == 'fire' else 5)
    atlas = np.zeros((N * ATLAS, N * ATLAS, 4))
    mask = edge_mask()
    for i, (dens, temp) in enumerate(frames[:FRAMES]):
        row, col = divmod(i, ATLAS)
        if kind == 'fire':
            heat = np.clip(temp * 1.4, 0, 1)
            alpha = np.clip(np.maximum(heat, dens * 0.6), 0, 1) * mask
            rgb = np.stack([heat, heat, heat], -1)
        else:
            d = blur(np.clip(dens, 0, None))
            alpha = np.clip(d * 1.6, 0, 1) ** 1.2 * mask
            # Soft self-shadowing from above: lighter on top of the puff, never darker than half.
            above = np.cumsum(d, axis=0) * 0.03
            light = 0.5 + 0.5 * np.exp(-above)
            rgb = np.stack([light, light, light], -1)
        atlas[row * N:(row + 1) * N, col * N:(col + 1) * N, :3] = rgb
        atlas[row * N:(row + 1) * N, col * N:(col + 1) * N, 3] = alpha
    os.makedirs(OUT, exist_ok=True)
    name = 'T_Flipbook_' + ('Fire' if kind == 'fire' else 'Smoke')
    Image.fromarray(np.clip(atlas * 255 + 0.5, 0, 255).astype(np.uint8), 'RGBA').save(os.path.join(OUT, name + '.png'))
    meta = {'guid': str(uuid.uuid5(uuid.NAMESPACE_URL, 'zeqoux3d/starter/' + name)), 'importer': 'Texture',
            'settings': {'srgb': False, 'normalMap': False, 'compression': 'BC7'}}
    with open(os.path.join(OUT, name + '.png.zmeta'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(meta, f, indent=2)
    print(name)


if __name__ == '__main__':
    bake('fire')
    bake('smoke')
