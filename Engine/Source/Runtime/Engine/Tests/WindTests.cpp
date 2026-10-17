// Wind: zones (directional, spherical falloff, gusts), per-object Wind Response (strength, shelter, own wind), particles
// drifting with the wind, rigid bodies pushed by it.

#include "Core/Testing/Test.h"
#include "Engine/GameRuntime.h"
#include "Engine/Particles.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/Wind.h"
#include "Render/Particles.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

ecs::Entity AddZone(ecs::World& world, WindZone z, Vec3 position = {}, Quat rotation = {})
{
    const ecs::Entity e = scene::CreateNode(world, "Wind", Transform(position, rotation));
    world.Add(e, z);
    scene::UpdateWorldTransforms(world);
    return e;
}

WindZone Steady(f32 speed)
{
    WindZone z;
    z.speed = speed;
    z.turbulence = 0.0f;
    z.gustStrength = 0.0f;
    return z;
}

} // namespace

ZX_TEST(Wind, ZonesAndPerObjectResponse)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    ZX_EXPECT(!HasWind(world));
    ZX_EXPECT(Length(WindAt(world, Vec3(0.0f))) == 0.0f); // still air
    // Directional along the entity's forward: rotated 90 degrees about Y -> towards +X.
    AddZone(world, Steady(8.0f), Vec3(0.0f), Quat::LookRotation(Vec3(1, 0, 0), Vec3(0, 1, 0)));
    ZX_EXPECT(HasWind(world));
    const Vec3 w = WindAt(world, Vec3(100, 3, -50)); // everywhere
    ZX_EXPECT_NEAR(w.x, 8.0f, 1e-3f);
    ZX_EXPECT_NEAR(Length(w), 8.0f, 1e-3f);

    // Spherical: out from its center, fading to nothing at the radius; zones add up.
    WindZone sphere = Steady(10.0f);
    sphere.mode = WindZoneMode::Spherical;
    sphere.radius = 10.0f;
    AddZone(world, sphere, Vec3(0, 0, 50));
    const Vec3 near = WindAt(world, Vec3(0, 0, 52)) - w;
    ZX_EXPECT_GT(near.z, 5.0f); // blowing outward (+Z from the center)
    ZX_EXPECT(Length(WindAt(world, Vec3(0, 0, 61)) - w) < 1e-4f);

    // Per object: half the wind, sheltered, or its own wind.
    const ecs::Entity half = scene::CreateNode(world, "Half");
    world.Add(half, WindResponse{0.5f});
    ZX_EXPECT_NEAR(WindFor(world, half, Vec3(0.0f)).x, 4.0f, 1e-3f);
    const ecs::Entity child = scene::CreateNode(world, "Child", Transform(), half); // inherits the parent's response
    ZX_EXPECT_NEAR(WindFor(world, child, Vec3(0.0f)).x, 4.0f, 1e-3f);
    const ecs::Entity sheltered = scene::CreateNode(world, "Sheltered");
    world.Add(sheltered, WindResponse{0.0f});
    ZX_EXPECT(Length(WindFor(world, sheltered, Vec3(0.0f))) == 0.0f);
    WindResponse own;
    own.customWind = true;
    own.customSpeed = 3.0f;
    own.customDirection = 0.0f; // towards +Z
    const ecs::Entity custom = scene::CreateNode(world, "Own");
    world.Add(custom, own);
    const Vec3 c = WindFor(world, custom, Vec3(0.0f));
    ZX_EXPECT_NEAR(c.z, 3.0f, 1e-4f);
    ZX_EXPECT_NEAR(c.x, 0.0f, 1e-4f);

    // Gusts make the speed vary over time (around the base speed, never below it).
    WindZone gusty = Steady(5.0f);
    gusty.gustStrength = 1.0f;
    gusty.gustFrequency = 0.5f;
    ecs::World g;
    scene::InstallHierarchyHooks(g);
    AddZone(g, gusty);
    f32 lo = 1e9f, hi = 0.0f;
    for (u32 i = 0; i < 600; ++i) {
        TickWind(g, 1.0f / 60.0f);
        const f32 s = Length(WindAt(g, Vec3(0.0f)));
        lo = Min(lo, s);
        hi = Max(hi, s);
    }
    ZX_EXPECT_GE(lo, 5.0f - 1e-3f);
    ZX_EXPECT_GT(hi - lo, 1.0f);
}

ZX_TEST(Wind, ParticlesDriftAndBodiesArePushed)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    AddZone(world, Steady(6.0f), Vec3(0.0f), Quat::LookRotation(Vec3(1, 0, 0), Vec3(0, 1, 0)));
    // Smoke-like particles (drag 2/s) take the wind's speed within a couple of seconds.
    ParticleEmitter e;
    e.shape = EmitterShape::Point;
    e.rate = 0.0f;
    e.burst = 10;
    e.speed = 0.0f;
    e.speedVariation = 0.0f;
    e.acceleration = Vec3(0.0f);
    e.drag = 2.0f;
    e.turbulence = 0.0f;
    e.lifetime = 10.0f;
    const ecs::Entity emitter = scene::CreateNode(world, "Smoke", Transform(Vec3(0, 2, 0)));
    world.Add(emitter, e);
    scene::UpdateWorldTransforms(world);
    ParticleSystem ps;
    for (u32 i = 0; i < 180; ++i)
        ps.Tick(world, 1.0f / 60.0f, true);
    Array<render::GpuParticle> out;
    ps.Gather(Vec3(0, 0, -10), out);
    ZX_REQUIRE(!out.Empty());
    ZX_EXPECT_GT(out[0].position.x, 8.0f); // ~6 m/s for most of 3 s, minus the spin-up
    // Sheltered emitter: they stay put.
    world.Add(emitter, WindResponse{0.0f});
    ParticleSystem still;
    for (u32 i = 0; i < 120; ++i)
        still.Tick(world, 1.0f / 60.0f, true);
    out.Clear();
    still.Gather(Vec3(0, 0, -10), out);
    ZX_REQUIRE(!out.Empty());
    ZX_EXPECT_NEAR(out[0].position.x, 0.0f, 0.05f);

    // A light box with a Wind Response slides downwind; one without stays.
    const ecs::Entity floor = scene::CreateNode(world, "Floor", Transform(Vec3(0, -0.5f, 0), Quat(), Vec3(200, 1, 200)));
    world.Add(floor, BoxCollider{});
    auto box = [&](StringView name, f32 z, bool windy) {
        const ecs::Entity b = scene::CreateNode(world, name, Transform(Vec3(0, 0.5f, z)));
        world.Add(b, BoxCollider{});
        RigidBody rb;
        rb.mass = 2.0f;
        rb.friction = 0.2f;
        world.Add(b, rb);
        if (windy) {
            WindResponse r;
            r.drag = 3.0f;
            world.Add(b, r);
        }
        return b;
    };
    const ecs::Entity windy = box("Windy", 0.0f, true);
    const ecs::Entity calm = box("Calm", 5.0f, false);
    scene::UpdateWorldTransforms(world);
    PhysicsSystem physics;
    physics.Begin(world, nullptr);
    GameSystems systems;
    systems.physics = &physics;
    for (u32 i = 0; i < 120; ++i)
        TickGame(world, systems, 1.0f / 60.0f, nullptr, ListenerPose{}, false);
    ZX_EXPECT_GT(world.Get<const Transform>(windy)->position.x, 0.5f);
    ZX_EXPECT_NEAR(world.Get<const Transform>(calm)->position.x, 0.0f, 0.01f);
    physics.End();
}
