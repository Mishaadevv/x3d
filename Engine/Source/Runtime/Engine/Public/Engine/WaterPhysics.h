#pragma once

// Water for gameplay: the height of the waves anywhere (the same waves the renderer draws - a CPU copy of the long
// cascades, render::OceanWaves), floating bodies and splashes.
//
//   Buoyancy   on a Rigid Body: it floats on the waves (Archimedes on sample points spread over its colliders) with water
//              drag, rocking and drifting with the waves. Density decides how high it floats: wood ~600 kg/m^3 floats
//              with 60 % under water, ice 920 barely, 1000+ sinks.
//   Splashes   any Rigid Body hitting the water fast throws up a splash (droplets + spray), floating bodies moving
//              fast leave spray at the waterline.
//
//   WaterSystem water;  water.Update(world);         // once per frame (game time = EnvironmentTime(world))
//   WaterSample s; if (water.Sample(position, s)) ... // surface height, normal and water velocity over a point
//   water.TickBuoyancy(world, physics, step);         // before every fixed physics step (PhysicsSystem::beforeStep)
//   water.TickSplashes(world, physics, particles, dt);

#include "Core/Containers/Array.h"
#include "Core/Math/Math.h"
#include "Core/Memory/Pointers.h"
#include "Core/Reflection/Reflection.h"
#include "Scene/ECS/World.h"

namespace zx {

namespace render {
class OceanWaves;
}
class PhysicsSystem;
class ParticleSystem;

struct Buoyancy
{
    f32 density = 600.0f;     // kg/m^3 of the object (wood 600, ice 920; water is 1000 - heavier sinks)
    f32 waterDrag = 1.0f;     // how much the water slows it (and carries it along with the waves)
    f32 angularDrag = 1.0f;   // how quickly the rocking calms down
    f32 volume = 0.0f;        // m^3 of the hull; 0 = from the colliders
    bool splashes = true;     // spray when it moves fast at the waterline
};
ZX_DECLARE_TYPE(Buoyancy);

/// The water over (or under) a point.
struct WaterSample
{
    f32 height = 0.0f;               // surface height (world) at the point's xz
    Vec3 normal = Vec3(0, 1, 0);
    Vec3 velocity;                   // of the water at the surface (orbital motion of the waves)
    f32 depth = 0.0f;                // how far the point is below the surface (negative above it)
    ecs::Entity water;               // the Water entity
};

/// Waves at game time: the clock of the scene (EnvironmentTime) wrapped like the renderer's.
inline f32 WaterTime(f64 clock) { return f32(clock - 3600.0 * f64(i64(clock / 3600.0))); }

class WaterSystem
{
public:
    WaterSystem();
    ~WaterSystem();
    ZX_NON_COPYABLE(WaterSystem)

    /// Collects the Water entities and evolves their waves to the scene's clock.
    void Update(ecs::World& world);
    /// The surface over `position` (false when no body of water lies there). The waves move points sideways too: the
    /// height is that of the surface point now above `position`.
    bool Sample(Vec3 position, WaterSample& out) const;
    /// Forces on every Rigid Body with Buoyancy for one fixed step (call before each PhysicsWorld step: forces are
    /// cleared after every step).
    void TickBuoyancy(ecs::World& world, PhysicsSystem& physics, f32 step);
    /// Splashes of bodies hitting the water and spray of fast floating ones (once per frame, after the physics).
    void TickSplashes(ecs::World& world, PhysicsSystem& physics, ParticleSystem& particles, f32 dt);
    /// Splashes since the last call (position, strength 0..1) - for sounds and gameplay.
    struct Splash
    {
        Vec3 position;
        f32 strength = 0.0f;
        ecs::Entity entity;
    };
    Span<const Splash> Splashes() const { return m_splashes.View(); }
    void ClearSplashes() { m_splashes.Clear(); }
    /// Forget the bodies' state (Play start / stop).
    void Reset();
    u32 WaterCount() const { return u32(m_bodies.Size()); }
    f32 Time() const { return m_time; }

private:
    struct Body
    {
        ecs::Entity entity;
        Mat4 toLocal;
        f32 surfaceY = 0.0f;
        Vec2 half;
        f32 amplitude = 1.0f;
        f32 edgeFade = 0.0f;
        u32 waves = 0; // index into m_waves
    };
    struct Floater
    {
        ecs::Entity entity;
        Array<Vec4> points; // local position + volume
        f32 halfHeight = 0.1f; // vertical half size of a point's cell
        bool wasInWater = false;
        f32 sprayCarry = 0.0f;
    };
    Vec3 Displacement(const Body& b, Vec2 xz, Vec3* velocity) const;
    Floater& FloaterOf(ecs::World& world, PhysicsSystem& physics, ecs::Entity e);

    Array<Body> m_bodies;
    Array<UniquePtr<render::OceanWaves>> m_waves;
    Array<Floater> m_floaters;
    Array<Splash> m_splashes;
    Array<ecs::Entity> m_inWater; // dynamic bodies under the surface last frame
    f32 m_time = 0.0f;
    u32 m_rng = 12345;
};

} // namespace zx
