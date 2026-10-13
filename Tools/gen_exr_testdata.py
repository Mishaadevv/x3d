"""Generates OpenEXR test files and their reference decodes for the engine's EXR decoder tests.

Writes Tests/Data/Images/exr_<name>.exr (written by the OpenEXR library) and exr_<name>.rgbaf (the same file decoded
by OpenEXR, as float32 RGBA rows: R, G, B from the R/G/B channels or from Y, A = 1 when there is no alpha channel).
The engine decodes the .exr with its own decoder and must match the reference exactly.
Tool-only dependency: `pip install OpenEXR numpy`.
"""
import os
import numpy as np
import OpenEXR

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'Tests', 'Data', 'Images')
os.makedirs(OUT, exist_ok=True)
rng = np.random.default_rng(4321)

C = OpenEXR.Compression


def pattern(w, h, hdr=True):
    """Smooth gradients + bright spots + a little noise (exercises the PIZ wavelet with both value ranges)."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    r = 0.5 + 0.5 * np.sin(x * 0.21) * np.cos(y * 0.17)
    g = (x + y) / float(w + h)
    b = 0.25 + 0.2 * np.sin((x - y) * 0.11)
    if hdr:
        spot = np.exp(-(((x - w * 0.7) ** 2 + (y - h * 0.3) ** 2) / 6.0)) * 2000.0
        r = r + spot
        g = g + spot * 0.9
        b = b + spot * 0.7
    noise = rng.standard_normal((3, h, w)) * 0.01
    return [np.maximum(c + n, 0.0) for c, n in zip((r, g, b), noise)]


def write(name, header, channels):
    path = os.path.join(OUT, 'exr_%s.exr' % name)
    with OpenEXR.File(header, channels) as f:
        f.write(path)
    # Reference: decode what was written with OpenEXR itself.
    with OpenEXR.File(path, separate_channels=True) as f:
        ch = f.channels()
        dw = f.header()['dataWindow']
        w = int(dw[1][0] - dw[0][0] + 1)
        h = int(dw[1][1] - dw[0][1] + 1)

        def get(n):
            return ch[n].pixels.astype(np.float32) if n in ch else None
        if 'Y' in ch and 'R' not in ch:
            yv = get('Y')
            rgb = [yv, yv, yv]
        else:
            rgb = [get(n) if n in ch else np.zeros((h, w), np.float32) for n in 'RGB']
        a = get('A') if 'A' in ch else np.ones((h, w), np.float32)
        rgba = np.stack(rgb + [a], axis=-1).astype('<f4')
        rgba.tofile(os.path.join(OUT, 'exr_%s.rgbaf' % name))
    print('%-22s %4dx%-4d %7d bytes' % (name, w, h, os.path.getsize(path)))


def scanline(compression, **extra):
    h = {'compression': compression, 'type': OpenEXR.scanlineimage}
    h.update(extra)
    return h


W, H = 37, 23  # odd sizes: partial blocks, odd wavelet rows/columns
r, g, b = pattern(W, H)
half = lambda c: c.astype(np.float16)
f32 = lambda c: c.astype(np.float32)

# Every scanline compression on RGB half data.
for name, comp in [('none', C.NO_COMPRESSION), ('rle', C.RLE_COMPRESSION), ('zips', C.ZIPS_COMPRESSION),
                   ('zip', C.ZIP_COMPRESSION), ('piz', C.PIZ_COMPRESSION), ('pxr24', C.PXR24_COMPRESSION)]:
    write(name + '_half', scanline(comp), {'R': half(r), 'G': half(g), 'B': half(b)})

# Float channels (PIZ treats each 32-bit value as two 16-bit words; PXR24 rounds floats to 24 bits).
for name, comp in [('zip', C.ZIP_COMPRESSION), ('piz', C.PIZ_COMPRESSION), ('pxr24', C.PXR24_COMPRESSION)]:
    write(name + '_float', scanline(comp), {'R': f32(r), 'G': f32(g), 'B': f32(b), 'A': f32(g * 0.5)})

# Mixed pixel types incl. a UINT channel that is ignored, RGBA.
ids = (np.arange(W * H, dtype=np.uint32).reshape(H, W) * 2654435761) & 0xFFFFFF
write('piz_mixed', scanline(C.PIZ_COMPRESSION), {'R': half(r), 'G': f32(g), 'B': half(b), 'A': half(np.clip(g, 0, 1)), 'id': ids})
write('zip_mixed', scanline(C.ZIP_COMPRESSION), {'R': half(r), 'G': f32(g), 'B': half(b), 'A': half(np.clip(g, 0, 1)), 'id': ids})

# Luminance only.
write('zip_y', scanline(C.ZIP_COMPRESSION), {'Y': half(g)})

# Larger image: several PIZ blocks of 32 lines, noisy data (16-bit wavelet path).
bw, bh = 200, 70
br, bg, bb = pattern(bw, bh)
noise = rng.random((bh, bw)) * 60000.0
write('piz_large', scanline(C.PIZ_COMPRESSION), {'R': half(br), 'G': half(noise), 'B': half(bb)})
write('zip_large', scanline(C.ZIP_COMPRESSION), {'R': half(br), 'G': half(bg), 'B': half(bb)})

# Data window not at the origin, decreasing line order.
write('zip_window', scanline(C.ZIP_COMPRESSION, dataWindow=((5, -3), (5 + W - 1, -3 + H - 1)),
                             displayWindow=((0, 0), (63, 63)), lineOrder=OpenEXR.DECREASING_Y),
      {'R': half(r), 'G': half(g), 'B': half(b)})

# Tiled, single level (the Python bindings can't write mip chains).
for name, comp, mode in [('tiled_zip', C.ZIP_COMPRESSION, OpenEXR.ONE_LEVEL), ('tiled_piz', C.PIZ_COMPRESSION, OpenEXR.ONE_LEVEL)]:
    tiles = OpenEXR.TileDescription()
    tiles.xSize, tiles.ySize, tiles.mode, tiles.roundingMode = 16, 8, mode, OpenEXR.ROUND_DOWN
    hd = {'compression': comp, 'type': OpenEXR.tiledimage, 'tiles': tiles}
    write(name, hd, {'R': half(r), 'G': half(g), 'B': half(b), 'A': half(np.clip(b, 0, 1))})
