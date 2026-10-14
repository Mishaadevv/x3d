#pragma once

// Physics in the scene (Unity-style): Box / Sphere / Capsule / Mesh Collider components make an entity solid; a
// RigidBody makes it move (gravity, collisions). Colliders on children of a RigidBody belong to its body (compound);
// colliders without a RigidBody above them are static. Mesh Renderers with Collision on and no collider components
// collide as static triangle meshes (level geometry works without setup). Scale is baked into the shapes.
//
//   PhysicsSystem physics;
//   physics.Begin(world, &renderAssets);          // Play: bodies from the components
//   physics.Tick(world, dt);                      // fixed 60 Hz steps, poses written back (interpolated)
//   physics.End();                                // Stop

#include "Engine/SceneRendering.h"
#include "Physics/PhysicsWorld.h"

namespace zx {

enum class RigidBodyType : u8
{
    Dynamic,   // simulated: gravity, collisions, pushes
    Kinematic, // moved by the game (scripts, animation); pushes dynamic bodies, nothing pushes it
};

struct RigidBody
{
    RigidBodyType type = RigidBodyType::Dynamic;
    f32 mass = 0.0f;          // kg; 0 = from the collider volumes (density of water, 1000 kg/m^3)
    f32 friction = 0.6f;
    f32 bounciness = 0.0f;    // 0..1
    f32 rollingFriction = 0.0f; // slows rolling (balls on grass ~0.05)
    f32 linearDamping = 0.05f;
    f32 angularDamping = 0.05f;
    f32 gravityScale = 1.0f;
    bool canSleep = true;
};

// Collider fields shared by every collider: isTrigger (reports overlaps to scripts, doesn't collide) and the
// physics layer 0..31 (queries filter by it; the body takes the layer of its own entity's collider).
struct BoxCollider
{
    Vec3 center;
    Vec3 size = Vec3(1.0f); // full size (scaled by the transform)
    bool isTrigger = false;
    i32 layer = 0;
};

struct SphereCollider
{
    Vec3 center;
    f32 radius = 0.5f;
    bool isTrigger = false;
    i32 layer = 0;
};

/// Capsule along the entity's local Y axis.
struct CapsuleCollider
{
    Vec3 center;
    f32 radius = 0.5f;
    f32 height = 2.0f; // end to end
    bool isTrigger = false;
    i32 layer = 0;
};

/// Upright cylinder along the local Y axis (a 32-vertex convex hull; non-uniform scale makes it elliptic).
struct CylinderCollider
{
    Vec3 center;
    f32 radius = 0.5f;
    f32 height = 1.0f;
    bool isTrigger = false;
    i32 layer = 0;
};

/// The triangles of the entity's Mesh Renderer (static collision: level geometry). Convex: the convex hull of the mesh
/// instead (at most 32 vertices) - collides as a dynamic Rigid Body (rocks, barrels, crates of any shape).
struct MeshCollider
{
    bool enabled = true;
    bool convex = false;
    i32 layer = 0;
};

// Joints (on an entity with a Rigid Body): connect it to the body of `connectedBody` (the name of another entity
// with a Rigid Body or a collider; empty = the world). Anchors and axes are in the entity's local space; angles in
// radians (shown in degrees). breakForce / breakTorque > 0: the joint breaks when pulled harder (newtons / N*m).

/// Rotation around one axis: doors, wheels, levers. Limits relative to the pose at Play; optional motor.
struct HingeJoint
{
    String connectedBody;
    Vec3 anchor;
    Vec3 axis = Vec3(0.0f, 1.0f, 0.0f);
    bool useLimits = false;
    f32 lowerLimit = -0.7853982f;
    f32 upperLimit = 0.7853982f;
    bool useMotor = false;
    f32 motorSpeed = 1.5707964f; // rad/s
    f32 motorTorque = 1000.0f;   // N*m
    f32 breakForce = 0.0f;
    f32 breakTorque = 0.0f;
    bool collideConnected = false;
};

/// Glues the entity to the connected body (or pins it in place).
struct FixedJoint
{
    String connectedBody;
    f32 breakForce = 0.0f;
    f32 breakTorque = 0.0f;
    bool collideConnected = false;
};

/// Translation along one axis: pistons, drawers, elevators. Limits in meters from the pose at Play.
struct SliderJoint
{
    String connectedBody;
    Vec3 axis = Vec3(1.0f, 0.0f, 0.0f);
    bool useLimits = false;
    f32 lowerLimit = -1.0f;
    f32 upperLimit = 1.0f;
    bool useMotor = false;
    f32 motorSpeed = 1.0f;     // m/s
    f32 motorForce = 1000.0f;  // N
    f32 breakForce = 0.0f;
    f32 breakTorque = 0.0f;
    bool collideConnected = false;
};

/// Rotation around a point: chains, ragdoll shoulders and hips. Swing cone around `axis` and twist limits.
struct BallJoint
{
    String connectedBody;
    Vec3 anchor;
    Vec3 axis = Vec3(0.0f, -1.0f, 0.0f);
    f32 swingLimit = 0.0f; // cone half angle, 0 = free
    f32 twistLimit = 0.0f; // +- around the axis, 0 = free
    f32 breakForce = 0.0f;
    f32 breakTorque = 0.0f;
    bool collideConnected = false;
};

/// Keeps two anchors apart: a rigid rod by default, a rope with Max Distance, a spring with Spring Frequency.
struct DistanceJoint
{
    String connectedBody;
    Vec3 anchor;
    Vec3 connectedAnchor;       // in the connected entity's space (world space without one)
    bool autoConnectedAnchor = true; // the connected entity's origin (without one: the anchor's position at Play)
    f32 restLength = 0.0f;      // 0 = the distance at Play
    f32 minDistance = 0.0f;
    f32 maxDistance = 0.0f;     // rope length
    f32 springFrequency = 0.0f; // Hz, 0 = no spring
    f32 springDamping = 0.2f;
    f32 breakForce = 0.0f;
    f32 breakTorque = 0.0f;
    bool collideConnected = false;
};

enum class JointMotion : u8
{
    Locked,
    Limited,
    Free,
};

/// Each axis of the joint frame (X = axis, Y/Z perpendicular) locked, limited or free, linear and angular.
struct ConfigurableJoint
{
    String connectedBody;
    Vec3 anchor;
    Vec3 axis = Vec3(1.0f, 0.0f, 0.0f);
    JointMotion xMotion = JointMotion::Locked;
    JointMotion yMotion = JointMotion::Locked;
    JointMotion zMotion = JointMotion::Locked;
    JointMotion angularXMotion = JointMotion::Locked;
    JointMotion angularYMotion = JointMotion::Locked;
    JointMotion angularZMotion = JointMotion::Locked;
    f32 linearLimit = 0.5f;        // +- meters on Limited axes
    f32 angularLimit = 0.7853982f; // +- radians on Limited axes
    f32 breakForce = 0.0f;
    f32 breakTorque = 0.0f;
    bool collideConnected = false;
};

ZX_DECLARE_ENUM(RigidBodyType);
ZX_DECLARE_ENUM(JointMotion);
ZX_DECLARE_TYPE(HingeJoint);
ZX_DECLARE_TYPE(FixedJoint);
ZX_DECLARE_TYPE(SliderJoint);
ZX_DECLARE_TYPE(BallJoint);
ZX_DECLARE_TYPE(DistanceJoint);
ZX_DECLARE_TYPE(ConfigurableJoint);
ZX_DECLARE_TYPE(RigidBody);
ZX_DECLARE_TYPE(BoxCollider);
ZX_DECLARE_TYPE(SphereCollider);
ZX_DECLARE_TYPE(CapsuleCollider);
ZX_DECLARE_TYPE(CylinderCollider);
ZX_DECLARE_TYPE(MeshCollider);

/// True if the entity has any collider component.
bool HasCollider(const ecs::World& world, ecs::Entity e);

/// Collider shapes of an entity in world space (for drawing); scale applied.
struct ColliderOutline
{
    physics::ShapeType type = physics::ShapeType::Box;
    Mat4 transform = Mat4::Identity(); // rotation + position (no scale)
    Vec3 halfExtents;
    f32 radius = 0.0f;
    f32 halfHeight = 0.0f;
    Array<Vec3> lines; // ConvexHull: world-space segments (2 points each)
};
/// `assets`: needed for convex Mesh Colliders (their hull is built from the loaded mesh).
void GatherColliderOutlines(ecs::World& world, ecs::Entity e, Array<ColliderOutline>& out, render::RenderAssets* assets = nullptr);

/// True if the entity has any joint component.
bool HasJoint(const ecs::World& world, ecs::Entity e);

/// The joints of an entity in world space (for drawing): anchor, axis and the connected entity's position.
struct JointGizmo
{
    Vec3 anchor;
    Vec3 axis;               // zero for joints without one (fixed, distance)
    Vec3 connected;          // the connected entity's position (or the connected anchor)
    bool hasConnected = false;
    bool linear = false;     // slider: the axis is a direction of travel
};
void GatherJointGizmos(ecs::World& world, ecs::Entity e, Array<JointGizmo>& out);

/// A contact event between two entities (per frame, see PhysicsSystem::Events).
struct PhysicsEvent
{
    physics::ContactEventType type = physics::ContactEventType::TouchBegin;
    ecs::Entity a, b;
    Vec3 point;  // begin events
    Vec3 normal; // from a to b
    f32 speed = 0.0f; // impact speed (begin events)
};

/// Hit of a scene query.
struct PhysicsHit
{
    ecs::Entity entity;
    Vec3 point;
    Vec3 normal;
    f32 distance = 0.0f;
};

class PhysicsSystem
{
public:
    static constexpr f32 FixedStep = 1.0f / 60.0f;

