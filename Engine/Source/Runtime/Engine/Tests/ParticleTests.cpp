// Particle Emitter simulation: emission rate and bursts, lifetime, motion, editor preview, scripted bursts, sorting.

#include "Core/Testing/Test.h"
#include "Engine/Particles.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

ecs::Entity AddEmitter(ecs::World& world, const ParticleEmitter& e, Vec3 position = {})
{
    const ecs::Entity entity = scene::CreateNode(world, "Emitter", Transform(position));
    world.Add(entity, e);
    scene::UpdateWorldTransforms(world);
    return entity;
}

void Run(ParticleSystem& ps, ecs::World& world, f32 seconds, bool playing)
{
    for (u32 i = 0; i < u32(seconds * 60.0f); ++i)
        ps.Tick(world, 1.0f / 60.0f, playing);
}

} // namespace

ZX_TEST(Particles, RateLifetimeAndBurst)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    ParticleEmitter e;
    e.rate = 30.0f;
    e.lifetime = 1.0f;
    e.lifetimeVariation = 0.0f;
    e.burst = 50;
    e.maxParticles = 1000;
    AddEmitter(world, e);
    ParticleSystem ps;
    ps.Tick(world, 1.0f / 60.0f, true);
    ZX_EXPECT(ps.ParticleCount() >= 50u); // the burst on start
    Run(ps, world, 3.0f, true);
    // Steady state: rate x lifetime alive (the burst has died out).
    ZX_EXPECT(ps.ParticleCount() >= 27u && ps.ParticleCount() <= 32u);
}

ZX_TEST(Particles, MotionAndEditorPreview)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    ParticleEmitter e;
    e.shape = EmitterShape::Point;
    e.rate = 0.0f;
    e.burst = 20;
    e.speed = 0.0f;
    e.speedVariation = 0.0f;
    e.acceleration = Vec3(0.0f, -10.0f, 0.0f);
    e.drag = 0.0f;
    e.turbulence = 0.0f;
    e.lifetime = 5.0f;
    e.simulateInEditor = false;
    const ecs::Entity entity = AddEmitter(world, e, Vec3(0, 10, 0));
    ParticleSystem ps;
    // Not previewed while editing.
    Run(ps, world, 0.5f, false);
    ZX_EXPECT_EQ(ps.ParticleCount(), 0u);
    // Playing: they fall under the acceleration (semi-implicit Euler: ~0.5 a t^2).
    Run(ps, world, 1.0f, true);
    Array<render::GpuParticle> out;
    ps.Gather(Vec3(0, 0, -10), out);
    ZX_REQUIRE(!out.Empty());
    ZX_EXPECT(Abs(out[0].position.y - 5.0f) < 0.3f);
    // Scripted burst.
    const u32 before = ps.ParticleCount();
    ps.Emit(entity, 7);
    ps.Tick(world, 1.0f / 60.0f, true);
    ZX_EXPECT_EQ(ps.ParticleCount(), before + 7u);
}

ZX_TEST(Particles, SortedBackToFront)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    ParticleEmitter e;
    e.shape = EmitterShape::Box;
    e.boxSize = Vec3(20.0f);
    e.rate = 0.0f;
    e.burst = 200;
    e.speed = 0.0f;
    e.lifetime = 10.0f;
    e.fadeIn = 0.0f; // visible from the first frame
    AddEmitter(world, e);
    ParticleSystem ps;
    ps.Tick(world, 1.0f / 60.0f, true);
    Array<render::GpuParticle> out;
    const Vec3 camera(0, 0, -30);
    ps.Gather(camera, out);
    ZX_REQUIRE(out.Size() == 200u);
    for (usize i = 1; i < out.Size(); ++i)
        if (DistanceSq(out[i - 1].position, camera) < DistanceSq(out[i].position, camera)) {
            test::ReportFailure(__FILE__, __LINE__, "particles not sorted back to front");
            break;
        }
}

