#include "Engine/PhysicsSystem.h"
#include "Core/Containers/Function.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflection.h"
#include "Engine/Gameplay.h"
#include "Engine/Terrain.h"
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
    ZX_FIELD(rollingFriction, Meta::Range(0, 1), Meta::Tooltip("Slows rolling: 0 = rolls forever, ~0.05 = a ball on grass"))
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

ZX_REFLECT_STRUCT_BEGIN(CylinderCollider)
    ZX_FIELD(center, Meta::Units("m"))
    ZX_FIELD(radius, Meta::Range(0.01, 100), Meta::Units("m"))
    ZX_FIELD(height, Meta::Range(0.02, 200), Meta::Units("m"), Meta::Tooltip("Along the local Y axis"))
    ZX_COLLIDER_COMMON_FIELDS
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(MeshCollider)
    ZX_FIELD(enabled, Meta::Tooltip("Static collision with the triangles of the Mesh Renderer"))
    ZX_FIELD(convex, Meta::Tooltip("Collide as the convex hull of the mesh (up to 32 vertices): works on a dynamic Rigid Body"))
    ZX_FIELD(layer, Meta::Range(0, 31), Meta::Tooltip("Physics layer: raycasts and other queries can filter by it"))
ZX_REFLECT_STRUCT_END()
#undef ZX_COLLIDER_COMMON_FIELDS

ZX_REFLECT_ENUM_BEGIN(JointMotion)
    ZX_ENUM_VALUE(Locked)
    ZX_ENUM_VALUE(Limited)
    ZX_ENUM_VALUE(Free)
ZX_REFLECT_ENUM_END()

#define ZX_JOINT_CONNECTED_FIELD                                                                                           \
    ZX_FIELD(connectedBody, Meta::Tooltip("Name of the entity to connect to (with a Rigid Body or a collider); empty = the world"))
#define ZX_JOINT_COMMON_FIELDS                                                                                             \
    ZX_FIELD(breakForce, Meta::Range(0, 1000000), Meta::Units("N"), Meta::Tooltip("Breaks when pulled harder; 0 = unbreakable"))      \
    ZX_FIELD(breakTorque, Meta::Range(0, 1000000), Meta::Units("N*m"), Meta::Tooltip("Breaks when twisted harder; 0 = unbreakable")) \
    ZX_FIELD(collideConnected, Meta::Tooltip("The two connected bodies collide with each other"))

