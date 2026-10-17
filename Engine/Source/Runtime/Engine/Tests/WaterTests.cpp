#include "Core/Testing/Test.h"
#include "Engine/SceneRendering.h"
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
