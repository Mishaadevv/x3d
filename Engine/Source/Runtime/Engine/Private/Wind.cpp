#include "Engine/Wind.h"
#include <cmath>
#include "Scene/Hierarchy.h"

namespace zx {

ZX_REFLECT_ENUM_BEGIN(WindZoneMode)
    ZX_ENUM_VALUE(Directional)
    ZX_ENUM_VALUE(Spherical)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(WindZone)
    ZX_FIELD(mode, Meta::Tooltip("Directional: everywhere, along the entity's forward (blue arrow). Spherical: out from the entity"))
    ZX_FIELD(speed, Meta::Range(0, 60), Meta::Units("m/s"))
    ZX_FIELD(radius, Meta::Range(0.5, 500), Meta::Units("m"), Meta::Tooltip("Spherical: the wind fades to zero here"))
    ZX_FIELD(turbulence, Meta::Range(0, 1), Meta::Tooltip("Swirls, as a fraction of the speed"))
    ZX_FIELD(gustStrength, Meta::Range(0, 1), Meta::Tooltip("How much stronger the gusts blow"))
    ZX_FIELD(gustFrequency, Meta::Range(0, 2), Meta::Units("1/s"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(WindResponse)
    ZX_FIELD(strength, Meta::Range(0, 5), Meta::Tooltip("Multiplier of the scene's wind (0 = sheltered)"))
    ZX_FIELD(customWind, Meta::Tooltip("Ignore the Wind Zones and use this object's own wind"))
    ZX_FIELD(customSpeed, Meta::Range(0, 60), Meta::Units("m/s"), Meta::EditCondition("customWind"))
    ZX_FIELD(customDirection, Meta::Range(-180, 180), Meta::Units("deg"), Meta::EditCondition("customWind"), Meta::Tooltip("Around +Y, 0 = towards +Z"))
    ZX_FIELD(drag, Meta::Range(0, 20), Meta::Units("m2"), Meta::Tooltip("Rigid Body: area x drag coefficient - how hard the wind pushes it"))
ZX_REFLECT_STRUCT_END()

namespace {

/// Smooth noise in 0..1 (sums of sines with incommensurate frequencies): gusts.
f32 Gust(f64 t, f32 phase)
{
    const f64 x = t + f64(phase);
    const f64 v = std::sin(x * 1.0) * 0.5 + std::sin(x * 2.31 + 1.3) * 0.3 + std::sin(x * 4.73 + 2.1) * 0.2;
    return f32(Clamp(v * 0.5 + 0.5, 0.0, 1.0));
}

/// Divergence-free-ish swirl in [-1, 1]^3 around the main flow.
Vec3 Swirl(Vec3 p, f64 t)
{
    const f32 s = f32(t);
    return Vec3(std::sin(p.y * 0.21f + s * 0.9f) + std::sin(p.z * 0.17f - s * 0.6f), std::sin(p.z * 0.19f + s * 0.7f) * 0.4f,
                std::sin(p.x * 0.23f - s * 0.8f) + std::sin(p.y * 0.15f + s * 0.5f)) *
           0.5f;
}

f64 Clock(ecs::World& world)
{
    const WindClock* c = world.GetSingleton<WindClock>();
    return c ? c->time : 0.0;
}

} // namespace

void TickWind(ecs::World& world, f32 dt)
{
    WindClock* c = world.GetSingleton<WindClock>();
    if (!c)
        c = &world.SetSingleton(WindClock{});
    c->time += f64(Clamp(dt, 0.0f, 0.25f));
}

f64 EnvironmentTime(ecs::World& world)
{
    const WindClock* c = world.GetSingleton<WindClock>();
    return c ? c->time : -1.0;
}

bool HasWind(ecs::World& world)
{
    bool any = false;
    world.Query<const WindZone>().Each([&](ecs::Entity, const WindZone&) { any = true; });
    return any;
}

namespace {

Vec3 ZonesAt(ecs::World& world, Vec3 position, bool steady)
{
    const f64 t = Clock(world);
    Vec3 wind;
    world.Query<const WindZone, const scene::WorldTransform>().Each([&](ecs::Entity e, const WindZone& z, const scene::WorldTransform& wt) {
        const f32 phase = f32(e.index % 97) * 1.37f;
        const f32 gust = steady ? 1.0f : 1.0f + z.gustStrength * Gust(t * f64(Max(z.gustFrequency, 0.0f)) * 6.2831853, phase) * 1.2f;
        Vec3 dir;
        f32 speed = z.speed * gust;
        if (z.mode == WindZoneMode::Directional) {
            dir = SafeNormalize(wt.matrix.TransformVector(Vec3(0, 0, 1)), Vec3(0, 0, 1));
        } else {
            const Vec3 d = position - wt.GetPosition();
            const f32 dist = Length(d);
            if (dist >= z.radius)
                return;
            dir = SafeNormalize(d, Vec3(0, 1, 0));
            const f32 fade = 1.0f - dist / Max(z.radius, 1e-3f);
            speed *= fade * fade;
        }
        Vec3 w = dir * speed;
        if (z.turbulence > 0.0f && !steady)
            w += Swirl(position + Vec3(phase), t) * (z.speed * z.turbulence);
        wind += w;
    });
    return wind;
}

Vec3 Felt(ecs::World& world, ecs::Entity entity, Vec3 position, bool steady)
{
    const WindResponse* r = nullptr;
    for (ecs::Entity x = entity; world.IsAlive(x) && !r;) {
        r = world.Get<const WindResponse>(x);
        const scene::Parent* p = world.Get<const scene::Parent>(x);
        if (!p)
            break;
        x = p->entity;
    }
    if (r && r->customWind) {
        const f32 a = DegToRad(r->customDirection);
        return Vec3(std::sin(a), 0.0f, std::cos(a)) * (r->customSpeed * r->strength);
    }
    const Vec3 w = ZonesAt(world, position, steady);
    return r ? w * r->strength : w;
}

} // namespace

Vec3 WindAt(ecs::World& world, Vec3 position) { return ZonesAt(world, position, false); }
Vec3 WindFor(ecs::World& world, ecs::Entity entity, Vec3 position) { return Felt(world, entity, position, false); }
Vec3 WindSteadyFor(ecs::World& world, ecs::Entity entity, Vec3 position) { return Felt(world, entity, position, true); }

Vec3 WindDragForce(Vec3 wind, Vec3 velocity, f32 dragArea)
{
    const Vec3 rel = wind - velocity;
    return rel * (0.5f * 1.225f * dragArea * Length(rel));
}

} // namespace zx