ZX_REFLECT_STRUCT_BEGIN(HingeJoint)
    ZX_JOINT_CONNECTED_FIELD
    ZX_FIELD(anchor, Meta::Units("m"), Meta::Tooltip("Pivot in the entity's local space"))
    ZX_FIELD(axis, Meta::Tooltip("Rotation axis in the entity's local space"))
    ZX_FIELD(useLimits)
    ZX_FIELD(lowerLimit, Meta::Angle(), Meta::Range(-3.14159, 0), Meta::EditCondition("useLimits"))
    ZX_FIELD(upperLimit, Meta::Angle(), Meta::Range(0, 3.14159), Meta::EditCondition("useLimits"))
    ZX_FIELD(useMotor)
    ZX_FIELD(motorSpeed, Meta::Angle(), Meta::Range(-62.8, 62.8), Meta::EditCondition("useMotor"), Meta::Tooltip("Degrees per second"))
    ZX_FIELD(motorTorque, Meta::Range(0, 1000000), Meta::Units("N*m"), Meta::EditCondition("useMotor"))
    ZX_JOINT_COMMON_FIELDS
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(FixedJoint)
    ZX_JOINT_CONNECTED_FIELD
    ZX_JOINT_COMMON_FIELDS
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(SliderJoint)
    ZX_JOINT_CONNECTED_FIELD
    ZX_FIELD(axis, Meta::Tooltip("Direction of travel in the entity's local space"))
    ZX_FIELD(useLimits)
    ZX_FIELD(lowerLimit, Meta::Range(-1000, 0), Meta::Units("m"), Meta::EditCondition("useLimits"))
    ZX_FIELD(upperLimit, Meta::Range(0, 1000), Meta::Units("m"), Meta::EditCondition("useLimits"))
    ZX_FIELD(useMotor)
    ZX_FIELD(motorSpeed, Meta::Range(-100, 100), Meta::Units("m/s"), Meta::EditCondition("useMotor"))
    ZX_FIELD(motorForce, Meta::Range(0, 1000000), Meta::Units("N"), Meta::EditCondition("useMotor"))
    ZX_JOINT_COMMON_FIELDS
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(BallJoint)
    ZX_JOINT_CONNECTED_FIELD
    ZX_FIELD(anchor, Meta::Units("m"), Meta::Tooltip("Pivot in the entity's local space"))
    ZX_FIELD(axis, Meta::Tooltip("Center of the swing cone and twist axis (local space)"))
    ZX_FIELD(swingLimit, Meta::Angle(), Meta::Range(0, 3.14159), Meta::Tooltip("Cone half angle; 0 = free"))
    ZX_FIELD(twistLimit, Meta::Angle(), Meta::Range(0, 3.14159), Meta::Tooltip("Twist around the axis either way; 0 = free"))
    ZX_JOINT_COMMON_FIELDS
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(DistanceJoint)
    ZX_JOINT_CONNECTED_FIELD
    ZX_FIELD(anchor, Meta::Units("m"), Meta::Tooltip("Attachment point in the entity's local space"))
    ZX_FIELD(autoConnectedAnchor, Meta::Tooltip("Attach to the connected entity's origin (without one: the anchor's position at Play)"))
    ZX_FIELD(connectedAnchor, Meta::Units("m"), Meta::Tooltip("In the connected entity's space (world space without one)"))
    ZX_FIELD(restLength, Meta::Range(0, 1000), Meta::Units("m"), Meta::Tooltip("0 = the distance at Play"))
    ZX_FIELD(minDistance, Meta::Range(0, 1000), Meta::Units("m"), Meta::Tooltip("Never closer (0 = no minimum)"))
    ZX_FIELD(maxDistance, Meta::Range(0, 1000), Meta::Units("m"), Meta::Tooltip("Never farther: a rope (0 = no maximum)"))
    ZX_FIELD(springFrequency, Meta::Range(0, 30), Meta::Units("Hz"), Meta::Tooltip("Spring towards the rest length; 0 = no spring (rigid rod when min and max are 0 too)"))
    ZX_FIELD(springDamping, Meta::Range(0, 2), Meta::Tooltip("Damping ratio: 0 = bounces forever, 1 = no overshoot"))
    ZX_JOINT_COMMON_FIELDS
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(ConfigurableJoint)
    ZX_JOINT_CONNECTED_FIELD
    ZX_FIELD(anchor, Meta::Units("m"))
    ZX_FIELD(axis, Meta::Tooltip("X axis of the joint frame (local space)"))
    ZX_FIELD(xMotion)
    ZX_FIELD(yMotion)
    ZX_FIELD(zMotion)
    ZX_FIELD(angularXMotion)
    ZX_FIELD(angularYMotion)
    ZX_FIELD(angularZMotion)
    ZX_FIELD(linearLimit, Meta::Range(0, 1000), Meta::Units("m"), Meta::Tooltip("Travel either way on Limited axes"))
    ZX_FIELD(angularLimit, Meta::Angle(), Meta::Range(0, 3.14159), Meta::Tooltip("Rotation either way on Limited angular axes"))
    ZX_JOINT_COMMON_FIELDS
ZX_REFLECT_STRUCT_END()
#undef ZX_JOINT_CONNECTED_FIELD
#undef ZX_JOINT_COMMON_FIELDS

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

/// Hull of a mesh at a scale (convex Mesh Colliders); InvalidHull when the mesh isn't loaded or is flat.
using HullMaker = Function<HullId(const render::GpuMesh*, Vec3)>; // null mesh: unit cylinder

