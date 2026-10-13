#pragma once

// Ray queries against the triangles of the scene's meshes (LOD0, CPU copies kept by RenderAssets). Used by editor
// picking, the Brush / Measure / Drop to Ground tools and (later) gameplay queries.
//
//   RaycastHit hit;
//   if (RaycastMeshes(world, renderAssets, ray, hit)) { hit.entity, hit.point, hit.normal, hit.distance }

#include "Core/Containers/Function.h"
#include "Core/Math/Math.h"
#include "Render/RenderAssets.h"
#include "Scene/ECS/World.h"

namespace zx {

struct RaycastHit
{
    ecs::Entity entity;
    Vec3 point;
    Vec3 normal; // world space, facing the ray origin
    f32 distance = Infinity;
};

/// Return false to skip an entity (e.g. the object being moved, painted instances).
using RaycastFilter = Function<bool(ecs::Entity), 32>;

/// Closest hit of visible MeshRenderers (resident meshes only). `filter` may be null.
bool RaycastMeshes(ecs::World& world, render::RenderAssets& assets, const Ray& ray, RaycastHit& hit, f32 maxDistance = Infinity,
                   const RaycastFilter* filter = nullptr);

} // namespace zx
