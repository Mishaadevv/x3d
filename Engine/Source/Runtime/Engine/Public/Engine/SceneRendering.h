#pragma once

// Glue between the ECS scene, assets and the renderer: render components, prefab instantiation and extraction of
// render instances.
//
//   Entity sponza = InstantiatePrefab(world, *assets.GetPrefab(id), Transform(Vec3(0, 0, 0)));
//   GatherRenderInstances(world, instances);   // after UpdateWorldTransforms()

#include "Asset/CookedAssets.h"
#include "Render/Reflections.h"
#include "Render/SceneRenderer.h"
#include "Render/Clouds.h"
#include "Render/Fluid.h"
#include "Render/VolumetricFog.h"
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

/// The sun (the first one counts): it shines along the entity's forward (+Z) axis - rotate the entity to move the
/// sun. Without one, the editor's Sun settings place it.
struct DirectionalLight
{
    f32 intensity = 1.0f;          // 1 = the real sun (~110 000 lux at noon before the atmosphere)
    Vec3 color = Vec3(1.0f, 1.0f, 1.0f);
    bool castShadows = true;
};

/// Light from the sky (the first one counts): how bright the ambient light and the sky itself are.
struct SkyLight
{
    f32 intensity = 1.0f;          // ambient light from the sky (multiplier)
    Vec3 tint = Vec3(1.0f, 1.0f, 1.0f);
    f32 skyBrightness = 1.0f;      // how bright the visible sky looks (multiplier)
};

ZX_DECLARE_TYPE(DirectionalLight);
ZX_DECLARE_TYPE(SkyLight);
/// Morph target (blend shape) weights of the Mesh Renderer on this entity, 0..1 each in the mesh's target order (set
/// on import from the model). While an Animator plays clips with morph animation, those weights win.
struct MorphTargets
{
    Array<String> names;
    Array<f32> weights;
};

ZX_DECLARE_TYPE(MeshRenderer);
ZX_DECLARE_TYPE(MorphTargets);
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

/// Exponential height fog for the whole scene (the first one counts), densest at the entity's height and thinning out
/// above it. Volumetric: near the camera the fog is a real volume - lit by the sun through the shadows (light shafts),
/// by point and spot lights (halos, fire glowing in smoke) and filled by Fog Volumes.
struct HeightFog
{
    f32 density = 0.003f;      // 1/m at the entity's height
    f32 falloff = 0.08f;       // how fast it thins out with height (1/m)
    Vec3 color = Vec3(0.9f, 0.95f, 1.0f); // albedo: the fraction of light scattered
    f32 sunAnisotropy = 0.6f;  // forward scattering: brighter towards the sun
    f32 maxDistance = 3000.0f; // meters
    bool volumetric = true;
    f32 volumetricDistance = 80.0f; // meters of volume in front of the camera
    f32 extinctionScale = 1.0f;     // denser (more visible light shafts) inside the volume
    f32 localLights = 1.0f;         // how strongly point / spot lights light the fog
};
ZX_DECLARE_TYPE(HeightFog);

/// A local volume of fog, mist, steam or smoke (needs a Height Fog with Volumetric on - or none: the default fog is
/// volumetric). Its size is the entity's scale (a 1 m box / sphere), density fades out at the edge and is broken up by
/// drifting noise.
/// Volumetric clouds over the whole scene (the first one counts): a layer of drifting 3D clouds lit by the sun and the
/// sky (golden at sunset), casting moving shadows on the ground and through the volumetric fog.
struct VolumetricClouds
{
    f32 coverage = 0.35f;       // 0 = clear sky, 1 = overcast
    f32 bottom = 1500.0f;       // m above the ground
    f32 thickness = 2500.0f;    // m
    f32 density = 0.03f;        // 1/m in the thick of a cloud
    f32 detail = 0.35f;         // edges eroded by fine noise (wispy)
    f32 windSpeed = 15.0f;      // m/s
    f32 windDirection = 60.0f;  // degrees around +Y
    Vec3 color = Vec3(1.0f);    // albedo
    f32 shadows = 0.75f;        // how dark their shadows are on the ground
};
ZX_DECLARE_TYPE(VolumetricClouds);

/// Real-time smoke and fire: a GPU fluid simulation inside the entity's box (its scale is the size in meters). Hot gas
/// rises and curls into vortices, smoke drifts and thins out, the hottest gas glows like flames. Expensive: keep the
/// resolution modest and use a few of them.
struct FluidVolume
{
    i32 resolution = 48;          // cells along the longest side (cost grows with the cube)
    bool simulateInEditor = true; // live while editing (off: only in Play)
    Vec3 sourcePosition = Vec3(0.0f, -0.4f, 0.0f); // in the box (-0.5 .. 0.5)
    f32 sourceRadius = 0.16f;     // fraction of the longest side
    Vec3 sourceVelocity = Vec3(0.0f, 1.0f, 0.0f); // m/s
    f32 smoke = 1.5f;             // smoke added per second
    f32 heat = 1.0f;              // temperature at the source: 0 = cold smoke / steam, 1 = flames
    f32 turbulence = 1.2f;        // how much the source flickers
    f32 buoyancy = 5.0f;          // how fast hot gas rises (m/s^2)
    f32 weight = 0.3f;            // how much smoke sinks
    f32 swirl = 12.0f;            // vortices (vorticity confinement)
    f32 smokeFade = 0.25f;        // per second
    f32 cooling = 0.9f;           // per second
    f32 density = 3.0f;           // smoke opacity (1/m)
    Vec3 smokeColor = Vec3(0.2f);
    f32 fireIntensity = 2.0f;     // glow of the hottest gas
    f32 fireThreshold = 0.15f;    // temperature above which gas glows
};
ZX_DECLARE_TYPE(FluidVolume);