ZX_TEST(Particles, CollisionBounceDieAndSplash)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    // Falling particles over a floor at y = 0.
    ParticleEmitter drops;
    drops.shape = EmitterShape::Point;
    drops.rate = 0.0f;
    drops.burst = 50;
    drops.speed = 0.0f;
    drops.speedVariation = 0.0f;
    drops.acceleration = Vec3(0.0f, -10.0f, 0.0f);
    drops.drag = 0.0f;
    drops.turbulence = 0.0f;
    drops.lifetime = 5.0f;
    drops.lifetimeVariation = 0.0f;
    drops.collision = ParticleCollision::Die;
    drops.onHitEmitter = "Splash";
    drops.onHitCount = 2;
    AddEmitter(world, drops, Vec3(0, 3, 0));
    ParticleEmitter bouncy = drops;
    bouncy.collision = ParticleCollision::Bounce;
    bouncy.onHitEmitter.Clear();
    AddEmitter(world, bouncy, Vec3(10, 3, 0));
    ParticleEmitter splash;
    splash.rate = 0.0f;
    splash.lifetime = 10.0f;
    splash.maxParticles = 1000;
    const ecs::Entity splashEntity = scene::CreateNode(world, "Splash", Transform(Vec3(0, -50, 0)));
    world.Add(splashEntity, splash);
    scene::UpdateWorldTransforms(world);
    ParticleSystem ps;
    ps.raycast = [](Vec3 from, Vec3 to, Vec3& point, Vec3& normal) {
        if (from.y < 0.0f || to.y >= 0.0f)
            return false;
        const f32 t = from.y / (from.y - to.y);
        point = Lerp(from, to, t);
        normal = Vec3(0, 1, 0);
        return true;
    };
    Run(ps, world, 1.5f, true); // ~0.77 s to fall 3 m
    Array<render::GpuParticle> out;
    ps.Gather(Vec3(0, 0, -10), out);
    u32 bounce = 0, splashes = 0;
    for (const render::GpuParticle& g : out) {
        ZX_EXPECT(g.position.y > -0.05f); // nothing fell through the floor
        if (g.position.x > 5.0f)
            ++bounce;
        else
            ++splashes; // the drops died: what is left near them are the splashes
    }
    ZX_EXPECT_EQ(bounce, 50u);    // bounced, still alive
    ZX_EXPECT_EQ(splashes, 100u); // 50 drops x 2 splash particles, from the floor
}

ZX_TEST(Particles, StartDelayAndRepeatedBursts)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    ParticleEmitter e;
    e.rate = 0.0f;
    e.burst = 20;
    e.burstInterval = 1.0f;
    e.startDelay = 0.5f;
    e.lifetime = 0.3f;
    e.lifetimeVariation = 0.0f;
    AddEmitter(world, e);
    ParticleSystem ps;
    Run(ps, world, 0.4f, true);
    ZX_EXPECT_EQ(ps.ParticleCount(), 0u); // still waiting
    Run(ps, world, 0.2f, true);           // 0.6 s: the first burst
    ZX_EXPECT_EQ(ps.ParticleCount(), 20u);
    Run(ps, world, 0.5f, true);           // 1.1 s: it has died out
    ZX_EXPECT_EQ(ps.ParticleCount(), 0u);
    Run(ps, world, 0.5f, true);           // 1.6 s: the second burst (0.5 + 1.0)
    ZX_EXPECT_EQ(ps.ParticleCount(), 20u);
}

ZX_TEST(Particles, OrbitKeepsTheRadiusAndGradientHasAMiddle)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    ParticleEmitter e;
    e.shape = EmitterShape::Ring;
    e.radius = 2.0f;
    e.coneAngle = 0.0f;
    e.speed = 0.0f;
    e.acceleration = Vec3();
    e.turbulence = 0.0f;
    e.drag = 0.0f;
    e.orbit = 3.0f;
    e.rate = 0.0f;
    e.burst = 10;
    e.lifetime = 4.0f;
    e.lifetimeVariation = 0.0f;
    e.startColor = Vec3(1, 0, 0);
    e.midColor = Vec3(0, 1, 0);
    e.midColorTime = 0.5f;
    e.endColor = Vec3(0, 0, 1);
    e.emissive = false;
    AddEmitter(world, e, Vec3(5, 1, 0));
    ParticleSystem ps;
    Run(ps, world, 2.0f, true); // half the life: the middle color
    Array<render::GpuParticle> out;
    ps.Gather(Vec3(0, 0, -10), out);
    ZX_REQUIRE(out.Size() == 10u);
    for (const render::GpuParticle& p : out) {
        const Vec3 d = p.position - Vec3(5, 1, 0);
        ZX_EXPECT_NEAR(Length(Vec3(d.x, 0, d.z)), 2.0f, 0.02f); // still on the ring after turning ~6 rad
        ZX_EXPECT_NEAR(d.y, 0.0f, 1e-3f);
        ZX_EXPECT_GT(p.color.y, 0.9f);
        ZX_EXPECT_LT(p.color.x, 0.1f);
    }
}
