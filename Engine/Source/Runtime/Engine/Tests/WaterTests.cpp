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
