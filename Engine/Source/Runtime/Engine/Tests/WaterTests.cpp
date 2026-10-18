#include "Core/Testing/Test.h"
#include <cmath>
#include "Engine/PhysicsSystem.h"
#include "Engine/SceneRendering.h"
#include "Engine/WaterPhysics.h"
#include "Engine/Wind.h"
#include "Render/OceanWaves.h"
#include "Render/Water.h"
#include "Scene/Hierarchy.h"

using namespace zx;

ZX_TEST(Water, GatherKeepsTheSurfaceLevel)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    // Tilted and scaled entity: the surface keeps only the position and the yaw; the size comes from the component.
    Transform t(Vec3(3, 1, -2), Quat::FromEulerDegrees(Vec3(20, 90, 10)), Vec3(5, 5, 5));
    const ecs::Entity e = scene::CreateNode(world, "Sea", t);
    Water w;
    w.size = Vec2(80, 40);
    w.windDirection = 90.0f;
    world.Add(e, w);
    scene::UpdateWorldTransforms(world);

    Array<render::WaterSurface> surfaces;
    GatherWater(world, surfaces);
    ZX_REQUIRE(surfaces.Size() == 1u);
    const render::WaterSurface& s = surfaces[0];
    ZX_EXPECT(IsNearlyEqual(s.world.GetTranslation(), Vec3(3, 1, -2), 1e-4f));
    ZX_EXPECT(IsNearlyEqual(s.world.TransformVector(Vec3::Up()), Vec3::Up(), 1e-3f)); // level, unscaled
    ZX_EXPECT_NEAR(Length(s.world.TransformVector(Vec3(1, 0, 0))), 1.0f, 1e-4f);
    ZX_EXPECT(s.size == Vec2(80, 40));
    ZX_EXPECT_NEAR(s.windDirection, DegToRad(90.0f), 1e-5f);
}

ZX_TEST(Water, PresetsFillTheSettings)
{
    ecs::World world;
    Water w;
    w.size = Vec2(12, 7);
    w.preset = WaterPreset::Storm; // chosen in Details
    const ecs::Entity e = world.Create(w);
    UpdateWaterPresets(world);
    const Water& after = *world.Get<const Water>(e);
    ZX_EXPECT_GT(after.windSpeed, 15.0f);
    ZX_EXPECT_GT(after.foam, 1.0f);
    ZX_EXPECT(after.size == Vec2(12, 7)); // size is left alone
    // Tweaks after applying stay (the preset applies once).
    world.Get<Water>(e)->windSpeed = 3.0f;
    UpdateWaterPresets(world);
    ZX_EXPECT_EQ(world.Get<const Water>(e)->windSpeed, 3.0f);
    // Ocean and pool make different seas; the same settings share one simulation.
    render::WaterSurface ocean, pool, ocean2;
    ocean.windSpeed = 11.0f;
    ocean.fetch = 400.0f;
    pool.windSpeed = 3.0f;
    pool.fetch = 0.03f;
    ocean2 = ocean;
    ocean2.waveAmount = 2.0f; // looks are not part of the spectrum
    ZX_EXPECT(render::WaterSpectrumKey(ocean) != render::WaterSpectrumKey(pool));
    ZX_EXPECT(render::WaterSpectrumKey(ocean) == render::WaterSpectrumKey(ocean2));
}

// --- Waves for gameplay (CPU copy of the ocean) ----------------------------------------------------------------------

namespace {

render::WaterSurface OpenSea()
{
    render::WaterSurface s;
    s.windSpeed = 10.0f;
    s.fetch = 100.0f;
    s.windDirection = 0.4f;
    s.choppiness = 1.0f;
    return s;
}

} // namespace

ZX_TEST(Water, SmallSpectrumIsTheMiddleOfTheBigOne)
{
    // Lite (64), Full (128) and the CPU copy (64) share their big waves: same h0 at the same wave numbers.
    render::OceanSpectrum small, big;
    render::BuildOceanSpectrum(OpenSea(), 64, small);
    render::BuildOceanSpectrum(OpenSea(), 128, big);
    for (u32 c = 0; c < render::OceanCascades; ++c) {
        ZX_EXPECT_EQ(small.patchSize[c], big.patchSize[c]);
        u32 nonZero = 0;
        for (u32 y = 1; y < 64; ++y)
            for (u32 x = 1; x < 64; ++x) {
                const Vec4 a = small.h0[c][y * 64 + x];
                const Vec4 b = big.h0[c][(y + 32) * 128 + x + 32];
                ZX_EXPECT(IsNearlyEqual(a, b, 1e-7f));
                nonZero += a.x != 0.0f ? 1u : 0u;
            }
        ZX_EXPECT_GT(nonZero, 100u);
    }
}