/// The mesh of a convex Mesh Collider when it is loaded, else null.
const render::GpuMesh* ConvexMesh(const ecs::World& world, Entity e, render::RenderAssets* assets)
{
    const MeshCollider* mc = world.Get<const MeshCollider>(e);
    const MeshRenderer* mr = world.Get<const MeshRenderer>(e);
    if (!mc || !mc->enabled || !mc->convex || !mr || !assets)
        return nullptr;
    const render::GpuMesh* mesh = assets->GetMesh(mr->mesh);
    return mesh && !mesh->cpuPositions.Empty() ? mesh : nullptr;
}

/// The colliders of entity `e` as shapes relative to `body` (a world pose without scale). Convex Mesh Colliders
/// become hulls through `makeHull` (skipped without it).
void AppendShapes(const ecs::World& world, Entity e, const Pose& body, Array<ShapeDesc>& out, render::RenderAssets* assets = nullptr,
                  HullMaker* makeHull = nullptr)
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
    if (const CylinderCollider* c = world.Get<const CylinderCollider>(e); c && makeHull) {
        const Vec3 size = Max(Vec3(c->radius * 2.0f * s.x, c->height * s.y, c->radius * 2.0f * s.z), Vec3(0.01f));
        const HullId hull = (*makeHull)(nullptr, size);
        if (hull != InvalidHull)
            place(sensor(ShapeDesc::Hull(hull), c->isTrigger), c->center);
    }
    if (makeHull)
        if (const render::GpuMesh* mesh = ConvexMesh(world, e, assets)) {
            const HullId hull = (*makeHull)(mesh, p.scale);
            if (hull != InvalidHull)
                place(ShapeDesc::Hull(hull), Vec3());
        }
}

/// Physics layer of an entity: from its first collider component.
u32 LayerOf(const ecs::World& world, Entity e)
{
    i32 layer = 0;
    const BoxCollider* box = world.Get<const BoxCollider>(e);
    const SphereCollider* sphere = world.Get<const SphereCollider>(e);
    const CapsuleCollider* capsule = world.Get<const CapsuleCollider>(e);
    const CylinderCollider* cylinder = world.Get<const CylinderCollider>(e);
    const MeshCollider* mesh = world.Get<const MeshCollider>(e);
    if (box)
        layer = box->layer;
    else if (sphere)
        layer = sphere->layer;
    else if (capsule)
        layer = capsule->layer;
    else if (cylinder)
        layer = cylinder->layer;
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
    return mr && mr->collision && mr->visible && !HasCollider(world, e) && !world.Get<const RigidBody>(e) && !InCharacter(world, e) &&
           !world.Get<const Terrain>(e); // terrains collide as height fields
}

/// The entity a joint connects to (by name; prefers one with a body), or none.
/// The entity a joint connects to: by name, preferring one with a body, then the nearest to `near` (several
/// instances of a prefab share names; each connects within itself).
Entity FindConnected(ecs::World& world, StringView name, const HashMap<u64, u32>* bodies, Vec3 near)
{
    if (name.Empty())
        return {};
    Entity found;
    f32 best = Infinity;
    bool foundHasBody = false;
    world.Query<const scene::EntityName>().Each([&](Entity e, const scene::EntityName& n) {
        if (n.value.View() != name)
            return;
        const bool hasBody = bodies && bodies->Contains(e.Pack());
        const f32 d = DistanceSq(WorldPose(world, e).position, near);
        if (!found || (hasBody && !foundHasBody) || (hasBody == foundHasBody && d < best)) {
            found = e;
            best = d;
            foundHasBody = hasBody;
        }
    });
    return found;
}

