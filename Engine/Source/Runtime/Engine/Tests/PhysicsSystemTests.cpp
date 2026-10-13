#include "Core/Testing/Test.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/Gameplay.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/SceneSerialization.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

struct Scene
{
    ecs::World world;
    Scene() { scene::InstallHierarchyHooks(world); }

    ecs::Entity Floor()
    {
        const ecs::Entity e = scene::CreateNode(world, "Floor", Transform(Vec3(0, -0.5f, 0), Quat(), Vec3(40, 1, 40)));
        world.Add(e, BoxCollider{}); // unit box scaled by the transform
        return e;
    }
    void Run(PhysicsSystem& p, f32 seconds)
    {
        for (u32 i = 0; i < u32(seconds * 60.0f); ++i) {
            scene::UpdateWorldTransforms(world);
            p.Tick(world, 1.0f / 60.0f);
        }
        scene::UpdateWorldTransforms(world);
    }
};

} // namespace

ZX_TEST(PhysicsSystem, RigidBodyFallsOntoScaledCollider)
{
    Scene s;
    s.Floor();
    const ecs::Entity cube = scene::CreateNode(s.world, "Cube", Transform(Vec3(0, 3, 0), Quat::FromEulerDegrees(Vec3(0, 30, 0)), Vec3(2, 2, 2)));
    s.world.Add(cube, BoxCollider{});
    s.world.Add(cube, RigidBody{});
    scene::UpdateWorldTransforms(s.world);
    PhysicsSystem p;
    p.Begin(s.world, nullptr);
    ZX_EXPECT(p.World()->BodyCount() == 2u);
    s.Run(p, 3.0f);
    // Scale 2: a 2 m cube rests with its center 1 m up; the entity keeps its scale.
    const Transform* t = s.world.Get<Transform>(cube);
    ZX_EXPECT_NEAR(t->position.y, 1.0f, 0.03f);
    ZX_EXPECT(t->scale == Vec3(2, 2, 2));
    ZX_EXPECT(p.World()->IsSleeping(p.BodyOf(cube)));
    p.End();
}

ZX_TEST(PhysicsSystem, ChildCollidersMakeACompoundAndChildrenFollow)
{
    Scene s;
    s.Floor();
    // A parent with a Rigid Body and two child colliders (a "dumbbell"), under a moved root.
    const ecs::Entity root = scene::CreateNode(s.world, "Root", Transform(Vec3(10, 0, 0)));
    const ecs::Entity body = scene::CreateNode(s.world, "Body", Transform(Vec3(0, 2, 0)), root);
    s.world.Add(body, RigidBody{});
    const ecs::Entity left = scene::CreateNode(s.world, "Left", Transform(Vec3(-1, 0, 0)), body);
    const ecs::Entity right = scene::CreateNode(s.world, "Right", Transform(Vec3(1, 0, 0)), body);
    s.world.Add(left, SphereCollider{});
    s.world.Add(right, SphereCollider{});
    scene::UpdateWorldTransforms(s.world);
    PhysicsSystem p;
    p.Begin(s.world, nullptr);
    ZX_EXPECT(p.World()->BodyCount() == 2u); // floor + one compound body
    ZX_EXPECT(p.World()->GetShapes(p.BodyOf(body)).Size() == 2u);
    s.Run(p, 3.0f);
    // The body rests on its spheres (radius 0.5): world y = 0.5, the local transform is relative to the root.
    const Vec3 world = s.world.Get<scene::WorldTransform>(body)->GetPosition();
    ZX_EXPECT_NEAR(world.y, 0.5f, 0.03f);
    ZX_EXPECT_NEAR(world.x, 10.0f, 0.05f);
    ZX_EXPECT_NEAR(s.world.Get<Transform>(body)->position.x, 0.0f, 0.05f);
    ZX_EXPECT_NEAR(s.world.Get<scene::WorldTransform>(left)->GetPosition().y, 0.5f, 0.03f);
    p.End();
}

ZX_TEST(PhysicsSystem, KinematicFollowsAndTeleports)
{
    Scene s;
    s.Floor();
    const ecs::Entity box = scene::CreateNode(s.world, "Box", Transform(Vec3(0, 0.5f, 0)));
    s.world.Add(box, BoxCollider{});
    s.world.Add(box, RigidBody{});
    const ecs::Entity pusher = scene::CreateNode(s.world, "Pusher", Transform(Vec3(-3, 0.5f, 0)));
    s.world.Add(pusher, BoxCollider{});
    RigidBody kinematic;
    kinematic.type = RigidBodyType::Kinematic;
    s.world.Add(pusher, kinematic);
    scene::UpdateWorldTransforms(s.world);
    PhysicsSystem p;
    p.Begin(s.world, nullptr);
    s.Run(p, 0.5f);
    // The game moves the kinematic entity; it pushes the box.
    for (u32 i = 0; i < 120; ++i) {
        s.world.Get<Transform>(pusher)->position.x += 2.0f / 60.0f;
        s.Run(p, 1.0f / 60.0f);
    }
    ZX_EXPECT(s.world.Get<Transform>(box)->position.x > 0.9f);
    // The game teleports the dynamic box: it continues from there, at rest.
    s.world.Get<Transform>(box)->position = Vec3(5, 4, 5);
    s.Run(p, 2.0f);
    const Vec3 b = s.world.Get<Transform>(box)->position;
    ZX_EXPECT_NEAR(b.x, 5.0f, 0.05f);
    ZX_EXPECT_NEAR(b.y, 0.5f, 0.03f);
    p.End();
}

ZX_TEST(PhysicsSystem, ComponentsSaveAndLoad)
{
    RegisterEngineComponents();
    Scene s;
    const ecs::Entity e = scene::CreateNode(s.world, "Thing");
    RigidBody rb;
    rb.mass = 12.0f;
    rb.type = RigidBodyType::Kinematic;
    s.world.Add(e, rb);
    s.world.Add(e, CapsuleCollider{Vec3(0, 1, 0), 0.3f, 1.8f});
    const JsonValue json = SaveScene(s.world, SceneIOOptions{});
    Scene loaded;
    ZX_REQUIRE(LoadScene(json, loaded.world, SceneIOOptions{}));
    u32 found = 0;
    loaded.world.Query<const RigidBody, const CapsuleCollider>().Each([&](const RigidBody& r, const CapsuleCollider& c) {
        found += r.mass == 12.0f && r.type == RigidBodyType::Kinematic && c.height == 1.8f && c.center == Vec3(0, 1, 0) ? 1 : 0;
    });
    ZX_EXPECT_EQ(found, 1u);
}
