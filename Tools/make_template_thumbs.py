"""Template thumbnails for the Project Browser from real editor captures.

Captures the viewport of each template (ZXEditor -screenshotviewport), crops it to the tile aspect and box-filters it
down to Engine/Content/Templates/<Template>.png. Pure Python (zlib only).

    python Tools/make_template_thumbs.py            # capture with Binaries/Win64/Development/ZXEditor.exe + convert
    python Tools/make_template_thumbs.py --convert  # only convert existing captures in Saved/Thumbs
    python Tools/make_template_thumbs.py physics    # only the named templates
"""
import os
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_W, OUT_H = 544, 320  # 1.7:1 like the template tiles, 2x for high-DPI screens
TEMPLATES = {  # -template value -> output name, extra console commands (camera framing)
    'blank': ('Blank', ''),
    'sample': ('Sample', ''),
    'firstperson': ('FirstPerson', ''),
    'thirdperson': ('ThirdPerson', ''),
    'water': ('Water', ''),
    'physics': ('Physics', ''),
    'vehicle': ('Vehicle', 'action view.gameUI'),  # the HUD would be cut off by the crop
}


def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and ctype in (2, 6) and interlace == 0, 'only 8-bit RGB/RGBA'
            bpp = 4 if ctype == 6 else 3
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bpp, out


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def thumbnail(src, dst):
    w, h, bpp, px = read_png(src)
    # Center crop to the output aspect, then box filter (area average) to the output size.
    aspect = OUT_W / OUT_H
    cw, ch = (w, int(w / aspect)) if w / h < aspect else (int(h * aspect), h)
    x0, y0 = (w - cw) // 2, (h - ch) // 2
    out = bytearray(OUT_W * OUT_H * 3)
    for oy in range(OUT_H):
        sy0, sy1 = y0 + oy * ch // OUT_H, max(y0 + (oy + 1) * ch // OUT_H, y0 + oy * ch // OUT_H + 1)
        for ox in range(OUT_W):
            sx0, sx1 = x0 + ox * cw // OUT_W, max(x0 + (ox + 1) * cw // OUT_W, x0 + ox * cw // OUT_W + 1)
            r = g = b = 0
            for sy in range(sy0, sy1):
                row = sy * w * bpp
                for sx in range(sx0, sx1):
                    i = row + sx * bpp
                    r += px[i]
                    g += px[i + 1]
                    b += px[i + 2]
            n = (sy1 - sy0) * (sx1 - sx0)
            o = (oy * OUT_W + ox) * 3
            out[o], out[o + 1], out[o + 2] = r // n, g // n, b // n
    write_png(dst, OUT_W, OUT_H, out)


def short_path(p):
    # The editor's command line splits at spaces: use the 8.3 form of the path.
    if os.name != 'nt':
        return p
    import ctypes
    buf = ctypes.create_unicode_buffer(1024)
    ctypes.windll.kernel32.GetShortPathNameW(p, buf, 1024)
    return (buf.value or p).replace('\\', '/')


def main():
    shots = os.path.join(ROOT, 'Saved', 'Thumbs')
    out_dir = os.path.join(ROOT, 'Engine', 'Content', 'Templates')
    os.makedirs(shots, exist_ok=True)
    os.makedirs(out_dir, exist_ok=True)
    only = [a for a in sys.argv[1:] if not a.startswith('--')]
    templates = {k: v for k, v in TEMPLATES.items() if not only or k in only}
    if '--convert' not in sys.argv:
        exe = os.path.join(ROOT, 'Binaries', 'Win64', 'Development', 'ZXEditor.exe')
        root = short_path(ROOT)
        for templ, (name, exec_cmds) in templates.items():
            project = os.path.join(ROOT, 'Saved', 'TestProjects', 'Shot_' + templ)
            shutil.rmtree(project, ignore_errors=True)
            args = [exe, f'-newproject={root}/Saved/TestProjects/Shot_{templ}', f'-template={templ}', '-nodocs', '-autotest=240',
                    '-resolution=1600x1000', '-resetlayout', '-screenshotviewport', f'-screenshot={root}/Saved/Thumbs/{templ}.png',
                    f'-recentfile={root}/Saved/TestProjects/recent.json']
            if exec_cmds:
                args.append(f'-exec={exec_cmds}')
            if templ == 'blank':
                args.append('-starter')  # the default in the New Project page: Starter Content showcase
            print(templ, subprocess.call(args))
    for templ, (name, _) in templates.items():
        src = os.path.join(shots, templ + '.png')
        if os.path.exists(src):
            thumbnail(src, os.path.join(out_dir, name + '.png'))
            print('->', name)


if __name__ == '__main__':
    main()
