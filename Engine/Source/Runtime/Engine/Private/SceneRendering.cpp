#include "Engine/SceneRendering.h"
#include "Engine/Fire.h"

#include "Core/Reflection/Reflection.h"

namespace zx {

ZX_REFLECT_STRUCT_BEGIN(MeshRenderer)
    ZX_FIELD(mesh, Meta::AssetType("Mesh"), Meta::ReadOnly())
    ZX_FIELD(materials, Meta::AssetType("Material"), Meta::ReadOnly())
    ZX_FIELD(visible)
    ZX_FIELD(collision, Meta::Tooltip("Solid: blocks Character Controllers and rigid bodies (as a static triangle mesh unless the entity has colliders)"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(DirectionalLight)
    ZX_FIELD(intensity, Meta::Range(0, 10), Meta::Tooltip("1 = the real sun; rotate the entity to move it (it shines along +Z)"))
    ZX_FIELD(color, Meta::Color())
    ZX_FIELD(castShadows)
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

ZX_REFLECT_STRUCT_BEGIN(VolumetricClouds)
    ZX_FIELD(coverage, Meta::Range(0, 1), Meta::Tooltip("0 = clear sky, 1 = overcast"))
    ZX_FIELD(bottom, Meta::Range(100, 10000), Meta::Units("m"))
    ZX_FIELD(thickness, Meta::Range(50, 10000), Meta::Units("m"))
    ZX_FIELD(density, Meta::Range(0, 1), Meta::Units("1/m"))
    ZX_FIELD(detail, Meta::Range(0, 1), Meta::Tooltip("Edges eroded by fine noise: wispy"))
    ZX_FIELD(windSpeed, Meta::Range(0, 200), Meta::Units("m/s"))
    ZX_FIELD(windDirection, Meta::Range(0, 360), Meta::Units("deg"))
    ZX_FIELD(color, Meta::Color())
    ZX_FIELD(shadows, Meta::Range(0, 1), Meta::Tooltip("How dark their shadows are on the ground"))
ZX_REFLECT_STRUCT_END()

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

ZX_REFLECT_STRUCT_BEGIN(Water)
    ZX_FIELD(size, Meta::Tooltip("Width and length in meters"))
    ZX_FIELD(waveHeight, Meta::Range(0, 5), Meta::Units("m"))
    ZX_FIELD(waveLength, Meta::Range(0.5, 100), Meta::Units("m"))
    ZX_FIELD(waveSpeed, Meta::Range(0, 5))
    ZX_FIELD(windDirection, Meta::Range(-180, 180), Meta::Units("deg"))
    ZX_FIELD(choppiness, Meta::Range(0, 1), Meta::Tooltip("0 = round waves, 1 = sharp crests"))
    ZX_FIELD(shallowColor, Meta::Color())
    ZX_FIELD(deepColor, Meta::Color())
    ZX_FIELD(clarity, Meta::Range(0.1, 50), Meta::Units("m"), Meta::Tooltip("How deep you can see into the water"))
    ZX_FIELD(refraction, Meta::Range(0, 0.2))
    ZX_FIELD(foam, Meta::Range(0, 2))
    ZX_FIELD(shoreFoam, Meta::Range(0, 5), Meta::Units("m"), Meta::Tooltip("Width of the foam line along shores"))
    ZX_FIELD(roughness, Meta::Range(0.01, 0.5), Meta::Tooltip("Sharpness of the sun glints"))
ZX_REFLECT_STRUCT_END()

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

Entity InstantiatePrefab(World& world, const asset::PrefabAsset& prefab, const Transform& rootTransform, Entity parent)
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
            world.Add(e, move(mr));
        }
        created.PushBack(e);
    }
    return root;
}

void GatherRenderInstances(World& world, Array<render::RenderInstance>& out)
{
    world.Query<const WorldTransform, const MeshRenderer>().Each([&](Entity e, const WorldTransform& wt, const MeshRenderer& mr) {
        if (!mr.visible || !mr.mesh.IsValid())
            return;
        render::RenderInstance& inst = out.AddDefaulted();
        inst.id = e.Pack();
        inst.world = wt.matrix;
        inst.mesh = mr.mesh;
        inst.materials = mr.materials.View();
        if (const Flammable* f = world.Get<const Flammable>(e); f && f->charRadius > 0.0f) {
            inst.burnOrigin = wt.matrix.TransformPointAffine(f->burnOrigin);
            inst.burnRadius = f->charRadius;
            inst.charring = f->charring;
            inst.burnGlow = f->glow;
            inst.embers = f->embers;
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
    u64 h = 0xCBF29CE484222325ull;
    for (const render::RenderInstance& i : instances) {
        h = HashBytes(h, &i.world, sizeof(i.world));
        h = HashBytes(h, &i.mesh, sizeof(i.mesh));
        for (const asset::AssetId& m : i.materials)
            h = HashBytes(h, &m, sizeof(m));
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
    world.Query<const VolumetricClouds>().Each([&](const VolumetricClouds& c) {
        if (s.enabled)
            return;
        s.enabled = true;
        s.coverage = c.coverage;
        s.bottom = c.bottom;
        s.thickness = c.thickness;
        s.density = c.density;
        s.detail = c.detail;
        s.windSpeed = c.windSpeed;
        s.windDirection = c.windDirection;
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
    world.Query<const WorldTransform, const FogVolume>().Each([&](const WorldTransform& wt, const FogVolume& v) {
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
    world.Query<const WorldTransform, const Water>().Each([&](const WorldTransform& wt, const Water& w) {
        render::WaterSurface& s = out.AddDefaulted();
        // Position + rotation only: the size is explicit.
        const Vec3 pos = wt.matrix.GetTranslation();
        const Vec3 fwd = wt.matrix.GetForward();
        const f32 yaw = (Abs(fwd.x) + Abs(fwd.z)) > 1e-6f ? Atan2(fwd.x, fwd.z) : 0.0f; // the surface stays level
        s.world = Mat4::TRS(pos, Quat::FromAxisAngle(Vec3::Up(), yaw), Vec3(1.0f));
        s.size = Vec2(Max(w.size.x, 0.1f), Max(w.size.y, 0.1f));
        s.waveHeight = w.waveHeight;
        s.waveLength = w.waveLength;
        s.waveSpeed = w.waveSpeed;
        s.windDirection = DegToRad(w.windDirection);
        s.choppiness = w.choppiness;
        s.shallowColor = w.shallowColor;
        s.deepColor = w.deepColor;
        s.clarity = w.clarity;
        s.refraction = w.refraction;
        s.foam = w.foam;
        s.shoreFoam = w.shoreFoam;
        s.roughness = w.roughness;
    });
}

} // namespace zx
