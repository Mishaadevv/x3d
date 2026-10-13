#include "Core/Testing/Test.h"
#include "Engine/SceneRendering.h"
#include "Engine/ViewportRenderer.h"
#include "Scene/Hierarchy.h"

using namespace zx;

ZX_TEST(ReflectionProbe, GathersBoxAndKey)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    Transform t(Vec3(10, 2, -4));
    t.scale = Vec3(2, 1, 1);
    const ecs::Entity e = scene::CreateNode(world, "Probe", t);
    ReflectionProbe p;
    p.extent = Vec3(3, 2, 5);
    p.priority = 4;
    p.blendDistance = 0.5f;
    world.Add(e, p);
    scene::UpdateWorldTransforms(world);

    Array<render::ReflectionProbeDesc> probes;
    GatherReflectionProbes(world, probes, 123);
    ZX_REQUIRE(probes.Size() == 1u);
    const render::ReflectionProbeDesc& d = probes[0];
    ZX_EXPECT_NEAR(d.position.x, 10.0f, 1e-5f);
    ZX_EXPECT_NEAR(d.boxMin.x, 4.0f, 1e-5f); // extent x 3 * scale 2
    ZX_EXPECT_NEAR(d.boxMax.x, 16.0f, 1e-5f);
    ZX_EXPECT_NEAR(d.boxMin.z, -9.0f, 1e-5f);
    ZX_EXPECT_NEAR(d.boxMax.y, 4.0f, 1e-5f);
    ZX_EXPECT_EQ(d.priority, 4);
    ZX_EXPECT_NEAR(d.blendDistance, 0.5f, 1e-6f);
    ZX_EXPECT(d.id != 0u);

    // The capture key follows the scene content and the probe's placement.
    Array<render::ReflectionProbeDesc> again;
    GatherReflectionProbes(world, again, 123);
    ZX_EXPECT_EQ(again[0].key, d.key);
    again.Clear();
    GatherReflectionProbes(world, again, 124);
    ZX_EXPECT_NE(again[0].key, d.key);
    world.Get<Transform>(e)->position.y = 3.0f;
    scene::UpdateWorldTransforms(world);
    again.Clear();
    GatherReflectionProbes(world, again, 123);
    ZX_EXPECT_NE(again[0].key, d.key);
}

ZX_TEST(ReflectionProbe, SceneContentKeyChangesWithTheScene)
{
    const asset::AssetId mats[1] = {};
    render::RenderInstance a;
    a.world = Mat4::Translation(Vec3(1, 0, 0));
    a.materials = Span<const asset::AssetId>(mats, 1);
    render::LightDesc l;
    l.position = Vec3(0, 3, 0);
    const u64 k0 = SceneContentKey(Span<const render::RenderInstance>(&a, 1), Span<const render::LightDesc>(&l, 1));
    ZX_EXPECT_EQ(k0, SceneContentKey(Span<const render::RenderInstance>(&a, 1), Span<const render::LightDesc>(&l, 1)));
    render::RenderInstance moved = a;
    moved.world = Mat4::Translation(Vec3(1, 0, 0.01f));
    ZX_EXPECT_NE(k0, SceneContentKey(Span<const render::RenderInstance>(&moved, 1), Span<const render::LightDesc>(&l, 1)));
    render::LightDesc brighter = l;
    brighter.intensity *= 2.0f;
    ZX_EXPECT_NE(k0, SceneContentKey(Span<const render::RenderInstance>(&a, 1), Span<const render::LightDesc>(&brighter, 1)));
    ZX_EXPECT_NE(k0, SceneContentKey({}, Span<const render::LightDesc>(&l, 1)));
}

ZX_TEST(DynamicResolution, FollowsTheFrameTime)
{
    DynamicResolutionSettings d;
    d.enabled = true;
    d.targetMs = 16.0f;
    d.minScale = 0.5f;
    d.maxScale = 1.0f;
    // Too slow: the scale drops (in 5 % steps), never below the minimum.
    f32 s = 1.0f;
    s = NextDynamicResolutionScale(s, 30.0f, d);
    ZX_EXPECT_LT(s, 1.0f);
    ZX_EXPECT_NEAR(s * 20.0f, Round(s * 20.0f), 1e-4f);
    for (u32 i = 0; i < 50; ++i)
        s = NextDynamicResolutionScale(s, 40.0f, d);
    ZX_EXPECT_NEAR(s, 0.5f, 1e-6f);
    // Fast: back up to the maximum.
    for (u32 i = 0; i < 50; ++i)
        s = NextDynamicResolutionScale(s, 6.0f, d);
    ZX_EXPECT_NEAR(s, 1.0f, 1e-6f);
    // On target: stays.
    ZX_EXPECT_NEAR(NextDynamicResolutionScale(0.75f, 16.0f, d), 0.75f, 1e-6f);
}
