#include "Engine/Gameplay.h"
#include "Engine/SceneRendering.h"
#include "Scene/Hierarchy.h"

namespace zx {

ZX_REFLECT_STRUCT_BEGIN(Rotator)
    ZX_FIELD(axis)
    ZX_FIELD(degreesPerSecond, Meta::Range(-3600, 3600), Meta::Units("deg/s"))
    ZX_FIELD(localSpace)
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(Camera)
    ZX_FIELD(fieldOfView, Meta::Range(10, 150), Meta::Units("deg"))
    ZX_FIELD(nearPlane, Meta::Range(0.001, 10), Meta::Units("m"))
    ZX_FIELD(active)
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(PaintedInstance)
    ZX_FIELD(brushItem, Meta::ReadOnly())
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(CharacterController)
    ZX_FIELD(height, Meta::Range(0.2, 10), Meta::Units("m"))
    ZX_FIELD(radius, Meta::Range(0.05, 5), Meta::Units("m"))
    ZX_FIELD(stepHeight, Meta::Range(0, 2), Meta::Units("m"))
    ZX_FIELD(gravity, Meta::Range(0, 100), Meta::Units("m/s2"))
    ZX_FIELD(maxFallSpeed, Meta::Range(1, 200), Meta::Units("m/s"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(Oscillator)
    ZX_FIELD(offset)
    ZX_FIELD(frequency, Meta::Range(0, 20), Meta::Units("Hz"))
    ZX_FIELD(phase, Meta::Range(0, 1))
ZX_REFLECT_STRUCT_END()

namespace {

/// Start position of an oscillating entity (added on its first tick; never serialized).
struct OscillatorOrigin
{
    Vec3 position;
};

} // namespace

void TickGameplay(ecs::World& world, f32 dt)
{
    GameplayClock* clock = world.GetSingleton<GameplayClock>();
    if (!clock)
        clock = &world.SetSingleton(GameplayClock{});
    clock->time += f64(dt);
    ++clock->frame;
    const f32 time = f32(clock->time);

    world.Query<const Rotator, Transform>().Each([dt](const Rotator& r, Transform& t) {
        const Vec3 axis = SafeNormalize(r.axis, Vec3::Up());
        const Quat delta = Quat::FromAxisAngle(axis, DegToRad(r.degreesPerSecond) * dt);
        t.rotation = Normalize(r.localSpace ? t.rotation * delta : delta * t.rotation);
    });

    // First tick: remember where each oscillator started (structural change, so outside the query).
    Array<ecs::Entity> fresh;
    world.Query<const Oscillator, const Transform>().Without<OscillatorOrigin>().Each(
        [&](ecs::Entity e, const Oscillator&, const Transform&) { fresh.PushBack(e); });
    for (ecs::Entity e : fresh)
        world.Add(e, OscillatorOrigin{world.Get<const Transform>(e)->position});
    world.Query<const Oscillator, const OscillatorOrigin, Transform>().Each(
        [time](const Oscillator& o, const OscillatorOrigin& origin, Transform& t) {
            t.position = origin.position + o.offset * Sin(TwoPi * (o.frequency * time + o.phase));
        });
}

// --- Characters -----------------------------------------------------------------------------------------------------------

namespace {

constexpr f32 Skin = 0.001f; // gap kept between the body and obstacles

struct Collider
{
    AABB box;
    ecs::Entity entity;
};

bool Overlaps(const AABB& a, const AABB& b)
{
    return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y && a.min.z < b.max.z && a.max.z > b.min.z;
}

AABB Body(Vec3 feet, const CharacterController& c)
{
    return AABB(Vec3(feet.x - c.radius, feet.y, feet.z - c.radius), Vec3(feet.x + c.radius, feet.y + c.height, feet.z + c.radius));
}

/// Moves along one horizontal axis; climbs obstacles up to stepHeight when grounded.
void MoveAxis(Vec3& feet, u32 axis, f32 delta, const CharacterController& c, Span<const Collider> colliders, bool grounded)
{
    if (delta == 0.0f)
        return;
    feet[axis] += delta;
    for (const Collider& col : colliders) {
        if (!Overlaps(Body(feet, c), col.box))
            continue;
        // Step up onto low obstacles if there is room above them.
        const f32 rise = col.box.max.y - feet.y;
        if (grounded && rise > 0.0f && rise <= c.stepHeight) {
            const Vec3 raised(feet.x, col.box.max.y + Skin, feet.z);
            bool blocked = false;
            for (const Collider& other : colliders)
                blocked |= Overlaps(Body(raised, c), other.box);
            if (!blocked) {
                feet = raised;
                continue;
            }
        }
        feet[axis] = delta > 0.0f ? col.box.min[axis] - c.radius - Skin : col.box.max[axis] + c.radius + Skin;
    }
}

bool IsPartOf(ecs::World& world, ecs::Entity e, ecs::Entity root)
{
    for (ecs::Entity p = e; p; p = scene::GetParent(world, p))
        if (p == root)
            return true;
    return false;
}

} // namespace

void TickCharacters(ecs::World& world, f32 dt, const MeshBoundsFn& meshBounds)
{
    if (dt <= 0.0f)
        return;
    // Static colliders: world boxes of meshes with collision (thin planes get a little thickness below).
    Array<Collider> all;
    world.Query<const MeshRenderer, const scene::WorldTransform>().Each(
        [&](ecs::Entity e, const MeshRenderer& mr, const scene::WorldTransform& wt) {
            AABB local;
            if (!mr.collision || !meshBounds || !meshBounds(mr.mesh, local) || !local.IsValid())
                return;
            AABB box = local.Transformed(wt.matrix);
            if (box.max.y - box.min.y < 0.05f)
                box.min.y = box.max.y - 0.05f;
            all.PushBack(Collider{box, e});
        });

    Array<ecs::Entity> characters;
    world.Query<const CharacterController, const Transform>().Each(
        [&](ecs::Entity e, const CharacterController&, const Transform&) { characters.PushBack(e); });
    Array<Collider> colliders;
    for (ecs::Entity e : characters) {
        // The character's own meshes (body, held items) don't block it.
        colliders.Clear();
        for (const Collider& col : all)
            if (!IsPartOf(world, col.entity, e))
                colliders.PushBack(col);
        const CharacterController c = *world.Get<const CharacterController>(e);
        if (!world.Has<CharacterState>(e))
            world.Add(e, CharacterState{});
        CharacterState& st = *world.Get<CharacterState>(e);
        Transform& t = *world.Get<Transform>(e);
        if (!st.initialized) {
            st.initialized = true;
            st.spawn = t.position;
        }
        Vec3 feet = t.position;
        // Vertical velocity: jumps (only from the ground) and gravity.
        if (st.jumpSpeed > 0.0f && st.grounded) {
            st.velocity.y = st.jumpSpeed;
            st.grounded = false;
        }
        st.jumpSpeed = 0.0f;
        st.velocity.y = Max(st.velocity.y - c.gravity * dt, -c.maxFallSpeed);
        st.velocity.x = st.desiredMove.x;
        st.velocity.z = st.desiredMove.z;
        st.desiredMove = Vec3(0.0f);

        const bool wasGrounded = st.grounded;
        MoveAxis(feet, 0, st.velocity.x * dt, c, colliders.View(), wasGrounded);
        MoveAxis(feet, 2, st.velocity.z * dt, c, colliders.View(), wasGrounded);

        // Vertical move: land on tops, bump heads on bottoms.
        st.grounded = false;
        feet.y += st.velocity.y * dt;
        for (const Collider& col : colliders) {
            if (!Overlaps(Body(feet, c), col.box))
                continue;
            if (st.velocity.y <= 0.0f) {
                feet.y = col.box.max.y + Skin;
                st.grounded = true;
            } else {
                feet.y = col.box.min.y - c.height - Skin;
            }
            st.velocity.y = 0.0f;
        }
        // Walking down stairs: stay on the ground within one step below.
        if (!st.grounded && wasGrounded && st.velocity.y <= 0.0f) {
            f32 best = -Infinity;
            const AABB probe(Vec3(feet.x - c.radius, feet.y - c.stepHeight, feet.z - c.radius), Vec3(feet.x + c.radius, feet.y, feet.z + c.radius));
            for (const Collider& col : colliders)
                if (Overlaps(probe, col.box))
                    best = Max(best, col.box.max.y);
            if (best > -Infinity && best <= feet.y) {
                feet.y = best + Skin;
                st.grounded = true;
                st.velocity.y = 0.0f;
            }
        }
        // Fell out of the world: back to the start.
        if (feet.y < st.spawn.y - 100.0f) {
            feet = st.spawn;
            st.velocity = Vec3(0.0f);
        }
        t.position = feet;
    }
}

ecs::Entity FindGameCamera(ecs::World& world)
{
    ecs::Entity found;
    world.Query<const Camera>().Each([&](ecs::Entity e, const Camera& c) {
        if (!found && c.active)
            found = e;
    });
    return found;
}

} // namespace zx
