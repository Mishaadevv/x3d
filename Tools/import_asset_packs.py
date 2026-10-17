"""Builds the optional asset packs in Engine/Content/Packs from the Quaternius archives (CC0).

    python Tools/import_asset_packs.py <dir with the zips>

Each pack becomes Engine/Content/Packs/<Folder>/ with Pack.json (name, description, author, license), License.txt,
Preview.jpg (when the archive has one) and the models. New projects copy the packs the user picks into
Content/Packs/<Folder> (Project Browser -> Asset Packs).

  Survival Pack       .blend files exported to .glb with Blender (flat-colored materials, one file per prop), scaled
                      to real-world sizes (SURVIVAL_SIZES) with the pivot at the bottom
  Medieval Village    the glTF folder as is (.gltf + .bin + shared PNG textures)
  Stylized Nature     every .glb embeds its own copy of the shared textures: they are written once to Textures/ and the
                      models become .gltf + .bin referencing them (86 MB -> a fraction); hashed duplicate names
                      ("Pine-699sFuLCN2") become numbered variants (Pine_2)
"""
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACKS = os.path.join(ROOT, 'Engine', 'Content', 'Packs')
BLENDER = r'C:\Program Files\Blender Foundation\Blender 4.5\blender.exe'

CC0 = ('CC0 1.0 Universal (CC0 1.0) Public Domain Dedication\n'
       'https://creativecommons.org/publicdomain/zero/1.0/\n')

INFO = {
    'SurvivalPack': {
        'zip': 'Survival Pack - Sept 2020',
        'name': 'Survival Pack',
        'description': 'Low-poly survival props: weapons, tools, food cans, first aid, a tent, a raft, a campfire, '
                       'torches and more (53 models).',
    },
    'MedievalVillage': {
        'zip': 'Medieval Village MegaKit',
        'name': 'Medieval Village MegaKit',
        'description': 'Modular medieval houses: walls, floors, roofs, doors, windows, balconies, stairs and props with '
                       'PBR textures - snap pieces together to build a village.',
    },
    'StylizedNature': {
        'zip': 'Stylized Nature MegaKit',
        'name': 'Stylized Nature MegaKit',
        'description': 'Stylized trees, pines, dead and twisted trees, bushes, grass, flowers, ferns, mushrooms, rocks '
                       'and path stones.',
    },
}

# The Survival Pack props come at arbitrary sizes (a 2.5 m axe, a 25 m tent): each is scaled so its longest side has a
# real-world length (meters) and its bottom sits at the origin.
SURVIVAL_SIZES = {
    'Axe': 0.8, 'Axe_Small': 0.5, 'Backpack': 0.6, 'Bandages': 0.1, 'Battery_Big': 0.07, 'Battery_Small': 0.05,
    'BearTrap_Closed': 0.45, 'BearTrap_Open': 0.45, 'Bonfire': 1.0, 'Bonfire_Fire': 1.06, 'Can_Broken': 0.12,
    'Can_Closed': 0.12, 'Can_Open': 0.12, 'Can_Red': 0.12, 'Compass_Closed': 0.07, 'Compass_Open': 0.08,
    'FirstAidKit': 0.35, 'FirstAidKit_Hard': 0.4, 'FlareGun': 0.25, 'GasCan': 0.45, 'Knife': 0.25, 'Match': 0.05,
    'Match_Burnt': 0.05, 'Match_Fire': 0.06, 'Matchbox': 0.06, 'Pan': 0.45, 'Pan_Small': 0.35, 'Phone': 0.15,
    'Pistol_1': 0.2, 'Pistol_2': 0.2, 'Pot': 0.35, 'Pot_Small': 0.25, 'PropaneTank': 0.6, 'Radio': 0.3, 'Raft': 4.0,
    'Raft_Paddle': 1.6, 'Revolver_1': 0.25, 'Revolver_2': 0.22, 'Revolver_3': 0.25, 'Shotgun_1': 1.1, 'Shotgun_2': 1.2,
    'Shotgun_SawedOff': 0.75, 'Shotgun_ShortStock': 0.95, 'Shovel': 1.1, 'Tent': 3.0, 'Torch': 0.25, 'Trashcan': 1.0,
    'WaterBottle_1': 0.25, 'WaterBottle_2': 0.25, 'WaterBottle_3': 0.2, 'WoodLog': 0.9, 'WoodenTorch': 0.6,
    'WoodenTorch_Fire': 0.8,
}

