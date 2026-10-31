"""Generates the FBX importer test data in Tests/Data/Models/Fbx.

One small rigged scene (a bent tube on a three-bone armature, two materials - one textured -, two actions) is built in
Blender and exported several ways; the tests import every FBX variant and compare it with the glTF export of the same
scene:

    Rig.glb          reference (glTF exporter)
    Rig.fbx          FBX 7.4, Blender defaults (Y up), texture embedded
    Rig_ZUp.fbx      FBX 7.4, Z up / Y forward, "apply scale: all" (unit scale 100), texture as a file next to it
    Rig_7500.fbx     Rig.fbx rewritten with FBX 7.5 node records (64-bit offsets) by this script
    (absolute paths of the generating machine are replaced by C:/FbxTestData in the files)
    RigTexture.png   the texture

Run (Blender 4.5):
    "C:/Program Files/Blender Foundation/Blender 4.5/blender.exe" -b --python Tools/gen_fbx_testdata.py
"""

import math
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "Tests", "Data", "Models", "Fbx")


def rewrite(src, dst, version, strings=None):
    """Re-encodes a binary FBX with the node record layout of `version` (64-bit offsets from 7500); `strings` maps
    string property values (absolute paths of the machine that generated the data must not end up in the repo)."""
    data = open(src, "rb").read()
    assert data[:20] == b"Kaydara FBX Binary  "
    wide_in = struct.unpack_from("<I", data, 23)[0] >= 7500
    wide_out = version >= 7500
    out = bytearray(data[:23] + struct.pack("<I", version))
    sizes = {"Y": 2, "C": 1, "I": 4, "F": 4, "D": 8, "L": 8}

    def props(p, count):
        blob = bytearray()
        for _ in range(count):
            t = chr(data[p])
            if t in sizes:
                blob += data[p:p + 1 + sizes[t]]
                p += 1 + sizes[t]
            elif t in "SR":
                n = struct.unpack_from("<I", data, p + 1)[0]
                value = data[p + 5:p + 5 + n]
                if t == "S" and strings:
                    value = strings(value)
                blob += t.encode() + struct.pack("<I", len(value)) + value
                p += 5 + n
            else:
                n = struct.unpack_from("<I", data, p + 9)[0]
                blob += data[p:p + 13 + n]
                p += 13 + n
        return p, blob

    def node(pos):
        if wide_in:
            end, nprops, _ = struct.unpack_from("<QQQ", data, pos)
            head = 25
        else:
            end, nprops, _ = struct.unpack_from("<III", data, pos)
            head = 13
        nlen = data[pos + head - 1]
        if end == 0:
            out.extend(b"\0" * (25 if wide_out else 13))
            return pos + head, False
        name = data[pos + head:pos + head + nlen]
        p, blob = props(pos + head + nlen, nprops)
        start = len(out)
        out.extend(b"\0" * (24 if wide_out else 12))
        out.append(nlen)
        out.extend(name)
        out.extend(blob)
        if wide_out:
            struct.pack_into("<QQ", out, start + 8, nprops, len(blob))
        else:
            struct.pack_into("<II", out, start + 4, nprops, len(blob))
        while p < end:
            p, more = node(p)
            if not more:
                break
        struct.pack_into("<Q" if wide_out else "<I", out, start, len(out))
        return end, True

    pos = 27
    while True:
        pos, more = node(pos)
        if not more:
            break
    out.extend(data[pos:])  # footer
    open(dst, "wb").write(out)


def strip_paths(value):
    """Machine-specific absolute paths -> a neutral one (the importer finds the files by name)."""
    text = value.decode("utf8", "replace")
    for prefix in (OUT, OUT.replace("\\", "/")):
        if prefix in text:
            text = text.replace(prefix, "C:\\FbxTestData")
    return text.encode("utf8")


