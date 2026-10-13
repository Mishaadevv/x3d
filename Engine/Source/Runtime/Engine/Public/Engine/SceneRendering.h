#pragma once

// Glue between the ECS scene, assets and the renderer: render components, prefab instantiation and extraction of
// render instances.
//
//   Entity sponza = InstantiatePrefab(world, *assets.GetPrefab(id), Transform(Vec3(0, 0, 0)));
//   GatherRenderInstances(world, instances);   // after UpdateWorldTransforms()

#include "Asset/CookedAssets.h"
#include "Render/Reflections.h"
#include "Render/SceneRenderer.h"
#include "Render/Water.h"
#include "Scene/Hierarchy.h"

namespace zx {

struct MeshRenderer
{
    asset::AssetId mesh;
    Array<asset::AssetId> materials; // per mesh material slot
    bool visible = true;
    bool collision = true; // blocks Character Controllers (world-space box until the physics milestone)
};

/// Point light at the entity's world position (physical units).
struct PointLight
{
    Vec3 color = Vec3(1, 1, 1);
    f32 lumens = 800.0f; // a 60 W incandescent bulb
    f32 range = 10.0f;   // meters
    bool castShadows = false;
};

/// Spot light shining along the entity's forward (+Z) axis.
struct SpotLight
{
    Vec3 color = Vec3(1, 1, 1);
    f32 lumens = 800.0f;
    f32 range = 15.0f;
    f32 innerAngle = 0.35f; // half-angles, radians
    f32 outerAngle = 0.6f;
    bool castShadows = false;
};

ZX_DECLARE_TYPE(MeshRenderer);
ZX_DECLARE_TYPE(PointLight);
ZX_DECLARE_TYPE(SpotLight);

/// A rectangular body of water centered on the entity (its local XZ plane; rotate around Y only, scale is ignored -
/// use `size`). Waves, colors and foam are per surface.
struct Water
{
    Vec2 size = Vec2(50.0f, 50.0f); // meters
    f32 waveHeight = 0.35f;         // meters
    f32 waveLength = 8.0f;          // meters
    f32 waveSpeed = 1.0f;
    f32 windDirection = 30.0f; // degrees around +Y
    f32 choppiness = 0.6f;
    Vec3 shallowColor = Vec3(0.10f, 0.55f, 0.55f);
    Vec3 deepColor = Vec3(0.01f, 0.07f, 0.13f);
    f32 clarity = 4.0f;     // meters
    f32 refraction = 0.04f;
    f32 foam = 1.0f;
    f32 shoreFoam = 0.8f;   // meters
    f32 roughness = 0.04f;
};
ZX_DECLARE_TYPE(Water);

/// Reflection Probe: captures the scene around it into a cubemap (automatically, again when the scene changes) that
/// surfaces inside its box reflect - box-projected, so floors and walls of rooms line up with the reflection.
struct ReflectionProbe
{
    Vec3 extent = Vec3(5.0f, 3.0f, 5.0f); // half size of the influence box (scaled by the transform, axis-aligned)
    f32 blendDistance = 1.0f;             // meters inside the box over which the probe fades in
    f32 intensity = 1.0f;
    i32 priority = 0;                     // higher wins where boxes overlap (then the smaller box)
    bool boxProjection = true;            // parallax-correct reflections against the box
};
ZX_DECLARE_TYPE(ReflectionProbe);

/// Creates entities for all prefab nodes (Transform, WorldTransform, EntityName, hierarchy, MeshRenderer).
/// Returns the instance root (a new entity parenting the prefab's root nodes).
ecs::Entity InstantiatePrefab(ecs::World& world, const asset::PrefabAsset& prefab, const Transform& rootTransform = {},
                              ecs::Entity parent = {});

/// Appends one RenderInstance per visible MeshRenderer. The materials spans point into the components, so the world
/// must not change structurally until the frame's render passes have been recorded.
void GatherRenderInstances(ecs::World& world, Array<render::RenderInstance>& out);
/// Appends a LightDesc per PointLight/SpotLight entity (needs WorldTransform).
void GatherLights(ecs::World& world, Array<render::LightDesc>& out);
/// Appends a WaterSurface per Water entity (needs WorldTransform; scale is removed from the matrix).
void GatherWater(ecs::World& world, Array<render::WaterSurface>& out);
/// Appends a probe per ReflectionProbe entity; `contentKey` (scene content hash) is part of every probe's capture key.
void GatherReflectionProbes(ecs::World& world, Array<render::ReflectionProbeDesc>& out, u64 contentKey);
/// Hash of what reflection captures see (mesh instances and lights): a change recaptures the probes.
u64 SceneContentKey(Span<const render::RenderInstance> instances, Span<const render::LightDesc> lights);

} // namespace zx
