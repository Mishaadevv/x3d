#pragma once

// ZPhys: the engine's own rigid-body physics.
//
//   PhysicsWorld world;
//   BodyDesc ground;                                  // static by default
//   ground.shapes.PushBack(ShapeDesc::Box(Vec3(50, 0.5f, 50)));
//   world.CreateBody(ground);
//   BodyDesc box;
//   box.motion = MotionType::Dynamic;
//   box.position = Vec3(0, 5, 0);
//   box.shapes.PushBack(ShapeDesc::Box(Vec3(0.5f)));
//   const BodyId id = world.CreateBody(box);
//   world.Step(1.0f / 60.0f);                         // fixed steps
//   Vec3 p = world.GetPosition(id);
//
// Shapes: sphere, box, capsule (along local Y), static triangle meshes; several shapes per body (compound, each with a
// local pose). Broadphase: two dynamic AABB trees (static / moving). Narrowphase: SAT with face clipping for boxes and
// triangles (up to 4 contact points), exact closest points for spheres and capsules, speculative contacts. Solver:
// soft-step sequential impulses (4 substeps with warm starting, soft spring-damper overlap correction and a relax
// pass, as in Box2D v3), friction, restitution, joints (fixed, hinge, slider, ball, distance/spring, configurable)
// with limits and motors. Islands sleep when they come to rest. Deterministic for the same
// inputs and build.

#include "Core/Containers/Array.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Geometry.h"
#include "Core/Math/Quat.h"
#include "Core/Memory/Pointers.h"

namespace zx::physics {

enum class MotionType : u8
{
    Static,    // never moves (level geometry)
    Kinematic, // moved by the game (MoveKinematic), pushes dynamic bodies, not pushed back
    Dynamic,   // simulated
};

enum class ShapeType : u8
{
    Sphere,
    Box,
    Capsule,      // segment along local Y (halfHeight) + radius
    ConvexHull,   // convex point cloud (CreateConvexHull): dynamic bodies of any convex shape
    TriangleMesh, // static bodies only
};

/// Opaque handle of a triangle mesh registered in a world (CreateTriangleMesh).
using MeshId = u32;
inline constexpr MeshId InvalidMesh = ~0u;
/// Opaque handle of a convex hull registered in a world (CreateConvexHull).
using HullId = u32;
inline constexpr HullId InvalidHull = ~0u;

struct ShapeDesc
{
    ShapeType type = ShapeType::Box;
    Vec3 halfExtents = Vec3(0.5f); // box
    f32 radius = 0.5f;             // sphere, capsule
    f32 halfHeight = 0.5f;         // capsule: half length of the inner segment
    MeshId mesh = InvalidMesh;     // triangle mesh
    HullId hull = InvalidHull;     // convex hull
    Vec3 position;                 // pose relative to the body origin
    Quat rotation;
    f32 density = 1000.0f; // kg/m^3 (water); a body's mass comes from its shapes unless BodyDesc::mass is set
    bool isSensor = false;  // trigger: reports overlaps (SensorEnter/Exit events), doesn't collide