def build_scene(bpy):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = 24
    scene.frame_start, scene.frame_end = 1, 25

    # Texture: 8x8 checker, saved next to the models.
    img = bpy.data.images.new("RigTexture", 8, 8)
    px = []
    for y in range(8):
        for x in range(8):
            c = 0.9 if (x + y) % 2 else 0.2
            px += [c, c * 0.8, 0.3, 1.0]
    img.pixels = px
    img.filepath_raw = os.path.join(OUT, "RigTexture.png")
    img.file_format = "PNG"
    img.save()

    red = bpy.data.materials.new("Red")
    red.use_nodes = True
    bsdf = red.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (0.8, 0.05, 0.05, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.4
    bsdf.inputs["Metallic"].default_value = 0.0
    checker = bpy.data.materials.new("Checker")
    checker.use_nodes = True
    bsdf = checker.node_tree.nodes["Principled BSDF"]
    tex = checker.node_tree.nodes.new("ShaderNodeTexImage")
    tex.image = img
    checker.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.7

    # Armature: Root -> Mid -> Tip along +Z (Blender is Z-up), placed off the origin, rotated and scaled.
    arm_data = bpy.data.armatures.new("RigArmature")
    arm = bpy.data.objects.new("Rig", arm_data)
    scene.collection.objects.link(arm)
    arm.location = (1.0, 0.5, 0.0)
    arm.rotation_euler = (0.0, 0.0, math.radians(30.0))
    arm.scale = (1.5, 1.5, 1.5)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    parent = None
    for name, z0, z1 in (("Root", 0.0, 0.6), ("Mid", 0.6, 1.2), ("Tip", 1.2, 1.8)):
        b = arm_data.edit_bones.new(name)
        b.head, b.tail = (0, 0, z0), (0, 0, z1)
        if parent:
            b.parent = parent
            b.use_connect = True
        parent = b
    bpy.ops.object.mode_set(mode="OBJECT")

    # Tube: 8 sides, 7 rings, quads; lower half Red, upper half Checker; cylindrical UVs.
    import bmesh
    mesh = bpy.data.meshes.new("Tube")
    bm = bmesh.new()
    sides, rings = 8, 7
    uv_layer = bm.loops.layers.uv.new("UVMap")
    verts = []
    for r in range(rings):
        z = 1.8 * r / (rings - 1)
        verts.append([bm.verts.new((0.2 * math.cos(2 * math.pi * s / sides), 0.2 * math.sin(2 * math.pi * s / sides), z)) for s in range(sides)])
    for r in range(rings - 1):
        for s in range(sides):
            f = bm.faces.new((verts[r][s], verts[r][(s + 1) % sides], verts[r + 1][(s + 1) % sides], verts[r + 1][s]))
            f.material_index = 0 if r < (rings - 1) // 2 else 1
            for loop, (u, v) in zip(f.loops, ((s, r), (s + 1, r), (s + 1, r + 1), (s, r + 1))):
                loop[uv_layer].uv = (u / sides, v / (rings - 1))
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(red)
    mesh.materials.append(checker)
    tube = bpy.data.objects.new("Tube", mesh)
    scene.collection.objects.link(tube)
    tube.parent = arm
    mod = tube.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    groups = {n: tube.vertex_groups.new(name=n) for n in ("Root", "Mid", "Tip")}
    for v in mesh.vertices:
        z = v.co.z
        for name, center in (("Root", 0.3), ("Mid", 0.9), ("Tip", 1.5)):
            w = max(0.0, 1.0 - abs(z - center) / 0.6)
            if w > 0:
                groups[name].add([v.index], w, "REPLACE")

    # Actions.
    arm.animation_data_create()
    def action(name, keys):
        act = bpy.data.actions.new(name)
        arm.animation_data.action = act
        for frame, bone, path, value in keys:
            pb = arm.pose.bones[bone]
            pb.rotation_mode = "XYZ"
            setattr(pb, path, value)
            pb.keyframe_insert(path, frame=frame)
        act.use_fake_user = True
        return act
    action("Bend", [
        (1, "Mid", "rotation_euler", (0, 0, 0)), (25, "Mid", "rotation_euler", (math.radians(45), 0, 0)),
        (1, "Tip", "rotation_euler", (0, 0, 0)), (25, "Tip", "rotation_euler", (math.radians(30), 0, math.radians(20))),
    ])
    action("Lift", [
        (1, "Root", "location", (0, 0, 0)), (13, "Root", "location", (0, 0.5, 0)),
        (1, "Tip", "scale", (1, 1, 1)), (13, "Tip", "scale", (1.5, 1.5, 1.5)),
    ])
    for pb in arm.pose.bones:
        pb.location, pb.rotation_euler, pb.scale = (0, 0, 0), (0, 0, 0), (1, 1, 1)
    arm.animation_data.action = None


def main():
    import bpy
    os.makedirs(OUT, exist_ok=True)
    build_scene(bpy)
    bpy.ops.export_scene.gltf(filepath=os.path.join(OUT, "Rig.glb"), export_format="GLB", export_animations=True,
                              export_animation_mode="ACTIONS")
    common = dict(use_selection=False, add_leaf_bones=False, bake_anim=True, bake_anim_use_all_actions=True,
                  bake_anim_use_nla_strips=False, bake_anim_force_startend_keying=True)
    bpy.ops.export_scene.fbx(filepath=os.path.join(OUT, "Rig.fbx"), path_mode="COPY", embed_textures=True, **common)
    bpy.ops.export_scene.fbx(filepath=os.path.join(OUT, "Rig_ZUp.fbx"), axis_up="Z", axis_forward="Y",
                             apply_scale_options="FBX_SCALE_ALL", path_mode="RELATIVE", **common)
    for name in ("Rig.fbx", "Rig_ZUp.fbx"):
        path = os.path.join(OUT, name)
        rewrite(path, path, 7400, strip_paths)
    rewrite(os.path.join(OUT, "Rig.fbx"), os.path.join(OUT, "Rig_7500.fbx"), 7500)
    print("FBX test data written to", OUT)


if __name__ == "__main__":
    main()
