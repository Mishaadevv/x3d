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
// pass, as in Box2D v3), friction, restitution. Islands sleep when they come to rest. Deterministic for the same
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
    TriangleMesh, // static bodies only
};

/// Opaque handle of a triangle mesh registered in a world (CreateTriangleMesh).
using MeshId = u32;
inline constexpr MeshId InvalidMesh = ~0u;

struct ShapeDesc
{
    ShapeType type = ShapeType::Box;
    Vec3 halfExtents = Vec3(0.5f); // box
    f32 radius = 0.5f;             // sphere, capsule
    f32 halfHeight = 0.5f;         // capsule: half length of the inner segment
    MeshId mesh = InvalidMesh;     // triangle mesh
    Vec3 position;                 // pose relative to the body origin
    Quat rotation;
    f32 density = 1000.0f; // kg/m^3 (water); a body's mass comes from its shapes unless BodyDesc::mass is set

    static ShapeDesc Box(Vec3 halfExtents, Vec3 position = {}, Quat rotation = {});
    static ShapeDesc Sphere(f32 radius, Vec3 position = {});
    static ShapeDesc Capsule(f32 radius, f32 halfHeight, Vec3 position = {}, Quat rotation = {});
    static ShapeDesc Mesh(MeshId mesh, Vec3 position = {}, Quat rotation = {});
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

struct PhysicsSettings
{
    Vec3 gravity = Vec3(0.0f, -9.81f, 0.0f);
    u32 substeps = 4;                // solver substeps per Step (stability of stacks and joints)
    u32 relaxIterations = 2;         // per substep, without position bias
    f32 contactHertz = 60.0f;        // stiffness of the overlap correction (capped at a quarter of the substep rate)
    f32 contactDampingRatio = 10.0f;
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

    /// Whether bodies on layers a and b collide (default: all pairs).
    void SetLayersCollide(u32 a, u32 b, bool collide);
    bool LayersCollide(u32 a, u32 b) const { return (m_layerMask[a & 31] >> (b & 31)) & 1u; }

    /// Advances the simulation by dt (use a fixed step, e.g. 1/60 s).
    void Step(f32 dt);

    // --- Queries ---
    /// Closest hit along the ray within maxDistance; layerMask selects the layers that can be hit.
    bool Raycast(const Ray& ray, f32 maxDistance, RaycastHit& hit, u32 layerMask = ~0u) const;
    /// Bodies whose bounds overlap `box`.
    void QueryAabb(const AABB& box, Array<BodyId>& out) const;

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
    PhysicsStats m_stats;
};

} // namespace zx::physics
