#include "Engine/Particles.h"
#include "Core/Reflection/Reflection.h"
#include "Scene/Hierarchy.h"

#include <algorithm>

namespace zx {

ZX_REFLECT_ENUM_BEGIN(EmitterShape)
    ZX_ENUM_VALUE(Point)
    ZX_ENUM_VALUE(Sphere)
    ZX_ENUM_VALUE(Box)
    ZX_ENUM_VALUE(Cone)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_ENUM_BEGIN(ParticleLook)
    ZX_ENUM_VALUE(Soft)
    ZX_ENUM_VALUE(Flame)
    ZX_ENUM_VALUE(Spark)
    ZX_ENUM_VALUE(Heat)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_ENUM_BEGIN(ParticleCollision)
    ZX_ENUM_VALUE(None)
    ZX_ENUM_VALUE(Bounce)
    ZX_ENUM_VALUE(Die)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_ENUM_BEGIN(ParticleBlend)
    ZX_ENUM_VALUE(Additive)
    ZX_ENUM_VALUE(Alpha)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(ParticleEmitter)
    ZX_FIELD(playing, Meta::Category("Emission"), Meta::Tooltip("Emits particles (scripts can toggle it)"))
    ZX_FIELD(simulateInEditor, Meta::Tooltip("Real-time preview while editing; off = simulated only in Play"))
    ZX_FIELD(rate, Meta::Range(0, 2000), Meta::Units("/s"))
    ZX_FIELD(burst, Meta::Range(0, 5000), Meta::Tooltip("Particles emitted at once on start (and by entity.emit)"))
    ZX_FIELD(maxParticles, Meta::Range(1, 20000))
    ZX_FIELD(lifetime, Meta::Range(0.05, 60), Meta::Units("s"))
    ZX_FIELD(lifetimeVariation, Meta::Range(0, 1))
    ZX_FIELD(shape, Meta::Category("Shape"))
    ZX_FIELD(radius, Meta::Range(0, 100), Meta::Units("m"))
    ZX_FIELD(coneAngle, Meta::Angle(), Meta::Range(0, 3.14159))
    ZX_FIELD(boxSize, Meta::Units("m"))
    ZX_FIELD(speed, Meta::Category("Motion"), Meta::Range(0, 200), Meta::Units("m/s"))
    ZX_FIELD(speedVariation, Meta::Range(0, 1))
    ZX_FIELD(acceleration, Meta::Units("m/s2"), Meta::Tooltip("World space: -Y = gravity, +Y = rising heat"))
    ZX_FIELD(drag, Meta::Range(0, 20))
    ZX_FIELD(turbulence, Meta::Range(0, 50), Meta::Tooltip("Swirling noise force"))
    ZX_FIELD(worldSpace, Meta::Tooltip("Particles stay where they were emitted when the entity moves"))
    ZX_FIELD(startSize, Meta::Category("Look"), Meta::Range(0, 100), Meta::Units("m"))
    ZX_FIELD(endSize, Meta::Range(0, 100), Meta::Units("m"))
    ZX_FIELD(sizeVariation, Meta::Range(0, 1))
    ZX_FIELD(spin, Meta::Range(0, 20), Meta::Units("rad/s"))
    ZX_FIELD(startColor, Meta::Color())
    ZX_FIELD(endColor, Meta::Color())
    ZX_FIELD(startAlpha, Meta::Range(0, 1))
    ZX_FIELD(endAlpha, Meta::Range(0, 1))
    ZX_FIELD(fadeIn, Meta::Range(0, 1), Meta::Tooltip("Fraction of the life spent fading in"))
    ZX_FIELD(look, Meta::Tooltip("Soft puff, flickering flame, spark stretched along its motion, or invisible heat haze (Alpha = strength)"))
    ZX_FIELD(blend, Meta::Tooltip("Additive glows and adds light; Alpha covers what is behind (smoke)"))
    ZX_FIELD(emissive, Meta::Tooltip("Glows by itself instead of being lit by the sun and sky"))
    ZX_FIELD(intensity, Meta::Range(0, 100), Meta::Tooltip("Brightness of emissive particles"))
    ZX_FIELD(lightFlicker, Meta::Range(0, 1), Meta::Tooltip("Flickers the Point Light on this entity (fire)"))
    ZX_FIELD(flipbook, Meta::Category("Flipbook"),
             Meta::Tooltip("Animated texture atlas, e.g. StarterContent/Flipbooks/T_Flipbook_Fire.png (Flame: r = heat, a = mask; Soft: rgb = shading, a = mask)"))
    ZX_FIELD(flipbookColumns, Meta::Range(1, 16))
    ZX_FIELD(flipbookRows, Meta::Range(1, 16))
    ZX_FIELD(flipbookFps, Meta::Range(0, 120), Meta::Tooltip("0 = play once over each particle's life; otherwise frames per second, looping"))
    ZX_FIELD(collision, Meta::Category("Collision"), Meta::Tooltip("Particles hit the world: bounce off it or disappear"))
    ZX_FIELD(bounciness, Meta::Range(0, 1), Meta::Tooltip("Bounce: fraction of the speed kept"))
    ZX_FIELD(onHitEmitter, Meta::Tooltip("Entity whose Particle Emitter bursts where particles hit (rain splashes)"))
    ZX_FIELD(onHitCount, Meta::Range(0, 100))
    ZX_FIELD(seed, Meta::Range(0, 100000))
    ZX_FIELD(gpuSimulation, Meta::Category("Emission"),
             Meta::Tooltip("Simulate on the GPU: hundreds of thousands of particles that collide with what the camera sees. Unsorted; no lights, heat haze or On Hit bursts"))
ZX_REFLECT_STRUCT_END()

namespace {

/// Smooth pseudo-random field for turbulence (cheap: sums of sines with incommensurate frequencies).
Vec3 Swirl(Vec3 p, f32 t)
{
    return Vec3(Sin(p.y * 1.7f + t * 1.3f) + Sin(p.z * 2.3f - t * 0.7f), Sin(p.z * 1.9f + t * 1.1f) + Sin(p.x * 2.1f + t * 0.9f) * 0.5f,
                Sin(p.x * 1.5f - t * 1.2f) + Sin(p.y * 2.7f + t * 0.8f)) *
           0.5f;
}

} // namespace

void ParticleSystem::PredictHit(const ParticleEmitter& d, Particle& p) const
{
    // One ray along the rest of the trajectory (chord of the drag-free parabola). Crossing the hit plane later is the
    // impact, so the exact timing along the chord doesn't matter.
    p.willHit = false;
    if (d.collision == ParticleCollision::None || !raycast)
        return;
    const f32 t = Max(p.life - p.age, 0.0f);
    const Vec3 end = p.position + p.velocity * t + d.acceleration * (0.5f * t * t);
    p.willHit = raycast(p.position, end, p.hitPoint, p.hitNormal);
}

void ParticleSystem::Spawn(Emitter& em, u32 count, const Vec3* origin)
{
    const ParticleEmitter& d = em.desc;
    const u32 room = u32(Max(d.maxParticles, 1)) > em.particles.Size() ? u32(Max(d.maxParticles, 1)) - u32(em.particles.Size()) : 0u;
    count = Min(count, room);
    const Mat4& m = em.world;
    const Vec3 up = SafeNormalize(m.TransformVector(Vec3(0, 1, 0)), Vec3(0, 1, 0));
    for (u32 i = 0; i < count; ++i) {
        Particle p;
        Vec3 local, dir(0, 1, 0);
        switch (d.shape) {
        case EmitterShape::Point: dir = em.rng.OnUnitSphere(); break;
        case EmitterShape::Sphere:
            local = em.rng.InsideUnitSphere() * d.radius;
            dir = SafeNormalize(local, em.rng.OnUnitSphere());
            break;
        case EmitterShape::Box:
            local = Vec3(em.rng.Range(-0.5f, 0.5f), em.rng.Range(-0.5f, 0.5f), em.rng.Range(-0.5f, 0.5f)) * d.boxSize;
            break;
        case EmitterShape::Cone: {
            const Vec2 disc = em.rng.InsideUnitCircle();
            local = Vec3(disc.x, 0.0f, disc.y) * d.radius;
            const f32 angle = d.coneAngle * Sqrt(em.rng.NextFloat());
            const f32 around = em.rng.NextFloat() * TwoPi;
            dir = Vec3(Sin(angle) * Cos(around), Cos(angle), Sin(angle) * Sin(around));
            break;
        }
        }
        p.position = d.worldSpace ? m.TransformPointAffine(local) : local;
        if (origin && d.worldSpace)
            p.position = *origin + m.TransformVector(local);
        Vec3 worldDir = d.shape == EmitterShape::Point ? dir : SafeNormalize(m.TransformVector(dir), up);
        if (!d.worldSpace)
            worldDir = dir;
        p.velocity = worldDir * (d.speed * (1.0f + d.speedVariation * em.rng.Range(-1.0f, 1.0f)));
        p.life = Max(d.lifetime * (1.0f + d.lifetimeVariation * em.rng.Range(-1.0f, 1.0f)), 0.02f);
        p.sizeScale = 1.0f + d.sizeVariation * em.rng.Range(-1.0f, 1.0f);
        p.rotation = em.rng.NextFloat() * TwoPi;
        p.spin = d.spin * em.rng.Range(-1.0f, 1.0f);
        p.seed = em.rng.NextFloat();
        if (d.collision != ParticleCollision::None && d.worldSpace && m_raycastBudget > 0) {
            --m_raycastBudget;
            PredictHit(d, p);
        }
        em.particles.PushBack(p);
    }
}

void ParticleSystem::AcquireGpuSlot(Emitter& em)
{
    if (em.gpuSlot != 0xFFFFFFFFu)
        return;
    if (!m_freeGpuSlots.Empty()) {
        em.gpuSlot = m_freeGpuSlots.Back();
        m_freeGpuSlots.PopBack();
    } else if (m_nextGpuSlot < MaxGpuEmitters) {
        em.gpuSlot = m_nextGpuSlot++;
    }
    em.particles.Clear(); // switched from the CPU
}

void ParticleSystem::ReleaseGpuSlot(Emitter& em)
{
    if (em.gpuSlot == 0xFFFFFFFFu)
        return;
    ++m_gpuGenerations[em.gpuSlot]; // its particles die on the GPU
    m_freeGpuSlots.PushBack(em.gpuSlot);
    em.gpuSlot = 0xFFFFFFFFu;
    em.gpuSpawn = 0;
}

void ParticleSystem::Tick(ecs::World& world, f32 dt, bool playing)
{
    ++m_tick;
    // Play start / stop: everything restarts from the scene state.
    if (playing != m_wasPlaying) {
        Clear();
        m_wasPlaying = playing;
    }
    dt = Clamp(dt, 0.0f, 0.1f);
    m_raycastBudget = 600; // rays per tick for colliding particles
    for (auto& [key, em] : m_emitters)
        em.alive = false;
    world.Query<const scene::WorldTransform, const ParticleEmitter>().Each(
        [&](ecs::Entity e, const scene::WorldTransform& wt, const ParticleEmitter& desc) {
            Emitter* found = m_emitters.Find(e.Pack());
            if (!found) {
                Emitter fresh;
                fresh.rng.Seed(u64(desc.seed) * 0x9E3779B97F4A7C15ull + e.index + 1);
                m_emitters.InsertOrAssign(e.Pack(), move(fresh));
                found = m_emitters.Find(e.Pack());
            }
            Emitter& em = *found;
            em.alive = true;
            em.desc = desc;
            em.lastWorld = em.world;
            em.world = wt.matrix;
            em.lightId = e.index + 1;
            em.gpuSpawn = 0;
            em.gpuDt = 0.0f;
            if (!playing && !desc.simulateInEditor) {
                em.particles.Clear();
                ReleaseGpuSlot(em);
                em.started = false;
                return;
            }
            if (UsesGpu(desc))
                AcquireGpuSlot(em); // no free slot: stays on the CPU
            else
                ReleaseGpuSlot(em);
            if (!em.started) {
                em.started = true;
                em.lastWorld = em.world;
                em.pendingBurst += u32(Max(desc.burst, 0));
            }
            // Flicker: two slow noises (the light breathes rather than strobes).
            em.flicker = 1.0f - desc.lightFlicker * (0.5f + 0.3f * Sin(em.time * 9.1f + f32(e.index)) + 0.2f * Sin(em.time * 23.7f));
            if (em.gpuSlot != 0xFFFFFFFFu) {
                em.time += dt;
                em.gpuDt = dt;
            } else {
                Advance(em, dt);
            }
            // Emit.
            u32 count = em.pendingBurst;
            em.pendingBurst = 0;
            if (desc.playing && desc.rate > 0.0f) {
                em.accumulator += desc.rate * dt;
                const u32 n = u32(em.accumulator);
                em.accumulator -= f32(n);
                count += n;
            }
            if (count && em.gpuSlot != 0xFFFFFFFFu)
                em.gpuSpawn += count;
            else if (count)
                Spawn(em, count);
        });
    // Scripted bursts.
    for (usize i = 0; i < m_pendingEmits.Size(); ++i)
        if (Emitter* em = m_emitters.Find(m_pendingEmits[i].Pack())) {
            if (em->gpuSlot != 0xFFFFFFFFu)
                em->gpuSpawn += m_pendingCounts[i]; // from the emitter (GPU bursts have no explicit position)
            else
                Spawn(*em, m_pendingCounts[i], m_pendingHasPosition[i] ? &m_pendingPositions[i] : nullptr);
        }
    m_pendingEmits.Clear();
    m_pendingPositions.Clear();
    m_pendingHasPosition.Clear();
    // Impacts burst their On Hit Emitters (by entity name) where they happened.
    if (!m_hits.Empty()) {
        HashMap<String, ecs::Entity> byName;
        world.Query<const scene::EntityName, const ParticleEmitter>().Each([&](ecs::Entity e, const scene::EntityName& n, const ParticleEmitter&) {
            if (!byName.Contains(n.value))
                byName.InsertOrAssign(n.value, e);
        });
        for (const Hit& h : m_hits)
            if (const ecs::Entity* target = byName.Find(h.emitter))
                if (Emitter* em = m_emitters.Find(target->Pack()); em && em->gpuSlot == 0xFFFFFFFFu)
                    Spawn(*em, h.count, &h.position);
        m_hits.Clear();
    }
    m_pendingCounts.Clear();
    for (auto& [key, em] : m_emitters)
        if (!em.alive)
            ReleaseGpuSlot(em);
    m_emitters.RemoveIf([](u64, const Emitter& em) { return !em.alive; });
    for (auto& [key, em] : m_free)
        Advance(em, dt);
    m_free.RemoveIf([](u64, const Emitter& em) { return em.particles.Empty(); });
}

void ParticleSystem::Advance(Emitter& em, f32 dt)
{
    const ParticleEmitter& desc = em.desc;
    em.time += dt;
    const f32 dragFactor = 1.0f / (1.0f + desc.drag * dt);
    usize w = 0;
    for (usize i = 0; i < em.particles.Size(); ++i) {
        Particle p = em.particles[i];
        p.age += dt;
        if (p.age >= p.life)
            continue;
        Vec3 accel = desc.acceleration;
        if (desc.turbulence > 0.0f)
            accel += Swirl(p.position * 1.3f + Vec3(p.seed * 10.0f), em.time) * desc.turbulence;
        p.velocity = (p.velocity + accel * dt) * dragFactor;
        p.position += p.velocity * dt;
        p.rotation += p.spin * dt;
        if (p.willHit && Dot(p.position - p.hitPoint, p.hitNormal) < 0.0f) {
            // Crossed the surface it was heading for.
            const Vec3 at = p.position - p.hitNormal * Dot(p.position - p.hitPoint, p.hitNormal);
            if (!desc.onHitEmitter.Empty() && desc.onHitCount > 0 && m_hits.Size() < 256)
                m_hits.PushBack(Hit{desc.onHitEmitter, at, u32(desc.onHitCount)});
            if (desc.collision == ParticleCollision::Die)
                continue;
            const f32 vn = Dot(p.velocity, p.hitNormal);
            p.velocity = (p.velocity - p.hitNormal * (vn * (1.0f + desc.bounciness))) * (0.5f + 0.5f * desc.bounciness);
            p.position = at + p.hitNormal * 0.01f;
            if (Dot(p.velocity, p.velocity) > 0.04f && m_raycastBudget > 0) {
                --m_raycastBudget;
                PredictHit(desc, p);
            } else {
                p.velocity = Vec3(); // at rest on the surface (the same plane keeps holding it up)
            }
        }
        em.particles[w++] = p;
    }
    em.particles.Resize(w);
}

void ParticleSystem::SpawnFree(u64 key, const ParticleEmitter& desc, Vec3 position, Vec3 velocity)
{
    Emitter* em = m_free.Find(key);
    if (!em) {
        Emitter fresh;
        fresh.rng.Seed(key * 0x9E3779B97F4A7C15ull + 1);
        fresh.started = true;
        m_free.InsertOrAssign(key, move(fresh));
        em = m_free.Find(key);
    }
    em->desc = desc;
    em->desc.worldSpace = true;
    if (em->particles.Size() >= usize(Max(desc.maxParticles, 1)))
        return;
    Particle p;
    p.position = position;
    p.velocity = velocity * (1.0f + desc.speedVariation * em->rng.Range(-1.0f, 1.0f));
    p.life = Max(desc.lifetime * (1.0f + desc.lifetimeVariation * em->rng.Range(-1.0f, 1.0f)), 0.02f);
    p.sizeScale = 1.0f + desc.sizeVariation * em->rng.Range(-1.0f, 1.0f);
    p.rotation = em->rng.NextFloat() * TwoPi;
    p.spin = desc.spin * em->rng.Range(-1.0f, 1.0f);
    p.seed = em->rng.NextFloat();
    em->particles.PushBack(p);
}

void ParticleSystem::AppendLights(Array<render::LightDesc>& lights) const
{
    for (const render::LightDesc& l : m_extraLights)
        lights.PushBack(l);
}

void ParticleSystem::Gather(Vec3 cameraPosition, Array<render::GpuParticle>& out, const Function<u32(StringView), 32>* textures) const
{
    const usize first = out.Size();
    auto gather = [&](const Emitter& em) {
        const ParticleEmitter& d = em.desc;
        const bool additive = d.blend == ParticleBlend::Additive;
        const render::ParticleShape shape = d.look == ParticleLook::Flame   ? render::ParticleShape::Flame
                                            : d.look == ParticleLook::Spark ? render::ParticleShape::Spark
                                            : d.look == ParticleLook::Heat  ? render::ParticleShape::Heat
                                                                            : render::ParticleShape::Soft;
        const u32 atlas = !d.flipbook.Empty() && textures && *textures ? (*textures)(d.flipbook) : 0xFFFFFFFFu;
        const u32 columns = u32(Clamp(d.flipbookColumns, 1, 16)), rows = u32(Clamp(d.flipbookRows, 1, 16));
        const u32 packedShape = atlas != 0xFFFFFFFFu ? render::PackParticleShape(u32(shape), columns, rows) : u32(shape);
        for (const Particle& p : em.particles) {
            const f32 t = Clamp(p.age / p.life, 0.0f, 1.0f);
            const f32 fade = d.fadeIn > 0.0f ? Min(t / d.fadeIn, 1.0f) : 1.0f;
            render::GpuParticle g;
            g.position = d.worldSpace ? p.position : em.world.TransformPointAffine(p.position);
            g.size = Max(Lerp(d.startSize, d.endSize, t) * p.sizeScale, 0.0f);
            const Vec3 rgb = Lerp(d.startColor, d.endColor, t) * (d.emissive ? d.intensity : 1.0f);
            g.color = Vec4(rgb.x, rgb.y, rgb.z, Clamp(Lerp(d.startAlpha, d.endAlpha, t) * fade, 0.0f, 1.0f));
            g.rotation = p.rotation;
            g.flags = (d.emissive ? u32(render::ParticleEmissive) : 0u) | (additive ? u32(render::ParticleAdditive) : 0u);
            g.texture = atlas;
            g.shape = packedShape;
            g.velocity = d.worldSpace ? p.velocity : em.world.TransformVector(p.velocity);
            g.seed = p.seed;
            if (atlas != 0xFFFFFFFFu) // animation position: over the life, or looping from a random frame
                g.seed = d.flipbookFps > 0.0f ? Frac(p.seed + p.age * d.flipbookFps / f32(columns * rows)) : Min(t, 0.999f);
            if (g.size > 0.0f && g.color.w > 0.001f)
                out.PushBack(g);
        }
    };
    for (const auto& [key, em] : m_emitters)
        gather(em);
    for (const auto& [key, em] : m_free)
        gather(em);
    // Back to front (alpha particles need it; additive ones don't mind).
    std::sort(out.Data() + first, out.Data() + out.Size(), [&](const render::GpuParticle& a, const render::GpuParticle& b) {
        return DistanceSq(a.position, cameraPosition) > DistanceSq(b.position, cameraPosition);
    });
}

void ParticleSystem::GatherGpu(Array<render::GpuEmitter>& out, render::GpuParticleFrame& frame, const Function<u32(StringView), 32>* textures) const
{
    out.Clear();
    frame.tick = m_tick;
    frame.resetGeneration = m_resetGeneration;
    if (m_nextGpuSlot == 0 || m_freeGpuSlots.Size() == m_nextGpuSlot) {
        frame.emitters = {};
        return;
    }
    out.Resize(m_nextGpuSlot); // unused slots stay inactive (flags 0)
    for (const auto& [key, em] : m_emitters) {
        if (em.gpuSlot == 0xFFFFFFFFu)
            continue;
        const ParticleEmitter& d = em.desc;
        render::GpuEmitter& g = out[em.gpuSlot];
        const bool additive = d.blend == ParticleBlend::Additive;
        const render::ParticleShape shape = d.look == ParticleLook::Flame   ? render::ParticleShape::Flame
                                            : d.look == ParticleLook::Spark ? render::ParticleShape::Spark
                                                                            : render::ParticleShape::Soft;
        const u32 atlas = !d.flipbook.Empty() && textures && *textures ? (*textures)(d.flipbook) : 0xFFFFFFFFu;
        const u32 columns = u32(Clamp(d.flipbookColumns, 1, 16)), rows = u32(Clamp(d.flipbookRows, 1, 16));
        const f32 glow = d.emissive ? d.intensity : 1.0f;
        g.world = em.world;
        g.acceleration = d.acceleration;
        g.drag = d.drag;
        g.startColor = d.startColor * glow;
        g.endColor = d.endColor * glow;
        g.startAlpha = d.startAlpha;
        g.endAlpha = d.endAlpha;
        g.boxSize = d.boxSize;
        g.radius = d.radius;
        g.startSize = d.startSize;
        g.endSize = d.endSize;
        g.sizeVariation = d.sizeVariation;
        g.spin = d.spin;
        g.speed = d.speed;
        g.speedVariation = d.speedVariation;
        g.lifetime = d.lifetime;
        g.lifetimeVariation = d.lifetimeVariation;
        g.coneAngle = d.coneAngle;
        g.turbulence = d.turbulence;
        g.fadeIn = d.fadeIn;
        g.bounciness = d.bounciness;
        g.dt = em.gpuDt;
        g.time = em.time;
        g.flipbookFps = d.flipbookFps;
        g.flipbookFrames = f32(columns * rows);
        g.spawnCount = em.gpuSpawn;
        g.seed = u32(m_tick * 0x9E3779B1ull) ^ (em.gpuSlot * 0x85EBCA6Bu) ^ u32(d.seed);
        g.generation = m_gpuGenerations[em.gpuSlot];
        g.emitShape = u32(d.shape);
        g.packedShape = atlas != 0xFFFFFFFFu ? render::PackParticleShape(u32(shape), columns, rows) : u32(shape);
        g.renderFlags = (d.emissive ? u32(render::ParticleEmissive) : 0u) | (additive ? u32(render::ParticleAdditive) : 0u);
        g.texture = atlas;
        g.flags = render::GpuEmitter::Active | (d.worldSpace ? u32(render::GpuEmitter::WorldSpace) : 0u) |
                  (d.collision == ParticleCollision::Bounce ? u32(render::GpuEmitter::Bounce) : 0u) |
                  (d.collision == ParticleCollision::Die ? u32(render::GpuEmitter::Die) : 0u);
    }
    frame.emitters = out.View();
}

void ParticleSystem::ModulateLights(Span<render::LightDesc> lights) const
{
    for (const auto& [key, em] : m_emitters) {
        if (em.desc.lightFlicker <= 0.0f || !em.started)
            continue;
        for (render::LightDesc& l : lights)
            if (l.id == em.lightId)
                l.intensity *= Max(em.flicker, 0.0f);
    }
}

void ParticleSystem::Emit(ecs::Entity e, u32 count, const Vec3* position)
{
    m_pendingEmits.PushBack(e);
    m_pendingCounts.PushBack(count);
    m_pendingPositions.PushBack(position ? *position : Vec3());
    m_pendingHasPosition.PushBack(position ? 1 : 0);
}

void ParticleSystem::Clear()
{
    m_emitters.Clear();
    m_freeGpuSlots.Clear();
    m_nextGpuSlot = 0;
    for (u16& g : m_gpuGenerations)
        ++g;
    ++m_resetGeneration;
    m_free.Clear();
    m_extraLights.Clear();
    m_extraFogVolumes.Clear();
    m_pendingEmits.Clear();
    m_pendingCounts.Clear();
    m_pendingPositions.Clear();
    m_pendingHasPosition.Clear();
    m_hits.Clear();
}

u32 ParticleSystem::ParticleCount() const
{
    u32 n = 0;
    for (const auto& [key, em] : m_emitters)
        n += u32(em.particles.Size());
    for (const auto& [key, em] : m_free)
        n += u32(em.particles.Size());
    return n;
}

} // namespace zx
