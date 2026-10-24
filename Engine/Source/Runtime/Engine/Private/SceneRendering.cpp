#include "Engine/SceneRendering.h"
#include "Engine/Foliage.h"
#include "Engine/Animation.h"
#include "Engine/SoftBody.h"
#include "Engine/Wind.h"
#include "Engine/Fire.h"

#include "Core/Jobs/JobSystem.h"
#include "Core/Reflection/Reflection.h"

namespace zx {

ZX_REFLECT_STRUCT_BEGIN(MorphTargets)
    ZX_FIELD(names, Meta::ReadOnly(), Meta::Tooltip("Morph targets of the mesh (from the model)"))
    ZX_FIELD(weights, Meta::Range(0, 1), Meta::Tooltip("Weight per morph target, in the order of Names (scripts: entity.setMorph(name, w))"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(MeshRenderer)
    ZX_FIELD(mesh, Meta::AssetType("Mesh"), Meta::ReadOnly())
    ZX_FIELD(materials, Meta::AssetType("Material"), Meta::ReadOnly())
    ZX_FIELD(visible)
    ZX_FIELD(collision, Meta::Tooltip("Solid: blocks Character Controllers and rigid bodies (as a static triangle mesh unless the entity has colliders)"))
    ZX_FIELD(lightmapScaleBias, Meta::Hidden())
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(DirectionalLight)
    ZX_FIELD(intensity, Meta::Range(0, 10), Meta::Tooltip("1 = the real sun; rotate the entity to move it (it shines along +Z)"))
    ZX_FIELD(color, Meta::Color())
    ZX_FIELD(castShadows, Meta::Category("Shadows"))
    ZX_FIELD(sourceAngle, Meta::Range(0, 10), Meta::Units("deg"),
             Meta::Tooltip("Apparent size of the sun: larger = softer shadows away from the contact (0.53 = the real sun). "
                           "Needs soft shadows (Shadow Quality Ultra, or the Full profile)"))
    ZX_FIELD(shadowDistance, Meta::Range(0, 2000), Meta::Units("m"),
             Meta::Tooltip("How far from the camera the sun casts shadows (0 = the engine profile's: 120 m cascades, 250 m virtual)"))
    ZX_FIELD(shadowBias, Meta::Range(0, 8),
             Meta::Tooltip("Pushes shadow lookups off the surface: raise against stripes (acne), lower if shadows detach from objects"))
    ZX_FIELD(contactShadows, Meta::Hidden()) // the checkbox in front of Contact Shadow Length
    ZX_FIELD(contactShadowLength, Meta::DisplayName("Contact Shadows"), Meta::Range(0.01, 2), Meta::Units("m"), Meta::EditCondition("contactShadows"),
             Meta::Tooltip("Short screen-space shadows where objects touch the ground (fine detail the shadow map misses)"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(SkyLight)
    ZX_FIELD(intensity, Meta::Range(0, 10), Meta::Tooltip("Ambient light from the sky (multiplier)"))
    ZX_FIELD(tint, Meta::Color())
    ZX_FIELD(skyBrightness, Meta::Range(0, 5), Meta::Tooltip("How bright the visible sky looks (multiplier)"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(HeightFog)
    ZX_FIELD(density, Meta::Range(0, 1), Meta::Units("1/m"), Meta::Tooltip("Fog density at the entity's height"))
    ZX_FIELD(falloff, Meta::Range(0.0001, 2), Meta::Units("1/m"), Meta::Tooltip("How fast it thins out with height"))
    ZX_FIELD(color, Meta::Color(), Meta::Tooltip("Albedo: the fraction of light the fog scatters"))
    ZX_FIELD(sunAnisotropy, Meta::Range(-0.9, 0.95), Meta::Tooltip("Forward scattering: brighter looking towards the sun"))
    ZX_FIELD(maxDistance, Meta::Range(10, 100000), Meta::Units("m"))
    ZX_FIELD(volumetric, Meta::Category("Volumetric"), Meta::Tooltip("Light shafts, lit by lights, Fog Volumes (costs GPU time)"))
    ZX_FIELD(volumetricDistance, Meta::Range(10, 500), Meta::Units("m"), Meta::Tooltip("Depth of the volume in front of the camera"))
    ZX_FIELD(extinctionScale, Meta::Range(0, 50), Meta::Tooltip("Denser inside the volume: stronger light shafts"))
    ZX_FIELD(localLights, Meta::Range(0, 20), Meta::Tooltip("How strongly point and spot lights light the fog"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_ENUM_BEGIN(CloudPreset)
    ZX_ENUM_VALUE(Custom)
    ZX_ENUM_VALUE(Clear)
    ZX_ENUM_VALUE(FairWeather)
    ZX_ENUM_VALUE(Scattered)
    ZX_ENUM_VALUE(Cloudy)
    ZX_ENUM_VALUE(Overcast)
    ZX_ENUM_VALUE(Storm)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(VolumetricClouds)
    ZX_FIELD(preset, Meta::Tooltip("Fills in every setting below for this kind of sky (then tweak them)"))
    ZX_FIELD(coverage, Meta::Range(0, 1), Meta::Category("Shape"), Meta::Tooltip("0 = clear sky, 1 = overcast"))
    ZX_FIELD(cloudType, Meta::Range(0, 1), Meta::Tooltip("0 = flat stratus layers, 0.5 = cumulus heaps, 1 = towering storm clouds"))
    ZX_FIELD(bottom, Meta::Range(100, 10000), Meta::Units("m"), Meta::Tooltip("Height of the cloud bases"))
    ZX_FIELD(thickness, Meta::Range(100, 12000), Meta::Units("m"), Meta::Tooltip("Height of the layer (storm clouds fill it)"))
    ZX_FIELD(density, Meta::Range(0, 0.3), Meta::Units("1/m"), Meta::Tooltip("Thicker = darker bases, sharper edges"))
    ZX_FIELD(detail, Meta::Range(0, 1), Meta::Tooltip("Edges eroded by fine noise: wispy"))
    ZX_FIELD(scale, Meta::Range(0.2, 5), Meta::Tooltip("Size of the clouds and of the gaps between them"))
    ZX_FIELD(evolution, Meta::Range(0, 30), Meta::Units("m/s"), Meta::Tooltip("How fast the clouds change shape"))
    ZX_FIELD(windSpeed, Meta::Range(0, 200), Meta::Units("m/s"), Meta::Category("Motion"))
    ZX_FIELD(windDirection, Meta::Range(0, 360), Meta::Units("deg"))
    ZX_FIELD(color, Meta::Color(), Meta::Category("Lighting"))
    ZX_FIELD(ambient, Meta::Range(0, 3), Meta::Tooltip("Sky light inside the clouds (brighter, softer shadows)"))
    ZX_FIELD(silverLining, Meta::Range(0, 3), Meta::Tooltip("Glow of the edges around the sun"))
    ZX_FIELD(shadows, Meta::Range(0, 1), Meta::Tooltip("How dark their shadows are on the ground"))
    ZX_FIELD(lightning, Meta::Range(0, 1), Meta::Tooltip("How often lightning flashes inside the clouds"))
    ZX_FIELD(seed, Meta::Range(1, 1000), Meta::Tooltip("Pattern of the cloud fields"))
    ZX_FIELD(appliedPreset, Meta::Hidden())
ZX_REFLECT_STRUCT_END()

void ApplyCloudPreset(VolumetricClouds& c, CloudPreset preset)
{
    struct P
    {
        f32 coverage, type, bottom, thickness, density, detail, scale, evolution, ambient, silver, shadows, lightning;
        Vec3 color;
    };
    static const P Presets[] = {
        {}, // Custom: left as is
        {0.22f, 0.42f, 1500.0f, 2500.0f, 0.035f, 0.85f, 0.8f, 1.5f, 1.0f, 1.0f, 0.6f, 0.0f, Vec3(1.0f)},          // Clear
        {0.40f, 0.45f, 1400.0f, 3000.0f, 0.040f, 0.85f, 1.0f, 2.0f, 1.0f, 1.0f, 0.7f, 0.0f, Vec3(1.0f)},          // Fair weather
        {0.48f, 0.55f, 1300.0f, 4000.0f, 0.045f, 0.80f, 1.2f, 2.5f, 1.0f, 1.0f, 0.75f, 0.0f, Vec3(1.0f)},         // Scattered
        {0.60f, 0.60f, 1100.0f, 5000.0f, 0.050f, 0.75f, 1.3f, 3.0f, 0.9f, 0.9f, 0.8f, 0.0f, Vec3(0.97f)},         // Cloudy
        {0.92f, 0.20f, 800.0f, 4000.0f, 0.050f, 0.70f, 1.4f, 1.5f, 0.8f, 0.6f, 0.95f, 0.0f, Vec3(0.92f)},         // Overcast
        {0.58f, 0.95f, 700.0f, 9000.0f, 0.080f, 0.70f, 1.6f, 6.0f, 0.65f, 0.7f, 0.92f, 0.45f, Vec3(0.85f)},       // Storm
    };
    c.appliedPreset = preset;
    c.preset = preset;
    if (preset == CloudPreset::Custom || u32(preset) >= ZX_ARRAY_COUNT(Presets))
        return;
    const P& p = Presets[u32(preset)];
    c.coverage = p.coverage;
    c.cloudType = p.type;
    c.bottom = p.bottom;
    c.thickness = p.thickness;
    c.density = p.density;
    c.detail = p.detail;
    c.scale = p.scale;
    c.evolution = p.evolution;
    c.ambient = p.ambient;
    c.silverLining = p.silver;
    c.shadows = p.shadows;
    c.lightning = p.lightning;
    c.color = p.color;
}

void UpdateCloudPresets(ecs::World& world)
{
    world.Query<VolumetricClouds>().Each([](ecs::Entity, VolumetricClouds& c) {
        if (c.preset != c.appliedPreset)
            ApplyCloudPreset(c, c.preset);
    });
}

ZX_REFLECT_STRUCT_BEGIN(FluidVolume)
    ZX_FIELD(resolution, Meta::Range(16, 96), Meta::Tooltip("Cells along the longest side (GPU cost grows with the cube)"))
    ZX_FIELD(simulateInEditor, Meta::Tooltip("Live while editing; off = only in Play"))
    ZX_FIELD(sourcePosition, Meta::Category("Source"), Meta::Tooltip("In the box: -0.5 .. 0.5 on each axis"))
    ZX_FIELD(sourceRadius, Meta::Range(0.01, 0.5))
    ZX_FIELD(sourceVelocity, Meta::Units("m/s"))
    ZX_FIELD(smoke, Meta::Range(0, 20), Meta::Tooltip("Smoke added per second"))
    ZX_FIELD(heat, Meta::Range(0, 1), Meta::Tooltip("0 = cold smoke or steam, 1 = flames"))
    ZX_FIELD(turbulence, Meta::Range(0, 3))
    ZX_FIELD(buoyancy, Meta::Category("Motion"), Meta::Range(0, 50), Meta::Units("m/s2"), Meta::Tooltip("How fast hot gas rises"))
    ZX_FIELD(weight, Meta::Range(0, 20), Meta::Tooltip("How much smoke sinks"))
    ZX_FIELD(swirl, Meta::Range(0, 40), Meta::Tooltip("Vortices: keeps small swirls alive"))
    ZX_FIELD(smokeFade, Meta::Range(0, 5), Meta::Units("1/s"))
    ZX_FIELD(cooling, Meta::Range(0, 10), Meta::Units("1/s"))
    ZX_FIELD(density, Meta::Category("Look"), Meta::Range(0, 50), Meta::Units("1/m"))
    ZX_FIELD(smokeColor, Meta::Color())
    ZX_FIELD(fireIntensity, Meta::Range(0, 50))
    ZX_FIELD(fireThreshold, Meta::Range(0, 0.95), Meta::Tooltip("Temperature above which gas glows"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_ENUM_BEGIN(FogShape)
    ZX_ENUM_VALUE(Box)
    ZX_ENUM_VALUE(Sphere)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(FogVolume)
    ZX_FIELD(shape, Meta::Tooltip("A 1 m box or sphere, sized by the entity's scale"))
    ZX_FIELD(density, Meta::Range(0, 20), Meta::Units("1/m"))
    ZX_FIELD(color, Meta::Color(), Meta::Tooltip("Albedo: light fog / steam ~0.9, smoke ~0.1"))
    ZX_FIELD(emissive, Meta::Color())
    ZX_FIELD(emissiveIntensity, Meta::Range(0, 100000), Meta::Units("nits/m"), Meta::Tooltip("Glow (smoke lit from inside by a fire)"))
    ZX_FIELD(edgeFade, Meta::Range(0.01, 1), Meta::Tooltip("Fraction of the size over which it fades out"))
    ZX_FIELD(noiseScale, Meta::Category("Noise"), Meta::Range(0.01, 20), Meta::Units("1/m"))
    ZX_FIELD(noiseAmount, Meta::Range(0, 1), Meta::Tooltip("0 = uniform, 1 = only wisps"))
    ZX_FIELD(wind, Meta::Units("m/s"), Meta::Tooltip("How the wisps drift"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_ENUM_BEGIN(WaterPreset)
    ZX_ENUM_VALUE(Custom)
    ZX_ENUM_VALUE(Ocean)
    ZX_ENUM_VALUE(Lake)
    ZX_ENUM_VALUE(Pool)
    ZX_ENUM_VALUE(Storm)
    ZX_ENUM_VALUE(Tropical)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(Water)
    ZX_FIELD(preset, Meta::Tooltip("Fills in every setting below for this kind of water (then tweak them)"))
    ZX_FIELD(size, Meta::Tooltip("Width and length in meters"))
    ZX_FIELD(windSpeed, Meta::Range(0.5, 40), Meta::Units("m/s"), Meta::Category("Waves"),
             Meta::Tooltip("The wind that raised the sea: stronger = bigger, longer waves (Wind Zones steer them)"))
    ZX_FIELD(fetch, Meta::Range(0.005, 2000), Meta::Units("km"), Meta::Tooltip("Open water the wind blew over: pool 0.03, lake 3, ocean 100+"))
    ZX_FIELD(windDirection, Meta::Range(-180, 180), Meta::Units("deg"), Meta::Tooltip("Where the waves run, around +Y (0 = towards +Z)"))
    ZX_FIELD(waveAmount, Meta::Range(0, 4), Meta::Tooltip("Height multiplier"))
    ZX_FIELD(choppiness, Meta::Range(0, 2), Meta::Tooltip("0 = round swells, 1+ = sharp, pinched crests"))
    ZX_FIELD(shallowColor, Meta::Color(), Meta::Category("Look"))
    ZX_FIELD(deepColor, Meta::Color())
    ZX_FIELD(scatterColor, Meta::Color(), Meta::Tooltip("Light shining through the wave crests"))
    ZX_FIELD(clarity, Meta::Range(0.1, 50), Meta::Units("m"), Meta::Tooltip("How deep you can see into the water"))
    ZX_FIELD(refraction, Meta::Range(0, 0.2))
    ZX_FIELD(roughness, Meta::Range(0.01, 0.5), Meta::Tooltip("Sharpness of the sun glints"))
    ZX_FIELD(foam, Meta::Range(0, 3), Meta::Tooltip("Whitecaps on breaking crests"))
    ZX_FIELD(shoreFoam, Meta::Range(0, 5), Meta::Units("m"), Meta::Tooltip("Width of the foam line along shores"))
    ZX_FIELD(caustics, Meta::Range(0, 3), Meta::Tooltip("Light patterns dancing on what lies below"))
    ZX_FIELD(reflections, Meta::Tooltip("The scene reflects in the water (screen space); off: only the sky"))
    ZX_FIELD(appliedPreset, Meta::Hidden())
ZX_REFLECT_STRUCT_END()

void ApplyWaterPreset(Water& w, WaterPreset preset)
{
    struct P
    {
        f32 wind, fetch, amount, chop;
        Vec3 shallow, deep, scatter;
        f32 clarity, roughness, foam, shore, caustics;
    };
    static const P Presets[] = {
        // Custom: left as is.
        {0, 0, 0, 0, {}, {}, {}, 0, 0, 0, 0, 0},
        // Ocean: long swell under a fresh breeze, deep blue-green.
        {11.0f, 400.0f, 1.0f, 1.1f, Vec3(0.03f, 0.36f, 0.42f), Vec3(0.004f, 0.03f, 0.07f), Vec3(0.03f, 0.45f, 0.42f), 6.0f, 0.06f, 1.0f, 1.5f, 0.8f},
        // Lake: short, choppy waves, greener and murkier.
        {6.0f, 3.0f, 1.0f, 0.8f, Vec3(0.12f, 0.40f, 0.32f), Vec3(0.01f, 0.05f, 0.06f), Vec3(0.06f, 0.35f, 0.25f), 3.0f, 0.05f, 0.4f, 0.6f, 0.8f},
        // Pool: clear, bright, tiny ripples, strong caustics.
        {3.0f, 0.03f, 1.5f, 0.5f, Vec3(0.20f, 0.75f, 0.80f), Vec3(0.02f, 0.25f, 0.45f), Vec3(0.10f, 0.60f, 0.70f), 12.0f, 0.02f, 0.0f, 0.15f, 1.5f},
        // Storm: towering, breaking seas, white with foam.
        {22.0f, 800.0f, 1.0f, 1.4f, Vec3(0.06f, 0.25f, 0.25f), Vec3(0.01f, 0.03f, 0.04f), Vec3(0.05f, 0.30f, 0.28f), 4.0f, 0.12f, 1.6f, 2.5f, 0.3f},
        // Tropical: turquoise lagoon, crystal clear.
        {5.0f, 30.0f, 0.8f, 0.9f, Vec3(0.15f, 0.85f, 0.75f), Vec3(0.0f, 0.12f, 0.25f), Vec3(0.05f, 0.70f, 0.60f), 9.0f, 0.04f, 0.5f, 1.2f, 1.6f},
    };
    w.preset = w.appliedPreset = preset;
    if (preset == WaterPreset::Custom)
        return;
    const P& p = Presets[u32(preset)];
    w.windSpeed = p.wind;
    w.fetch = p.fetch;
    w.waveAmount = p.amount;
    w.choppiness = p.chop;
    w.shallowColor = p.shallow;
    w.deepColor = p.deep;
    w.scatterColor = p.scatter;
    w.clarity = p.clarity;
    w.roughness = p.roughness;
    w.foam = p.foam;
    w.shoreFoam = p.shore;
    w.caustics = p.caustics;
}

void UpdateWaterPresets(ecs::World& world)
{
    world.Query<Water>().Each([](ecs::Entity, Water& w) {
        if (w.preset != w.appliedPreset)
            ApplyWaterPreset(w, w.preset);
    });
}

ZX_REFLECT_STRUCT_BEGIN(ReflectionProbe)
    ZX_FIELD(extent, Meta::Tooltip("Half size of the box (meters, scaled by the transform) whose surfaces use this probe"))
    ZX_FIELD(blendDistance, Meta::Range(0, 10), Meta::Units("m"), Meta::Tooltip("Fade-in distance inside the box"))
    ZX_FIELD(intensity, Meta::Range(0, 4))
    ZX_FIELD(priority, Meta::Tooltip("Higher priority probes win where boxes overlap"))
    ZX_FIELD(boxProjection, Meta::Tooltip("Parallax-correct the reflection against the box (rooms, corridors)"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(Decal)
    ZX_FIELD(texture, Meta::AssetType("Texture"), Meta::Tooltip("Image projected onto surfaces (alpha = coverage). Drop one from the Content Browser"))
    ZX_FIELD(normalTexture, Meta::AssetType("Texture"), Meta::Tooltip("Optional normal map"))
    ZX_FIELD(extent, Meta::Tooltip("Half size in meters: x/z across the surface, y = how deep it projects"))
    ZX_FIELD(color, Meta::Color())
    ZX_FIELD(opacity, Meta::Range(0, 1))
    ZX_FIELD(affectRoughness)
    ZX_FIELD(roughness, Meta::Range(0, 1), Meta::EditCondition("affectRoughness"))
    ZX_FIELD(normalStrength, Meta::Range(0, 2))
    ZX_FIELD(angleFade, Meta::Range(0, 1), Meta::Tooltip("Fade on surfaces that turn away from the projection (0 = hard cut)"))
    ZX_FIELD(emissive, Meta::Range(0, 50), Meta::Tooltip("Glow (multiplies the color)"))
    ZX_FIELD(sortOrder, Meta::Tooltip("Higher values are drawn over lower ones"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_ENUM_BEGIN(HdriSunMode)
    ZX_ENUM_VALUE(FromHDRI)
    ZX_ENUM_VALUE(SceneSun)
    ZX_ENUM_VALUE(None)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(HdriSky)
    ZX_FIELD(texture, Meta::AssetType("Texture"), Meta::Tooltip("HDR panorama (.hdr / .exr, lat-long). Drop one from the Content Browser"))
    ZX_FIELD(intensity, Meta::Range(0, 100000), Meta::Units("nits"), Meta::Tooltip("Luminance (cd/m2) of a pixel value of 1"))
    ZX_FIELD(rotation, Meta::Range(-180, 180), Meta::Units("deg"), Meta::Tooltip("Turns the panorama (and its sun) around the vertical axis"))
    ZX_FIELD(sun, Meta::Tooltip("Directional light: the sun found in the panorama, the scene's sun, or none"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(PointLight)
    ZX_FIELD(color, Meta::Color())
    ZX_FIELD(lumens, Meta::Range(0, 200000), Meta::Units("lm"), Meta::Tooltip("Luminous power"))
    ZX_FIELD(range, Meta::Range(0.1, 200), Meta::Units("m"))
    ZX_FIELD(castShadows)
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(SpotLight)
    ZX_FIELD(color, Meta::Color())
    ZX_FIELD(lumens, Meta::Range(0, 200000), Meta::Units("lm"))
    ZX_FIELD(range, Meta::Range(0.1, 200), Meta::Units("m"))
    ZX_FIELD(innerAngle, Meta::Angle(), Meta::Range(0, 1.57))
    ZX_FIELD(outerAngle, Meta::Angle(), Meta::Range(0, 1.57))
    ZX_FIELD(castShadows)
ZX_REFLECT_STRUCT_END()

using namespace ecs;
using namespace scene;

Entity InstantiatePrefab(World& world, const asset::PrefabAsset& prefab, const Transform& rootTransform, Entity parent,
                         const asset::AssetManager* assets)
{
    const Entity root = CreateNode(world, prefab.name.View(), rootTransform, parent);
    Array<Entity> created;
    created.Reserve(prefab.nodes.Size());
    for (const asset::PrefabNode& node : prefab.nodes) {
        const Entity p = node.parent >= 0 ? created[usize(node.parent)] : root;
        const Entity e = CreateNode(world, node.name.View(), node.local, p);
        if (node.mesh.IsValid()) {
            MeshRenderer mr;
            mr.mesh = node.mesh;
            mr.materials = node.materials;
            mr.collision = !node.skeleton.IsValid(); // an animated mesh is no static obstacle
            world.Add(e, move(mr));
            if (!node.morphNames.Empty()) {
                MorphTargets morphs;
                morphs.names = node.morphNames;
                morphs.weights = node.morphWeights;
                morphs.weights.Resize(morphs.names.Size(), 0.0f);
                world.Add(e, move(morphs));
            }
        }
        if (node.skeleton.IsValid()) {
            Animator a;
            a.skeleton = node.skeleton;
            a.skinToNode = Transform::FromMatrix(node.skinToNode);
            a.clips = node.animations;
            if (!a.clips.Empty())
                a.clip = a.clips[0];
            if (assets)
                for (const asset::AssetId& clip : a.clips)
                    if (assets->GetName(clip).View().EqualsIgnoreCase("Idle"))
                        a.clip = clip;
            world.Add(e, move(a));
        }
        created.PushBack(e);
    }
    return root;
}

void GatherRenderInstances(World& world, Array<render::RenderInstance>& out, const AnimationSystem* animation, const Vec3* viewer)
{
    // Plain mesh renderers (almost all of them): visible ones counted per chunk, then the chunks fill their ranges in
    // parallel. Flammable ones (few) follow, serially.
    struct ChunkRef
    {
        const Entity* entities;
        const WorldTransform* transforms;
        const MeshRenderer* renderers;
        u32 count;
        u32 first;
    };
    Array<ChunkRef> chunks;
    u32 total = 0;
    world.Query<const WorldTransform, const MeshRenderer>().Without<Flammable>().Without<Animator>().Without<MorphTargets>().Without<SoftBody>().EachChunk([&](auto& v) {
        ChunkRef c{v.entities, v.template Column<0>(), v.template Column<1>(), v.count, total};
        for (u32 i = 0; i < v.count; ++i)
            total += c.renderers[i].visible && c.renderers[i].mesh.IsValid() ? 1u : 0u;
        chunks.PushBack(c);
    });
    const usize base = out.Size();
    out.ResizeUninitialized(base + total); // every element is written below
    auto fill = [&](u32 begin, u32 end) {
        for (u32 k = begin; k < end; ++k) {
            const ChunkRef& c = chunks[k];
            usize at = base + c.first;
            for (u32 i = 0; i < c.count; ++i) {
                const MeshRenderer& mr = c.renderers[i];
                if (!mr.visible || !mr.mesh.IsValid())
                    continue;
                render::RenderInstance inst;
                inst.world = c.transforms[i].matrix;
                inst.id = c.entities[i].Pack();
                inst.mesh = mr.mesh;
                inst.materials = mr.materials.View();
                inst.lightmapScaleBias = mr.lightmapScaleBias;
                MemCopy(&out[at++], &inst, sizeof(inst));
            }
        }
    };
    if (jobs::IsInitialized() && total > 4096)
        jobs::ParallelFor(u32(chunks.Size()), 1, fill);
    else
        fill(0, u32(chunks.Size()));
    // Foliage: every plant within its cull distance, one instance per model part.
    world.Query<const WorldTransform, Foliage>().Each([&](Entity e, const WorldTransform& wt, Foliage& f) {
        if (f.parts.Empty())
            return;
        DecodeFoliage(f);
        const f32 cull2 = f.cullDistance * f.cullDistance;
        const Vec3 origin = wt.matrix.GetTranslation();
        Array<Mat4> partMatrices;
        for (const FoliagePart& p : f.parts)
            partMatrices.PushBack(p.local.ToMatrix());
        const u64 base = HashMix64(e.Pack());
        for (usize i = 0; i < f.instances.Size(); ++i) {
            const FoliageInstance& in = f.instances[i];
            const Vec3 p = origin + in.position;
            if (viewer) {
                const Vec3 d = p - *viewer;
                if (Dot(d, d) > cull2)
                    continue;
            }
            const Mat4 m = wt.matrix * Mat4::TRS(in.position, Quat::FromAxisAngle(Vec3(0, 1, 0), in.yaw), Vec3(in.scale));
            for (usize k = 0; k < f.parts.Size(); ++k) {
                render::RenderInstance& inst = out.AddDefaulted();
                inst.world = m * partMatrices[k];
                inst.id = base ^ (u64(i) * 0x9E3779B97F4A7C15ull) ^ k;
                inst.mesh = f.parts[k].mesh;
                inst.materials = f.parts[k].materials.View();
                inst.castShadows = f.castShadows;
            }
        }
    });
    // Skinned meshes (few): their pose this frame.
    world.Query<const WorldTransform, const MeshRenderer, const Animator>().Each([&](Entity e, const WorldTransform& wt, const MeshRenderer& mr, const Animator&) {
        if (!mr.visible || !mr.mesh.IsValid())
            return;
        render::RenderInstance& inst = out.AddDefaulted();
        inst.id = e.Pack();
        inst.world = wt.matrix;
        inst.mesh = mr.mesh;
        inst.materials = mr.materials.View();
        if (animation) {
            inst.skinMatrices = animation->Palette(e);
            inst.morphWeights = animation->MorphWeights(e);
        }
        if (inst.morphWeights.Empty())
            if (const MorphTargets* m = world.Get<const MorphTargets>(e))
                inst.morphWeights = m->weights.View();
    });
    // Soft bodies: skinned to their nodes.
    world.Query<const WorldTransform, const MeshRenderer, const SoftBody>().Without<Animator>().Each(
        [&](Entity e, const WorldTransform& wt, const MeshRenderer& mr, const SoftBody& sb) {
            if (!mr.visible || !mr.mesh.IsValid())
                return;
            render::RenderInstance& inst = out.AddDefaulted();
            inst.id = e.Pack();
            inst.world = wt.matrix;
            inst.mesh = mr.mesh;
            inst.materials = mr.materials.View();
            inst.skinMatrices = sb.palette.View();
        });
    // Meshes with morph targets (no Animator).
    world.Query<const WorldTransform, const MeshRenderer, const MorphTargets>().Without<Animator>().Without<Flammable>().Without<SoftBody>().Each(
        [&](Entity e, const WorldTransform& wt, const MeshRenderer& mr, const MorphTargets& m) {
            if (!mr.visible || !mr.mesh.IsValid())
                return;
            render::RenderInstance& inst = out.AddDefaulted();
            inst.id = e.Pack();
            inst.world = wt.matrix;
            inst.mesh = mr.mesh;
            inst.materials = mr.materials.View();
            inst.morphWeights = m.weights.View();
        });
    world.Query<const WorldTransform, const MeshRenderer, const Flammable>().Without<Animator>().Without<SoftBody>().Each([&](Entity e, const WorldTransform& wt, const MeshRenderer& mr, const Flammable& f) {
        if (!mr.visible || !mr.mesh.IsValid())
            return;
        render::RenderInstance& inst = out.AddDefaulted();
        inst.id = e.Pack();
        inst.world = wt.matrix;
        inst.mesh = mr.mesh;
        inst.materials = mr.materials.View();
        if (f.charRadius > 0.0f) {
            inst.burnOrigin = wt.matrix.TransformPointAffine(f.burnOrigin);
            inst.burnRadius = f.charRadius;
            inst.charring = f.charring;
            inst.burnGlow = f.glow;
            inst.embers = f.embers;
        }
    });
}

void GatherLights(World& world, Array<render::LightDesc>& out)
{
    world.Query<const WorldTransform, const PointLight>().Each([&](Entity e, const WorldTransform& wt, const PointLight& l) {
        render::LightDesc& d = out.AddDefaulted();
        d.type = render::LightType::Point;
        d.castShadows = l.castShadows;
        d.id = e.index + 1;
        d.position = wt.matrix.GetTranslation();
        d.color = l.color;
        d.intensity = render::LumensToCandela(l.lumens);
        d.range = l.range;
    });
    world.Query<const WorldTransform, const SpotLight>().Each([&](Entity e, const WorldTransform& wt, const SpotLight& l) {
        render::LightDesc& d = out.AddDefaulted();
        d.type = render::LightType::Spot;
        d.castShadows = l.castShadows;
        d.id = e.index + 1;
        d.position = wt.matrix.GetTranslation();
        d.direction = SafeNormalize(wt.matrix.GetForward(), Vec3(0, 0, 1));
        d.color = l.color;
        d.intensity = render::LumensToCandela(l.lumens);
        d.range = l.range;
        d.innerAngle = l.innerAngle;
        d.outerAngle = l.outerAngle;
    });
}

namespace {
u64 HashBytes(u64 h, const void* data, usize size)
{
    const u8* p = static_cast<const u8*>(data);
    for (usize i = 0; i < size; ++i)
        h = (h ^ p[i]) * 0x100000001B3ull; // FNV-1a
    return h;
}
} // namespace

u64 SceneContentKey(Span<const render::RenderInstance> instances, Span<const render::LightDesc> lights)
{
    // Word-wise (64-bit) mixing: this runs over every instance each frame.
    u64 h = 0xCBF29CE484222325ull;
    auto words = [&h](const void* data, usize size) {
        const u8* p = static_cast<const u8*>(data);
        for (usize o = 0; o + 8 <= size; o += 8) {
            u64 w;
            MemCopy(&w, p + o, 8);
            h = (h ^ w) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
        }
    };
    for (const render::RenderInstance& i : instances) {
        words(&i.world, sizeof(i.world));
        words(&i.mesh, sizeof(i.mesh));
        for (const asset::AssetId& m : i.materials)
            words(&m, sizeof(m));
    }
    for (const render::LightDesc& l : lights) {
        h = HashBytes(h, &l.position, sizeof(l.position));
        h = HashBytes(h, &l.color, sizeof(l.color));
        h = HashBytes(h, &l.intensity, sizeof(l.intensity));
    }
    return h;
}

void GatherReflectionProbes(World& world, Array<render::ReflectionProbeDesc>& out, u64 contentKey)
{
    world.Query<const WorldTransform, const ReflectionProbe>().Each([&](ecs::Entity e, const WorldTransform& wt, const ReflectionProbe& p) {
        render::ReflectionProbeDesc d;
        d.id = e.index + 1;
        d.position = wt.matrix.GetTranslation();
        const Vec3 s = wt.matrix.GetScale();
        const Vec3 half(Abs(p.extent.x * s.x), Abs(p.extent.y * s.y), Abs(p.extent.z * s.z));
        d.boxMin = d.position - half;
        d.boxMax = d.position + half;
        d.blendDistance = p.blendDistance;
        d.intensity = p.intensity;
        d.priority = p.priority;
        d.boxProjection = p.boxProjection;
        u64 h = HashBytes(contentKey ^ e.Pack(), &d.position, sizeof(d.position));
        d.key = HashBytes(h, &half, sizeof(half));
        out.PushBack(d);
    });
}

const HdriSky* FindHdriSky(World& world)
{
    const HdriSky* found = nullptr;
    world.Query<const HdriSky>().Each([&](const HdriSky& s) {
        if (!found)
            found = &s;
    });
    return found;
}

const DirectionalLight* FindDirectionalLight(World& world, Vec3* direction)
{
    const DirectionalLight* found = nullptr;
    world.Query<const WorldTransform, const DirectionalLight>().Each([&](const WorldTransform& wt, const DirectionalLight& l) {
        if (found)
            return;
        found = &l;
        if (direction)
            *direction = SafeNormalize(wt.matrix.GetForward(), Vec3(0, -1, 0));
    });
    return found;
}

const SkyLight* FindSkyLight(World& world)
{
    const SkyLight* found = nullptr;
    world.Query<const SkyLight>().Each([&](const SkyLight& l) {
        if (!found)
            found = &l;
    });
    return found;
}

const HeightFog* FindHeightFog(World& world, f32* height)
{
    const HeightFog* found = nullptr;
    world.Query<const WorldTransform, const HeightFog>().Each([&](const WorldTransform& wt, const HeightFog& f) {
        if (found)
            return;
        found = &f;
        if (height)
            *height = wt.matrix.GetTranslation().y;
    });
    return found;
}

render::CloudSettings FindClouds(World& world)
{
    render::CloudSettings s;
    const bool sceneWind = HasWind(world);
    world.Query<const VolumetricClouds>().Each([&](Entity e, const VolumetricClouds& c) {
        if (s.enabled)
            return;
        s.enabled = true;
        s.coverage = c.coverage;
        s.cloudType = c.cloudType;
        s.bottom = c.bottom;
        s.thickness = c.thickness;
        s.density = c.density;
        s.detail = c.detail;
        s.scale = c.scale;
        s.evolution = c.evolution;
        s.ambient = c.ambient;
        s.silverLining = c.silverLining;
        s.lightning = c.lightning;
        s.seed = u32(Max(c.seed, 1));
        s.windSpeed = c.windSpeed;
        s.windDirection = c.windDirection;
        // Wind Zones (or the entity's own Wind Response) steer them; winds aloft blow ~2.5x harder than at the ground.
        if (const WindResponse* r = world.Get<const WindResponse>(e); sceneWind || (r && r->customWind)) {
            const Vec3 w = WindFor(world, e, Vec3(0.0f));
            s.windSpeed = Length(Vec3(w.x, 0.0f, w.z)) * 2.5f;
            if (s.windSpeed > 1e-3f)
                s.windDirection = RadToDeg(Atan2(w.x, w.z));
        }
        s.albedo = c.color;
        s.shadowStrength = c.shadows;
    });
    return s;
}

void GatherFluidVolumes(World& world, Array<render::FluidVolumeDesc>& out, bool playing)
{
    world.Query<const WorldTransform, const FluidVolume>().Each([&](Entity e, const WorldTransform& wt, const FluidVolume& f) {
        render::FluidVolumeDesc& d = out.AddDefaulted();
        d.id = e.Pack();
        d.world = wt.matrix;
        d.resolution = u32(Max(f.resolution, 16));
        d.simulate = playing || f.simulateInEditor;
        d.sourcePosition = f.sourcePosition;
        d.sourceRadius = f.sourceRadius;
        d.sourceVelocity = f.sourceVelocity;
        d.smokeRate = f.smoke;
        d.heat = f.heat;
        d.noise = f.turbulence;
        d.buoyancy = f.buoyancy;
        d.weight = f.weight;
        d.vorticity = f.swirl;
        d.smokeFade = f.smokeFade;
        d.coolRate = f.cooling;
        d.density = f.density;
        d.smokeColor = f.smokeColor;
        d.fireIntensity = f.fireIntensity;
        d.fireThreshold = f.fireThreshold;
    });
}

void GatherFogVolumes(World& world, Array<render::FogVolumeDesc>& out)
{
    const bool sceneWind = HasWind(world);
    world.Query<const WorldTransform, const FogVolume>().Each([&](Entity e, const WorldTransform& wt, const FogVolume& v) {
        if (v.density <= 0.0f)
            return;
        render::FogVolumeDesc& d = out.AddDefaulted();
        d.worldToVolume = Inverse(wt.matrix);
        d.shape = v.shape == FogShape::Sphere ? render::FogVolumeShape::Sphere : render::FogVolumeShape::Box;
        d.density = v.density;
        d.albedo = v.color;
        d.emissive = v.emissive * v.emissiveIntensity;
        d.edgeFade = v.edgeFade;
        d.noiseScale = v.noiseScale;
        d.noiseAmount = v.noiseAmount;
        d.wind = v.wind;
        if (const WindResponse* r = world.Get<const WindResponse>(e); sceneWind || (r && r->customWind))
            d.wind = WindFor(world, e, wt.GetPosition()) * 0.6f; // the fog's noise drifts with the wind
    });
}

void GatherDecals(World& world, Array<render::DecalDesc>& out, const Function<u32(StringView, bool)>& texture)
{
    world.Query<const WorldTransform, const Decal>().Each([&](const WorldTransform& wt, const Decal& d) {
        render::DecalDesc g;
        g.world = wt.matrix * Mat4::Scale(Vec3(Max(Abs(d.extent.x), 0.001f), Max(Abs(d.extent.y), 0.001f), Max(Abs(d.extent.z), 0.001f)));
        if (texture) {
            if (!d.texture.Empty())
                g.albedo = texture(d.texture, false);
            if (!d.normalTexture.Empty())
                g.normal = texture(d.normalTexture, true);
            if (!d.texture.Empty() && g.albedo == rhi::InvalidDescriptor)
                return; // still loading: no solid-color flash
        }
        g.color = d.color;
        g.opacity = d.opacity;
        g.affectRoughness = d.affectRoughness;
        g.roughness = d.roughness;
        g.normalStrength = d.normalStrength;
        g.angleFade = d.angleFade;
        g.emissive = d.emissive;
        g.sortOrder = d.sortOrder;
        out.PushBack(g);
    });
}

void GatherWater(World& world, Array<render::WaterSurface>& out)
{
    const bool sceneWind = HasWind(world);
    world.Query<const WorldTransform, const Water>().Each([&](Entity e, const WorldTransform& wt, const Water& w) {
        render::WaterSurface& s = out.AddDefaulted();
        // Position + rotation only: the size is explicit.
        const Vec3 pos = wt.matrix.GetTranslation();
        const Vec3 fwd = wt.matrix.GetForward();
        const f32 yaw = (Abs(fwd.x) + Abs(fwd.z)) > 1e-6f ? Atan2(fwd.x, fwd.z) : 0.0f; // the surface stays level
        s.world = Mat4::TRS(pos, Quat::FromAxisAngle(Vec3::Up(), yaw), Vec3(1.0f));
        s.size = Vec2(Max(w.size.x, 0.1f), Max(w.size.y, 0.1f));
        s.windSpeed = w.windSpeed;
        s.fetch = w.fetch;
        s.windDirection = DegToRad(w.windDirection);
        s.waveAmount = w.waveAmount;
        // Wind Zones (or the entity's own wind) steer the waves; a stronger wind raises a bigger sea, a breeze doesn't
        // calm an ocean (its swell comes from far away). The steady wind: gusts would rebuild the spectrum all the time.
        if (const WindResponse* r = world.Get<const WindResponse>(e); sceneWind || (r && r->customWind)) {
            const Vec3 wind = WindSteadyFor(world, e, pos);
            const f32 speed = Length(Vec3(wind.x, 0.0f, wind.z));
            if (speed > 0.05f)
                s.windDirection = Atan2(wind.x, wind.z);
            s.windSpeed = Max(s.windSpeed, speed);
        }
        s.choppiness = w.choppiness;
        s.shallowColor = w.shallowColor;
        s.deepColor = w.deepColor;
        s.scatterColor = w.scatterColor;
        s.caustics = w.caustics;
        s.reflections = w.reflections;
        s.clarity = w.clarity;
        s.refraction = w.refraction;
        s.foam = w.foam;
        s.shoreFoam = w.shoreFoam;
        s.roughness = w.roughness;
    });
}

} // namespace zx