    static ShapeDesc Box(Vec3 halfExtents, Vec3 position = {}, Quat rotation = {});
    static ShapeDesc Sphere(f32 radius, Vec3 position = {});
    static ShapeDesc Capsule(f32 radius, f32 halfHeight, Vec3 position = {}, Quat rotation = {});
    static ShapeDesc Mesh(MeshId mesh, Vec3 position = {}, Quat rotation = {});
    static ShapeDesc Hull(HullId hull, Vec3 position = {}, Quat rotation = {});
};

struct BodyDesc
{
    MotionType motion = MotionType::Static;
    Vec3 position; // body origin (world)
    Quat rotation;
    Array<ShapeDesc> shapes;
    f32 mass = 0.0f;           // > 0 overrides the mass from the shape densities (inertia scales with it)
    f32 friction = 0.6f;
    f32 restitution = 0.0f;    // bounciness 0..1
    f32 linearDamping = 0.05f;
    f32 angularDamping = 0.05f;
    f32 gravityScale = 1.0f;
    Vec3 linearVelocity;
    Vec3 angularVelocity;
    u32 layer = 0;             // 0..31 (see PhysicsWorld::SetLayersCollide)
    bool allowSleep = true;
    u64 userData = 0;
};

/// Body handle: index + generation (a destroyed body's id never matches a new one).
struct BodyId
{
    u32 index = ~0u;
    u32 generation = 0;
    bool IsValid() const { return index != ~0u; }
    friend bool operator==(BodyId, BodyId) = default;
};

enum class JointType : u8
{
    Fixed,        // glues two bodies together
    Hinge,        // rotation around one axis (doors, wheels); limits + motor
    Slider,       // translation along one axis (pistons, drawers); limits + motor
    Ball,         // rotation around a point (ragdolls, chains); swing cone + twist limits
    Distance,     // keeps two anchors at a distance: rigid rod, rope (max), strut (min) or spring
    Configurable, // each linear / angular axis of the joint frame locked, limited or free
};

enum class AxisMotion : u8
{
    Locked,
    Limited,
    Free,
};

/// A constraint between two bodies (or a body and the world). Anchor and axes are given in world space for the
/// poses at creation; the joint keeps that relative arrangement.
struct JointDesc
{
    JointType type = JointType::Fixed;
    BodyId a;              // must be valid
    BodyId b;              // invalid = attached to the world
    Vec3 anchor;           // world point (on A; for Distance joints the anchor on A)
    Vec3 anchorB;          // Distance joints: the anchor on B (world point)
    Vec3 axis = Vec3(0.0f, 1.0f, 0.0f); // hinge / slider axis, cone / twist axis, X axis of the configurable frame
    bool useLimits = false;
    f32 lower = 0.0f, upper = 0.0f; // radians (Hinge) or meters (Slider) relative to the creation pose
    bool useMotor = false;
    f32 motorSpeed = 0.0f;      // rad/s or m/s
    f32 maxMotorForce = 1000.0f; // N*m or N
    f32 swingLimit = 0.0f;      // Ball: cone half angle (radians, 0 = free)
    f32 twistLimit = 0.0f;      // Ball: +-twist around the axis (radians, 0 = free)
    f32 minDistance = 0.0f;     // Distance: 0 = none; with neither min, max nor spring the joint is a rigid rod
    f32 maxDistance = 0.0f;
    f32 springHertz = 0.0f;     // Distance: > 0 = spring towards the rest length
    f32 springDamping = 0.2f;   // damping ratio (1 = critical)
    f32 restLength = 0.0f;      // Distance: <= 0 = the distance between the anchors at creation
    AxisMotion linear[3] = {AxisMotion::Locked, AxisMotion::Locked, AxisMotion::Locked};
    AxisMotion angular[3] = {AxisMotion::Locked, AxisMotion::Locked, AxisMotion::Locked};
    f32 linearLimit = 0.0f;  // Configurable: +-meters on Limited linear axes
    f32 angularLimit = 0.0f; // Configurable: +-radians on Limited angular axes
    bool collideConnected = false; // contacts between the two bodies
    f32 breakForce = 0.0f;  // N: the joint breaks (is destroyed) above this force; 0 = unbreakable
    f32 breakTorque = 0.0f; // N*m, the same for torque
};

struct JointId
{
    u32 index = ~0u;
    u32 generation = 0;
    bool IsValid() const { return index != ~0u; }
    friend bool operator==(JointId, JointId) = default;
};

struct PhysicsSettings
{
    Vec3 gravity = Vec3(0.0f, -9.81f, 0.0f);
    u32 substeps = 4;                // solver substeps per Step (stability of stacks and joints)
    u32 relaxIterations = 2;         // per substep, without position bias
    f32 contactHertz = 60.0f;        // stiffness of the overlap correction (capped at a quarter of the substep rate)
    f32 contactDampingRatio = 10.0f;
    f32 jointHertz = 60.0f;          // stiffness of the joint error correction
    f32 jointDampingRatio = 2.0f;
    f32 maxPushVelocity = 3.0f;      // m/s: how fast overlaps are pushed apart
    f32 speculativeDistance = 0.02f; // contacts are created this far before touching (meters)
    f32 restitutionThreshold = 1.0f; // m/s: slower impacts don't bounce
    f32 sleepVelocity = 0.05f;       // m/s (and rad/s) below which bodies count as resting
    f32 timeToSleep = 0.5f;          // seconds an island must rest before it sleeps
};

struct RaycastHit
{
    BodyId body;
    Vec3 point;
    Vec3 normal;
    f32 distance = 0.0f;
    u32 shape = 0; // index into the body's shapes
};

struct PhysicsStats
{
    u32 bodies = 0;
    u32 dynamic = 0;
    u32 awake = 0;
    u32 pairs = 0;    // broadphase pairs
    u32 contacts = 0; // touching manifolds
    u32 points = 0;   // contact points
    u32 islands = 0;
    f64 stepMs = 0.0;
    f64 broadphaseMs = 0.0;
    f64 boundsMs = 0.0; // part of the broadphase: bounds + tree updates
    f64 narrowphaseMs = 0.0;
    f64 solverMs = 0.0;
};

enum class ContactEventType : u8
{
    TouchBegin, // two solid bodies started touching
    TouchEnd,
    SensorEnter, // a body entered a sensor shape (of either body)
    SensorExit,
};

/// Begin/end of contact between two bodies (per body pair, not per shape), gathered over Steps until TakeEvents.
struct ContactEvent
{
    ContactEventType type = ContactEventType::TouchBegin;
    BodyId a, b;
    Vec3 point;  // a contact point (begin events)
    Vec3 normal; // from a to b (begin events)
    f32 speed = 0.0f; // approach speed along the normal at the begin (m/s)
};

/// Contact point for debug drawing.
struct DebugContact
{
    Vec3 position;
    Vec3 normal;
    f32 separation;
};

class PhysicsWorld
{
public:
    PhysicsWorld();
    ~PhysicsWorld();
    ZX_NON_COPYABLE(PhysicsWorld)