/// Joint description of the entity's joint component in world space (bodies not set); false without one.
/// `kind`: 0..5 = only that joint component (Hinge, Fixed, Slider, Ball, Distance, Configurable), ~0u = the first one.
bool DescribeJoint(const ecs::World& world, Entity e, JointDesc& d, StringView& connectedName, Vec3& connectedAnchor, bool& autoAnchor,
                   u32 kind = ~0u)
{
    const Pose p = WorldPose(world, e);
    auto point = [&](Vec3 local) { return p.position + p.rotation * (local * p.scale); };
    auto direction = [&](Vec3 local) { return SafeNormalize(p.rotation * local, Vec3::Up()); };
    autoAnchor = false;
    connectedAnchor = Vec3();
    if (const HingeJoint* j = kind == ~0u || kind == 0 ? world.Get<const HingeJoint>(e) : nullptr) {
        d.type = JointType::Hinge;
        d.anchor = point(j->anchor);
        d.axis = direction(j->axis);
        d.useLimits = j->useLimits;
        d.lower = Min(j->lowerLimit, j->upperLimit);
        d.upper = Max(j->lowerLimit, j->upperLimit);
        d.useMotor = j->useMotor;
        d.motorSpeed = j->motorSpeed;
        d.maxMotorForce = j->motorTorque;
        d.breakForce = j->breakForce;
        d.breakTorque = j->breakTorque;
        d.collideConnected = j->collideConnected;
        connectedName = j->connectedBody.View();
        return true;
    }
    if (const FixedJoint* j = kind == ~0u || kind == 1 ? world.Get<const FixedJoint>(e) : nullptr) {
        d.type = JointType::Fixed;
        d.anchor = p.position;
        d.breakForce = j->breakForce;
        d.breakTorque = j->breakTorque;
        d.collideConnected = j->collideConnected;
        connectedName = j->connectedBody.View();
        return true;
    }
    if (const SliderJoint* j = kind == ~0u || kind == 2 ? world.Get<const SliderJoint>(e) : nullptr) {
        d.type = JointType::Slider;
        d.anchor = p.position;
        d.axis = direction(j->axis);
        d.useLimits = j->useLimits;
        d.lower = Min(j->lowerLimit, j->upperLimit);
        d.upper = Max(j->lowerLimit, j->upperLimit);
        d.useMotor = j->useMotor;
        d.motorSpeed = j->motorSpeed;
        d.maxMotorForce = j->motorForce;
        d.breakForce = j->breakForce;
        d.breakTorque = j->breakTorque;
        d.collideConnected = j->collideConnected;
        connectedName = j->connectedBody.View();
        return true;
    }
    if (const BallJoint* j = kind == ~0u || kind == 3 ? world.Get<const BallJoint>(e) : nullptr) {
        d.type = JointType::Ball;
        d.anchor = point(j->anchor);
        d.axis = direction(j->axis);
        d.swingLimit = j->swingLimit;
        d.twistLimit = j->twistLimit;
        d.breakForce = j->breakForce;
        d.breakTorque = j->breakTorque;
        d.collideConnected = j->collideConnected;
        connectedName = j->connectedBody.View();
        return true;
    }
    if (const DistanceJoint* j = kind == ~0u || kind == 4 ? world.Get<const DistanceJoint>(e) : nullptr) {
        d.type = JointType::Distance;
        d.anchor = point(j->anchor);
        d.restLength = j->restLength;
        d.minDistance = j->minDistance;
        d.maxDistance = j->maxDistance;
        d.springHertz = j->springFrequency;
        d.springDamping = j->springDamping;
        d.breakForce = j->breakForce;
        d.breakTorque = j->breakTorque;
        d.collideConnected = j->collideConnected;
        connectedName = j->connectedBody.View();
        connectedAnchor = j->connectedAnchor;
        autoAnchor = j->autoConnectedAnchor;
        return true;
    }
    if (const ConfigurableJoint* j = kind == ~0u || kind == 5 ? world.Get<const ConfigurableJoint>(e) : nullptr) {
        d.type = JointType::Configurable;
        d.anchor = point(j->anchor);
        d.axis = direction(j->axis);
        auto motion = [](JointMotion m) {
            return m == JointMotion::Locked ? AxisMotion::Locked : m == JointMotion::Limited ? AxisMotion::Limited : AxisMotion::Free;
        };
        d.linear[0] = motion(j->xMotion);
        d.linear[1] = motion(j->yMotion);
        d.linear[2] = motion(j->zMotion);
        d.angular[0] = motion(j->angularXMotion);
        d.angular[1] = motion(j->angularYMotion);
        d.angular[2] = motion(j->angularZMotion);
        d.linearLimit = j->linearLimit;
        d.angularLimit = j->angularLimit;
        d.breakForce = j->breakForce;
        d.breakTorque = j->breakTorque;
        d.collideConnected = j->collideConnected;
        connectedName = j->connectedBody.View();
        return true;
    }
    return false;
}

