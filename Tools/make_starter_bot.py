"""Builds the Starter Content robot: Engine/Content/StarterContent/Characters/SK_Bot.glb.

A stocky robot whose skeleton differs from SK_Human on purpose, to show animation retargeting: Unreal-style bone names
(pelvis, spine_01, thigh_l, ...), a root bone under the pelvis, two spine bones instead of three, an A-pose (arms 45
degrees down) instead of a T-pose, other proportions (short legs, long arms) and random bone rolls (local axes). It has
no clips of its own: its Animator plays SK_Human's clips retargeted.

    "C:/Program Files/Blender Foundation/Blender 4.5/blender.exe" -b --factory-startup \
        --python Tools/make_starter_bot.py -- Engine/Content/StarterContent/Characters/SK_Bot.glb
"""

import math
import random
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

out = sys.argv[sys.argv.index('--') + 1]

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
random.seed(20261116)


def material(name, color, metallic, roughness, emission=None):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes['Principled BSDF']
    bsdf.inputs['Base Color'].default_value = (*color, 1.0)
    bsdf.inputs['Metallic'].default_value = metallic
    bsdf.inputs['Roughness'].default_value = roughness
    if emission:
        bsdf.inputs['Emission Color'].default_value = (*emission, 1.0)
        bsdf.inputs['Emission Strength'].default_value = 4.0
    return m


body = material('M_BotBody', (0.62, 0.65, 0.7), 0.85, 0.32)
dark = material('M_BotJoint', (0.08, 0.085, 0.1), 0.6, 0.45)
accent = material('M_BotAccent', (1.0, 0.42, 0.08), 0.2, 0.38)
visor = material('M_BotVisor', (0.02, 0.05, 0.06), 0.0, 0.15, emission=(0.1, 0.85, 1.0))

# Blender: Z up, the robot faces -Y, so its left side is +X. (name, parent, head, tail)
L = 1
BONES = [
    ('root', None, (0, 0, 0), (0, -0.25, 0)),
    ('pelvis', 'root', (0, 0, 0.85), (0, 0, 0.97)),
    ('spine_01', 'pelvis', (0, 0, 0.97), (0, 0, 1.18)),
    ('spine_02', 'spine_01', (0, 0, 1.18), (0, 0, 1.46)),
    ('neck_01', 'spine_02', (0, 0, 1.46), (0, 0, 1.55)),
    ('head', 'neck_01', (0, 0, 1.55), (0, 0, 1.82)),
]
for side, s in (('l', 1), ('r', -1)):
    BONES += [
        (f'clavicle_{side}', 'spine_02', (0.06 * s, 0, 1.4), (0.24 * s, 0, 1.4)),
        (f'upperarm_{side}', f'clavicle_{side}', (0.24 * s, 0, 1.4), (0.47 * s, 0, 1.17)),
        (f'lowerarm_{side}', f'upperarm_{side}', (0.47 * s, 0, 1.17), (0.69 * s, 0, 0.95)),
        (f'hand_{side}', f'lowerarm_{side}', (0.69 * s, 0, 0.95), (0.79 * s, 0, 0.85)),
        (f'thigh_{side}', 'pelvis', (0.13 * s, 0, 0.85), (0.13 * s, 0, 0.48)),
        (f'calf_{side}', f'thigh_{side}', (0.13 * s, 0, 0.48), (0.13 * s, 0.02, 0.1)),
        (f'foot_{side}', f'calf_{side}', (0.13 * s, 0.02, 0.1), (0.13 * s, -0.14, 0.03)),
        (f'ball_{side}', f'foot_{side}', (0.13 * s, -0.14, 0.03), (0.13 * s, -0.23, 0.03)),
    ]

arm_data = bpy.data.armatures.new('BotRig')
rig = bpy.data.objects.new('BotRig', arm_data)
scene.collection.objects.link(rig)
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='EDIT')
for name, parent, head, tail in BONES:
    b = arm_data.edit_bones.new(name)
    b.head = head
    b.tail = tail
    b.roll = random.uniform(-math.pi, math.pi)  # local axes unlike any other rig
    if parent:
        b.parent = arm_data.edit_bones[parent]
        b.use_connect = False
bpy.ops.object.mode_set(mode='OBJECT')
heads = {n: Vector(h) for n, _p, h, _t in BONES}
tails = {n: Vector(t) for n, _p, _h, t in BONES}

