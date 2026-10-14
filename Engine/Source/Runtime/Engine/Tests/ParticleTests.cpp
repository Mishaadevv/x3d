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