BLENDER_EXPORT = r'''
import bpy, json, sys
from mathutils import Vector
args = sys.argv[sys.argv.index('--') + 1:]
sizes = json.loads(args[0])
rest = args[1:]
for src, dst, name in zip(rest[0::3], rest[1::3], rest[2::3]):
    bpy.ops.wm.open_mainfile(filepath=src)
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    bpy.context.view_layer.update()
    pts = [o.matrix_world @ Vector(c) for o in meshes for c in o.bound_box]
    lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    target = sizes.get(name)
    f = target / max(hi - lo) if target else 1.0
    lift = -lo.z * f  # Blender is Z-up: the bottom goes to 0
    for o in bpy.context.scene.objects:
        if o.parent is None:
            o.location = o.location * f + Vector((0.0, 0.0, lift))
            o.scale = o.scale * f
    bpy.context.view_layer.update()
    bpy.ops.export_scene.gltf(filepath=dst, export_format='GLB', export_cameras=False, export_lights=False,
                              export_apply=True, export_animations=False)
'''


def find_zip(folder, prefix):
    for name in os.listdir(folder):
        if name.startswith(prefix) and name.endswith('.zip'):
            return os.path.join(folder, name)
    sys.exit(f'missing archive {prefix}*.zip in {folder}')


def safe_extract(zip_path, dst):
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            target = os.path.realpath(os.path.join(dst, info.filename))
            assert target.startswith(os.path.realpath(dst)), info.filename
        z.extractall(dst)


def write_pack_json(folder, key, count):
    info = INFO[key]
    with open(os.path.join(folder, 'Pack.json'), 'w', encoding='utf-8') as f:
        json.dump({'name': info['name'], 'description': info['description'], 'author': 'Quaternius',
                   'url': 'https://quaternius.com', 'license': 'CC0 1.0', 'models': count}, f, indent=2)
        f.write('\n')


def reset(folder):
    if os.path.isdir(folder):
        shutil.rmtree(folder)
    os.makedirs(folder)


def survival(src, dst):
    base = os.path.join(src, 'Survival Pack - Sept 2020')
    models = os.path.join(dst, 'Models')
    os.makedirs(models)
    blends = sorted(f for f in os.listdir(os.path.join(base, 'Blends')) if f.endswith('.blend'))
    script = os.path.join(tempfile.gettempdir(), 'zx_export_glb.py')
    with open(script, 'w') as f:
        f.write(BLENDER_EXPORT)
    args = [json.dumps(SURVIVAL_SIZES)]
    for b in blends:
        args += [os.path.join(base, 'Blends', b), os.path.join(models, b[:-6] + '.glb'), b[:-6]]
    subprocess.run([BLENDER, '-b', '--factory-startup', '-P', script, '--'] + args, check=True,
                   stdout=subprocess.DEVNULL)
    shutil.copy(os.path.join(base, 'License.txt'), os.path.join(dst, 'License.txt'))
    shutil.copy(os.path.join(base, 'Preview.jpg'), os.path.join(dst, 'Preview.jpg'))
    return len(blends)


def medieval(src, dst):
    base = os.path.join(src, 'Medieval Village MegaKit[Standard]')
    shutil.copytree(os.path.join(base, 'glTF'), os.path.join(dst, 'Models'))
    shutil.copy(os.path.join(base, 'License_Standard.txt'), os.path.join(dst, 'License.txt'))
    if os.path.exists(os.path.join(base, 'Preview.jpg')):
        shutil.copy(os.path.join(base, 'Preview.jpg'), os.path.join(dst, 'Preview.jpg'))
    return sum(1 for f in os.listdir(os.path.join(dst, 'Models')) if f.endswith('.gltf'))


def clean_name(stem, used):
    base = re.sub(r'-[A-Za-z0-9]{10}$', '', stem).strip().replace(' ', '_')
    n, name = 1, base
    while name in used:
        n += 1
        name = f'{base}_{n}'
    used.add(name)
    return name


