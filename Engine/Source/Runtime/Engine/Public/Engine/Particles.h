#pragma once

// Particle Emitter component (fire, smoke, sparks, dust, fog banks, magic) and its CPU simulation.
//
//   ParticleSystem particles;
//   particles.Tick(world, dt, playing);   // editor: emitters with Simulate In Editor run live, all of them in Play
//   particles.Gather(cameraPosition, out); // GPU particles, back to front (render::ParticleRenderer)
//
// Emitters spawn from a point, sphere, box or cone; particles fly with an initial speed, acceleration (buoyancy or
// gravity), drag and turbulence, change size and color over their life, and can carry a flickering light (the
// Point Light on the same entity is modulated by Light Flicker). Everything is deterministic per emitter seed.

#include "Core/Containers/Array.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Random.h"
#include "Core/Reflection/Reflection.h"
#include "Scene/ECS/World.h"
#include "Render/Particles.h"
#include "Render/SceneRenderer.h"

namespace zx {

enum class EmitterShape : u8
{
    Point,
    Sphere, // inside a sphere of Radius, moving outwards
    Box,    // inside Box Size, moving along the entity's +Y
    Cone,   // from a disc of Radius, spreading by Cone Angle around +Y
};

enum class ParticleLook : u8
{
    Soft,  // round puffs: smoke, dust, steam, fog, magic orbs
    Flame, // flickering teardrops: fire, torches, jets
    Spark, // bright dots stretched along their motion: sparks, embers, rain
};

enum class ParticleBlend : u8
{
    Additive, // glows and adds light (fire, sparks, magic)
    Alpha,    // covers what is behind it (smoke, dust, fog)
};

struct ParticleEmitter
{
    bool playing = true;          // emit (games can toggle it; bursts still fire)
    bool simulateInEditor = true; // real-time preview while editing (off: only in Play)
    f32 rate = 20.0f;             // particles per second
    i32 burst = 0;                // particles emitted at once on start (and by entity.emit)
    i32 maxParticles = 300;
    f32 lifetime = 1.5f;          // seconds
    f32 lifetimeVariation = 0.3f; // fraction
    EmitterShape shape = EmitterShape::Cone;
    f32 radius = 0.2f;
    f32 coneAngle = 0.35f; // radians
    Vec3 boxSize = Vec3(1.0f);
    f32 speed = 1.0f;      // m/s
    f32 speedVariation = 0.3f;
    Vec3 acceleration = Vec3(0.0f, 0.5f, 0.0f); // world m/s^2 (negative Y = gravity, positive = rising heat)
    f32 drag = 0.5f;       // 1/s
    f32 turbulence = 0.5f; // m/s^2 of swirling noise
    f32 startSize = 0.5f;
    f32 endSize = 0.2f;
    f32 sizeVariation = 0.3f;
    f32 spin = 0.5f;       // max rad/s either way
    Vec3 startColor = Vec3(1.0f, 0.6f, 0.2f);
    Vec3 endColor = Vec3(0.6f, 0.1f, 0.02f);
    f32 startAlpha = 1.0f;
    f32 endAlpha = 0.0f;
    f32 fadeIn = 0.1f;     // fraction of the life spent fading in
    f32 intensity = 1.0f;  // brightness of emissive (glowing) particles
    ParticleLook look = ParticleLook::Soft;
    ParticleBlend blend = ParticleBlend::Additive;
    bool emissive = true;  // glows (not lit by the sun)
    bool worldSpace = true; // particles stay where they were emitted when the entity moves
    f32 lightFlicker = 0.0f; // 0..1: flickers the Point Light on this entity (fire)
    i32 seed = 0;
};

ZX_DECLARE_ENUM(EmitterShape);
ZX_DECLARE_ENUM(ParticleLook);
ZX_DECLARE_ENUM(ParticleBlend);
ZX_DECLARE_TYPE(ParticleEmitter);

class ParticleSystem
{
public:
    /// Advances every emitter: all of them while `playing`, otherwise those with Simulate In Editor.
    void Tick(ecs::World& world, f32 dt, bool playing);
    /// Every live particle for drawing, sorted back to front from `cameraPosition`.
    void Gather(Vec3 cameraPosition, Array<render::GpuParticle>& out) const;
    /// Point Lights on entities with a flickering emitter: intensity modulated (render-side only).
    void ModulateLights(Span<render::LightDesc> lights) const;
    /// Emits `count` particles from the entity's emitter right away (scripts: entity.emit(n)).
    void Emit(ecs::Entity e, u32 count);
    /// Drops every particle (Play start / stop, scene change).
    void Clear();
    u32 ParticleCount() const;

private:
    struct Particle
    {
        Vec3 position, velocity;
        f32 age = 0.0f, life = 1.0f;
        f32 sizeScale = 1.0f;
        f32 rotation = 0.0f, spin = 0.0f;
        f32 seed = 0.0f;
    };
    struct Emitter
    {
        ParticleEmitter desc;
        Mat4 world = Mat4::Identity();
        Mat4 lastWorld = Mat4::Identity();
        Array<Particle> particles;
        f32 accumulator = 0.0f;
        Random rng;
        u32 pendingBurst = 0;
        bool started = false;
        bool alive = false;
        u32 lightId = 0; // entity index + 1 (LightDesc::id)
        f32 flicker = 1.0f;
        f32 time = 0.0f;
    };
    void Spawn(Emitter& em, u32 count);

    HashMap<u64, Emitter> m_emitters; // entity -> state
    Array<ecs::Entity> m_pendingEmits;
    Array<u32> m_pendingCounts;
    bool m_wasPlaying = false;
};

} // namespace zx