    PhysicsSystem();
    ~PhysicsSystem();
    ZX_NON_COPYABLE(PhysicsSystem)

    /// Creates the physics world from the scene (WorldTransforms must be up to date). renderAssets supplies the
    /// triangles of Mesh Colliders (meshes still loading are added when they arrive).
    void Begin(ecs::World& world, render::RenderAssets* renderAssets);
    void End();
    bool IsRunning() const { return m_world != nullptr; }

    /// Fixed steps for `dt`, then the poses of moving bodies are written to their entities (interpolated between the
    /// last two steps). Entities moved by the game are teleported (and stop); kinematic bodies follow their entity.
    void Tick(ecs::World& world, f32 dt);

    physics::PhysicsWorld* World() { return m_world.Get(); }
    /// Body of an entity (invalid if it has none).
    physics::BodyId BodyOf(ecs::Entity e) const;
    /// Closest collider hit; the entity is returned in `entity`.
    bool Raycast(const Ray& ray, f32 maxDistance, physics::RaycastHit& hit, ecs::Entity& entity) const;
    // Scene queries for gameplay code (layerMask: bit per layer; triggers are not hit by casts).
    bool Raycast(Vec3 origin, Vec3 direction, f32 maxDistance, PhysicsHit& hit, u32 layerMask = ~0u, ecs::Entity ignore = {}) const;
    bool SphereCast(Vec3 origin, f32 radius, Vec3 direction, f32 maxDistance, PhysicsHit& hit, u32 layerMask = ~0u,
                    ecs::Entity ignore = {}) const;
    void OverlapSphere(Vec3 center, f32 radius, Array<ecs::Entity>& out, u32 layerMask = ~0u) const;
    /// Nearest point on a solid collider within maxDistance (distance 0 inside one).
    bool ClosestPoint(Vec3 point, f32 maxDistance, PhysicsHit& hit, u32 layerMask = ~0u, ecs::Entity ignore = {}) const;
    /// Contact / trigger events of the last Tick.
    Span<const PhysicsEvent> Events() const { return m_events.View(); }