/// World position of the far end of a distance joint (or of the connected entity for others).
Vec3 ConnectedPoint(const ecs::World& world, Entity connected, const JointDesc& d, Vec3 connectedAnchor, bool autoAnchor)
{
    if (connected) {
        const Pose c = WorldPose(world, connected);
        return autoAnchor || d.type != JointType::Distance ? c.position : c.position + c.rotation * (connectedAnchor * c.scale);
    }
    return autoAnchor ? d.anchor : connectedAnchor;
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
           world.Get<const CylinderCollider>(e) ||
           world.Get<const MeshCollider>(e);
}

bool HasJoint(const ecs::World& world, Entity e)
{
    return world.Get<const HingeJoint>(e) || world.Get<const FixedJoint>(e) || world.Get<const SliderJoint>(e) || world.Get<const BallJoint>(e) ||
           world.Get<const DistanceJoint>(e) || world.Get<const ConfigurableJoint>(e);
}

void GatherJointGizmos(ecs::World& world, Entity e, Array<JointGizmo>& out)
{
    for (u32 kind = 0; kind < 6; ++kind) {
        JointDesc d;
        StringView name;
        Vec3 connectedAnchor;
        bool autoAnchor = false;
        if (!DescribeJoint(world, e, d, name, connectedAnchor, autoAnchor, kind))
            continue;
        JointGizmo g;
        g.anchor = d.anchor;
        g.axis = d.type == JointType::Fixed || d.type == JointType::Distance ? Vec3() : d.axis;
        g.linear = d.type == JointType::Slider;
        const Entity connected = FindConnected(world, name, nullptr, d.anchor);
        g.hasConnected = connected || d.type == JointType::Distance;
        g.connected = ConnectedPoint(world, connected, d, connectedAnchor, autoAnchor);
        out.PushBack(g);
    }
}

void GatherColliderOutlines(ecs::World& world, Entity e, Array<ColliderOutline>& out, render::RenderAssets* assets)
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
    if (const CylinderCollider* c = world.Get<const CylinderCollider>(e)) {
        const Vec3 s = Abs(p.scale);
        const f32 rx = c->radius * s.x, rz = c->radius * s.z, hy = c->height * s.y * 0.5f;
        const Vec3 center = p.position + p.rotation * (p.scale * c->center);
        ColliderOutline o;
        o.type = ShapeType::ConvexHull;
        o.transform = Mat4::TRS(center, p.rotation, Vec3(1.0f));
        for (u32 i = 0; i < 24; ++i) {
            const f32 a0 = f32(i) * (TwoPi / 24.0f), a1 = f32(i + 1) * (TwoPi / 24.0f);
            for (f32 y : {-hy, hy}) {
                o.lines.PushBack(center + p.rotation * Vec3(Cos(a0) * rx, y, Sin(a0) * rz));
                o.lines.PushBack(center + p.rotation * Vec3(Cos(a1) * rx, y, Sin(a1) * rz));
            }
            if (i % 6 == 0) {
                o.lines.PushBack(center + p.rotation * Vec3(Cos(a0) * rx, -hy, Sin(a0) * rz));
                o.lines.PushBack(center + p.rotation * Vec3(Cos(a0) * rx, hy, Sin(a0) * rz));
            }
        }
        out.PushBack(move(o));
    }
    // Convex Mesh Collider: the hull's edges (rebuilt only when the mesh or the scale changes).
    if (const render::GpuMesh* mesh = ConvexMesh(world, e, assets)) {
        struct Cached
        {
            const render::GpuMesh* mesh = nullptr;
            usize count = 0;
            Vec3 scale;
            Array<Vec3> vertices;
            Array<u32> edges;
        };
        static Array<Cached> cache;
        Cached* c = nullptr;
        for (Cached& k : cache)
            if (k.mesh == mesh && k.count == mesh->cpuPositions.Size() && k.scale == p.scale)
                c = &k;
        if (!c) {
            if (cache.Size() >= 16)
                cache.Clear();
            Cached k;
            k.mesh = mesh;
            k.count = mesh->cpuPositions.Size();
            k.scale = p.scale;
            Array<Vec3> points;
            points.Reserve(mesh->cpuPositions.Size());
            for (Vec3 v : mesh->cpuPositions)
                points.PushBack(v * p.scale);
            PhysicsWorld::BuildHullEdges(points.View(), k.vertices, k.edges);
            cache.PushBack(move(k));
            c = &cache.Back();
        }
        ColliderOutline o;
        o.type = ShapeType::ConvexHull;
        o.transform = Mat4::TRS(p.position, p.rotation, Vec3(1.0f));
        for (u32 i : c->edges)
            o.lines.PushBack(p.position + p.rotation * c->vertices[i]);
        out.PushBack(move(o));
    }
}