    PhysicsSettings& Settings() { return m_settings; }

    /// Registers a static triangle mesh (positions + 3 indices per triangle); a BVH is built here.
    MeshId CreateTriangleMesh(Span<const Vec3> positions, Span<const u32> indices);
    /// Registers the convex hull of `points` (shape space; quickhull, at most 32 vertices kept). Invalid when the points
    /// are flat or too few.
    HullId CreateConvexHull(Span<const Vec3> points);
    /// Hull geometry for debug drawing: vertices and 2 indices per edge (empty for an invalid id).
    void GetHullEdges(HullId hull, Array<Vec3>& vertices, Array<u32>& edges) const;
    /// The hull CreateConvexHull would make of `points`, as edges (editor gizmos without a world). False when flat.
    static bool BuildHullEdges(Span<const Vec3> points, Array<Vec3>& vertices, Array<u32>& edges);

    BodyId CreateBody(const BodyDesc& desc);
    void DestroyBody(BodyId id);
    bool IsValid(BodyId id) const;
    u32 BodyCount() const { return m_bodyCount; }

    // --- State ---
    Vec3 GetPosition(BodyId id) const; // body origin
    Quat GetRotation(BodyId id) const;
    Vec3 GetCenterOfMass(BodyId id) const;
    Vec3 GetLinearVelocity(BodyId id) const;
    Vec3 GetAngularVelocity(BodyId id) const;
    MotionType GetMotionType(BodyId id) const;
    f32 GetMass(BodyId id) const;
    u64 GetUserData(BodyId id) const;
    bool IsSleeping(BodyId id) const;
    AABB GetBounds(BodyId id) const;
    const Array<ShapeDesc>& GetShapes(BodyId id) const;

