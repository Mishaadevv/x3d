#include "Engine/PhysicsSystem.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflection.h"
#include "Engine/Gameplay.h"
#include "Render/RenderAssets.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogPhysics, Info);

namespace zx {

ZX_REFLECT_ENUM_BEGIN(RigidBodyType)
    ZX_ENUM_VALUE(Dynamic)
    ZX_ENUM_VALUE(Kinematic)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(RigidBody)
    ZX_FIELD(type, Meta::Tooltip("Dynamic: simulated. Kinematic: moved by the game, pushes dynamic bodies"))
    ZX_FIELD(mass, Meta::Range(0, 100000), Meta::Units("kg"), Meta::Tooltip("0 = from the collider volumes (water density)"))
    ZX_FIELD(friction, Meta::Range(0, 2))
    ZX_FIELD(bounciness, Meta::Range(0, 1))
    ZX_FIELD(linearDamping, Meta::Range(0, 10))
    ZX_FIELD(angularDamping, Meta::Range(0, 10))
    ZX_FIELD(gravityScale, Meta::Range(-2, 5))
    ZX_FIELD(canSleep, Meta::Tooltip("Resting bodies stop simulating until something touches them"))
ZX_REFLECT_STRUCT_END()

#define ZX_COLLIDER_COMMON_FIELDS                                                                                          \
    ZX_FIELD(isTrigger, Meta::Tooltip("Reports overlaps to scripts (triggerEnter / triggerExit) instead of colliding"))     \
    ZX_FIELD(layer, Meta::Range(0, 31), Meta::Tooltip("Physics layer: raycasts and other queries can filter by it"))

ZX_REFLECT_STRUCT_BEGIN(BoxCollider)
    ZX_FIELD(center, Meta::Units("m"))
    ZX_FIELD(size, Meta::Units("m"), Meta::Tooltip("Full size, scaled by the transform"))
    ZX_COLLIDER_COMMON_FIELDS
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(SphereCollider)
    ZX_FIELD(center, Meta::Units("m"))
    ZX_FIELD(radius, Meta::Range(0.01, 100), Meta::Units("m"))
    ZX_COLLIDER_COMMON_FIELDS
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(CapsuleCollider)
    ZX_FIELD(center, Meta::Units("m"))
    ZX_FIELD(radius, Meta::Range(0.01, 100), Meta::Units("m"))
    ZX_FIELD(height, Meta::Range(0.02, 200), Meta::Units("m"), Meta::Tooltip("End to end, along the local Y axis"))
    ZX_COLLIDER_COMMON_FIELDS
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(MeshCollider)
    ZX_FIELD(enabled, Meta::Tooltip("Static collision with the triangles of the Mesh Renderer"))
    ZX_FIELD(layer, Meta::Range(0, 31), Meta::Tooltip("Physics layer: raycasts and other queries can filter by it"))
ZX_REFLECT_STRUCT_END()
#undef ZX_COLLIDER_COMMON_FIELDS

using namespace physics;
using ecs::Entity;

namespace {

struct Pose
{
    Vec3 position;
    Quat rotation;
    Vec3 scale = Vec3(1.0f);
};

Pose WorldPose(const ecs::World& world, Entity e)
{
    Pose p;
    if (const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(e))
        Decompose(wt->matrix, p.position, p.rotation, p.scale);
    else if (const Transform* t = world.Get<const Transform>(e)) {
        p.position = t->position;
        p.rotation = t->rotation;
        p.scale = t->scale;
    }
    p.rotation = Normalize(p.rotation);
    return p;
}

/// The colliders of entity `e` as shapes relative to `body` (a world pose without scale).
void AppendShapes(const ecs::World& world, Entity e, const Pose& body, Array<ShapeDesc>& out)
{
    const Pose p = WorldPose(world, e);
    const Vec3 s = Abs(p.scale);
    const Quat inv = Conjugate(body.rotation);
    auto place = [&](ShapeDesc shape, Vec3 center) {
        const Vec3 worldCenter = p.position + p.rotation * (p.scale * center);
        shape.position = inv * (worldCenter - body.position);
        shape.rotation = Normalize(inv * p.rotation);
        out.PushBack(shape);
    };
    auto sensor = [](ShapeDesc d, bool trigger) {
        d.isSensor = trigger;
        return d;
    };
    if (const BoxCollider* c = world.Get<const BoxCollider>(e))
        place(sensor(ShapeDesc::Box(Max(Abs(c->size) * s * 0.5f, Vec3(0.005f))), c->isTrigger), c->center);
    if (const SphereCollider* c = world.Get<const SphereCollider>(e))
        place(sensor(ShapeDesc::Sphere(Max(c->radius * Max(s.x, Max(s.y, s.z)), 0.005f)), c->isTrigger), c->center);
    if (const CapsuleCollider* c = world.Get<const CapsuleCollider>(e)) {
        const f32 r = Max(c->radius * Max(s.x, s.z), 0.005f);
        const f32 half = Max(c->height * s.y * 0.5f - r, 0.0f);
        place(sensor(ShapeDesc::Capsule(r, half), c->isTrigger), c->center);
    }
}

/// Physics layer of an entity: from its first collider component.
u32 LayerOf(const ecs::World& world, Entity e)
{
    i32 layer = 0;
    const BoxCollider* box = world.Get<const BoxCollider>(e);
    const SphereCollider* sphere = world.Get<const SphereCollider>(e);
    const CapsuleCollider* capsule = world.Get<const CapsuleCollider>(e);
    const MeshCollider* mesh = world.Get<const MeshCollider>(e);
    if (box)
        layer = box->layer;
    else if (sphere)
        layer = sphere->layer;
    else if (capsule)
        layer = capsule->layer;
    else if (mesh)
        layer = mesh->layer;
    return u32(Clamp(layer, 0, 31));
}

/// Part of a character (its body is the controller's capsule).
bool InCharacter(const ecs::World& world, Entity e)
{
    for (Entity p = e; p; p = scene::GetParent(world, p))
        if (world.Get<const CharacterController>(p))
            return true;
    return false;
}

/// Mesh Renderers with Collision on and no collider components collide as static triangle meshes.
bool ImplicitMeshCollision(const ecs::World& world, Entity e)
{
    const MeshRenderer* mr = world.Get<const MeshRenderer>(e);
    return mr && mr->collision && mr->visible && !HasCollider(world, e) && !world.Get<const RigidBody>(e) && !InCharacter(world, e);
}

bool HasRigidBodyAbove(const ecs::World& world, Entity e)
{
    for (Entity p = scene::GetParent(world, e); p; p = scene::GetParent(world, p))
        if (world.Get<const RigidBody>(p))
            return true;
    return false;
}

} // namespace

bool HasCollider(const ecs::World& world, Entity e)
{
    return world.Get<const BoxCollider>(e) || world.Get<const SphereCollider>(e) || world.Get<const CapsuleCollider>(e) ||
           world.Get<const MeshCollider>(e);
}

void GatherColliderOutlines(ecs::World& world, Entity e, Array<ColliderOutline>& out)
{
    const Pose p = WorldPose(world, e);
    Array<ShapeDesc> shapes;
    const Pose identity{p.position, p.rotation, Vec3(1.0f)};
    AppendShapes(world, e, identity, shapes);
    for (const ShapeDesc& s : shapes) {
        ColliderOutline o;
        o.type = s.type;
        o.transform = Mat4::TRS(p.position + p.rotation * s.position, p.rotation * s.rotation, Vec3(1.0f));
        o.halfExtents = s.halfExtents;
        o.radius = s.radius;
        o.halfHeight = s.halfHeight;
        out.PushBack(o);
    }
}

PhysicsSystem::PhysicsSystem() = default;
PhysicsSystem::~PhysicsSystem() = default;

void PhysicsSystem::Begin(ecs::World& world, render::RenderAssets* renderAssets)
{
    End();
    m_world.Reset(new PhysicsWorld());
    m_assets = renderAssets;
    m_accumulator = 0.0f;
    CreateBodies(world);
    u32 moving = 0;
    for (const Tracked& t : m_tracked)
        moving += t.moves ? 1u : 0u;
    ZX_LOG_INFO(LogPhysics, "Physics: {} bodies ({} moving, {} static), {} mesh colliders waiting for their mesh", m_world->BodyCount(), moving,
                m_world->BodyCount() - moving, m_pendingMeshes.Size());
}

void PhysicsSystem::End()
{
    m_world.Reset();
    m_tracked.Clear();
    m_byEntity.Clear();
    m_pendingMeshes.Clear();
}

bool PhysicsSystem::CreateBody(ecs::World& world, Entity e, bool dynamic)
{
    const Pose pose = WorldPose(world, e);
    const Pose bodyPose{pose.position, pose.rotation, Vec3(1.0f)};
    BodyDesc d;
    d.position = pose.position;
    d.rotation = pose.rotation;
    d.userData = e.Pack();
    d.layer = LayerOf(world, e);
    const RigidBody* rb = world.Get<const RigidBody>(e);
    if (dynamic && rb) {
        d.motion = rb->type == RigidBodyType::Kinematic ? MotionType::Kinematic : MotionType::Dynamic;
        d.mass = rb->mass;
        d.friction = rb->friction;
        d.restitution = rb->bounciness;
        d.linearDamping = rb->linearDamping;
        d.angularDamping = rb->angularDamping;
        d.gravityScale = rb->gravityScale;
        d.allowSleep = rb->canSleep;
        // Compound: colliders of the entity and of children without their own RigidBody.
        Array<Entity> stack{e};
        while (!stack.Empty()) {
            const Entity c = stack.PopBackValue();
            AppendShapes(world, c, bodyPose, d.shapes);
            for (Entity child : scene::GetChildren(world, c))
                if (!world.Get<const RigidBody>(child))
                    stack.PushBack(child);
        }
        if (d.shapes.Empty()) {
            ZX_LOG_WARN(LogPhysics, "RigidBody on '{}' has no Box/Sphere/Capsule Collider: not simulated",
                        world.Get<const scene::EntityName>(e) ? world.Get<const scene::EntityName>(e)->value.View() : StringView("entity"));
            return false;
        }
    } else {
        AppendShapes(world, e, bodyPose, d.shapes);
        const MeshCollider* mc = world.Get<const MeshCollider>(e);
        const MeshRenderer* mr = world.Get<const MeshRenderer>(e);
        if (((mc && mc->enabled) || ImplicitMeshCollision(world, e)) && mr) {
            const render::GpuMesh* mesh = m_assets ? m_assets->GetMesh(mr->mesh) : nullptr;
            if (!mesh || mesh->cpuIndices.Empty()) {
                if (d.shapes.Empty()) {
                    m_pendingMeshes.PushBack(e);
                    return false;
                }
                m_pendingMeshes.PushBack(e); // primitives now, the mesh when it arrives (as its own body)
            } else {
                Array<Vec3> positions;
                positions.Reserve(mesh->cpuPositions.Size());
                for (Vec3 v : mesh->cpuPositions)
                    positions.PushBack(v * pose.scale);
                // Negative scale flips the winding (triangles must face outwards).
                Array<u32> indices = mesh->cpuIndices;
                if (pose.scale.x * pose.scale.y * pose.scale.z < 0.0f)
                    for (usize i = 0; i + 2 < indices.Size(); i += 3)
                        swap(indices[i + 1], indices[i + 2]);
                d.shapes.PushBack(ShapeDesc::Mesh(m_world->CreateTriangleMesh(positions.View(), indices.View())));
            }
        }
        if (d.shapes.Empty())
            return false;
    }
    Tracked t;
    t.entity = e;
    t.body = m_world->CreateBody(d);
    t.moves = d.motion != MotionType::Static;
    t.kinematic = d.motion == MotionType::Kinematic;
    t.prevPos = t.currPos = t.written = pose.position;
    t.prevRot = t.currRot = t.writtenRot = pose.rotation;
    if (!m_byEntity.Contains(e.Pack()))
        m_byEntity.InsertOrAssign(e.Pack(), u32(m_tracked.Size()));
    m_tracked.PushBack(t);
    return true;
}

void PhysicsSystem::CreateBodies(ecs::World& world)
{
    Array<Entity> dynamicBodies, staticBodies;
    world.Query<const RigidBody>().Each([&](Entity e, const RigidBody&) { dynamicBodies.PushBack(e); });
    auto addStatic = [&](Entity e) {
        if (!world.Get<const RigidBody>(e) && !HasRigidBodyAbove(world, e)) {
            for (Entity s : staticBodies)
                if (s == e)
                    return;
            staticBodies.PushBack(e);
        }
    };
    world.Query<const BoxCollider>().Each([&](Entity e, const BoxCollider&) { addStatic(e); });
    world.Query<const SphereCollider>().Each([&](Entity e, const SphereCollider&) { addStatic(e); });
    world.Query<const CapsuleCollider>().Each([&](Entity e, const CapsuleCollider&) { addStatic(e); });
    world.Query<const MeshCollider>().Each([&](Entity e, const MeshCollider&) { addStatic(e); });
    world.Query<const MeshRenderer>().Each([&](Entity e, const MeshRenderer&) {
        if (ImplicitMeshCollision(world, e))
            addStatic(e);
    });
    for (Entity e : staticBodies)
        CreateBody(world, e, false);
    for (Entity e : dynamicBodies)
        CreateBody(world, e, true);
    // Characters push dynamic bodies: a kinematic capsule follows each Character Controller.
    world.Query<const CharacterController>().Each([&](Entity e, const CharacterController& c) {
        if (m_byEntity.Contains(e.Pack()))
            return;
        const Pose pose = WorldPose(world, e);
        BodyDesc d;
        d.motion = MotionType::Kinematic;
        d.position = pose.position;
        d.userData = e.Pack();
        const f32 r = Max(c.radius, 0.05f);
        d.shapes.PushBack(ShapeDesc::Capsule(r, Max(c.height * 0.5f - r, 0.0f), Vec3(0.0f, c.height * 0.5f, 0.0f)));
        Tracked t;
        t.entity = e;
        t.body = m_world->CreateBody(d);
        t.moves = false; // the character moves itself; the body only follows
        t.kinematic = true;
        t.prevPos = t.currPos = t.written = pose.position;
        m_byEntity.InsertOrAssign(e.Pack(), u32(m_tracked.Size()));
        m_tracked.PushBack(t);
    });
}

void PhysicsSystem::Tick(ecs::World& world, f32 dt)
{
    m_events.Clear();
    if (!m_world)
        return;
    // Mesh colliders whose meshes arrived since.
    for (usize i = 0; i < m_pendingMeshes.Size();) {
        const Entity e = m_pendingMeshes[i];
        const MeshRenderer* mr = world.IsAlive(e) ? world.Get<const MeshRenderer>(e) : nullptr;
        const render::GpuMesh* mesh = mr && m_assets ? m_assets->GetMesh(mr->mesh) : nullptr;
        if (!mr) {
            m_pendingMeshes.RemoveAtSwap(i);
        } else if (mesh && !mesh->cpuIndices.Empty()) {
            m_pendingMeshes.RemoveAtSwap(i);
            // Mesh only (primitives on the same entity already have their body).
            const Pose pose = WorldPose(world, e);
            Array<Vec3> positions;
            for (Vec3 v : mesh->cpuPositions)
                positions.PushBack(v * pose.scale);
            Array<u32> indices = mesh->cpuIndices;
            if (pose.scale.x * pose.scale.y * pose.scale.z < 0.0f)
                for (usize k = 0; k + 2 < indices.Size(); k += 3)
                    swap(indices[k + 1], indices[k + 2]);
            BodyDesc d;
            d.position = pose.position;
            d.rotation = pose.rotation;
            d.userData = e.Pack();
            d.layer = LayerOf(world, e);
            d.shapes.PushBack(ShapeDesc::Mesh(m_world->CreateTriangleMesh(positions.View(), indices.View())));
            m_world->CreateBody(d);
            if (m_pendingMeshes.Empty())
                ZX_LOG_INFO(LogPhysics, "Physics: mesh colliders ready ({} bodies)", m_world->BodyCount());
        } else {
            ++i;
        }
    }

    // The game moved an entity: kinematic bodies follow it, dynamic ones are teleported.
    for (Tracked& t : m_tracked) {
        if (!world.IsAlive(t.entity)) {
            if (m_world->IsValid(t.body))
                m_world->DestroyBody(t.body);
            continue;
        }
        if (!t.kinematic && !t.moves)
            continue;
        const Pose p = WorldPose(world, t.entity);
        if (t.kinematic)
            continue; // handled per step below
        if (DistanceSq(p.position, t.written) > 1e-6f || !IsNearlyEqual(p.rotation, t.writtenRot, 1e-4f)) {
            m_world->SetTransform(t.body, p.position, p.rotation);
            m_world->SetLinearVelocity(t.body, Vec3());
            m_world->SetAngularVelocity(t.body, Vec3());
            t.prevPos = t.currPos = p.position;
            t.prevRot = t.currRot = p.rotation;
        }
    }

    m_accumulator = Min(m_accumulator + dt, FixedStep * 4.0f); // at most 4 steps per frame (no spiral of death)
    while (m_accumulator >= FixedStep) {
        for (Tracked& t : m_tracked)
            if (t.kinematic && world.IsAlive(t.entity) && m_world->IsValid(t.body)) {
                const Pose p = WorldPose(world, t.entity);
                m_world->MoveKinematic(t.body, p.position, p.rotation, FixedStep);
            }
        m_world->Step(FixedStep);
        m_accumulator -= FixedStep;
        for (Tracked& t : m_tracked)
            if (t.moves && m_world->IsValid(t.body)) {
                t.prevPos = t.currPos;
                t.prevRot = t.currRot;
                t.currPos = m_world->GetPosition(t.body);
                t.currRot = m_world->GetRotation(t.body);
            }
    }

    // Contact events of this frame's steps, as entities.
    m_rawEvents.Clear();
    m_world->TakeEvents(m_rawEvents);
    for (const ContactEvent& r : m_rawEvents) {
        PhysicsEvent e;
        e.type = r.type;
        e.a = Entity::Unpack(m_world->IsValid(r.a) ? m_world->GetUserData(r.a) : 0);
        e.b = Entity::Unpack(m_world->IsValid(r.b) ? m_world->GetUserData(r.b) : 0);
        e.point = r.point;
        e.normal = r.normal;
        e.speed = r.speed;
        if (e.a && e.b)
            m_events.PushBack(e);
    }

    // Write the interpolated poses of simulated bodies to their entities.
    const f32 alpha = m_accumulator / FixedStep;
    for (Tracked& t : m_tracked) {
        if (!t.moves || t.kinematic || !world.IsAlive(t.entity) || !m_world->IsValid(t.body))
            continue;
        const Vec3 pos = Lerp(t.prevPos, t.currPos, alpha);
        const Quat rot = Nlerp(t.prevRot, t.currRot, alpha);
        Transform* local = world.Get<Transform>(t.entity);
        if (!local)
            continue;
        const Entity parent = scene::GetParent(world, t.entity);
        if (parent) {
            const Transform parentWorld = Transform::FromMatrix(scene::ComputeWorldMatrix(world, parent));
            const Transform inv = parentWorld.Inverse();
            local->position = inv.TransformPoint(pos);
            local->rotation = Normalize(inv.rotation * rot);
        } else {
            local->position = pos;
            local->rotation = rot;
        }
        t.written = pos;
        t.writtenRot = rot;
    }
}

physics::BodyId PhysicsSystem::BodyOf(Entity e) const
{
    const u32* i = m_byEntity.Find(e.Pack());
    return i ? m_tracked[*i].body : physics::BodyId{};
}

Entity PhysicsSystem::EntityOf(physics::BodyId id) const
{
    return m_world && m_world->IsValid(id) ? Entity::Unpack(m_world->GetUserData(id)) : Entity{};
}

bool PhysicsSystem::Raycast(Vec3 origin, Vec3 direction, f32 maxDistance, PhysicsHit& hit, u32 layerMask, Entity ignore) const
{
    if (!m_world || LengthSq(direction) < 1e-12f)
        return false;
    // Rays ignore triggers and the given entity: walk the hits front to back.
    Vec3 o = origin;
    const Vec3 d = Normalize(direction);
    f32 travelled = 0.0f;
    for (u32 attempt = 0; attempt < 16; ++attempt) {
        physics::RaycastHit h;
        if (!m_world->Raycast(Ray(o, d), maxDistance - travelled, h, layerMask))
            return false;
        const Entity e = EntityOf(h.body);
        const bool sensor = h.shape < m_world->GetShapes(h.body).Size() && m_world->GetShapes(h.body)[h.shape].isSensor;
        if (!sensor && !(ignore && e == ignore)) {
            hit.entity = e;
            hit.point = h.point;
            hit.normal = h.normal;
            hit.distance = travelled + h.distance;
            return true;
        }
        travelled += h.distance + 1e-3f;
        o = origin + d * travelled;
    }
    return false;
}

bool PhysicsSystem::SphereCast(Vec3 origin, f32 radius, Vec3 direction, f32 maxDistance, PhysicsHit& hit, u32 layerMask, Entity ignore) const
{
    if (!m_world || LengthSq(direction) < 1e-12f)
        return false;
    physics::RaycastHit h;
    const physics::BodyId skip = ignore ? BodyOf(ignore) : physics::BodyId{};
    if (!m_world->ShapeCast(ShapeDesc::Sphere(Max(radius, 0.001f)), origin, Quat(), direction, maxDistance, h, layerMask, skip))
        return false;
    hit.entity = EntityOf(h.body);
    hit.point = h.point;
    hit.normal = h.normal;
    hit.distance = h.distance;
    return true;
}

void PhysicsSystem::OverlapSphere(Vec3 center, f32 radius, Array<Entity>& out, u32 layerMask) const
{
    if (!m_world)
        return;
    Array<physics::BodyId> bodies;
    m_world->Overlap(ShapeDesc::Sphere(Max(radius, 0.001f)), center, Quat(), bodies, layerMask);
    for (physics::BodyId b : bodies) {
        const Entity e = EntityOf(b);
        bool seen = false;
        for (Entity x : out)
            seen |= x == e;
        if (e && !seen)
            out.PushBack(e);
    }
}

void PhysicsSystem::AddForce(Entity e, Vec3 force)
{
    if (m_world)
        m_world->AddForce(BodyOf(e), force);
}

void PhysicsSystem::AddImpulse(Entity e, Vec3 impulse)
{
    if (m_world)
        m_world->AddImpulse(BodyOf(e), impulse);
}

void PhysicsSystem::AddImpulseAtPoint(Entity e, Vec3 impulse, Vec3 point)
{
    if (m_world)
        m_world->AddImpulseAtPoint(BodyOf(e), impulse, point);
}

void PhysicsSystem::AddTorque(Entity e, Vec3 torque)
{
    if (m_world)
        m_world->AddTorque(BodyOf(e), torque);
}

Vec3 PhysicsSystem::GetVelocity(Entity e) const { return m_world ? m_world->GetLinearVelocity(BodyOf(e)) : Vec3(); }
Vec3 PhysicsSystem::GetAngularVelocity(Entity e) const { return m_world ? m_world->GetAngularVelocity(BodyOf(e)) : Vec3(); }

void PhysicsSystem::SetVelocity(Entity e, Vec3 v)
{
    if (m_world)
        m_world->SetLinearVelocity(BodyOf(e), v);
}

void PhysicsSystem::SetAngularVelocity(Entity e, Vec3 w)
{
    if (m_world)
        m_world->SetAngularVelocity(BodyOf(e), w);
}

bool PhysicsSystem::Raycast(const Ray& ray, f32 maxDistance, physics::RaycastHit& hit, Entity& entity) const
{
    if (!m_world || !m_world->Raycast(ray, maxDistance, hit))
        return false;
    const u64 packed = m_world->GetUserData(hit.body);
    entity = Entity{u32(packed), u32(packed >> 32)};
    return true;
}

} // namespace zx