enum class FogShape : u8
{
    Box,
    Sphere,
};

struct FogVolume
{
    FogShape shape = FogShape::Box;
    f32 density = 0.3f;            // 1/m
    Vec3 color = Vec3(0.85f);      // albedo (dark for smoke)
    Vec3 emissive = Vec3(0.0f);    // glow color
    f32 emissiveIntensity = 0.0f;  // cd/m2 per meter
    f32 edgeFade = 0.3f;           // fraction of the size
    f32 noiseScale = 0.5f;         // 1/m
    f32 noiseAmount = 0.6f;        // 0 = uniform
    Vec3 wind = Vec3(0.3f, 0.05f, 0.1f); // m/s the noise drifts
};
ZX_DECLARE_ENUM(FogShape);
ZX_DECLARE_TYPE(FogVolume);

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

/// Where the directional light comes from while an HDRI Sky is active.
enum class HdriSunMode : u8
{
    FromHDRI, // found in the panorama: direction, color and brightness (removed from the sky lighting)
    SceneSun, // the scene's sun settings (Sun popup), as with the physical sky
    None,     // no directional light: the panorama lights everything
};

/// HDRI Sky: an HDR panorama (.hdr / .exr, lat-long) replaces the physical sky - the background, the sky lighting,
/// fog color, water and reflections; its sun becomes the directional light. One per scene (the first one wins); the
/// entity's transform is ignored (use `rotation`).
struct HdriSky
{
    String texture;                       // Content-relative .hdr / .exr
    f32 intensity = 5000.0f;              // luminance (cd/m2) of a texel value of 1
    f32 rotation = 0.0f;                  // degrees around +Y
    HdriSunMode sun = HdriSunMode::FromHDRI;
};
ZX_DECLARE_ENUM(HdriSunMode);
ZX_DECLARE_TYPE(HdriSky);

/// Decal: projects an image (and optionally a normal map) onto whatever is inside its box - along the entity's -Y,
/// so with no rotation it paints the ground below. Lit like the surface it lands on.
struct Decal
{
    String texture;       // Content-relative image: rgb color, alpha coverage (empty = solid color)
    String normalTexture; // optional normal map (tangent space, OpenGL convention)
    Vec3 extent = Vec3(1.0f, 0.5f, 1.0f); // half size: x/z across the surface, y = projection depth (scaled by the transform)
    Vec3 color = Vec3(1.0f, 1.0f, 1.0f);
    f32 opacity = 1.0f;
    bool affectRoughness = false;
    f32 roughness = 0.5f;
    f32 normalStrength = 1.0f;
    f32 angleFade = 0.5f;
    f32 emissive = 0.0f;
    i32 sortOrder = 0;
};
ZX_DECLARE_TYPE(Decal);

/// Creates entities for all prefab nodes (Transform, WorldTransform, EntityName, hierarchy, MeshRenderer; an Animator
/// on skinned meshes, playing "Idle" when `assets` can name the clips, else the first one).
/// Returns the instance root (a new entity parenting the prefab's root nodes).
ecs::Entity InstantiatePrefab(ecs::World& world, const asset::PrefabAsset& prefab, const Transform& rootTransform = {},
                              ecs::Entity parent = {}, const asset::AssetManager* assets = nullptr);

/// Appends one RenderInstance per visible MeshRenderer. The materials spans point into the components, so the world
/// must not change structurally until the frame's render passes have been recorded.
/// Skinned meshes (Animator) take their skinning matrices from `animation` (none: drawn in the bind pose).
void GatherRenderInstances(ecs::World& world, Array<render::RenderInstance>& out, const class AnimationSystem* animation = nullptr);
/// Appends a LightDesc per PointLight/SpotLight entity (needs WorldTransform).
void GatherLights(ecs::World& world, Array<render::LightDesc>& out);
/// Appends a WaterSurface per Water entity (needs WorldTransform; scale is removed from the matrix).
void GatherWater(ecs::World& world, Array<render::WaterSurface>& out);
/// Appends a probe per ReflectionProbe entity; `contentKey` (scene content hash) is part of every probe's capture key.
void GatherReflectionProbes(ecs::World& world, Array<render::ReflectionProbeDesc>& out, u64 contentKey);
/// Appends a DecalDesc per Decal entity; `texture` turns a Content-relative path (normal map flag) into a texture SRV
/// (invalid while it loads). Null = solid-color decals.
void GatherDecals(ecs::World& world, Array<render::DecalDesc>& out, const Function<u32(StringView path, bool normalMap)>& texture);
/// The scene's HDRI Sky (first one found); null when there is none.
const HdriSky* FindHdriSky(ecs::World& world);
/// The scene's Directional Light and the direction its light travels (null when there is none).
const DirectionalLight* FindDirectionalLight(ecs::World& world, Vec3* direction = nullptr);
const SkyLight* FindSkyLight(ecs::World& world);
/// The scene's Height Fog and the world height of its entity (null when there is none).
const HeightFog* FindHeightFog(ecs::World& world, f32* height = nullptr);
void GatherFogVolumes(ecs::World& world, Array<render::FogVolumeDesc>& out);
/// Cloud settings from the scene's Volumetric Clouds (disabled when there is none).
render::CloudSettings FindClouds(ecs::World& world);
/// Fluid Volumes to draw; `playing` decides which ones simulate (all in Play, Simulate In Editor ones otherwise).
void GatherFluidVolumes(ecs::World& world, Array<render::FluidVolumeDesc>& out, bool playing);
/// Hash of what reflection captures see (mesh instances and lights): a change recaptures the probes.
u64 SceneContentKey(Span<const render::RenderInstance> instances, Span<const render::LightDesc> lights);

} // namespace zx