parts = []


def finish(name, bm, bone, mat, smooth=False):
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    for p in mesh.polygons:
        p.use_smooth = smooth
    obj = bpy.data.objects.new(name, mesh)
    scene.collection.objects.link(obj)
    obj.data.materials.append(mat)
    group = obj.vertex_groups.new(name=bone)
    group.add(list(range(len(mesh.vertices))), 1.0, 'REPLACE')
    parts.append(obj)


def box(bone, center, size, mat, bevel=0.018, matrix=None):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co = Vector((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2]))
    bmesh.ops.bevel(bm, geom=list(bm.edges), offset=min(bevel, min(size) * 0.3), segments=2, affect='EDGES')
    m = matrix if matrix is not None else Matrix.Identity(4)
    for v in bm.verts:
        v.co = m @ v.co + Vector(center)
    finish(f'{bone}_box', bm, bone, mat)


def sphere(bone, center, radius, mat):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=20, v_segments=12, radius=radius)
    for v in bm.verts:
        v.co = v.co + Vector(center)
    finish(f'{bone}_sphere', bm, bone, mat, smooth=True)


def limb(bone, width, depth, mat, start=0.06, end=0.94):
    """Box from the bone's head to its tail (inset at both ends so the joint spheres show)."""
    h, t = heads[bone], tails[bone]
    d = t - h
    rot = d.normalized().to_track_quat('Z', 'Y').to_matrix().to_4x4()
    length = d.length * (end - start)
    center = h + d * ((start + end) * 0.5)
    box(bone, center, (width, depth, length), mat, matrix=rot)


# Torso.
box('pelvis', (0, 0, 0.9), (0.36, 0.24, 0.16), body)
box('spine_01', (0, 0, 1.07), (0.28, 0.19, 0.2), dark)
box('spine_02', (0, 0, 1.31), (0.52, 0.32, 0.32), body, bevel=0.04)
box('spine_02', (0, -0.165, 1.33), (0.3, 0.03, 0.17), accent)
box('spine_02', (0, 0.17, 1.3), (0.34, 0.06, 0.22), dark)  # back pack
box('neck_01', (0, 0, 1.5), (0.1, 0.1, 0.1), dark)
box('head', (0, 0, 1.67), (0.29, 0.27, 0.26), body, bevel=0.05)
box('head', (0, -0.136, 1.69), (0.23, 0.02, 0.075), visor, bevel=0.008)
box('head', (0.09, 0.02, 1.84), (0.025, 0.025, 0.1), accent, bevel=0.006)
sphere('head', (0.09, 0.02, 1.9), 0.025, visor)
for side, s in (('l', 1), ('r', -1)):
    sphere(f'clavicle_{side}', (0.27 * s, 0, 1.41), 0.105, accent)
    limb(f'upperarm_{side}', 0.12, 0.12, body)
    sphere(f'lowerarm_{side}', heads[f'lowerarm_{side}'], 0.07, dark)
    limb(f'lowerarm_{side}', 0.11, 0.11, body)
    limb(f'hand_{side}', 0.13, 0.07, dark, start=0.0, end=1.1)
    sphere(f'thigh_{side}', heads[f'thigh_{side}'] + Vector((0, 0, -0.02)), 0.09, dark)
    limb(f'thigh_{side}', 0.16, 0.16, body)
    sphere(f'calf_{side}', heads[f'calf_{side}'], 0.085, accent)
    limb(f'calf_{side}', 0.14, 0.14, body)
    box(f'foot_{side}', (0.13 * s, -0.05, 0.055), (0.16, 0.26, 0.11), dark)
    box(f'ball_{side}', (0.13 * s, -0.21, 0.035), (0.15, 0.08, 0.07), accent)

bpy.ops.object.select_all(action='DESELECT')
for p in parts:
    p.select_set(True)
bpy.context.view_layer.objects.active = parts[0]
bpy.ops.object.join()
bot = bpy.context.view_layer.objects.active
bot.name = 'Bot'
bot.data.name = 'Bot'
bot.parent = rig
mod = bot.modifiers.new('Armature', 'ARMATURE')
mod.object = rig

bpy.ops.object.select_all(action='SELECT')
bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', export_animations=False, export_skins=True, export_yup=True,
                          export_apply=False)
print('Exported', out)
