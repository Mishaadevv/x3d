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
    f32 linearDamping = 0.05f;
    f32 angularDamping = 0.05f;
    f32 gravityScale = 1.0f;
    bool canSleep = true;
};

struct BoxCollider
{
    Vec3 center;
    Vec3 size = Vec3(1.0f); // full size (scaled by the transform)
};

struct SphereCollider
{
    Vec3 center;
    f32 radius = 0.5f;
};

/// Capsule along the entity's local Y axis.
struct CapsuleCollider
{
    Vec3 center;
    f32 radius = 0.5f;
    f32 height = 2.0f; // end to end
};

/// The triangles of the entity's Mesh Renderer (static collision: level geometry).
struct MeshCollider
{
    bool enabled = true;
};

ZX_DECLARE_ENUM(RigidBodyType);
ZX_DECLARE_TYPE(RigidBody);
ZX_DECLARE_TYPE(BoxCollider);
ZX_DECLARE_TYPE(SphereCollider);
ZX_DECLARE_TYPE(CapsuleCollider);
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
};
void GatherColliderOutlines(ecs::World& world, ecs::Entity e, Array<ColliderOutline>& out);

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
    bool CreateBody(ecs::World& world, ecs::Entity e, bool dynamic);

    UniquePtr<physics::PhysicsWorld> m_world;
    render::RenderAssets* m_assets = nullptr;
    Array<Tracked> m_tracked;
    HashMap<u64, u32> m_byEntity; // entity -> index in m_tracked
    Array<ecs::Entity> m_pendingMeshes; // static mesh colliders whose mesh is not loaded yet
    f32 m_accumulator = 0.0f;
};

} // namespace zx