HullId PhysicsSystem::HullOf(const render::GpuMesh* mesh, Vec3 scale)
{
    for (const CachedHull& c : m_hulls)
        if (c.mesh == mesh && c.scale == scale)
            return c.hull;
    Array<Vec3> points;
    if (mesh) {
        points.Reserve(mesh->cpuPositions.Size());
        for (Vec3 v : mesh->cpuPositions)
            points.PushBack(v * scale);
    } else {
        // Cylinder: two rings of 16 (exactly the hull's 32-vertex budget).
        for (u32 i = 0; i < 16; ++i) {
            const f32 a = f32(i) * (TwoPi / 16.0f);
            for (f32 y : {-0.5f, 0.5f})
                points.PushBack(Vec3(Cos(a) * 0.5f, y, Sin(a) * 0.5f) * scale);
        }
    }
    CachedHull c;
    c.mesh = mesh;
    c.scale = scale;
    c.hull = m_world->CreateConvexHull(points.View());
    m_hulls.PushBack(c);
    return c.hull;
}

PhysicsSystem::PhysicsSystem() = default;
PhysicsSystem::~PhysicsSystem() = default;

void PhysicsSystem::Begin(ecs::World& world, render::RenderAssets* renderAssets)
{
    End();
    m_world.Reset(new PhysicsWorld());
    m_world->Settings().stayEvents = true; // collisionStay / triggerStay callbacks
    m_assets = renderAssets;
    m_accumulator = 0.0f;
    CreateBodies(world);
    CreateJoints(world);
    u32 moving = 0;
    for (const Tracked& t : m_tracked)
        moving += t.moves ? 1u : 0u;
    ZX_LOG_INFO(LogPhysics, "Physics: {} bodies ({} moving, {} static), {} joints, {} mesh colliders waiting for their mesh",
                m_world->BodyCount(), moving, m_world->BodyCount() - moving, m_world->JointCount(), m_pendingMeshes.Size());
}

