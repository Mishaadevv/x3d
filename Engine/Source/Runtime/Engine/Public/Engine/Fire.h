#pragma once

// Fire that spreads (Play only). A Flammable object heats up next to flames - other burning objects, fire Particle
// Emitters, scripts - and catches fire at its hottest point once it reaches its ignition temperature. The flames then
// creep over its surface from that point, every spot burning for Burn Time before it goes out, so a plank burns from
// one end to the other. The burnt part chars and glows with embers, smoke rises in proportion to how much is in flames
// (thin and pale once only embers are left), and when the fuel is gone the object stays charred, crumbles to ash or
// drops off its joints. Water puts it out.
//
//   FireSystem fire;
//   fire.meshBounds = ...; fire.physics = &physics;
//   fire.Tick(world, dt, particles); // while playing, before particles.Tick
//   fire.Ignite(world, entity, worldPoint);              // scripts: entity.ignite(), fire.ignite(point, radius)

#include "Core/Containers/Array.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Random.h"
#include "Core/Reflection/Reflection.h"
#include "Engine/Gameplay.h"
#include "Scene/ECS/World.h"
#include "Render/SceneRenderer.h"

namespace zx {

class ParticleSystem;
class PhysicsSystem;

enum class BurntResult : u8
{
    Charred, // stays, black and smouldering
    Crumble, // shrinks into ash and disappears
};

enum class FireState : u8
{
    None,
    Burning,
    Burnt,
};

struct Flammable
{
    bool startBurning = false;          // on fire when Play starts
    f32 ignitionTemperature = 300.0f;   // degrees C at which it catches fire
    f32 resistance = 1.0f;              // thermal mass: higher = heats up slower
    f32 burnTime = 20.0f;               // seconds a spot burns before it goes out (fuel)
    f32 spreadSpeed = 0.12f;            // m/s the flames creep over the surface
    f32 heat = 1.0f;                    // heat given off (ignites neighbours, flame size)
    f32 heatRange = 0.8f;               // m around the flames that gets hot
    f32 flameHeight = 0.6f;             // m
    f32 flameDensity = 1.0f;            // flame particles per area (multiplier)
    Vec3 flameColor = Vec3(1.0f, 0.42f, 0.1f);
    f32 smoke = 1.0f;                   // smoke per area (multiplier)
    Vec3 smokeColor = Vec3(0.09f, 0.085f, 0.08f);
    f32 charring = 0.92f;               // how black the burnt part turns
    f32 light = 1.0f;                   // brightness of the fire's light (multiplier)
    BurntResult whenBurnt = BurntResult::Charred;
    bool breakJoints = true;            // its joints break once it has burnt through
    bool waterExtinguishes = true;      // goes out under a Water surface
    String sound;                       // looping crackle (Content-relative); volume follows the fire

    // Runtime state (Play only, not saved).
    FireState state = FireState::None;
    f32 temperature = 20.0f;  // degrees C
    f32 intensity = 0.0f;     // 0..1: how much of the object is in flames
    Vec3 burnOrigin;          // local ignition point
    f32 charRadius = 0.0f;    // m around the origin that is burnt (rendered charred)
    f32 glow = 0.0f;          // 0..1 glowing edge of the flames
    f32 embers = 0.0f;        // 0..1 embers glowing in the charred part
};

ZX_DECLARE_ENUM(BurntResult);
ZX_DECLARE_TYPE(Flammable);

class FireSystem
{
public:
    /// Local bounds of meshes (the shape of Flammable objects without a Box / Sphere / Capsule Collider).
    MeshBoundsFn meshBounds;
    /// Joints of burnt objects break through it (null: they stay).
    PhysicsSystem* physics = nullptr;

    /// Spreads, burns and renders every fire (call only while playing; frozen when paused). Flames, smoke and embers
    /// go to `particles`, the fire lights too (ParticleSystem::SetExtraLights).
    void Tick(ecs::World& world, f32 dt, ParticleSystem& particles);
    /// Sets the entity on fire at `worldPoint` (default: the bottom of the object). False if it isn't Flammable or has
    /// burnt out.
    bool Ignite(ecs::World& world, ecs::Entity e, const Vec3* worldPoint = nullptr);
    /// Every Flammable object within `radius` of `point` catches fire at its closest point. Returns how many.
    u32 IgniteAt(ecs::World& world, Vec3 point, f32 radius);
    /// Puts the entity's fire out (it stays charred and can catch fire again).
    void Extinguish(ecs::World& world, ecs::Entity e);
    /// Puts out every fire within `radius` of `point`.
    u32 ExtinguishAt(ecs::World& world, Vec3 point, f32 radius);
    /// Fire lights of this frame (point lights over the flames, flickering).
    Span<const render::LightDesc> Lights() const { return m_lights.View(); }
    /// Drops all fire state (Play start / stop).
    void Clear();
    u32 BurningCount() const { return m_burningCount; }

private:
    enum class Shape : u8 { Box, Sphere };
    struct Burn
    {
        Shape shape = Shape::Box;
        Vec3 center;         // local
        Vec3 half = Vec3(0.5f); // local half extents (box) / radius in x (sphere)
        Vec3 worldHalf = Vec3(0.5f); // half extents scaled to world size
        Array<Vec3> samples; // local surface points (area-weighted)
        f32 area = 1.0f;     // world surface area (m^2)
        f32 extent = 1.0f;   // m: farthest surface point from the origin
        f32 time = 0.0f;     // s since ignition
        f32 outer = 0.0f, inner = 0.0f; // m: flames between inner and outer
        f32 burntFor = 0.0f; // s since it burnt out
        f32 crumble = 0.0f;  // 0..1 shrinking into ash
        Vec3 scale = Vec3(1.0f);
        Vec3 position;       // where it started crumbling (local)
        f32 flicker = 1.0f;
        f32 emitFlame = 0.0f, emitSmoke = 0.0f, emitEmber = 0.0f;
        Random rng;
        bool initialized = false;
        bool addedSound = false;
        bool jointsBroken = false;
    };
    Burn& StateOf(ecs::World& world, ecs::Entity e);
    void Setup(ecs::World& world, ecs::Entity e, Burn& b);
    static Vec3 RandomSurfacePoint(Burn& b, Vec3* normal = nullptr);
    static Vec3 ClosestPoint(const Burn& b, const Mat4& world, const Mat4& inverse, Vec3 point);
    void StartBurning(ecs::World& world, ecs::Entity e, Flammable& f, Burn& b, Vec3 localPoint);
    void Steam(Vec3 position, f32 size);

    HashMap<u64, Burn> m_burns; // entity -> state
    Array<render::LightDesc> m_lights;
    struct SoundChange
    {
        ecs::Entity entity;
        String clip; // empty: remove the Audio Source the fire added
    };
    Array<SoundChange> m_soundChanges;
    Array<ecs::Entity> m_toDestroy;
    ParticleSystem* m_particles = nullptr;
    f32 m_time = 0.0f;
    u32 m_burningCount = 0;
    bool m_started = false;
};

} // namespace zx
