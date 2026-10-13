#pragma once

// Transform hierarchy on top of ZECS.
//
// Components: zx::Transform (local TRS, from Core/Math), WorldTransform (computed), Parent, Children, EntityName.
// Call InstallHierarchyHooks(world) once so removing/destroying entities keeps Parent/Children consistent, change
// parents only through SetParent(), and run UpdateWorldTransforms() once per frame (after gameplay, before render).

#include "Core/Math/Math.h"
#include "Core/Reflection/Reflection.h"
#include "Core/String/String.h"
#include "Scene/ECS/World.h"

namespace zx::scene {

using ecs::Entity;
using ecs::World;

struct WorldTransform
{
    Mat4 matrix = Mat4::Identity();
    Vec3 GetPosition() const { return matrix.GetTranslation(); }
};

struct Parent
{
    Entity entity;
};

struct Children
{
    Array<Entity> entities;
};

struct EntityName
{
    String value;
};
ZX_DECLARE_TYPE(EntityName);

void InstallHierarchyHooks(World& world);

/// Re-parents `child` (NullEntity detaches). keepWorldTransform adjusts the local transform so the world pose stays
/// the same. Returns false (and does nothing) if it would create a cycle or an entity is dead.
bool SetParent(World& world, Entity child, Entity parent, bool keepWorldTransform = false);
Entity GetParent(const World& world, Entity e);
Span<const Entity> GetChildren(const World& world, Entity e);
bool IsAncestorOf(const World& world, Entity ancestor, Entity e);
/// Destroys the entity and all its descendants.
void DestroyHierarchy(World& world, Entity root);

/// World matrix computed by walking up the parent chain (does not need UpdateWorldTransforms).
Mat4 ComputeWorldMatrix(const World& world, Entity e);
/// Recomputes WorldTransform of every entity that has one (roots, then descendants; parallel over root subtrees).
void UpdateWorldTransforms(World& world);

/// Convenience: entity with Transform + WorldTransform (+ EntityName if a name is given).
Entity CreateNode(World& world, StringView name = {}, const Transform& local = Transform::Identity(), Entity parent = {});

} // namespace zx::scene
