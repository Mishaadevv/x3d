#include "Engine/Raycast.h"
#include "Engine/SceneRendering.h"

namespace zx {

bool RaycastMeshes(ecs::World& world, render::RenderAssets& assets, const Ray& ray, RaycastHit& hit, f32 maxDistance,
                   const RaycastFilter* filter)
{
    hit = RaycastHit{};
    hit.distance = maxDistance;
    bool found = false;
    world.Query<const MeshRenderer, const scene::WorldTransform>().Each(
        [&](ecs::Entity e, const MeshRenderer& mr, const scene::WorldTransform& wt) {
            if (!mr.visible || (filter && !(*filter)(e)))
                return;
            const render::GpuMesh* mesh = assets.GetMesh(mr.mesh);
            if (!mesh || mesh->cpuIndices.Empty())
                return;
            // Test in mesh space; distances convert back through the direction's length.
            const Mat4 inv = Inverse(wt.matrix);
            const Vec3 o = inv.TransformPoint(ray.origin);
            const Vec3 d = inv.TransformVector(ray.direction);
            const f32 len = Length(d);
            if (len < 1e-12f)
                return;
            const Ray local(o, d / len);
            const f32 localMax = hit.distance * len;
            f32 tn, tf;
            if (!RayIntersectsAABB(local, mesh->bounds, tn, tf, localMax))
                return;
            for (usize s = 0; s < mesh->sections.Size(); ++s) {
                if (!RayIntersectsAABB(local, mesh->sections[s].bounds, tn, tf, hit.distance * len))
                    continue;
                const UVec2 range = mesh->cpuSections[s];
                for (u32 i = range.x; i + 2 < range.x + range.y; i += 3) {
                    const Vec3 a = mesh->cpuPositions[mesh->cpuIndices[i]];
                    const Vec3 b = mesh->cpuPositions[mesh->cpuIndices[i + 1]];
                    const Vec3 c = mesh->cpuPositions[mesh->cpuIndices[i + 2]];
                    f32 t, u, v;
                    if (!RayIntersectsTriangle(local, a, b, c, t, u, v) || t <= 0.0f)
                        continue;
                    const f32 worldT = t / len;
                    if (worldT >= hit.distance)
                        continue;
                    // Normal: inverse-transpose of the world matrix, turned towards the ray.
                    Vec3 n = SafeNormalize(Transpose(inv).TransformVector(Cross(b - a, c - a)), Vec3::Up());
                    if (Dot(n, ray.direction) > 0.0f)
                        n = -n;
                    hit.entity = e;
                    hit.distance = worldT;
                    hit.point = ray.At(worldT);
                    hit.normal = n;
                    found = true;
                }
            }
        });
    return found;
}

} // namespace zx