    // Forces and velocities of an entity's Rigid Body (no effect without one or while not playing).
    void AddForce(ecs::Entity e, Vec3 force);
    void AddImpulse(ecs::Entity e, Vec3 impulse);
    void AddImpulseAtPoint(ecs::Entity e, Vec3 impulse, Vec3 point);
    void AddTorque(ecs::Entity e, Vec3 torque);
    Vec3 GetVelocity(ecs::Entity e) const;
    void SetVelocity(ecs::Entity e, Vec3 v);
    Vec3 GetAngularVelocity(ecs::Entity e) const;
    void SetAngularVelocity(ecs::Entity e, Vec3 w);
    /// The joint of an entity (the first joint component; invalid when it has none or it broke).
    physics::JointId JointOf(ecs::Entity e) const;
    /// Hinge angle (radians), slider offset (m), distance (m) of the entity's joint.
    f32 GetJointValue(ecs::Entity e) const;
    /// Motor of the entity's hinge / slider joint (speed in rad/s or m/s; force <= 0 keeps the current one).
    void SetJointMotor(ecs::Entity e, bool enabled, f32 speed, f32 force = 0.0f);
    u32 JointCount() const { return m_world ? m_world->JointCount() : 0u; }
    /// Destroys every joint attached to the entity's body (burnt through, cut loose).
    void BreakJoints(ecs::Entity e);

private:
    struct Tracked
    {
        ecs::Entity entity;
        physics::BodyId body;
        bool moves = false; // dynamic or kinematic
        bool kinematic = false;
        Vec3 prevPos, currPos;
        Quat prevRot, currRot;
        Vec3 written; // last pose written to the entity (teleport detection)
        Quat writtenRot;
    };

    void CreateBodies(ecs::World& world);
    void CreateJoints(ecs::World& world);
    bool CreateBody(ecs::World& world, ecs::Entity e, bool dynamic);
    physics::HullId HullOf(const render::GpuMesh* mesh, Vec3 scale); // null mesh: unit cylinder (diameter 1, height 1)

    UniquePtr<physics::PhysicsWorld> m_world;
    render::RenderAssets* m_assets = nullptr;
    Array<Tracked> m_tracked;
    HashMap<u64, u32> m_byEntity; // entity -> index in m_tracked
    Array<ecs::Entity> m_pendingMeshes; // static mesh colliders whose mesh is not loaded yet
    Array<ecs::Entity> m_pendingBodies; // rigid bodies waiting for the meshes of their convex Mesh Colliders
    Array<PhysicsEvent> m_events;
    Array<physics::ContactEvent> m_rawEvents;
    HashMap<u64, physics::JointId> m_joints; // entity -> its joint
    struct CachedHull
    {
        const render::GpuMesh* mesh = nullptr;
        Vec3 scale;
        physics::HullId hull = physics::InvalidHull;
    };
    Array<CachedHull> m_hulls; // convex Mesh Colliders: instances of one mesh at one scale share a hull
    ecs::Entity EntityOf(physics::BodyId id) const;
    f32 m_accumulator = 0.0f;
};

} // namespace zx
