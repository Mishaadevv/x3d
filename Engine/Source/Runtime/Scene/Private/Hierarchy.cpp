#include "Scene/Hierarchy.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Reflection/Reflection.h"

namespace zx::scene {

ZX_REFLECT_STRUCT_BEGIN(EntityName)
    ZX_FIELD(value)
ZX_REFLECT_STRUCT_END()


namespace {

void RemoveFromChildren(World& world, Entity parent, Entity child)
{
    if (Children* c = world.Get<Children>(parent))
        c->entities.Remove(child);
}

Mat4 LocalMatrix(const World& world, Entity e)
{
    const Transform* t = world.Get<Transform>(e);
    return t ? t->ToMatrix() : Mat4::Identity();
}

void Propagate(World& world, Entity e, const Mat4& parentMatrix)
{
    const Children* children = world.Get<const Children>(e);
    if (!children)
        return;
    for (Entity child : children->entities) {
        const Mat4 m = parentMatrix * LocalMatrix(world, child);
        if (WorldTransform* wt = world.Get<WorldTransform>(child))
            wt->matrix = m;
        Propagate(world, child, m);
    }
}

} // namespace

void InstallHierarchyHooks(World& world)
{
    world.OnRemove<Parent>([](World& w, Entity e, Parent& p) { RemoveFromChildren(w, p.entity, e); });
    world.OnRemove<Children>([](World& w, Entity, Children& c) {
        // Orphan the children (copy: removing Parent edits this list through the hook above).
        const Array<Entity> children = c.entities;
        for (Entity child : children)
            if (w.IsAlive(child))
                w.Remove<Parent>(child);
    });
}

Entity GetParent(const World& world, Entity e)
{
    const Parent* p = world.Get<Parent>(e);
    return p ? p->entity : ecs::NullEntity;
}

Span<const Entity> GetChildren(const World& world, Entity e)
{
    const Children* c = world.Get<Children>(e);
    return c ? c->entities.View() : Span<const Entity>();
}

bool IsAncestorOf(const World& world, Entity ancestor, Entity e)
{
    for (Entity p = GetParent(world, e); p; p = GetParent(world, p))
        if (p == ancestor)
            return true;
    return false;
}

bool SetParent(World& world, Entity child, Entity parent, bool keepWorldTransform)
{
    if (!world.IsAlive(child) || (parent && !world.IsAlive(parent)))
        return false;
    if (parent == child || (parent && IsAncestorOf(world, child, parent)))
        return false;
    const Entity old = GetParent(world, child);
    if (old == parent)
        return true;
    const Mat4 worldMatrix = keepWorldTransform ? ComputeWorldMatrix(world, child) : Mat4::Identity();

    if (old)
        RemoveFromChildren(world, old, child);
    if (parent) {
        if (Parent* p = world.Get<Parent>(child))
            p->entity = parent;
        else
            world.Add(child, Parent{parent});
        Children* c = world.Get<Children>(parent);
        if (!c)
            c = world.Add(parent, Children{});
        c->entities.PushBack(child);
    } else {
        // The OnRemove hook would try to unlink from `old` again - harmless, already removed.
        world.Remove<Parent>(child);
    }

    if (keepWorldTransform) {
        const Mat4 parentMatrix = parent ? ComputeWorldMatrix(world, parent) : Mat4::Identity();
        const Transform local = Transform::FromMatrix(Inverse(parentMatrix) * worldMatrix);
        if (Transform* t = world.Get<Transform>(child))
            *t = local;
        else
            world.Add(child, local);
    }
    return true;
}

void DestroyHierarchy(World& world, Entity root)
{
    if (!world.IsAlive(root))
        return;
    Array<Entity> stack{root};
    Array<Entity> all;
    while (!stack.Empty()) {
        const Entity e = stack.PopBackValue();
        all.PushBack(e);
        for (Entity c : GetChildren(world, e))
            stack.PushBack(c);
    }
    // Leaves first, so hooks don't re-parent anything that is about to die.
    for (usize i = all.Size(); i > 0; --i)
        world.Destroy(all[i - 1]);
}

Mat4 ComputeWorldMatrix(const World& world, Entity e)
{
    Mat4 m = LocalMatrix(world, e);
    for (Entity p = GetParent(world, e); p; p = GetParent(world, p))
        m = LocalMatrix(world, p) * m;
    return m;
}

void UpdateWorldTransforms(World& world)
{
    Array<Entity> rootsWithChildren;
    world.Query<WorldTransform>().Without<Parent>().Each([&](Entity e, WorldTransform& wt) {
        wt.matrix = LocalMatrix(world, e);
    });
    world.Query<const Children>().Without<Parent>().Each([&](Entity e, const Children&) { rootsWithChildren.PushBack(e); });
    World::IterationScope scope(world);
    jobs::ParallelFor(u32(rootsWithChildren.Size()), 4, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i)
            Propagate(world, rootsWithChildren[i], LocalMatrix(world, rootsWithChildren[i]));
    });
}

Entity CreateNode(World& world, StringView name, const Transform& local, Entity parent)
{
    const Entity e = world.Create(Transform(local), WorldTransform{});
    if (!name.Empty())
        world.Add(e, EntityName{String(name)});
    if (parent)
        SetParent(world, e, parent);
    return e;
}

} // namespace zx::scene
