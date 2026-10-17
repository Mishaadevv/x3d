"""Builds the Starter Content slime (morph targets + skin + animation): Engine/Content/StarterContent/Characters/SK_Blob.glb.

A soft blob with eyes on a one-bone armature. Shape keys: Squash, Stretch (whole body, eyes follow) and Blink (eyes).
Clips (bone + shape key weights merged by NLA track name): Idle (breathing, a blink) and Bounce (hop with squash and
stretch).

    "C:/Program Files/Blender Foundation/Blender 4.5/blender.exe" -b --factory-startup \
        --python Tools/make_starter_blob.py -- Engine/Content/StarterContent/Characters/SK_Blob.glb
"""

import sys

import bmesh
import bpy
from mathutils import Vector

out = sys.argv[sys.argv.index('--') + 1]

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = 30


def material(name, color, roughness, subsurface=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes['Principled BSDF']
    bsdf.inputs['Base Color'].default_value = (*color, 1.0)
    bsdf.inputs['Roughness'].default_value = roughness
    return m


def sphere(name, center, radius, scale, mat, segments=40, rings=24):
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=segments, v_segments=rings, radius=radius)
    for v in bm.verts:
        v.co = Vector((v.co.x * scale[0], v.co.y * scale[1], v.co.z * scale[2])) + Vector(center)
    bm.to_mesh(mesh)
    bm.free()
    for p in mesh.polygons:
        p.use_smooth = True
    obj = bpy.data.objects.new(name, mesh)
    scene.collection.objects.link(obj)
    obj.data.materials.append(mat)
    return obj


slime = material('M_Slime', (0.18, 0.75, 0.32), 0.25)
white = material('M_EyeWhite', (0.95, 0.95, 0.95), 0.3)
black = material('M_Pupil', (0.02, 0.02, 0.025), 0.15)

# Blender: Z up, front = -Y. The blob sits on the ground (z = 0).
body = sphere('Body', (0, 0, 0.42), 0.5, (1.0, 1.0, 0.84), slime)
eyes = []
for side in (-1, 1):
    eyes.append(('eye', sphere('EyeWhite', (0.16 * side, -0.4, 0.55), 0.1, (1.0, 0.6, 1.15), white, 24, 16), (0.16 * side, -0.4, 0.55)))
    eyes.append(('eye', sphere('Pupil', (0.16 * side, -0.455, 0.54), 0.05, (1.0, 0.6, 1.15), black, 16, 12), (0.16 * side, -0.4, 0.55)))


def squash(p, amount):
    # Wider and lower, the bottom stays on the ground.
    s = 1.0 + 0.3 * amount
    return Vector((p.x * s, p.y * s, p.z * (1.0 - 0.38 * amount)))


def stretch(p, amount):
    s = 1.0 - 0.18 * amount
    return Vector((p.x * s, p.y * s, p.z * (1.0 + 0.35 * amount)))


def add_keys(obj, eye_center=None):
    obj.shape_key_add(name='Basis', from_mix=False)
    sq = obj.shape_key_add(name='Squash', from_mix=False)
    st = obj.shape_key_add(name='Stretch', from_mix=False)
    bl = obj.shape_key_add(name='Blink', from_mix=False)
    for i, v in enumerate(obj.data.vertices):
        sq.data[i].co = squash(v.co, 1.0)
        st.data[i].co = stretch(v.co, 1.0)
        if eye_center is not None:  # eyelids close: flatten towards the eye's horizontal center line
            c = Vector(eye_center)
            bl.data[i].co = Vector((v.co.x, v.co.y, c.z + (v.co.z - c.z) * 0.08))


add_keys(body)
for _, obj, center in eyes:
    add_keys(obj, center)

# Join (shape keys merge by name), skin to one bone.
bpy.ops.object.select_all(action='DESELECT')
for _, obj, _c in eyes:
    obj.select_set(True)
body.select_set(True)
bpy.context.view_layer.objects.active = body
bpy.ops.object.join()
blob = bpy.context.view_layer.objects.active
blob.name = 'Blob'
blob.data.name = 'Blob'

arm_data = bpy.data.armatures.new('BlobRig')
rig = bpy.data.objects.new('BlobRig', arm_data)
scene.collection.objects.link(rig)
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='EDIT')
bone = arm_data.edit_bones.new('Root')
bone.head = (0, 0, 0)
bone.tail = (0, 0, 0.4)
bpy.ops.object.mode_set(mode='OBJECT')
group = blob.vertex_groups.new(name='Root')
group.add(list(range(len(blob.data.vertices))), 1.0, 'REPLACE')
blob.parent = rig
mod = blob.modifiers.new('Armature', 'ARMATURE')
mod.object = rig

keys = blob.data.shape_keys
keys.use_relative = True


def clip(name, frames, bone_keys, shape_keys):
    """bone_keys: [(frame, height)], shape_keys: {key: [(frame, value)]}; one NLA track named `name` on each owner."""
    rig.animation_data_create()
    keys.animation_data_create()
    pb = rig.pose.bones['Root']
    for f, h in bone_keys:
        pb.location = (0, h, 0)  # bone space: Y along the bone = up
        pb.keyframe_insert('location', frame=f)
    for k, points in shape_keys.items():
        block = keys.key_blocks[k]
        for f, v in points:
            block.value = v
            block.keyframe_insert('value', frame=f)
    for owner in (rig, keys):
        action = owner.animation_data.action
        action.name = name
        track = owner.animation_data.nla_tracks.new()
        track.name = name
        strip = track.strips.new(name, int(action.frame_range[0]), action)
        strip.name = name
        owner.animation_data.action = None
    for k in shape_keys:
        keys.key_blocks[k].value = 0.0
    pb.location = (0, 0, 0)


# Idle: breathing over 2 s, a blink at 1.3 s.
clip('Idle', 60,
     [(1, 0.0), (61, 0.0)],
     {'Squash': [(1, 0.0), (31, 0.18), (61, 0.0)],
      'Stretch': [(1, 0.08), (31, 0.0), (61, 0.08)],
      'Blink': [(1, 0.0), (38, 0.0), (41, 1.0), (44, 0.0), (61, 0.0)]})
# Bounce: land (squash), push off (stretch), hang, fall, land - one second.
clip('Bounce', 30,
     [(1, 0.0), (5, 0.0), (10, 0.55), (16, 0.85), (23, 0.5), (28, 0.05), (31, 0.0)],
     {'Squash': [(1, 0.9), (5, 0.6), (9, 0.0), (26, 0.0), (29, 0.5), (31, 0.9)],
      'Stretch': [(1, 0.0), (6, 0.0), (10, 0.9), (16, 0.15), (23, 0.6), (27, 0.7), (29, 0.0), (31, 0.0)],
      'Blink': [(1, 0.6), (4, 0.0), (28, 0.0), (31, 0.6)]})

bpy.ops.object.select_all(action='SELECT')
bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', export_animations=True, export_animation_mode='NLA_TRACKS',
                          export_skins=True, export_morph=True, export_morph_normal=True, export_yup=True, export_apply=False)
print('Exported', out)
