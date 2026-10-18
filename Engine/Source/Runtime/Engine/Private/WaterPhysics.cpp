#include "Engine/WaterPhysics.h"
#include <cmath>
#include "Engine/Particles.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/SceneRendering.h"
#include "Engine/Wind.h"
#include "Physics/PhysicsWorld.h"
#include "Render/OceanWaves.h"
#include "Render/Water.h"
#include "Scene/Hierarchy.h"

namespace zx {

ZX_REFLECT_STRUCT_BEGIN(Buoyancy)
    ZX_FIELD(density, Meta::Range(50, 3000), Meta::Units("kg/m3"),
             Meta::Tooltip("Density of the object: wood ~600 floats, ice ~920 barely; water is 1000, heavier sinks"))
    ZX_FIELD(waterDrag, Meta::Range(0, 5), Meta::Tooltip("How much the water slows it down and carries it along with the waves"))
    ZX_FIELD(angularDrag, Meta::Range(0, 5), Meta::Tooltip("How quickly the rocking calms down"))
    ZX_FIELD(volume, Meta::Range(0, 1000), Meta::Units("m3"), Meta::Tooltip("Volume of the hull; 0 = from the colliders"))
    ZX_FIELD(splashes, Meta::Tooltip("Spray at the waterline when it moves fast"))
ZX_REFLECT_STRUCT_END()

namespace {

constexpr f32 WaterDensity = 1000.0f;
constexpr f32 Gravity = 9.81f;

f32 Random01(u32& state)
{
    state = state * 1664525u + 1013904223u;
    return f32(state >> 8) / 16777216.0f;
}

ParticleEmitter DropletLook(f32 scale)
{
    ParticleEmitter d;
    d.look = ParticleLook::Spark; // stretched along their motion
    d.blend = ParticleBlend::Alpha;
    d.emissive = false;
    d.lifetime = 1.1f;
    d.lifetimeVariation = 0.35f;
    d.acceleration = Vec3(0.0f, -Gravity, 0.0f);
    d.drag = 0.25f;
    d.turbulence = 0.0f;
    d.startSize = 0.08f * scale;
    d.endSize = 0.05f * scale;
    d.sizeVariation = 0.5f;
    d.startColor = Vec3(0.86f, 0.93f, 1.0f);
    d.endColor = Vec3(0.8f, 0.9f, 1.0f);
    d.startAlpha = 0.9f;
    d.endAlpha = 0.5f;
    d.fadeIn = 0.0f;
    d.maxParticles = 2000;
    return d;
}

ParticleEmitter MistLook(f32 scale)
{
    ParticleEmitter d;
    d.look = ParticleLook::Soft;
    d.blend = ParticleBlend::Alpha;
    d.emissive = false;
    d.lifetime = 1.6f;
    d.lifetimeVariation = 0.3f;
    d.acceleration = Vec3(0.0f, -1.2f, 0.0f);
    d.drag = 1.6f;
    d.turbulence = 0.6f;
    d.startSize = 0.3f * scale;
    d.endSize = 0.9f * scale;
    d.sizeVariation = 0.3f;
    d.startColor = Vec3(0.92f, 0.96f, 1.0f);
    d.endColor = Vec3(0.9f, 0.95f, 1.0f);
    d.startAlpha = 0.28f;
    d.endAlpha = 0.0f;
    d.fadeIn = 0.15f;
    d.maxParticles = 600;
    return d;
}

constexpr u64 DropletKey = 0x5B1A5400ull, MistKey = 0x5B1A5401ull, SprayKey = 0x5B1A5402ull;

} // namespace

WaterSystem::WaterSystem() = default;
WaterSystem::~WaterSystem() = default;

void WaterSystem::Reset()
{
    m_floaters.Clear();
    m_inWater.Clear();
    m_splashes.Clear();
}

void WaterSystem::Update(ecs::World& world)
{
    Array<render::WaterSurface> surfaces;
    GatherWater(world, surfaces);
    Array<ecs::Entity> entities; // same query order as GatherWater
    world.Query<const scene::WorldTransform, const Water>().Each([&](ecs::Entity e, const scene::WorldTransform&, const Water&) { entities.PushBack(e); });
    const f64 clock = EnvironmentTime(world);
    m_time = WaterTime(clock >= 0.0 ? clock : 0.0);

    m_bodies.Clear();
    Array<bool> used;
    used.Resize(m_waves.Size(), false);
    for (usize i = 0; i < surfaces.Size(); ++i) {
        const render::WaterSurface& s = surfaces[i];
        const u64 key = render::WaterSpectrumKey(s);
        u32 index = ~0u;
        for (u32 w = 0; w < m_waves.Size(); ++w)
            if (m_waves[w]->Key() == key)
                index = w;
        if (index == ~0u) {
            // Reuse an unused slot before growing (the spectrum changes while a preset or the wind is edited).
            for (u32 w = 0; w < m_waves.Size() && index == ~0u; ++w)
                if (!used[w] && i == surfaces.Size() - 1)
                    index = w;
            if (index == ~0u) {
                index = u32(m_waves.Size());
                m_waves.PushBack(UniquePtr<render::OceanWaves>(new render::OceanWaves()));
                used.PushBack(false);
            }
            m_waves[index]->Build(s);
        }
        used[index] = true;
        m_waves[index]->Update(m_time);
        Body& b = m_bodies.AddDefaulted();
        b.entity = i < entities.Size() ? entities[i] : ecs::Entity();
        b.toLocal = Inverse(s.world);
        b.surfaceY = s.world.GetTranslation().y;
        b.half = s.size * 0.5f;
        b.amplitude = Max(s.waveAmount, 0.0f);
        b.edgeFade = Min(3.0f, Min(s.size.x, s.size.y) * 0.1f); // as the renderer
        b.waves = index;
    }
    // Drop simulations nobody uses (keep the array compact; indices were assigned above).
    if (m_waves.Size() > m_bodies.Size() + 2)
        for (usize w = m_waves.Size(); w-- > 0;)
            if (!used[w] && w + 1 == m_waves.Size())
                m_waves.PopBack();
}

Vec3 WaterSystem::Displacement(const Body& b, Vec2 xz, Vec3* velocity) const
{
    const Vec3 local = b.toLocal.TransformPoint(Vec3(xz.x, b.surfaceY, xz.y));
    const Vec2 toEdge(b.half.x - Abs(local.x), b.half.y - Abs(local.z));
    const f32 fade = b.edgeFade > 0.0f ? Clamp(Min(toEdge.x, toEdge.y) / b.edgeFade, 0.0f, 1.0f) : 1.0f;
    Vec3 d;
    m_waves[b.waves]->Sample(xz, d, velocity);
    const f32 k = b.amplitude * fade;
    if (velocity)
        *velocity *= k;
    return d * k;
}

bool WaterSystem::Sample(Vec3 position, WaterSample& out) const
{
    for (const Body& b : m_bodies) {
        const Vec3 local = b.toLocal.TransformPoint(position);
        if (Abs(local.x) > b.half.x || Abs(local.z) > b.half.y)
            continue;
        const Vec2 p(position.x, position.z);
        // The waves move points sideways: find the resting point that is now above `position`.
        auto surfaceAt = [&](Vec2 target, Vec3* velocity) {
            Vec2 rest = target;
            Vec3 d;
            for (u32 i = 0; i < 4; ++i) {
                d = Displacement(b, rest, nullptr);
                rest = target - Vec2(d.x, d.z);
            }
            d = Displacement(b, rest, velocity);
            return b.surfaceY + d.y;
        };
        out.height = surfaceAt(p, &out.velocity);
        const f32 e = 0.35f;
        const f32 hx = surfaceAt(p + Vec2(e, 0.0f), nullptr) - surfaceAt(p - Vec2(e, 0.0f), nullptr);
        const f32 hz = surfaceAt(p + Vec2(0.0f, e), nullptr) - surfaceAt(p - Vec2(0.0f, e), nullptr);
        out.normal = Normalize(Vec3(-hx, 2.0f * e, -hz));
        out.depth = out.height - position.y;
        out.water = b.entity;
        return true;
    }
    return false;
}

WaterSystem::Floater& WaterSystem::FloaterOf(ecs::World&, PhysicsSystem& physics, ecs::Entity e)
{
    for (Floater& f : m_floaters)
        if (f.entity == e)
            return f;
    Floater& f = m_floaters.AddDefaulted();
    f.entity = e;
    physics::PhysicsWorld& pw = *physics.World();
    const physics::BodyId id = physics.BodyOf(e);
    const Vec3 origin = pw.GetPosition(id);
    const Quat invRot = Conjugate(pw.GetRotation(id));
    f32 cellSum = 0.0f;
    // Eight sample points per collider (a 2x2x2 split of its box), each holding an eighth of its volume.
    auto addBox = [&](Vec3 center, Quat rotation, Vec3 half, f32 volume) {
        for (u32 i = 0; i < 8; ++i) {
            const Vec3 corner((i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f);
            const Vec3 p = center + (rotation) * (corner * half);
            f.points.PushBack(Vec4(p.x, p.y, p.z, volume / 8.0f));
        }
        cellSum += half.y * 0.5f;
    };
    u32 boxes = 0;
    for (const physics::ShapeDesc& sh : pw.GetShapes(id)) {
        if (sh.isSensor)
            continue;
        switch (sh.type) {
        case physics::ShapeType::Box:
            addBox(sh.position, sh.rotation, sh.halfExtents, 8.0f * sh.halfExtents.x * sh.halfExtents.y * sh.halfExtents.z);
            break;
        case physics::ShapeType::Sphere:
            addBox(sh.position, sh.rotation, Vec3(sh.radius), 4.0f / 3.0f * Pi * sh.radius * sh.radius * sh.radius);
            break;
        case physics::ShapeType::Capsule:
            addBox(sh.position, sh.rotation, Vec3(sh.radius, sh.halfHeight + sh.radius, sh.radius),
                   Pi * sh.radius * sh.radius * (2.0f * sh.halfHeight + 4.0f / 3.0f * sh.radius));
            break;
        case physics::ShapeType::ConvexHull: {
            Array<Vec3> vertices;
            Array<u32> edges;
            pw.GetHullEdges(sh.hull, vertices, edges);
            if (vertices.Empty())
                break;
            Vec3 lo = vertices[0], hi = vertices[0];
            for (Vec3 v : vertices) {
                lo = Min(lo, v);
                hi = Max(hi, v);
            }
            const Vec3 half = (hi - lo) * 0.5f;
            addBox(sh.position + (sh.rotation) * ((hi + lo) * 0.5f), sh.rotation, half, 8.0f * half.x * half.y * half.z * 0.65f);
            break;
        }
        default:
            continue;
        }
        ++boxes;
    }
    if (f.points.Empty()) { // no usable collider: the body's bounds
        const AABB bounds = pw.GetBounds(id);
        const Vec3 half = bounds.Extents();
        addBox((invRot) * (bounds.Center() - origin), Quat(), half, 8.0f * half.x * half.y * half.z);
        boxes = 1;
    }
    f.halfHeight = Max(cellSum / f32(Max(boxes, 1u)), 0.02f);
    return f;
}

void WaterSystem::TickBuoyancy(ecs::World& world, PhysicsSystem& physics, f32 step)
{
    physics::PhysicsWorld* pw = physics.World();
    if (!pw || m_bodies.Empty() || step <= 0.0f)
        return;
    world.Query<const Buoyancy, const RigidBody>().Each([&](ecs::Entity e, const Buoyancy& buoy, const RigidBody&) {
        const physics::BodyId id = physics.BodyOf(e);
        if (!pw->IsValid(id) || pw->GetMotionType(id) != physics::MotionType::Dynamic)
            return;
        Floater& f = FloaterOf(world, physics, e);
        f32 volume = 0.0f;
        for (const Vec4& p : f.points)
            volume += p.w;
        if (volume <= 0.0f)
            return;
        const f32 volumeScale = buoy.volume > 0.0f ? buoy.volume / volume : 1.0f;
        volume *= volumeScale;
        const f32 mass = pw->GetMass(id);
        // The body floats as if it had `density` (without changing its mass for collisions).
        const f32 densityScale = buoy.density > 0.0f ? mass / (buoy.density * volume) : 1.0f;
        const Vec3 origin = pw->GetPosition(id);
        const Quat rot = pw->GetRotation(id);
        const Vec3 com = pw->GetCenterOfMass(id);
        const Vec3 v = pw->GetLinearVelocity(id);
        const Vec3 w = pw->GetAngularVelocity(id);
        const f32 pointMass = mass / f32(f.points.Size());
        const f32 maxRate = 0.9f / step; // never reverse a velocity within one step
        f32 submerged = 0.0f;
        for (const Vec4& lp : f.points) {
            const Vec3 p = origin + (rot) * (Vec3(lp.x, lp.y, lp.z));
            WaterSample s;
            if (!Sample(p, s))
                continue;
            const f32 fraction = Clamp((s.height - (p.y - f.halfHeight)) / (2.0f * f.halfHeight), 0.0f, 1.0f);
            if (fraction <= 0.0f)
                continue;
            submerged += fraction / f32(f.points.Size());
            const Vec3 lift(0.0f, WaterDensity * Gravity * lp.w * volumeScale * fraction * densityScale, 0.0f);
            const Vec3 rel = v + Cross(w, p - com) - s.velocity;
            const f32 rate = Min(fraction * buoy.waterDrag * (1.2f + 0.5f * Length(rel)), maxRate);
            // Heave / pitch / roll calm down as the body radiates waves: damp the vertical motion at ~60 % of critical
            // for the spring the buoyancy makes (light bodies would bounce out of the water otherwise).
            const f32 stiffness = WaterDensity * Gravity * lp.w * volumeScale * densityScale / (2.0f * f.halfHeight); // N/m
            const f32 heaveRate = Min(1.2f * Sqrt(stiffness / Max(pointMass, 1e-3f)) * Min(fraction * 4.0f, 1.0f) * buoy.waterDrag, maxRate);
            Vec3 drag = rel * (pointMass * rate);
            drag.y += rel.y * pointMass * Max(heaveRate - rate, 0.0f);
            pw->AddForceAtPoint(id, lift - drag, p);
        }
        if (submerged > 0.0f && buoy.angularDrag > 0.0f) {
            const f32 radius2 = std::pow(volume, 2.0f / 3.0f) / 6.0f; // inertia ~ m r^2 of a box of this volume
            const f32 rate = Min(submerged * buoy.angularDrag * 2.0f, maxRate);
            pw->AddTorque(id, -w * (mass * radius2 * rate));
        }
    });
}

void WaterSystem::TickSplashes(ecs::World& world, PhysicsSystem& physics, ParticleSystem& particles, f32 dt)
{
    physics::PhysicsWorld* pw = physics.World();
    if (!pw || m_bodies.Empty() || dt <= 0.0f)
        return;
    auto splash = [&](Vec3 at, f32 strength, f32 size, ecs::Entity e) {
        strength = Clamp(strength, 0.0f, 1.0f);
        m_splashes.PushBack(Splash{at, strength, e});
        const f32 scale = Clamp(size, 0.4f, 3.0f);
        const ParticleEmitter drops = DropletLook(Clamp(size, 0.6f, 2.0f));
        const u32 count = u32(16.0f + 110.0f * strength * scale);
        for (u32 i = 0; i < count; ++i) {
            const f32 a = Random01(m_rng) * 2.0f * Pi, r = Random01(m_rng);
            const Vec3 out(std::cos(a), 0.0f, std::sin(a));
            const Vec3 from = at + out * (size * 0.5f * Sqrt(r));
            const f32 up = (2.0f + 7.5f * strength * Random01(m_rng)) * Sqrt(scale);
            particles.SpawnFree(DropletKey, drops, from, out * (0.6f + 2.8f * strength * Random01(m_rng)) + Vec3(0.0f, up, 0.0f));
        }
        const ParticleEmitter mist = MistLook(scale);
        const u32 puffs = u32(4.0f + 14.0f * strength);
        for (u32 i = 0; i < puffs; ++i) {
            const f32 a = Random01(m_rng) * 2.0f * Pi;
            const Vec3 out(std::cos(a), 0.0f, std::sin(a));
            particles.SpawnFree(MistKey, mist, at + out * (size * 0.4f) + Vec3(0.0f, 0.1f, 0.0f),
                                out * (0.5f + 1.5f * strength) + Vec3(0.0f, 1.0f + 2.5f * strength * Random01(m_rng), 0.0f));
        }
    };

    // Bodies hitting the water.
    Array<ecs::Entity> inWater;
    world.Query<const RigidBody>().Each([&](ecs::Entity e, const RigidBody&) {
        const physics::BodyId id = physics.BodyOf(e);
        if (!pw->IsValid(id) || pw->GetMotionType(id) != physics::MotionType::Dynamic)
            return;
        const AABB bounds = pw->GetBounds(id);
        WaterSample s;
        if (!Sample(bounds.Center(), s) || bounds.min.y > s.height)
            return;
        inWater.PushBack(e);
        bool was = false;
        for (ecs::Entity o : m_inWater)
            was |= o == e;
        const Vec3 v = pw->GetLinearVelocity(id);
        const f32 size = Length(bounds.Extents()) * 1.2f;
        if (!was && v.y < -2.0f)
            splash(Vec3(bounds.Center().x, s.height, bounds.Center().z), (-v.y - 2.0f) / 10.0f * Clamp(size, 0.3f, 1.5f), size, e);
    });
    m_inWater = move(inWater);

    // Spray of fast floating bodies at the waterline (bow waves).
    const ParticleEmitter spray = DropletLook(0.8f);
    world.Query<const Buoyancy, const RigidBody>().Each([&](ecs::Entity e, const Buoyancy& b, const RigidBody&) {
        if (!b.splashes)
            return;
        const physics::BodyId id = physics.BodyOf(e);
        if (!pw->IsValid(id))
            return;
        const AABB bounds = pw->GetBounds(id);
        WaterSample s;
        if (!Sample(bounds.Center(), s) || bounds.min.y > s.height || bounds.max.y < s.height)
            return;
        Vec3 v = pw->GetLinearVelocity(id) - s.velocity;
        v.y = 0.0f;
        const f32 speed = Length(v);
        if (speed < 1.5f)
            return;
        for (Floater& f : m_floaters) {
            if (f.entity != e)
                continue;
            const Vec3 dir = v / speed;
            const Vec3 ext = bounds.Extents();
            f.sprayCarry += dt * (speed - 1.5f) * 25.0f * Clamp(Length(Vec3(ext.x, 0.0f, ext.z)), 0.3f, 3.0f);
            for (; f.sprayCarry >= 1.0f; f.sprayCarry -= 1.0f) {
                const Vec3 side = Vec3(-dir.z, 0.0f, dir.x) * ((Random01(m_rng) - 0.5f) * 2.0f);
                const Vec3 bow = bounds.Center() + dir * (Abs(dir.x) * ext.x + Abs(dir.z) * ext.z) + side * Max(ext.x, ext.z) * 0.6f;
                particles.SpawnFree(SprayKey, spray, Vec3(bow.x, s.height + 0.05f, bow.z),
                                    dir * (speed * 0.4f) + side * (speed * 0.5f) + Vec3(0.0f, 1.0f + speed * 0.35f * Random01(m_rng), 0.0f));
            }
        }
    });
}

} // namespace zx