void PhysicsSystem::End()
{
    m_world.Reset();
    m_tracked.Clear();
    m_byEntity.Clear();
    m_pendingMeshes.Clear();
    m_pendingBodies.Clear();
    m_joints.Clear();
    m_hulls.Clear();
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
    HullMaker makeHull = [this](const render::GpuMesh* mesh, Vec3 scale) { return HullOf(mesh, scale); };
    const RigidBody* rb = world.Get<const RigidBody>(e);
    if (dynamic && rb) {
        d.motion = rb->type == RigidBodyType::Kinematic ? MotionType::Kinematic : MotionType::Dynamic;
        d.mass = rb->mass;
        d.friction = rb->friction;
        d.restitution = rb->bounciness;
        d.rollingFriction = rb->rollingFriction;
        d.linearDamping = rb->linearDamping;
        d.angularDamping = rb->angularDamping;
        d.gravityScale = rb->gravityScale;
        d.allowSleep = rb->canSleep;
        // Compound: colliders of the entity and of children without their own RigidBody.
        Array<Entity> stack{e};
        while (!stack.Empty()) {
            const Entity c = stack.PopBackValue();
            const MeshCollider* mc = world.Get<const MeshCollider>(c);
            const MeshRenderer* mr = world.Get<const MeshRenderer>(c);
            if (mc && mc->enabled && mc->convex && mr && m_assets && !m_assets->GetMesh(mr->mesh)) {
                // The mesh is still loading: the body is created once it arrives.
                for (Entity p : m_pendingBodies)
                    if (p == e)
                        return false;
                m_pendingBodies.PushBack(e);
                return false;
            }
            AppendShapes(world, c, bodyPose, d.shapes, m_assets, &makeHull);
            for (Entity child : scene::GetChildren(world, c))
                if (!world.Get<const RigidBody>(child))
                    stack.PushBack(child);
        }
        if (d.shapes.Empty()) {
            ZX_LOG_WARN(LogPhysics, "RigidBody on '{}' has no Box/Sphere/Capsule or convex Mesh Collider (with its mesh loaded): not simulated",
                        world.Get<const scene::EntityName>(e) ? world.Get<const scene::EntityName>(e)->value.View() : StringView("entity"));
            return false;
        }
    } else {
        AppendShapes(world, e, bodyPose, d.shapes, m_assets, &makeHull);
        if (const Terrain* terrain = world.Get<const Terrain>(e); terrain && terrain->collision) {
            Array<f32> heights;
            GenerateTerrainHeights(*terrain, heights);
            const Vec3 s = Abs(pose.scale);
            for (f32& h : heights)
                h *= s.y;
            const u32 n = TerrainSamples(*terrain);
            const HeightFieldId field = m_world->CreateHeightField(n, n, heights.View(), TerrainSpacing(*terrain) * s.x, TerrainSpacing(*terrain) * s.z);
            if (field != InvalidHeightField)
                d.shapes.PushBack(ShapeDesc::HeightField(field));
        }
        const MeshCollider* mc = world.Get<const MeshCollider>(e);
        const MeshRenderer* mr = world.Get<const MeshRenderer>(e);
        if (((mc && mc->enabled && !mc->convex) || ImplicitMeshCollision(world, e)) && mr) {
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
    world.Query<const CylinderCollider>().Each([&](Entity e, const CylinderCollider&) { addStatic(e); });
    world.Query<const MeshCollider>().Each([&](Entity e, const MeshCollider&) { addStatic(e); });
    world.Query<const Terrain>().Each([&](Entity e, const Terrain&) { addStatic(e); });
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

void PhysicsSystem::CreateJoints(ecs::World& world)
{
    Array<Entity> owners;
    auto add = [&](Entity e) {
        if (!m_joints.Contains(e.Pack())) {
            m_joints.InsertOrAssign(e.Pack(), JointId{});
            owners.PushBack(e);
        }
    };
    world.Query<const HingeJoint>().Each([&](Entity e, const HingeJoint&) { add(e); });
    world.Query<const FixedJoint>().Each([&](Entity e, const FixedJoint&) { add(e); });
    world.Query<const SliderJoint>().Each([&](Entity e, const SliderJoint&) { add(e); });
    world.Query<const BallJoint>().Each([&](Entity e, const BallJoint&) { add(e); });
    world.Query<const DistanceJoint>().Each([&](Entity e, const DistanceJoint&) { add(e); });
    world.Query<const ConfigurableJoint>().Each([&](Entity e, const ConfigurableJoint&) { add(e); });
    auto nameOf = [&](Entity e) {
        const scene::EntityName* n = world.Get<const scene::EntityName>(e);
        return n ? n->value.View() : StringView("entity");
    };
    for (Entity e : owners) {
        // Every joint component of the entity (e.g. a bridge plank hinged to its neighbour and pinned to a post);
        // scripts drive the first one.
        for (u32 kind = 0; kind < 6; ++kind) {
            JointDesc d;
            StringView connectedName;
            Vec3 connectedAnchor;
            bool autoAnchor = false;
            if (!DescribeJoint(world, e, d, connectedName, connectedAnchor, autoAnchor, kind))
                continue;
            d.a = BodyOf(e);
            if (!d.a.IsValid() || m_world->GetMotionType(d.a) != MotionType::Dynamic) {
                ZX_LOG_WARN(LogPhysics, "Joint on '{}' needs a dynamic Rigid Body with a collider: ignored", nameOf(e));
                break;
            }
            const Entity connected = FindConnected(world, connectedName, &m_byEntity, d.anchor);
            if (!connectedName.Empty() && !connected)
                ZX_LOG_WARN(LogPhysics, "Joint on '{}': no entity named '{}', connected to the world", nameOf(e), connectedName);
            if (connected) {
                d.b = BodyOf(connected);
                if (!d.b.IsValid()) {
                    ZX_LOG_WARN(LogPhysics, "Joint on '{}': '{}' has no Rigid Body or collider, connected to the world", nameOf(e), connectedName);
                } else if (d.b == d.a) {
                    ZX_LOG_WARN(LogPhysics, "Joint on '{}' is connected to itself: ignored", nameOf(e));
                    continue;
                }
            }
            d.anchorB = ConnectedPoint(world, connected, d, connectedAnchor, autoAnchor);
            const JointId id = m_world->CreateJoint(d);
            if (!m_joints.Find(e.Pack())->IsValid())
                m_joints.InsertOrAssign(e.Pack(), id);
        }
    }
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

    // Rigid bodies whose convex meshes arrived since.
    if (!m_pendingBodies.Empty()) {
        Array<Entity> waiting = move(m_pendingBodies);
        m_pendingBodies.Clear();
        for (Entity e : waiting)
            if (world.IsAlive(e))
                CreateBody(world, e, true); // re-queues itself while a mesh is still loading
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
        if (beforeStep)
            beforeStep(FixedStep);
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

    // Broken joints.
    Array<JointId> broken;
    m_world->TakeBrokenJoints(broken);
    for (JointId id : broken)
        for (auto& [entity, joint] : m_joints)
            if (joint == id) {
                const scene::EntityName* n = world.Get<const scene::EntityName>(Entity::Unpack(entity));
                ZX_LOG_INFO(LogPhysics, "Joint on '{}' broke", n ? n->value.View() : StringView("entity"));
                joint = JointId{};
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
    // The ignored entity's body is skipped by the query itself (a ray starting inside it, e.g. a character's own
    // capsule, would otherwise hit it at distance 0 again and again).
    Vec3 o = origin;
    const Vec3 d = Normalize(direction);
    f32 travelled = 0.0f;
    const physics::BodyId skip = ignore ? BodyOf(ignore) : physics::BodyId{};
    for (u32 attempt = 0; attempt < 16; ++attempt) {
        physics::RaycastHit h;
        if (!m_world->Raycast(Ray(o, d), maxDistance - travelled, h, layerMask, skip))
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

bool PhysicsSystem::ClosestPoint(Vec3 point, f32 maxDistance, PhysicsHit& hit, u32 layerMask, Entity ignore) const
{
    if (!m_world)
        return false;
    physics::RaycastHit h;
    const physics::BodyId skip = ignore ? BodyOf(ignore) : physics::BodyId{};
    if (!m_world->ClosestPoint(point, maxDistance, h, layerMask, skip))
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

physics::JointId PhysicsSystem::JointOf(Entity e) const
{
    const JointId* id = m_joints.Find(e.Pack());
    return id && m_world && m_world->IsValid(*id) ? *id : JointId{};
}

f32 PhysicsSystem::GetJointValue(Entity e) const { return m_world ? m_world->GetJointCoordinate(JointOf(e)) : 0.0f; }

void PhysicsSystem::BreakJoints(Entity e)
{
    const BodyId body = BodyOf(e);
    if (!m_world || !body.IsValid())
        return;
    Array<JointId> joints;
    m_world->GetJoints(joints);
    for (JointId id : joints) {
        const JointDesc& d = m_world->GetJointDesc(id);
        if (d.a == body || d.b == body)
            m_world->DestroyJoint(id);
    }
}

void PhysicsSystem::SetJointMotor(Entity e, bool enabled, f32 speed, f32 force)
{
    const JointId id = JointOf(e);
    if (!id.IsValid())
        return;
    m_world->SetJointMotor(id, enabled, speed, force > 0.0f ? force : m_world->GetJointDesc(id).maxMotorForce);
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
