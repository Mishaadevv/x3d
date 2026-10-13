#include "Engine/SceneRendering.h"

#include "Core/Reflection/Reflection.h"

namespace zx {

ZX_REFLECT_STRUCT_BEGIN(MeshRenderer)
    ZX_FIELD(mesh, Meta::AssetType("Mesh"), Meta::ReadOnly())
    ZX_FIELD(materials, Meta::AssetType("Material"), Meta::ReadOnly())
    ZX_FIELD(visible)
    ZX_FIELD(collision, Meta::Tooltip("Blocks Character Controllers (box collision until M11 physics)"))
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
    world.Query<const WorldTransform, const MeshRenderer>().Each([&](const WorldTransform& wt, const MeshRenderer& mr) {
        if (!mr.visible || !mr.mesh.IsValid())
            return;
        render::RenderInstance& inst = out.AddDefaulted();
        inst.world = wt.matrix;
        inst.mesh = mr.mesh;
        inst.materials = mr.materials.View();
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
