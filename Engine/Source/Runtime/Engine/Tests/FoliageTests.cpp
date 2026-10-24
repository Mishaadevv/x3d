// Foliage: instances survive the packed form, the scatter rules respect slope and height bands, the same rule over the
// same area gives the same plants, and every instance becomes a render instance (within the cull distance).

#include "Core/Testing/Test.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/Foliage.h"
#include "Engine/SceneRendering.h"
#include "Scene/Hierarchy.h"

using namespace zx;

ZX_TEST(Foliage, PackedInstancesRoundTrip)
{
    Foliage f;
    const FoliageInstance in[] = {{Vec3(1, 2, 3), 0.5f, 1.2f}, {Vec3(-4, 0, 9), 3.0f, 0.8f}};
    EncodeFoliage(f, in);
    Foliage g;
    g.data = f.data;
    g.dataId = f.dataId;
    DecodeFoliage(g);
    ZX_REQUIRE(g.instances.Size() == 2);
    ZX_EXPECT(g.instances[1].position == Vec3(-4, 0, 9));
    ZX_EXPECT_NEAR(g.instances[0].yaw, 0.5f, 1e-6f);
    ZX_EXPECT_NEAR(g.instances[1].scale, 0.8f, 1e-6f);
}

ZX_TEST(Foliage, ScatterFollowsTheRules)
{
    // Ground: flat at y = 0 for x < 0, a steep slope (rise 2 per meter) for x >= 0.
    auto ground = [](f32 x, f32) { return x < 0.0f ? 0.0f : x * 2.0f; };
    FoliageRule rule;
    rule.density = 0.05f;
    rule.maxSlope = 0.5f;
    rule.maxHeight = 50.0f;
    Array<FoliageInstance> a, b;
    ScatterFoliage(rule, Vec2(-100, -100), Vec2(200, 200), ground, a);
    ScatterFoliage(rule, Vec2(-100, -100), Vec2(200, 200), ground, b);
    ZX_EXPECT(a.Size() > 500u);
    ZX_EXPECT_EQ(a.Size(), b.Size()); // deterministic
    bool onSteep = false;
    for (const FoliageInstance& in : a)
        onSteep |= in.position.x > 2.0f;
    ZX_EXPECT(!onSteep);
    // Clumping thins plants out to patches.
    rule.clumping = 1.0f;
    Array<FoliageInstance> clumped;
    ScatterFoliage(rule, Vec2(-100, -100), Vec2(200, 200), ground, clumped);
    ZX_EXPECT(clumped.Size() < a.Size());
}

ZX_TEST(Foliage, InstancesBecomeRenderInstances)
{
    RegisterEngineComponents();
    ecs::World w;
    const ecs::Entity e = scene::CreateNode(w, "Plants", Transform(Vec3(100, 0, 0)));
    Foliage f;
    FoliagePart part;
    part.mesh = Guid::New();
    f.parts.PushBack(part);
    f.cullDistance = 50.0f;
    const FoliageInstance in[] = {{Vec3(0, 0, 0), 0.0f, 1.0f}, {Vec3(0, 0, 500), 0.0f, 1.0f}};
    EncodeFoliage(f, in);
    w.Add(e, move(f));
    scene::UpdateWorldTransforms(w);
    Array<render::RenderInstance> all, near;
    GatherRenderInstances(w, all);
    ZX_EXPECT_EQ(all.Size(), 2u);
    const Vec3 viewer(100, 0, 0);
    GatherRenderInstances(w, near, nullptr, &viewer);
    ZX_REQUIRE(near.Size() == 1u);
    ZX_EXPECT(near[0].world.GetTranslation() == Vec3(100, 0, 0));
}