def nature(src, dst):
    models = os.path.join(dst, 'Models')
    textures = os.path.join(models, 'Textures')
    os.makedirs(textures)
    written = {}  # texture name -> sha1
    used = set()
    files = sorted(f for f in os.listdir(src) if f.endswith('.glb'))
    # Plain names first so "Pine.glb" stays Pine and the hashed copies become Pine_2, Pine_3...
    files.sort(key=lambda f: (re.search(r'-[A-Za-z0-9]{10}\.glb$', f) is not None, f))
    for f in files:
        data = open(os.path.join(src, f), 'rb').read()
        magic, _, _ = struct.unpack('<III', data[:12])
        assert magic == 0x46546C67
        json_len = struct.unpack('<I', data[12:16])[0]
        gltf = json.loads(data[20:20 + json_len])
        bin_off = 20 + json_len
        bin_len = struct.unpack('<I', data[bin_off:bin_off + 4])[0]
        blob = data[bin_off + 8:bin_off + 8 + bin_len]
        name = clean_name(f[:-4], used)
        image_views = set()
        for img in gltf.get('images', []):
            view = gltf['bufferViews'][img['bufferView']]
            pixels = blob[view.get('byteOffset', 0):view.get('byteOffset', 0) + view['byteLength']]
            tex = img.get('name') or f'{name}_{len(image_views)}.png'
            if not tex.lower().endswith('.png'):
                tex += '.png'
            digest = hashlib.sha1(pixels).hexdigest()
            if tex in written and written[tex] != digest:  # same name, different pixels
                tex = tex[:-4] + '_' + digest[:6] + '.png'
            if tex not in written:
                open(os.path.join(textures, tex), 'wb').write(pixels)
                written[tex] = digest
            image_views.add(img['bufferView'])
            img.pop('bufferView')
            img.pop('mimeType', None)
            img['uri'] = 'Textures/' + tex
        # The FBX -> glTF export left foliage as BLEND and everything 40 % metallic: leaves, grass and petals are cutouts
        # (alpha test, both sides) and nothing in the kit is metal.
        for mat in gltf.get('materials', []):
            mat.pop('extras', None)
            pbr = mat.setdefault('pbrMetallicRoughness', {})
            pbr['metallicFactor'] = 0.0
            if mat.get('alphaMode') == 'BLEND':
                mat['alphaMode'] = 'MASK'
                mat['alphaCutoff'] = 0.5
                mat['doubleSided'] = True
        # Rebuild the buffer without the image views (indices of the remaining views are remapped).
        new_blob = bytearray()
        remap = {}
        views = []
        for i, view in enumerate(gltf['bufferViews']):
            if i in image_views:
                continue
            off = view.get('byteOffset', 0)
            while len(new_blob) % 4:
                new_blob.append(0)
            view = dict(view)
            view['byteOffset'] = len(new_blob)
            new_blob += blob[off:off + view['byteLength']]
            remap[i] = len(views)
            views.append(view)
        gltf['bufferViews'] = views
        for acc in gltf.get('accessors', []):
            if 'bufferView' in acc:
                acc['bufferView'] = remap[acc['bufferView']]
        gltf['buffers'] = [{'byteLength': len(new_blob), 'uri': name + '.bin'}]
        open(os.path.join(models, name + '.bin'), 'wb').write(new_blob)
        with open(os.path.join(models, name + '.gltf'), 'w') as out:
            json.dump(gltf, out, separators=(',', ':'))
    with open(os.path.join(dst, 'License.txt'), 'w') as out:
        out.write('Stylized Nature MegaKit by @Quaternius (https://quaternius.com)\n\nLicense:\n' + CC0)
    return len(files)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    zips = sys.argv[1]
    os.makedirs(PACKS, exist_ok=True)
    for key, fn in (('SurvivalPack', survival), ('MedievalVillage', medieval), ('StylizedNature', nature)):
        with tempfile.TemporaryDirectory() as tmp:
            safe_extract(find_zip(zips, INFO[key]['zip']), tmp)
            dst = os.path.join(PACKS, key)
            reset(dst)
            count = fn(tmp, dst)
            write_pack_json(dst, key, count)
            print(f'{key}: {count} models')


if __name__ == '__main__':
    main()
