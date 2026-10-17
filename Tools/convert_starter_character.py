"""Converts Quaternius' "Animated Human" (CC0, https://opengameart.org/content/animated-human-low-poly) into the
Starter Content character: Engine/Content/StarterContent/Characters/SK_Human.glb (skin + Idle, Walk, Run, Jump, Punch,
Working, Death clips).

    "C:/Program Files/Blender Foundation/Blender 4.5/blender.exe" -b "<unzipped>/Blend/Animated Human.blend" \
        --python Tools/convert_starter_character.py -- Engine/Content/StarterContent/Characters/SK_Human.glb
"""

import os
import sys

import bpy

out = sys.argv[sys.argv.index('--') + 1]
# The palette textures sit in Blend/Textures, the .blend expects them next to itself.
textures = os.path.join(os.path.dirname(bpy.data.filepath), 'Textures')
for image in bpy.data.images:
    candidate = os.path.join(textures, bpy.path.basename(image.filepath))
    if image.filepath and os.path.exists(candidate):
        image.filepath = candidate
        image.reload()
# Diffuse BSDF does not export to glTF: rebuild the materials as Principled BSDF fed by the same texture.
for material in bpy.data.materials:
    tree = material.node_tree
    if not tree:
        continue
    diffuse = next((n for n in tree.nodes if n.type == 'BSDF_DIFFUSE'), None)
    output = next((n for n in tree.nodes if n.type == 'OUTPUT_MATERIAL'), None)
    image = next((n for n in tree.nodes if n.type == 'TEX_IMAGE'), None)
    if not diffuse or not output:
        continue
    principled = tree.nodes.new('ShaderNodeBsdfPrincipled')
    principled.inputs['Roughness'].default_value = 0.8
    if image:
        image.interpolation = 'Closest'  # palette texture
        tree.links.new(image.outputs['Color'], principled.inputs['Base Color'])
    tree.links.new(principled.outputs['BSDF'], output.inputs['Surface'])
    tree.nodes.remove(diffuse)
# Leftover unnamed actions of the source file are not clips.
for action in list(bpy.data.actions):
    if action.name.startswith('ArmatureAction'):
        bpy.data.actions.remove(action)
bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', export_animations=True, export_animation_mode='ACTIONS',
                          export_skins=True, export_yup=True, export_apply=False)
print('Exported', out, [a.name for a in bpy.data.actions])
