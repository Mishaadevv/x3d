// Foliage: instances survive the packed form, the scatter rules respect slope and height bands, the same rule over the
// same area gives the same plants, and every instance becomes a render instance (within the cull distance).

#include "Core/Testing/Test.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/Foliage.h"
#include "Engine/SceneRendering.h"
#include "Engine/Wind.h"
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

ZX_TEST(Foliage, TrunkFromTheMeshBase)
{
    // A 10 m "tree": a trunk of radius 0.3 m (two rings at 0 and 4 m) under a wide canopy from 3 to 10 m.
    Array<Vec3> points;
    for (u32 i = 0; i < 16; ++i) {
        const f32 a = f32(i) * TwoPi / 16.0f;
        for (f32 y : {0.0f, 4.0f})
            points.PushBack(Vec3(2.0f + Cos(a) * 0.3f, y, -1.0f + Sin(a) * 0.3f));
        for (f32 y : {3.0f, 10.0f})
            points.PushBack(Vec3(2.0f + Cos(a) * 2.5f, y, -1.0f + Sin(a) * 2.5f));
    }
    FoliageTrunk t;
    ZX_REQUIRE(EstimateFoliageTrunk(points.View(), t));
    ZX_EXPECT_LT(Length(t.base - Vec3(2.0f, 0.0f, -1.0f)), 1e-3f);
    ZX_EXPECT_NEAR(t.radius, 0.3f, 1e-3f);
    ZX_EXPECT_NEAR(t.height, 5.0f, 1e-3f);
    ZX_EXPECT(!EstimateFoliageTrunk({}, t));
}

ZX_TEST(Foliage, CollisionAndWindByModelName)
{
    auto make = [](const char* model) {
        Foliage f;
        f.model = String::Format("Packs/StylizedNature/{}.gltf", model);
        return f;
    };
    ZX_EXPECT(FoliageCollisionOf(make("Tree_3")) == FoliageCollision::Trunk);
    ZX_EXPECT(FoliageCollisionOf(make("Pine_2")) == FoliageCollision::Trunk);
    ZX_EXPECT(FoliageCollisionOf(make("Dead_Tree")) == FoliageCollision::Trunk);
    ZX_EXPECT(FoliageCollisionOf(make("Rock_Medium_2")) == FoliageCollision::Hull);
    ZX_EXPECT(FoliageCollisionOf(make("Grass_Wispy")) == FoliageCollision::None);
    ZX_EXPECT(FoliageCollisionOf(make("Bush_with_Flowers")) == FoliageCollision::None);
    Foliage forced = make("Grass");
    forced.collision = FoliageCollision::Hull;
    ZX_EXPECT(FoliageCollisionOf(forced) == FoliageCollision::Hull);

    ZX_EXPECT_EQ(FoliageWindSway(make("Rock_Medium")), 0.0f);
    ZX_EXPECT_EQ(FoliageWindSway(make("Tall_Grass")), 3.0f);
    ZX_EXPECT_EQ(FoliageWindSway(make("Bush")), 1.5f);
    ZX_EXPECT_EQ(FoliageWindSway(make("Tree")), 1.0f);
    Foliage stiff = make("Tall_Grass");
    stiff.windSway = 0.5f;
    ZX_EXPECT_EQ(FoliageWindSway(stiff), 0.5f);
}

ZX_TEST(Foliage, PlantsGetTheWindRocksDoNot)
{
    RegisterEngineComponents();
    ecs::World w;
    const ecs::Entity zone = scene::CreateNode(w, "Wind", Transform(Vec3(), Quat::FromAxisAngle(Vec3(0, 1, 0), HalfPi))); // blows +X
    WindZone z;
    z.speed = 10.0f;
    z.turbulence = 0.0f;
    z.gustStrength = 0.0f;
    w.Add(zone, z);
    auto plants = [&](const char* model, Vec3 at) {
        const ecs::Entity e = scene::CreateNode(w, model, Transform(at));
        Foliage f;
        f.model = String::Format("{}.gltf", model);
        FoliagePart part;
        part.mesh = Guid::New();
        f.parts.PushBack(part);
        const FoliageInstance in[] = {{Vec3(), 0.0f, 1.0f}};
        EncodeFoliage(f, in);
        w.Add(e, move(f));
        return e;
    };
    plants("Grass", Vec3(0, 0, 0));
    plants("Rock_Medium", Vec3(5, 0, 0));
    scene::UpdateWorldTransforms(w);
    TickWind(w, 0.1f);
    Array<render::RenderInstance> out;
    GatherRenderInstances(w, out);
    ZX_REQUIRE(out.Size() == 2u);
    const render::RenderInstance& grass = out[0].world.GetTranslation().x < 1.0f ? out[0] : out[1];
    const render::RenderInstance& rock = out[0].world.GetTranslation().x < 1.0f ? out[1] : out[0];
    ZX_EXPECT_NEAR(grass.wind.x, 30.0f, 0.5f); // 10 m/s along +X, grass sways 3x
    ZX_EXPECT_NEAR(grass.wind.y, 0.0f, 0.5f);
    ZX_EXPECT(rock.wind == Vec2());
}