    /// Teleports a body (wakes it and what touches it).
    void SetTransform(BodyId id, Vec3 position, Quat rotation);
    void SetLinearVelocity(BodyId id, Vec3 v);
    void SetAngularVelocity(BodyId id, Vec3 w);
    /// Kinematic bodies: velocity that reaches the target pose by the end of the next step (pushes what it hits).
    void MoveKinematic(BodyId id, Vec3 position, Quat rotation, f32 dt);
    void AddForce(BodyId id, Vec3 force);                  // newtons, applied at the center of mass this step
    void AddForceAtPoint(BodyId id, Vec3 force, Vec3 worldPoint);
    void AddImpulse(BodyId id, Vec3 impulse);              // N*s
    void AddImpulseAtPoint(BodyId id, Vec3 impulse, Vec3 worldPoint);
    void AddTorque(BodyId id, Vec3 torque);
    void Wake(BodyId id);

    // --- Joints ---
    /// Creates a joint (destroyed with either body). Returns an invalid id when body A is invalid.
    JointId CreateJoint(const JointDesc& desc);
    void DestroyJoint(JointId id);
    bool IsValid(JointId id) const;
    u32 JointCount() const { return m_jointCount; }
    const JointDesc& GetJointDesc(JointId id) const;
    /// Hinge: angle (radians), Slider: offset along the axis, Distance: current length; 0 for others.
    f32 GetJointCoordinate(JointId id) const;
    /// World anchors on A and B (they coincide while the joint holds).
    void GetJointAnchors(JointId id, Vec3& onA, Vec3& onB) const;
    void SetJointMotor(JointId id, bool enabled, f32 speed, f32 maxForce);
    void SetJointLimits(JointId id, bool enabled, f32 lower, f32 upper);
    /// Every joint id (in index order).
    void GetJoints(Array<JointId>& out) const;
    /// Joints broken (breakForce / breakTorque exceeded) in the Steps since the last call; they are already destroyed.
    void TakeBrokenJoints(Array<JointId>& out);

    /// Whether bodies on layers a and b collide (default: all pairs).
    void SetLayersCollide(u32 a, u32 b, bool collide);
    bool LayersCollide(u32 a, u32 b) const { return (m_layerMask[a & 31] >> (b & 31)) & 1u; }

    /// Advances the simulation by dt (use a fixed step, e.g. 1/60 s).
    void Step(f32 dt);

    // --- Queries ---
    /// Closest hit along the ray within maxDistance; layerMask selects the layers that can be hit.
    bool Raycast(const Ray& ray, f32 maxDistance, RaycastHit& hit, u32 layerMask = ~0u, BodyId ignore = {}) const;
    /// Bodies whose bounds overlap `box`.
    void QueryAabb(const AABB& box, Array<BodyId>& out) const;
    /// Bodies a shape (sphere / box / capsule) placed at position/rotation overlaps (sensors included).
    void Overlap(const ShapeDesc& shape, Vec3 position, Quat rotation, Array<BodyId>& out, u32 layerMask = ~0u, BodyId ignore = {}) const;
    /// Sweeps a shape from position along unit `direction`; the first solid body it touches (sensors are ignored).
    /// Starting inside something reports a hit at distance 0 with the push-out normal.
    bool ShapeCast(const ShapeDesc& shape, Vec3 position, Quat rotation, Vec3 direction, f32 maxDistance, RaycastHit& hit,
                   u32 layerMask = ~0u, BodyId ignore = {}) const;
    /// Contact events of the Steps since the last call.
    void TakeEvents(Array<ContactEvent>& out);

    const PhysicsStats& Stats() const { return m_stats; }
    /// Contact points of the last step (debug drawing).
    void GetDebugContacts(Array<DebugContact>& out) const;
    /// Every body id (in index order).
    void GetBodies(Array<BodyId>& out) const;

    struct Impl;

private:
    PhysicsSettings m_settings;
    UniquePtr<Impl> m_impl;
    u32 m_layerMask[32];
    u32 m_bodyCount = 0;
    u32 m_jointCount = 0;
    PhysicsStats m_stats;
};

} // namespace zx::physics
