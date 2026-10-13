"""Builds the ZeqouX3D application icons from the brand artwork (Engine/Content/Branding/Source):

  ZeqouX3D_Icon_Source.png  - the mark (white X, gold slash, cube) on a dark tile, no text
  ZeqouX3D_Logo_Source.png  - the same mark with the "ZeqouX3D" wordmark below

Outputs (the engine reads only these; it never needs this script or Pillow at runtime):
  Engine/Content/Icons/ZeqouX3D.ico          - Windows icon for ZXEditor.exe (16..256, PNG entries)
  Engine/Content/Icons/ZeqouX3D.png          - 256 x 256 app icon (title bar, Project Browser)
  Engine/Content/Branding/ZeqouX3D_Icon.png  - 512 x 512 app icon
  Engine/Content/Branding/ZeqouX3D_Logo.png  - 512 x 512 icon with the wordmark (About, splash)

The artwork is re-framed onto a clean rounded tile with transparent corners: the mark is cropped by its content
bounds and centered so it fills the same share of the tile at every size.
Usage: python Tools/import_brand_icons.py   (needs Pillow; a tool-only dependency)
"""
import io, os, struct
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'Engine', 'Content', 'Branding', 'Source')
BRANDING = os.path.join(ROOT, 'Engine', 'Content', 'Branding')
ICONS = os.path.join(ROOT, 'Engine', 'Content', 'Icons')

RIM = (0x2A, 0x2A, 0x2E)       # 1px rim so the tile reads on dark taskbars
RADIUS = 0.225                 # corner radius / size (Windows 11 / macOS style)


def content_box(im, threshold=110):
    """Bounding box of the bright artwork (ignores the dark tile and its faint edge)."""
    mask = im.convert('L').point(lambda v: 255 if v > threshold else 0)
    return mask.getbbox()


def framed(src, size, fill):
    """Artwork centered on a rounded tile; `fill` = share of the tile the artwork's larger side takes."""
    big = size * 4  # supersampled, downscaled at the end
    l, t, r, b = content_box(src)
    pad = (r - l) // 20  # keep a margin around the artwork for the feathered edge
    art = src.crop((max(0, l - pad), max(0, t - pad), min(src.width, r + pad), min(src.height, b + pad)))
    scale = fill * big / max(art.width, art.height)
    art = art.resize((max(1, round(art.width * scale)), max(1, round(art.height * scale))), Image.LANCZOS)
    # Tile color = the artwork's own background (median of the crop's border), so no seam shows around it.
    border = [art.getpixel((x, 0)) for x in range(0, art.width, 7)] + [art.getpixel((0, y)) for y in range(0, art.height, 7)]
    border.sort(key=sum)
    bg = border[len(border) // 2]
    tile = Image.new('RGBA', (big, big), bg + (255,))
    # Feathered edge: the crop blends into the tile instead of showing a rectangle.
    feather = max(2, min(art.width, art.height) // 25)
    fade = Image.new('L', art.size, 0)
    ImageDraw.Draw(fade).rectangle((feather, feather, art.width - 1 - feather, art.height - 1 - feather), fill=255)
    fade = fade.filter(ImageFilter.GaussianBlur(feather / 2))
    tile.paste(art, ((big - art.width) // 2, (big - art.height) // 2), fade)
    mask = Image.new('L', (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, big - 1, big - 1), radius=RADIUS * big, fill=255)
    rim = Image.new('L', (big, big), 0)
    w = max(4, big // 128)
    ImageDraw.Draw(rim).rounded_rectangle((0, 0, big - 1, big - 1), radius=RADIUS * big, outline=255, width=w)
    tile.paste(Image.new('RGBA', (big, big), RIM + (255,)), (0, 0), rim)
    tile.putalpha(mask)
    out = tile.resize((size, size), Image.LANCZOS)
    if size <= 32:
        out = out.filter(ImageFilter.UnsharpMask(radius=0.6, percent=60, threshold=0))
    return out


def write_ico(path, images):
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for im in images:
        buf = io.BytesIO()
        im.save(buf, 'PNG', optimize=True)
        data = buf.getvalue()
        w = im.width if im.width < 256 else 0
        entries.append(struct.pack('<BBBBHHII', w, w, 0, 0, 1, 32, len(data), offset))
        blobs.append(data)
        offset += len(data)
    with open(path, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 1, len(images)))
        for e in entries:
            f.write(e)
        for b in blobs:
            f.write(b)


def main():
    icon_src = Image.open(os.path.join(SRC, 'ZeqouX3D_Icon_Source.png')).convert('RGB')
    logo_src = Image.open(os.path.join(SRC, 'ZeqouX3D_Logo_Source.png')).convert('RGB')
    os.makedirs(ICONS, exist_ok=True)
    # Small sizes: the mark fills more of the tile so it stays readable at 16 px.
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    write_ico(os.path.join(ICONS, 'ZeqouX3D.ico'), [framed(icon_src, s, 0.80 if s <= 32 else 0.70) for s in sizes])
    framed(icon_src, 256, 0.70).save(os.path.join(ICONS, 'ZeqouX3D.png'), optimize=True)
    framed(icon_src, 512, 0.70).save(os.path.join(BRANDING, 'ZeqouX3D_Icon.png'), optimize=True)
    framed(logo_src, 512, 0.74).save(os.path.join(BRANDING, 'ZeqouX3D_Logo.png'), optimize=True)
    print('Wrote ZeqouX3D.ico, ZeqouX3D.png, Branding/ZeqouX3D_Icon.png, Branding/ZeqouX3D_Logo.png')


if __name__ == '__main__':
    main()