ZX_TEST(Water, OceanWavesHaveARealisticHeight)
{
    render::OceanWaves waves;
    waves.Build(OpenSea());
    waves.Update(12.0f);
    // Significant wave height (4 standard deviations of the surface) of a 10 m/s wind over 100 km: ~1.6 m (JONSWAP).
    f64 sum = 0.0, sum2 = 0.0;
    u32 count = 0;
    for (f32 z = 0.0f; z < 200.0f; z += 2.0f)
        for (f32 x = 0.0f; x < 200.0f; x += 2.0f) {
            Vec3 d;
            waves.Sample(Vec2(x, z), d);
            sum += d.y;
            sum2 += f64(d.y) * d.y;
            ++count;
        }
    const f64 mean = sum / count;
    const f64 hs = 4.0 * std::sqrt(sum2 / count - mean * mean);
    ZX_EXPECT_LT(Abs(f32(mean)), 0.2f);
    ZX_EXPECT_GT(f32(hs), 0.6f);
    ZX_EXPECT_LT(f32(hs), 4.0f);
}

ZX_TEST(Water, OceanVelocityIsTheRateOfDisplacement)
{
    render::OceanWaves waves;
    waves.Build(OpenSea());
    const Vec2 p(17.0f, -43.0f);
    const f32 t = 30.0f, h = 0.01f;
    Vec3 a, b, d, v;
    waves.Update(t - h);
    waves.Sample(p, a);
    waves.Update(t + h);
    waves.Sample(p, b);
    waves.Update(t);
    waves.Sample(p, d, &v);
    const Vec3 rate = (b - a) / (2.0f * h);
    ZX_EXPECT_GT(Length(v), 0.05f);
    ZX_EXPECT_LT(Length(rate - v), 0.05f * Max(Length(v), 1.0f));
}

ZX_TEST(Water, SampleFindsTheSurface)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    const ecs::Entity e = scene::CreateNode(world, "Sea", Transform(Vec3(0, 2, 0)));
    Water w;
    w.size = Vec2(100, 60);
    world.Add(e, w);
    scene::UpdateWorldTransforms(world);
    TickWind(world, 5.0f); // the clock the waves run on
    WaterSystem water;
    water.Update(world);
    WaterSample s;
    ZX_REQUIRE(water.Sample(Vec3(10, -3, 5), s));
    ZX_EXPECT_NEAR(s.height, 2.0f, 1.5f);
    ZX_EXPECT_NEAR(s.depth, s.height + 3.0f, 1e-4f);
    ZX_EXPECT_GT(s.normal.y, 0.7f);
    ZX_EXPECT(s.water == e);
    ZX_EXPECT(!water.Sample(Vec3(60, 0, 0), s)); // outside the rectangle
}

namespace {

/// A 2 x 0.5 x 2 m plank (flat: stable even when light) dropped into calm water; returns how deep its center settles
/// (m below the surface).
f32 SettledDepth(f32 density, f32* finalHeight = nullptr)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    const ecs::Entity sea = scene::CreateNode(world, "Sea", Transform(Vec3(0, 0, 0)));
    Water w;
    w.size = Vec2(40, 40);
    w.waveAmount = 0.0f; // flat: equilibrium is exact
    world.Add(sea, w);
    const ecs::Entity box = scene::CreateNode(world, "Box", Transform(Vec3(0, 1.0f, 0)));
    world.Add(box, BoxCollider{Vec3(), Vec3(2.0f, 0.5f, 2.0f)});
    world.Add(box, RigidBody{});
    Buoyancy b;
    b.density = density;
    world.Add(box, b);
    scene::UpdateWorldTransforms(world);
    PhysicsSystem physics;
    physics.Begin(world, nullptr);
    WaterSystem water;
    physics.beforeStep = [&](f32 step) { water.TickBuoyancy(world, physics, step); };
    for (u32 i = 0; i < 60 * 12; ++i) {
        TickWind(world, 1.0f / 60.0f);
        water.Update(world);
        physics.Tick(world, 1.0f / 60.0f);
        scene::UpdateWorldTransforms(world);
    }
    const f32 y = world.Get<const Transform>(box)->position.y;
    if (finalHeight)
        *finalHeight = y;
    physics.End();
    return -y;
}

} // namespace

ZX_TEST(Water, WoodFloatsSteelSinks)
{
    // Wood (600 kg/m^3): 60 % of the plank under water -> its center 0.05 m below the surface.
    ZX_EXPECT_NEAR(SettledDepth(600.0f), 0.05f, 0.03f);
    // Light foam floats high (20 % under: center 0.15 m above).
    ZX_EXPECT_NEAR(SettledDepth(200.0f), -0.15f, 0.03f);
    // Steel keeps sinking (no bottom here).
    ZX_EXPECT_GT(SettledDepth(7800.0f), 5.0f);
}
