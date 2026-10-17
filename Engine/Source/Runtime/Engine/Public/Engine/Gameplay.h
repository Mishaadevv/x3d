#pragma once

// Built-in behaviour components (until scripting, M10) and the game tick that runs them. The editor only ticks the
// game while playing (PIE); edit mode shows the scene static.
//
//   TickGameplay(world, dt);   // before UpdateWorldTransforms()

#include "Asset/CookedAssets.h"
#include "Core/Containers/Function.h"
#include "Core/Math/Math.h"
#include "Core/Reflection/Reflection.h"
#include "Scene/ECS/World.h"

namespace zx {

/// Spins the entity around an axis.
struct Rotator
{
    Vec3 axis = Vec3(0, 1, 0);
    f32 degreesPerSecond = 45.0f;
    bool localSpace = true; // axis in the entity's own space (else in its parent's)
};

/// Moves the entity back and forth along an offset (sine), around the position it had when play started.
struct Oscillator
{
    Vec3 offset = Vec3(0, 0.5f, 0);
    f32 frequency = 0.5f; // Hz
    f32 phase = 0.0f;     // 0..1
};

/// The view the game is rendered from while playing (the first active one found). Looks along the entity's +Z.
struct Camera
{
    f32 fieldOfView = 70.0f; // vertical, degrees
    f32 nearPlane = 0.05f;   // meters
    bool active = true;
};

/// A walking character: moved by scripts (character.move / jump) and gravity. With physics (the usual case while
/// playing) its body is a capsule swept through the physics world: it slides along walls, climbs steps up to
/// stepHeight and slopes up to maxSlope, rides moving platforms and pushes rigid bodies according to their mass.
/// Without physics it falls back to the boxes of MeshRenderers with collision.
/// The entity's position is the bottom center of its body (its feet); keep the entity at the root of the hierarchy.
struct CharacterController
{
    f32 height = 1.8f;      // meters
    f32 radius = 0.35f;     // meters
    f32 stepHeight = 0.35f; // obstacles up to this height are climbed (stairs)
    f32 maxSlope = 50.0f;   // degrees: steeper surfaces are walls
    f32 gravity = 18.0f;    // m/s^2 (games feel better above 9.81)
    f32 maxFallSpeed = 40.0f;
    f32 pushMass = 80.0f;   // kg: how hard the character pushes rigid bodies (a light box moves, a heavy one barely)
};

/// Runtime state of a character while playing (not saved).
struct CharacterState
{
    Vec3 velocity;        // m/s (vertical part from gravity / jumps)
    Vec3 desiredMove;     // horizontal velocity requested this frame (cleared after each step)
    Vec3 rootMotion;      // horizontal displacement from animation root motion since the last step (m, world)
    f32 jumpSpeed = 0.0f; // requested jump (applied when grounded)
    bool grounded = false;
    bool initialized = false;
    Vec3 spawn; // respawn point when falling out of the world
    // Moving ground (physics): the body it stands on and the feet in that body's frame (rides platforms).
    u32 groundBodyIndex = ~0u;
    u32 groundBodyGeneration = 0;
    Vec3 groundLocal;
    Vec3 groundNormal = Vec3(0, 1, 0);
};

ZX_DECLARE_TYPE(Rotator);
ZX_DECLARE_TYPE(Oscillator);
/// Marks an entity placed by the editor's Brush tool (so the brush can erase what it painted and not paint on it).
struct PaintedInstance
{
    u32 brushItem = 0; // index of the brush item it came from
};

ZX_DECLARE_TYPE(Camera);
ZX_DECLARE_TYPE(PaintedInstance);
ZX_DECLARE_TYPE(CharacterController);

/// Local-space bounds of a mesh (false while it is not loaded yet).
using MeshBoundsFn = Function<bool(asset::AssetId mesh, AABB& bounds), 32>;

/// Moves every CharacterController by its requested move, gravity and jumps; resolves collisions against the
/// world boxes of MeshRenderers (collision = true) that are not part of the character itself.
void TickCharacters(ecs::World& world, f32 dt, const MeshBoundsFn& meshBounds);
class PhysicsSystem;
/// Moves every CharacterController as a capsule swept through the running physics world (see CharacterController).
void TickCharacters(ecs::World& world, f32 dt, PhysicsSystem& physics);

/// The first active Camera (NullEntity if none).
ecs::Entity FindGameCamera(ecs::World& world);

/// Game time state kept per world while playing.
struct GameplayClock
{
    f64 time = 0.0;
    u64 frame = 0;
};

/// Advances all behaviour components by dt (seconds).
void TickGameplay(ecs::World& world, f32 dt);

} // namespace zx
