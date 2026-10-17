#include "Engine/Fire.h"
#include "Engine/Wind.h"
#include "Engine/AudioSystem.h"
#include "Engine/Particles.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/SceneRendering.h"
#include "Scene/Hierarchy.h"

namespace zx {

ZX_REFLECT_ENUM_BEGIN(BurntResult)
    ZX_ENUM_VALUE(Charred)
    ZX_ENUM_VALUE(Crumble)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(Flammable)
    ZX_FIELD(startBurning, Meta::Category("Ignition"), Meta::Tooltip("On fire when Play starts"))
    ZX_FIELD(ignitionTemperature, Meta::Range(30, 2000), Meta::Units("C"), Meta::Tooltip("Catches fire at this temperature (wood ~300, dry grass ~200, paper ~230)"))
    ZX_FIELD(resistance, Meta::Range(0.05, 20), Meta::Tooltip("Thermal mass: higher heats up slower"))
    ZX_FIELD(waterExtinguishes, Meta::Tooltip("Goes out under a Water surface"))
    ZX_FIELD(burnTime, Meta::Category("Burning"), Meta::Range(0.5, 600), Meta::Units("s"), Meta::Tooltip("Seconds a spot burns before it goes out (fuel)"))
    ZX_FIELD(spreadSpeed, Meta::Range(0.005, 5), Meta::Units("m/s"), Meta::Tooltip("How fast the flames creep over the surface"))
    ZX_FIELD(heat, Meta::Range(0, 10), Meta::Tooltip("Heat given off: ignites neighbours, sizes the flames"))
    ZX_FIELD(heatRange, Meta::Range(0.05, 10), Meta::Units("m"), Meta::Tooltip("How far from the flames it gets hot (more above them)"))
    ZX_FIELD(whenBurnt, Meta::Tooltip("Charred stays black and smoulders; Crumble shrinks into ash and disappears"))
    ZX_FIELD(breakJoints, Meta::Tooltip("Its joints break once it has burnt halfway through (bridges, doors)"))
    ZX_FIELD(flameHeight, Meta::Category("Look"), Meta::Range(0.05, 10), Meta::Units("m"))
    ZX_FIELD(flameDensity, Meta::Range(0, 10), Meta::Tooltip("Flame particles per square meter (multiplier)"))
    ZX_FIELD(flameColor, Meta::Color())
    ZX_FIELD(smoke, Meta::Range(0, 10), Meta::Tooltip("Smoke per square meter in flames (multiplier)"))
    ZX_FIELD(smokeColor, Meta::Color())
    ZX_FIELD(charring, Meta::Range(0, 1), Meta::Tooltip("How black the burnt part turns"))
    ZX_FIELD(light, Meta::Range(0, 20), Meta::Tooltip("Brightness of the fire's light (multiplier)"))
    ZX_FIELD(sound, Meta::Tooltip("Looping crackle (Content-relative WAV / Ogg); its volume follows the fire"))
ZX_REFLECT_STRUCT_END()

namespace {

constexpr f32 Ambient = 20.0f;      // degrees C
constexpr f32 FlameTemperature = 900.0f;
constexpr f32 HeatGain = 420.0f;    // degrees C per second per unit of heat flux
constexpr f32 Cooling = 0.12f;      // per second, towards the ambient temperature
constexpr u32 SampleCount = 64;     // surface points per object (flame coverage, heat sources)

u64 FreeKey(ecs::Entity e, u32 kind) { return (u64(0xF17Eu) << 48) ^ (u64(kind) << 40) ^ e.Pack(); }

Mat4 WorldOf(ecs::World& world, ecs::Entity e)
{
    const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(e);
    return wt ? wt->matrix : scene::ComputeWorldMatrix(world, e);
}

struct Source
{
    Vec3 position;
    f32 power;
    f32 range;
};

ParticleEmitter FlameLook(const Flammable& f)
{
    ParticleEmitter d;
    d.look = ParticleLook::Flame;
    d.blend = ParticleBlend::Additive;
    d.emissive = true;
    d.lifetime = Clamp(0.25f + f.flameHeight * 0.6f, 0.25f, 2.0f);
    d.lifetimeVariation = 0.35f;
    d.startSize = f.flameHeight * 0.85f;
    d.endSize = f.flameHeight * 0.2f;
    d.sizeVariation = 0.4f;
    d.startColor = Lerp(f.flameColor, Vec3(1.0f, 0.7f, 0.3f), 0.25f);
    d.endColor = f.flameColor * Vec3(0.7f, 0.3f, 0.2f);
    d.startAlpha = 0.8f;
    d.endAlpha = 0.0f;
    d.fadeIn = 0.12f;
    d.intensity = 4.0f;
    // Baked fluid-simulation flames when the project has the Starter Content (else the procedural tongues).
    d.flipbook = "StarterContent/Flipbooks/T_Flipbook_Fire.png";
    d.flipbookFps = 24.0f;
    d.acceleration = Vec3(0.0f, 2.2f * Clamp(f.flameHeight / 0.6f, 0.3f, 4.0f), 0.0f);
    d.drag = 1.6f;
    d.turbulence = 1.8f;
    d.spin = 1.2f;
    d.speedVariation = 0.3f;
    d.maxParticles = 900;
    return d;
}

ParticleEmitter SmokeLook(const Flammable& f, bool smouldering)
{
    ParticleEmitter d;
    d.look = ParticleLook::Soft;
    d.blend = ParticleBlend::Alpha;
    d.emissive = false;
    d.lifetime = smouldering ? 4.0f : 5.5f;
    d.lifetimeVariation = 0.3f;
    d.startSize = Max(f.flameHeight * 0.6f, 0.2f);
    d.endSize = Max(f.flameHeight * 3.5f, 1.2f);
    d.sizeVariation = 0.35f;
    const Vec3 pale = f.smokeColor * 2.5f + Vec3(0.18f);
    d.startColor = smouldering ? pale : f.smokeColor;
    d.endColor = smouldering ? pale * 1.2f : Lerp(f.smokeColor, pale, 0.6f);
    d.startAlpha = smouldering ? 0.18f : 0.55f;
    d.endAlpha = 0.0f;
    d.fadeIn = 0.12f;
    d.acceleration = Vec3(0.0f, smouldering ? 0.35f : 0.7f, 0.0f);
    d.drag = 0.45f;
    d.turbulence = 0.7f;
    d.spin = 0.35f;
    d.speedVariation = 0.3f;
    d.maxParticles = 700;
    return d;
}

ParticleEmitter HeatLook(const Flammable& f)
{
    ParticleEmitter d;
    d.look = ParticleLook::Heat;
    d.emissive = false;
    d.lifetime = 1.2f;
    d.startSize = Max(f.flameHeight * 1.4f, 0.4f);
    d.endSize = Max(f.flameHeight * 2.4f, 0.8f);
    d.startAlpha = 1.0f;
    d.endAlpha = 0.0f;
    d.fadeIn = 0.25f;
    d.acceleration = Vec3(0.0f, 0.6f, 0.0f);
    d.drag = 0.8f;
    d.turbulence = 0.4f;
    d.maxParticles = 120;
    return d;
}

ParticleEmitter EmberLook()
{
    ParticleEmitter d;
    d.look = ParticleLook::Spark;
    d.blend = ParticleBlend::Additive;
    d.emissive = true;
    d.lifetime = 1.6f;
    d.lifetimeVariation = 0.5f;
    d.startSize = 0.035f;
    d.endSize = 0.01f;
    d.startColor = Vec3(1.0f, 0.55f, 0.18f);
    d.endColor = Vec3(0.9f, 0.18f, 0.03f);
    d.fadeIn = 0.05f;
    d.intensity = 2.5f;
    d.acceleration = Vec3(0.0f, 0.9f, 0.0f);
    d.drag = 0.8f;
    d.turbulence = 5.0f;
    d.maxParticles = 300;
    return d;
}

/// Is the point under the surface of a Water component?
bool UnderWater(Span<const Mat4> waterInverse, Span<const Vec2> waterSize, Vec3 p)
{
    for (usize i = 0; i < waterInverse.Size(); ++i) {
        const Vec3 l = waterInverse[i].TransformPointAffine(p);
        if (l.y < 0.0f && Abs(l.x) <= waterSize[i].x * 0.5f && Abs(l.z) <= waterSize[i].y * 0.5f)
            return true;
    }
    return false;
}

} // namespace

void FireSystem::Clear()
{
    m_burns.Clear();
    m_lights.Clear();
    m_soundChanges.Clear();
    m_toDestroy.Clear();
    m_time = 0.0f;
    m_burningCount = 0;
    m_started = false;
}

FireSystem::Burn& FireSystem::StateOf(ecs::World& world, ecs::Entity e)
{
    Burn* b = m_burns.Find(e.Pack());
    if (!b) {
        Burn fresh;
        m_burns.InsertOrAssign(e.Pack(), move(fresh));
        b = m_burns.Find(e.Pack());
    }
    if (!b->initialized)
        Setup(world, e, *b);
    return *b;
}

void FireSystem::Setup(ecs::World& world, ecs::Entity e, Burn& b)
{
    b.initialized = true;
    b.rng.Seed(e.Pack() * 0x9E3779B97F4A7C15ull + 7);
    // Shape: the collider if there is one (what the object physically is), else the mesh bounds.
    if (const SphereCollider* s = world.Get<const SphereCollider>(e)) {
        b.shape = Shape::Sphere;
        b.center = s->center;
        b.half = Vec3(s->radius);
    } else if (const BoxCollider* bc = world.Get<const BoxCollider>(e)) {
        b.center = bc->center;
        b.half = bc->size * 0.5f;
    } else if (const CapsuleCollider* c = world.Get<const CapsuleCollider>(e)) {
        b.center = c->center;
        b.half = Vec3(c->radius, Max(c->height * 0.5f, c->radius), c->radius);
    } else if (const CylinderCollider* cy = world.Get<const CylinderCollider>(e)) {
        b.center = cy->center;
        b.half = Vec3(cy->radius, cy->height * 0.5f, cy->radius);
    } else {
        const MeshRenderer* mr = world.Get<const MeshRenderer>(e);
        AABB box;
        if (mr && meshBounds && meshBounds(mr->mesh, box) && box.IsValid()) {
            b.center = box.Center();
            b.half = Max(box.Extents(), Vec3(0.01f));
        }
    }
    const Mat4 m = WorldOf(world, e);
    const Vec3 scale = m.GetScale();
    if (const Transform* t = world.Get<const Transform>(e))
        b.scale = t->scale;
    b.worldHalf = b.half * scale;
    if (b.shape == Shape::Sphere) {
        const f32 r = b.half.x * Max(Max(scale.x, scale.y), scale.z);
        b.worldHalf = Vec3(r);
        b.area = 4.0f * Pi * r * r;
    } else {
        const Vec3 h = b.worldHalf;
        b.area = 8.0f * (h.x * h.y + h.y * h.z + h.z * h.x);
    }
    b.samples.Clear();
    for (u32 i = 0; i < SampleCount; ++i)
        b.samples.PushBack(RandomSurfacePoint(b));
}

Vec3 FireSystem::RandomSurfacePoint(Burn& b, Vec3* normal)
{
    if (b.shape == Shape::Sphere) {
        const Vec3 dir = b.rng.OnUnitSphere();
        if (normal)
            *normal = dir;
        return b.center + dir * b.half.x;
    }
    // A face picked by its (world) area, then a point on it.
    const Vec3 h = b.worldHalf;
    const f32 ax = h.y * h.z, ay = h.x * h.z, az = h.x * h.y;
    const f32 pick = b.rng.NextFloat() * Max(ax + ay + az, 1e-6f);
    const u32 axis = pick < ax ? 0u : pick < ax + ay ? 1u : 2u;
    const f32 side = b.rng.NextFloat() < 0.5f ? -1.0f : 1.0f;
    Vec3 local(b.rng.Range(-1.0f, 1.0f), b.rng.Range(-1.0f, 1.0f), b.rng.Range(-1.0f, 1.0f));
    Vec3 n;
    (&local.x)[axis] = side;
    (&n.x)[axis] = side;
    if (normal)
        *normal = n;
    return b.center + local * b.half;
}

Vec3 FireSystem::ClosestPoint(const Burn& b, const Mat4& world, const Mat4& inverse, Vec3 point)
{
    const Vec3 local = inverse.TransformPointAffine(point) - b.center;
    if (b.shape == Shape::Sphere) {
        const f32 len = Length(local);
        return len <= b.half.x ? point : world.TransformPointAffine(b.center + local * (b.half.x / len));
    }
    return world.TransformPointAffine(b.center + Clamp(local, -b.half, b.half));
}

void FireSystem::StartBurning(ecs::World& world, ecs::Entity e, Flammable& f, Burn& b, Vec3 localPoint)
{
    // Charred before (put out, now relit): it keeps burning from where it started, so the char doesn't jump.
    if (f.charRadius <= 0.05f)
        f.burnOrigin = localPoint;
    f.state = FireState::Burning;
    f.temperature = FlameTemperature;
    f.glow = 1.0f;
    f.embers = 1.0f;
    b.time = 0.0f;
    b.outer = b.inner = 0.0f;
    b.burntFor = 0.0f;
    b.jointsBroken = false;
    const Mat4 m = WorldOf(world, e);
    const Vec3 origin = m.TransformPointAffine(f.burnOrigin);
    b.extent = 0.0f;
    for (const Vec3& s : b.samples)
        b.extent = Max(b.extent, Distance(m.TransformPointAffine(s), origin));
    if (b.shape == Shape::Box)
        for (u32 c = 0; c < 8; ++c) {
            const Vec3 corner = b.center + b.half * Vec3(c & 1 ? 1.0f : -1.0f, c & 2 ? 1.0f : -1.0f, c & 4 ? 1.0f : -1.0f);
            b.extent = Max(b.extent, Distance(m.TransformPointAffine(corner), origin));
        }
    if (!f.sound.Empty() && !world.Has<AudioSource>(e) && !b.addedSound) {
        m_soundChanges.PushBack(SoundChange{e, f.sound});
        b.addedSound = true;
    }
}

bool FireSystem::Ignite(ecs::World& world, ecs::Entity e, const Vec3* worldPoint)
{
    Flammable* f = world.IsAlive(e) ? world.Get<Flammable>(e) : nullptr;
    if (!f || f->state == FireState::Burnt)
        return false;
    if (f->state == FireState::Burning)
        return true;
    Burn& b = StateOf(world, e);
    Vec3 local = b.center - Vec3(0.0f, b.half.y, 0.0f); // the bottom
    if (worldPoint) {
        const Mat4 m = WorldOf(world, e);
        const Mat4 inverse = InverseAffine(m);
        local = inverse.TransformPointAffine(ClosestPoint(b, m, inverse, *worldPoint));
    }
    StartBurning(world, e, *f, b, local);
    return true;
}

u32 FireSystem::IgniteAt(ecs::World& world, Vec3 point, f32 radius)
{
    Array<ecs::Entity> hits;
    world.Query<const Flammable>().Each([&](ecs::Entity e, const Flammable&) { hits.PushBack(e); });
    u32 count = 0;
    for (ecs::Entity e : hits) {
        Burn& b = StateOf(world, e);
        const Mat4 m = WorldOf(world, e);
        const Vec3 closest = ClosestPoint(b, m, InverseAffine(m), point);
        if (Distance(closest, point) <= radius && Ignite(world, e, &closest))
            ++count;
    }
    return count;
}

void FireSystem::Steam(Vec3 position, f32 size)
{
    if (!m_particles)
        return;
    ParticleEmitter d;
    d.look = ParticleLook::Soft;
    d.blend = ParticleBlend::Alpha;
    d.emissive = false;
    d.lifetime = 2.5f;
    d.startSize = 0.3f * size;
    d.endSize = 1.6f * size;
    d.startColor = d.endColor = Vec3(0.85f, 0.87f, 0.9f);
    d.startAlpha = 0.5f;
    d.acceleration = Vec3(0.0f, 0.8f, 0.0f);
    d.drag = 0.8f;
    d.turbulence = 0.8f;
    d.maxParticles = 400;
    Random rng(u64(position.x * 1000.0f) ^ u64(position.z * 7919.0f));
    for (u32 i = 0; i < 24; ++i)
        m_particles->SpawnFree(0x57EA4ull, d, position + rng.InsideUnitSphere() * (0.4f * size),
                               Vec3(0.0f, 1.2f, 0.0f) + rng.InsideUnitSphere() * 0.6f);
}

void FireSystem::Extinguish(ecs::World& world, ecs::Entity e)
{
    Flammable* f = world.IsAlive(e) ? world.Get<Flammable>(e) : nullptr;
    if (!f || f->state != FireState::Burning)
        return;
    f->state = FireState::None;
    f->temperature = Ambient;
    f->intensity = 0.0f;
    f->glow = 0.0f;
    f->embers = 0.0f;
    if (Burn* b = m_burns.Find(e.Pack())) {
        if (b->addedSound) {
            m_soundChanges.PushBack(SoundChange{e, String()});
            b->addedSound = false;
        }
        Steam(WorldOf(world, e).TransformPointAffine(b->center), Clamp(Length(b->worldHalf), 0.3f, 3.0f));
    }
}

u32 FireSystem::ExtinguishAt(ecs::World& world, Vec3 point, f32 radius)
{
    Array<ecs::Entity> burning;
    world.Query<const Flammable>().Each([&](ecs::Entity e, const Flammable& f) {
        if (f.state == FireState::Burning)
            burning.PushBack(e);
    });
    u32 count = 0;
    for (ecs::Entity e : burning) {
        Burn& b = StateOf(world, e);
        const Mat4 m = WorldOf(world, e);
        if (Distance(ClosestPoint(b, m, InverseAffine(m), point), point) <= radius) {
            Extinguish(world, e);
            ++count;
        }
    }
    return count;
}

void FireSystem::Tick(ecs::World& world, f32 dt, ParticleSystem& particles)
{
    m_particles = &particles;
    dt = Clamp(dt, 0.0f, 0.1f);
    m_time += dt;
    m_lights.Clear();
    m_smoke.Clear();
    m_burningCount = 0;

    struct Item
    {
        ecs::Entity entity;
        Mat4 world, inverse;
        Vec3 center;  // world
        f32 radius;   // world bounding sphere
    };
    Array<Item> items;
    world.Query<const scene::WorldTransform, const Flammable>().Each([&](ecs::Entity e, const scene::WorldTransform& wt, const Flammable&) {
        items.PushBack(Item{e, wt.matrix, InverseAffine(wt.matrix), Vec3(), 0.0f});
    });
    for (Item& it : items) {
        Burn& b = StateOf(world, it.entity);
        it.center = it.world.TransformPointAffine(b.center);
        it.radius = Length(b.worldHalf);
    }
    if (!m_started) {
        m_started = true;
        for (const Item& it : items)
            if (const Flammable* f = world.Get<const Flammable>(it.entity); f && f->startBurning)
                Ignite(world, it.entity);
    }

    Array<Mat4> waterInverse;
    Array<Vec2> waterSize;
    world.Query<const scene::WorldTransform, const Water>().Each([&](const scene::WorldTransform& wt, const Water& w) {
        waterInverse.PushBack(InverseAffine(wt.matrix));
        waterSize.PushBack(w.size);
    });

    // Heat sources: the burning spots of every burning object, and fire Particle Emitters (campfires, torches).
    Array<Source> sources;
    for (const Item& it : items) {
        const Flammable* f = world.Get<const Flammable>(it.entity);
        const Burn* b = m_burns.Find(it.entity.Pack());
        if (!f || !b || f->state != FireState::Burning || f->heat <= 0.0f)
            continue;
        const Vec3 origin = it.world.TransformPointAffine(f->burnOrigin);
        const f32 power = f->heat * 3.0f / f32(SampleCount);
        for (const Vec3& s : b->samples) {
            const Vec3 p = it.world.TransformPointAffine(s);
            const f32 d = Distance(p, origin);
            if (d >= b->inner && d <= b->outer)
                sources.PushBack(Source{p + Vec3(0.0f, f->flameHeight * 0.3f, 0.0f), power, f->heatRange + f->flameHeight * 0.5f});
        }
    }
    world.Query<const scene::WorldTransform, const ParticleEmitter>().Each([&](const scene::WorldTransform& wt, const ParticleEmitter& pe) {
        if (!pe.playing || pe.look != ParticleLook::Flame || !pe.emissive || pe.rate <= 0.0f)
            return;
        const Vec3 p = wt.GetPosition() + Vec3(0.0f, pe.speed * pe.lifetime * 0.25f, 0.0f);
        sources.PushBack(Source{p, Clamp(pe.rate / 60.0f, 0.3f, 2.0f) * 0.5f, Max(0.5f, pe.radius + pe.startSize)});
    });

    for (const Item& it : items) {
        Flammable* f = world.Get<Flammable>(it.entity);
        Burn& b = *m_burns.Find(it.entity.Pack());
        if (!f)
            continue;
        const bool wet = f->waterExtinguishes && UnderWater(waterInverse.View(), waterSize.View(), it.center);

        if (f->state == FireState::None) {
            // Heating up: heat flux from every source near it, more from flames below (heat rises).
            f32 flux = 0.0f, best = 0.0f;
            Vec3 hottest = it.center;
            for (const Source& s : sources) {
                const f32 reach = it.radius + s.range;
                if (DistanceSq(s.position, it.center) > reach * reach)
                    continue;
                const Vec3 closest = ClosestPoint(b, it.world, it.inverse, s.position);
                const f32 d = Distance(closest, s.position);
                if (d >= s.range)
                    continue;
                const f32 x = 1.0f - d / s.range;
                const f32 above = d > 1e-3f ? Max((closest.y - s.position.y) / d, 0.0f) : 0.0f;
                const f32 q = s.power * x * x * (1.0f + 1.5f * above);
                flux += q;
                if (q > best) {
                    best = q;
                    hottest = closest;
                }
            }
            f->temperature += (flux * HeatGain / Max(f->resistance, 0.05f) - (f->temperature - Ambient) * Cooling) * dt;
            if (wet)
                f->temperature = Ambient;
            if (f->temperature >= f->ignitionTemperature && !wet)
                StartBurning(world, it.entity, *f, b, it.inverse.TransformPointAffine(hottest));
            continue;
        }

        const Vec3 up(0.0f, 1.0f, 0.0f);
        if (f->state == FireState::Burning) {
            if (wet) {
                Extinguish(world, it.entity);
                continue;
            }
            ++m_burningCount;
            b.time += dt;
            const f32 speed = Max(f->spreadSpeed, 0.001f);
            b.outer = Min(0.08f + b.time * speed, b.extent + 0.05f);
            b.inner = (b.time - Max(f->burnTime, 0.1f)) * speed;
            f->charRadius = Max(f->charRadius, b.outer);
            f->temperature = FlameTemperature;
            // Coverage: the surface points in flames (between the burnt-out inner front and the spreading outer one).
            const Vec3 origin = it.world.TransformPointAffine(f->burnOrigin);
            u32 active = 0, burntOut = 0;
            Vec3 centroid;
            for (const Vec3& s : b.samples) {
                const Vec3 p = it.world.TransformPointAffine(s);
                const f32 d = Distance(p, origin);
                if (d < b.inner)
                    ++burntOut;
                else if (d <= b.outer) {
                    ++active;
                    centroid += p;
                }
            }
            const f32 coverage = f32(active) / f32(SampleCount);
            f->intensity += (coverage - f->intensity) * Min(dt * 3.0f, 1.0f);
            f->glow = 1.0f;
            f->embers = 1.0f;
            if (f->breakJoints && !b.jointsBroken && burntOut * 2 >= SampleCount) {
                b.jointsBroken = true;
                if (physics)
                    physics->BreakJoints(it.entity);
            }
            if (b.inner >= b.extent) {
                f->state = FireState::Burnt;
                f->intensity = 0.0f;
                f->glow = 0.0f;
                b.burntFor = 0.0f;
                if (f->breakJoints && !b.jointsBroken && physics) {
                    b.jointsBroken = true;
                    physics->BreakJoints(it.entity);
                }
                if (b.addedSound) {
                    m_soundChanges.PushBack(SoundChange{it.entity, String()});
                    b.addedSound = false;
                }
                continue;
            }

            // Particles over the burning part, in proportion to its area.
            const f32 activeArea = Max(f->intensity, coverage) * b.area;
            const f32 areaTerm = Pow(Min(activeArea, 8.0f), 0.8f);
            auto burningPoint = [&](Vec3& normal) {
                // A random surface point inside the flame band (a few tries, else any active sample).
                for (u32 attempt = 0; attempt < 8; ++attempt) {
                    Vec3 n;
                    const Vec3 local = RandomSurfacePoint(b, &n);
                    const Vec3 p = it.world.TransformPointAffine(local);
                    const f32 d = Distance(p, origin);
                    if (d >= b.inner && d <= b.outer) {
                        normal = SafeNormalize(it.world.TransformVector(n), up);
                        return p;
                    }
                }
                normal = up;
                return active ? centroid / f32(active) : origin;
            };
            if (active) {
                const ParticleEmitter flame = FlameLook(*f);
                b.emitFlame += (60.0f * f->flameDensity * areaTerm + 6.0f) * dt;
                for (; b.emitFlame >= 1.0f; b.emitFlame -= 1.0f) {
                    Vec3 n;
                    const Vec3 p = burningPoint(n);
                    const Vec3 v = n * 0.2f + up * (f->flameHeight * 1.4f) + Vec3(b.rng.Range(-0.15f, 0.15f), 0.0f, b.rng.Range(-0.15f, 0.15f));
                    particles.SpawnFree(FreeKey(it.entity, 0), flame, p + n * 0.03f, v);
                }
                const ParticleEmitter smoke = SmokeLook(*f, false);
                b.emitSmoke += 9.0f * f->smoke * areaTerm * (0.4f + 0.6f * f->intensity) * dt;
                for (; b.emitSmoke >= 1.0f; b.emitSmoke -= 1.0f) {
                    Vec3 n;
                    const Vec3 p = burningPoint(n) + up * (f->flameHeight * 0.9f);
                    particles.SpawnFree(FreeKey(it.entity, 1), smoke, p, up * 0.9f + b.rng.InsideUnitSphere() * 0.25f);
                }
                // Heat haze rising over the flames.
                const ParticleEmitter heatHaze = HeatLook(*f);
                b.emitHeat += (4.0f + 6.0f * Sqrt(activeArea)) * dt;
                for (; b.emitHeat >= 1.0f; b.emitHeat -= 1.0f) {
                    Vec3 n;
                    const Vec3 p = burningPoint(n) + up * (f->flameHeight * 1.1f);
                    particles.SpawnFree(FreeKey(it.entity, 4), heatHaze, p, up * 0.8f + b.rng.InsideUnitSphere() * 0.2f);
                }
                const ParticleEmitter ember = EmberLook();
                b.emitEmber += 3.0f * areaTerm * dt;
                for (; b.emitEmber >= 1.0f; b.emitEmber -= 1.0f) {
                    Vec3 n;
                    const Vec3 p = burningPoint(n);
                    particles.SpawnFree(FreeKey(it.entity, 2), ember, p, up * 1.6f + b.rng.InsideUnitSphere() * 1.0f);
                }
            }
            // The fire's light: over the flames, breathing.
            if (f->intensity > 0.02f && f->light > 0.0f && active) {
                const f32 phase = f32(it.entity.index) * 1.7f;
                b.flicker = 0.78f + 0.14f * Sin(m_time * 8.3f + phase) + 0.08f * Sin(m_time * 21.7f + phase * 2.0f);
                render::LightDesc l;
                l.type = render::LightType::Point;
                l.position = centroid / f32(active) + up * (f->flameHeight * 0.6f);
                l.color = SafeNormalize(Lerp(f->flameColor, Vec3(1.0f, 0.6f, 0.3f), 0.5f), Vec3(1.0f, 0.5f, 0.2f)) * 1.4f;
                l.intensity = render::LumensToCandela(3000.0f * f->light * f->intensity * Clamp(Sqrt(b.area), 0.5f, 3.0f) * b.flicker);
                l.range = Clamp(3.0f + 3.0f * Sqrt(activeArea), 3.0f, 12.0f);
                l.id = 0x40000000u | it.entity.index;
                m_lights.PushBack(l);
            }
            // Volumetric smoke: a column of drifting wisps rising over the flames (lit by the fire's light).
            if (active && f->smoke > 0.0f) {
                const f32 width = 0.6f + Sqrt(activeArea) * 0.6f, height = 2.5f + f->flameHeight * 3.0f + Sqrt(activeArea);
                const Vec3 base = centroid / f32(active) + up * (f->flameHeight * 0.6f);
                render::FogVolumeDesc v;
                v.shape = render::FogVolumeShape::Sphere;
                v.worldToVolume = Inverse(Mat4::Translation(base + up * (height * 0.45f)) * Mat4::Scale(Vec3(width, height, width)));
                v.density = 0.9f * f->smoke * f->intensity;
                v.albedo = f->smokeColor * 2.0f;
                v.edgeFade = 0.6f;
                v.noiseScale = 1.4f;
                v.noiseAmount = 0.75f;
                const Vec3 wind = WindFor(world, it.entity, base);
                v.wind = Vec3(0.1f, 1.6f, 0.05f) + wind * 0.7f; // rising, bent over by the wind
                // The column leans downwind.
                const Vec3 lean = Vec3(wind.x, 0.0f, wind.z) * 0.12f;
                v.worldToVolume = Inverse(Mat4::Translation(base + up * (height * 0.45f) + lean * height * 0.5f) * Mat4::Scale(Vec3(width, height, width)));
                m_smoke.PushBack(v);
            }
            if (b.addedSound)
                if (AudioSource* a = world.Get<AudioSource>(it.entity))
                    a->volume = Clamp(0.15f + f->intensity, 0.0f, 1.0f);
            continue;
        }

        // Burnt: embers fade, thin pale smoke, maybe crumbling to ash.
        b.burntFor += dt;
        f->embers = Exp(-b.burntFor / 5.0f);
        if (f->embers > 0.03f && f->smoke > 0.0f) {
            const ParticleEmitter smoke = SmokeLook(*f, true);
            b.emitSmoke += 3.0f * f->smoke * f->embers * Pow(Min(b.area, 6.0f), 0.7f) * dt;
            for (; b.emitSmoke >= 1.0f; b.emitSmoke -= 1.0f) {
                const Vec3 p = it.world.TransformPointAffine(b.samples[b.rng.NextU32() % SampleCount]);
                particles.SpawnFree(FreeKey(it.entity, 1), smoke, p, up * 0.5f + b.rng.InsideUnitSphere() * 0.15f);
            }
        }
        if (f->whenBurnt == BurntResult::Crumble) {
            const bool starting = b.crumble == 0.0f;
            b.crumble = Clamp((b.burntFor - 0.3f) / 2.0f, 0.0f, 1.0f);
            if (Transform* t = world.Get<Transform>(it.entity)) {
                // Settles into a low heap: shrinks (mostly in height) with its bottom staying on the ground.
                if (starting)
                    b.position = t->position;
                const f32 sy = 1.0f - 0.95f * b.crumble;
                t->scale = b.scale * Vec3(1.0f - 0.5f * b.crumble, sy, 1.0f - 0.5f * b.crumble);
                t->position = b.position + Vec3(0.0f, (b.center.y - b.half.y) * b.scale.y * (1.0f - sy), 0.0f);
            }
            if (b.crumble > 0.0f && b.crumble < 1.0f) {
                // Ash falling apart.
                ParticleEmitter ash = SmokeLook(*f, true);
                ash.startColor = ash.endColor = Vec3(0.25f, 0.24f, 0.23f);
                ash.acceleration = Vec3(0.0f, -0.4f, 0.0f);
                ash.startAlpha = 0.5f;
                ash.lifetime = 1.8f;
                ash.endSize = 0.8f;
                if (b.rng.NextFloat() < dt * 25.0f) {
                    const Vec3 p = it.world.TransformPointAffine(b.samples[b.rng.NextU32() % SampleCount]);
                    particles.SpawnFree(FreeKey(it.entity, 3), ash, p, b.rng.InsideUnitSphere() * 0.4f);
                }
            }
            if (b.crumble >= 1.0f)
                m_toDestroy.PushBack(it.entity);
        }
    }

    particles.SetExtraLights(m_lights.View());
    particles.SetExtraFogVolumes(m_smoke.View());

    // Structural changes last (they move components between archetypes).
    for (const SoundChange& c : m_soundChanges) {
        if (!world.IsAlive(c.entity))
            continue;
        if (c.clip.Empty()) {
            world.Remove<AudioSource>(c.entity);
        } else if (!world.Has<AudioSource>(c.entity)) {
            AudioSource a;
            a.clip = c.clip;
            a.loop = true;
            a.volume = 0.15f;
            a.minDistance = 2.0f;
            a.maxDistance = 35.0f;
            world.Add(c.entity, move(a));
        }
    }
    m_soundChanges.Clear();
    for (ecs::Entity e : m_toDestroy)
        if (world.IsAlive(e)) {
            if (physics)
                physics->BreakJoints(e);
            m_burns.Remove(e.Pack());
            scene::DestroyHierarchy(world, e);
        }
    m_toDestroy.Clear();
}

} // namespace zx
