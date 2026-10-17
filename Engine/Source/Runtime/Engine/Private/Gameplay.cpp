#include "Engine/Gameplay.h"
#include "Engine/PhysicsSystem.h"
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
    ZX_FIELD(maxSlope, Meta::Range(0, 89), Meta::Units("deg"), Meta::Tooltip("Steeper surfaces are walls"))
    ZX_FIELD(gravity, Meta::Range(0, 100), Meta::Units("m/s2"))
    ZX_FIELD(maxFallSpeed, Meta::Range(1, 200), Meta::Units("m/s"))
    ZX_FIELD(pushMass, Meta::Range(0, 10000), Meta::Units("kg"), Meta::Tooltip("How hard the character pushes rigid bodies"))
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
        st.velocity.x = st.desiredMove.x + (dt > 0.0f ? st.rootMotion.x / dt : 0.0f);
        st.velocity.z = st.desiredMove.z + (dt > 0.0f ? st.rootMotion.z / dt : 0.0f);
        st.desiredMove = Vec3(0.0f);
        st.rootMotion = Vec3(0.0f);

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

void TickCharacters(ecs::World& world, f32 dt, PhysicsSystem& physics)
{
    physics::PhysicsWorld* pw = physics.World();
    if (dt <= 0.0f || !pw)
        return;
    constexpr f32 Gap = 0.01f; // kept between the capsule and what it touches
    Array<ecs::Entity> characters;
    world.Query<const CharacterController, const Transform>().Each(
        [&](ecs::Entity e, const CharacterController&, const Transform&) { characters.PushBack(e); });
    for (ecs::Entity e : characters) {
        const CharacterController c = *world.Get<const CharacterController>(e);
        if (!world.Has<CharacterState>(e))
            world.Add(e, CharacterState{});
        CharacterState& st = *world.Get<CharacterState>(e);
        Transform& t = *world.Get<Transform>(e);
        if (!st.initialized) {
            st.initialized = true;
            st.spawn = t.position;
        }
        const physics::BodyId self = physics.BodyOf(e);
        const f32 r = Max(c.radius, 0.05f);
        const f32 halfHeight = Max(c.height * 0.5f - r, 0.0f);
        const physics::ShapeDesc capsule = physics::ShapeDesc::Capsule(r, halfHeight);
        const Vec3 toCenter(0.0f, r + halfHeight, 0.0f);
        const f32 walkable = Cos(DegToRad(Clamp(c.maxSlope, 0.0f, 89.0f)));
        Vec3 feet = t.position;

        // Riding: follow the moving body we stand on.
        const physics::BodyId ground{st.groundBodyIndex, st.groundBodyGeneration};
        if (st.grounded && pw->IsValid(ground) && pw->GetMotionType(ground) != physics::MotionType::Static) {
            const Vec3 carried = pw->GetPosition(ground) + pw->GetRotation(ground) * st.groundLocal;
            if (DistanceSq(carried, feet) < 4.0f)
                feet = carried;
        }

        // One sweep of the capsule; stops `Gap` before what it hits.
        auto sweep = [&](Vec3 move, physics::RaycastHit& hit) {
            const f32 len = Length(move);
            if (len < 1e-6f)
                return false;
            const Vec3 dir = move / len;
            if (pw->ShapeCast(capsule, feet + toCenter, Quat(), dir, len + Gap, hit, ~0u, self)) {
                feet += dir * Max(hit.distance - Gap, 0.0f);
                return true;
            }
            feet += move;
            return false;
        };
        // The surface under a contact point: the capsule's round bottom touching a step edge reports a diagonal normal,
        // but what matters for standing is the face below that point.
        auto supportNormal = [&](const physics::RaycastHit& h) {
            if (h.point.y > feet.y + r * 0.9f)
                return h.normal; // touching with the side, not the bottom
            Vec3 inward = h.point - (feet + toCenter);
            inward.y = 0.0f;
            inward = SafeNormalize(inward, Vec3());
            physics::RaycastHit below;
            if (pw->Raycast(Ray(h.point + Vec3(0.0f, 0.05f, 0.0f) + inward * 0.02f, Vec3(0, -1, 0)), 0.1f, below, ~0u, self) &&
                below.distance > 1e-4f && below.normal.y > h.normal.y) // (a ray starting inside something tells nothing)
                return below.normal;
            return h.normal;
        };
        // Rigid bodies in the way get pushed: velocity along the push as two masses sharing the character's speed.
        auto push = [&](const physics::RaycastHit& hit, Vec3 velocity) {
            if (pw->GetMotionType(hit.body) != physics::MotionType::Dynamic || c.pushMass <= 0.0f)
                return;
            Vec3 dir(-hit.normal.x, 0.0f, -hit.normal.z);
            const f32 len = Length(dir);
            if (len < 1e-4f)
                return;
            dir = dir / len;
            const f32 speed = Dot(velocity, dir);
            if (speed <= 0.0f)
                return;
            const f32 mass = pw->GetMass(hit.body);
            const f32 target = speed * c.pushMass / (c.pushMass + mass);
            const f32 current = Dot(pw->GetLinearVelocity(hit.body), dir);
            if (current < target)
                pw->AddImpulseAtPoint(hit.body, dir * ((target - current) * mass), hit.point);
        };

        // Vertical velocity: jumps (only from the ground) and gravity.
        const bool wasGrounded = st.grounded;
        if (st.jumpSpeed > 0.0f && st.grounded) {
            st.velocity.y = st.jumpSpeed;
            st.grounded = false;
        }
        const bool jumped = st.velocity.y > 0.0f && wasGrounded && !st.grounded;
        st.jumpSpeed = 0.0f;
        st.velocity.y = Max(st.velocity.y - c.gravity * dt, -c.maxFallSpeed);
        st.velocity.x = st.desiredMove.x + (dt > 0.0f ? st.rootMotion.x / dt : 0.0f);
        st.velocity.z = st.desiredMove.z + (dt > 0.0f ? st.rootMotion.z / dt : 0.0f);
        st.desiredMove = Vec3(0.0f);
        st.rootMotion = Vec3(0.0f);

        // Horizontal: step up, slide along what blocks us, step back down.
        const Vec3 horizontal(st.velocity.x * dt, 0.0f, st.velocity.z * dt);
        f32 raised = 0.0f;
        physics::RaycastHit hit;
        if (LengthSq(horizontal) > 0.0f) {
            if (wasGrounded && !jumped && c.stepHeight > 0.0f) {
                const f32 before = feet.y;
                sweep(Vec3(0.0f, c.stepHeight, 0.0f), hit);
                raised = feet.y - before;
            }
            Vec3 move = horizontal;
            for (u32 it = 0; it < 4 && LengthSq(move) > 1e-10f; ++it) {
                const Vec3 start = feet;
                if (!sweep(move, hit))
                    break;
                push(hit, Vec3(st.velocity.x, 0.0f, st.velocity.z));
                Vec3 n = hit.normal;
                if (n.y < walkable) { // walls (and too steep slopes) block horizontally only
                    n.y = 0.0f;
                    n = SafeNormalize(n, -SafeNormalize(move, Vec3(1, 0, 0)));
                }
                move = move - (feet - start);
                move = move - n * Dot(move, n);
                move.y = Max(move.y, 0.0f);
            }
            if (raised > 0.0f) {
                // Back down by the raise: onto the step, or to the old height (then gravity takes over).
                physics::RaycastHit down;
                sweep(Vec3(0.0f, -raised, 0.0f), down);
            }
        }

        // Vertical: fall / jump, land on walkable ground, slide down steep slopes.
        st.grounded = false;
        Vec3 vertical(0.0f, st.velocity.y * dt, 0.0f);
        for (u32 it = 0; it < 3 && LengthSq(vertical) > 1e-10f; ++it) {
            const Vec3 start = feet;
            if (!sweep(vertical, hit))
                break;
            if (vertical.y > 0.0f) {
                st.velocity.y = 0.0f; // head bump
                break;
            }
            if (hit.normal.y >= walkable || supportNormal(hit).y >= walkable) {
                st.grounded = true;
                st.velocity.y = 0.0f;
                break;
            }
            vertical = vertical - (feet - start);
            vertical = vertical - hit.normal * Dot(vertical, hit.normal); // slide down the steep surface
        }
        // Stay on the ground walking down stairs and slopes: snap within a step below.
        physics::RaycastHit groundHit;
        bool haveGround = false;
        if (!st.grounded && wasGrounded && !jumped && st.velocity.y <= 0.0f) {
            const Vec3 start = feet;
            if (sweep(Vec3(0.0f, -(c.stepHeight + 0.05f), 0.0f), groundHit) &&
                (groundHit.normal.y >= walkable || supportNormal(groundHit).y >= walkable)) {
                st.grounded = true;
                st.velocity.y = 0.0f;
                haveGround = true;
            } else {
                feet = start;
            }
        }
        // What we stand on (for riding platforms and the ground normal).
        if (st.grounded && !haveGround) {
            const Vec3 start = feet;
            haveGround = pw->ShapeCast(capsule, feet + toCenter, Quat(), Vec3(0, -1, 0), Gap * 3.0f, groundHit, ~0u, self) &&
                         (groundHit.normal.y >= walkable || supportNormal(groundHit).y >= walkable);
            feet = start;
        }
        st.groundBodyIndex = ~0u;
        if (st.grounded && haveGround) {
            st.groundBodyIndex = groundHit.body.index;
            st.groundBodyGeneration = groundHit.body.generation;
            st.groundLocal = Conjugate(pw->GetRotation(groundHit.body)) * (feet - pw->GetPosition(groundHit.body));
            st.groundNormal = groundHit.normal;
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
