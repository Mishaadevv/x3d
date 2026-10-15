#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Log/Log.h"
#include "Core/Testing/Test.h"
#include "Engine/Scripting.h"
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

ZX_TEST(PhysicsSystem, ScriptsGetCollisionsAndQueries)
{
    const String root = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_script_test/physics");
    platform::DeleteDirectoryTree(root);
    platform::CreateDirectoryTree(root + "/Scripts");
    // A ball launched sideways: it reports hitting the wall and passing a trigger; raycast and overlap from scripts.
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Ball.zs", R"zs(
        script Ball {
            fn start() {
                entity.addImpulse(vec3(0, 0, entity.velocity.z + 300))
                let hit = physics.raycast(vec3(0, 5, 0), vec3(0, -1, 0))
                log("ray " + hit.entity.name + " " + hit.distance)
                let near = physics.overlapSphere(vec3(0, 0.5, 6), 0.5)
                log("overlap " + len(near))
                let miss = physics.raycast(vec3(0, 5, 0), vec3(0, -1, 0), 100, 3)
                log("filtered " + (miss == nil))
            }
            fn collisionEnter(other, speed) { log("hit " + other.name) }
            fn triggerEnter(other) { log("enter " + other.name) }
            fn triggerExit(other) { log("exit " + other.name) }
        }
    )zs"));
    Scene s;
    const ecs::Entity floor = s.Floor();
    s.world.Get<scene::EntityName>(floor)->value = "Floor";
    const ecs::Entity wall = scene::CreateNode(s.world, "Wall", Transform(Vec3(0, 1, 8)));
    s.world.Add(wall, BoxCollider{Vec3(), Vec3(4, 2, 0.5f)});
    const ecs::Entity zone = scene::CreateNode(s.world, "Zone", Transform(Vec3(0, 0.5f, 4)));
    BoxCollider trigger;
    trigger.size = Vec3(4, 2, 1);
    trigger.isTrigger = true;
    s.world.Add(zone, trigger);
    const ecs::Entity ball = scene::CreateNode(s.world, "Ball", Transform(Vec3(0, 0.3f, 0)));
    s.world.Add(ball, SphereCollider{Vec3(), 0.3f});
    RigidBody rb;
    rb.mass = 50.0f; // 300 N*s -> 6 m/s
    rb.friction = 0.0f;
    s.world.Add(ball, rb);
    s.world.Add(ball, Script{String("Scripts/Ball.zs"), String()});
    scene::UpdateWorldTransforms(s.world);

    PhysicsSystem p;
    p.Begin(s.world, nullptr);
    ScriptSystem scripts;
    scripts.SetContentRoot(root);
    scripts.physics = &p;
    String log;
    scripts.onLog = [&](StringView msg, bool) {
        log += msg;
        log += '\n';
    };
    for (u32 i = 0; i < 150; ++i) {
        scene::UpdateWorldTransforms(s.world);
        scripts.Tick(s.world, 1.0f / 60.0f, nullptr);
        scene::UpdateWorldTransforms(s.world);
        p.Tick(s.world, 1.0f / 60.0f);
        scripts.DispatchPhysicsEvents(s.world, p.Events());
    }
    scripts.End();
    p.End();
    platform::DeleteDirectoryTree(root);
    if (!log.View().Contains("ray Ball") || !log.View().Contains("overlap 1") || !log.View().Contains("filtered true") ||
        !log.View().Contains("hit Wall") || !log.View().Contains("enter Zone") || !log.View().Contains("exit Zone"))
        test::ReportFailure(__FILE__, __LINE__, String::Format("script log:\n{}", log));
}

ZX_TEST(PhysicsSystem, JointComponents)
{
    RegisterEngineComponents();
    Scene s;
    s.Floor();
    // A door on a motorized hinge (to the world), a lamp hanging from a "Ceiling" entity by a rope, and two crates
    // welded together by name.
    const ecs::Entity door = scene::CreateNode(s.world, "Door", Transform(Vec3(0.5f, 1.5f, 5)));
    s.world.Add(door, RigidBody{});
    s.world.Add(door, BoxCollider{Vec3(), Vec3(1.0f, 2.0f, 0.1f)});
    HingeJoint hinge;
    hinge.anchor = Vec3(-0.5f, 0, 0);
    hinge.useMotor = true;
    hinge.motorSpeed = 1.0f;
    s.world.Add(door, hinge);

    const ecs::Entity ceiling = scene::CreateNode(s.world, "Ceiling", Transform(Vec3(5, 6, 0)));
    s.world.Add(ceiling, BoxCollider{Vec3(), Vec3(1, 0.2f, 1)});
    const ecs::Entity lamp = scene::CreateNode(s.world, "Lamp", Transform(Vec3(5, 5, 0)));
    s.world.Add(lamp, RigidBody{});
    s.world.Add(lamp, SphereCollider{Vec3(), 0.2f});
    DistanceJoint rope;
    rope.connectedBody = "Ceiling";
    rope.maxDistance = 2.0f;
    s.world.Add(lamp, rope);

    const ecs::Entity a = scene::CreateNode(s.world, "CrateA", Transform(Vec3(-5, 3, 0)));
    s.world.Add(a, RigidBody{});
    s.world.Add(a, BoxCollider{});
    const ecs::Entity b = scene::CreateNode(s.world, "CrateB", Transform(Vec3(-4, 3, 0)));
    s.world.Add(b, RigidBody{});
    s.world.Add(b, BoxCollider{});
    FixedJoint weld;
    weld.connectedBody = "CrateA";
    s.world.Add(b, weld);

    // Saved and loaded with the scene.
    const JsonValue json = SaveScene(s.world, SceneIOOptions{});
    Scene loaded;
    ZX_REQUIRE(LoadScene(json, loaded.world, SceneIOOptions{}));
    u32 found = 0;
    loaded.world.Query<const DistanceJoint>().Each([&](const DistanceJoint& d) { found += d.connectedBody == "Ceiling" && d.maxDistance == 2.0f; });
    loaded.world.Query<const HingeJoint>().Each([&](const HingeJoint& h) { found += h.useMotor && h.anchor == Vec3(-0.5f, 0, 0); });
    ZX_EXPECT_EQ(found, 2u);

    scene::UpdateWorldTransforms(s.world);
    PhysicsSystem p;
    p.Begin(s.world, nullptr);
    ZX_EXPECT_EQ(p.JointCount(), 3u);
    s.Run(p, 2.0f);
    // The door turned ~1.9 rad around its hinge (left edge stays at x = 0, z = 5).
    ZX_EXPECT_NEAR(p.GetJointValue(door), 1.9f, 0.15f); // spin-up of a 200 kg door
    ZX_EXPECT_NEAR(p.GetAngularVelocity(door).y, 1.0f, 0.03f);
    const Transform& dt = *s.world.Get<Transform>(door);
    const Vec3 edge = dt.position + dt.rotation * Vec3(-0.5f, 0, 0);
    ZX_EXPECT(Distance(edge, Vec3(0, 1.5f, 5)) < 0.02f);
    // The lamp hangs 2 m below the ceiling.
    ZX_EXPECT_NEAR(s.world.Get<Transform>(lamp)->position.y, 4.0f, 0.03f);
    // The crates landed together, still 1 m apart.
    const Vec3 pa = s.world.Get<Transform>(a)->position, pb = s.world.Get<Transform>(b)->position;
    ZX_EXPECT_NEAR(Distance(pa, pb), 1.0f, 0.01f);
    ZX_EXPECT(pa.y < 0.6f);
    // Stopping the motor from game code.
    p.SetJointMotor(door, true, 0.0f);
    s.Run(p, 0.5f);
    ZX_EXPECT(Abs(p.GetVelocity(door).x) < 0.01f && Abs(p.GetAngularVelocity(door).y) < 0.01f);
    p.End();
}

ZX_TEST(PhysicsSystem, CylinderColliderStandsOnItsEnd)
{
    Scene s;
    s.Floor();
    // Scaled 2x in Y: a 1 m wide, 2 m tall cylinder standing upright, its center 1 m up.
    const ecs::Entity c = scene::CreateNode(s.world, "Cylinder", Transform(Vec3(0, 2, 0), Quat(), Vec3(1, 2, 1)));
    s.world.Add(c, CylinderCollider{});
    s.world.Add(c, RigidBody{});
    scene::UpdateWorldTransforms(s.world);
    PhysicsSystem p;
    p.Begin(s.world, nullptr);
    s.Run(p, 3.0f);
    const Transform* t = s.world.Get<Transform>(c);
    ZX_EXPECT_NEAR(t->position.y, 1.0f, 0.03f);
    ZX_EXPECT(Abs((t->rotation * Vec3(0, 1, 0)).y) > 0.99f); // still upright
    Array<ColliderOutline> outlines;
    GatherColliderOutlines(s.world, c, outlines);
    ZX_EXPECT(outlines.Size() == 1u && !outlines[0].lines.Empty());
    p.End();
}

ZX_TEST(PhysicsSystem, ScriptsSeeSurfacesAndStay)
{
    const String root = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_script_test/surfaces");
    platform::DeleteDirectoryTree(root);
    platform::CreateDirectoryTree(root + "/Scripts");
    // A character standing on grass asks what is underfoot; a resting ball reports collisionStay every step.
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Walker.zs", R"zs(
        script Walker {
            var said = false
            fn update(dt) {
                if not said {
                    said = true
                    log("under " + character.surface)
                    let hit = physics.raycast(vec3(-3, 5, 0), vec3(0, -1, 0))
                    log("ray " + hit.surface)
                }
            }
        }
    )zs"));
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Ball.zs", R"zs(
        script Ball {
            var stays = 0
            fn collisionStay(other) { stays += 1 }
            fn stop() { log("stays " + stays) }
            fn update(dt) { if stays == 30 { log("stayed") } }
        }
    )zs"));
    Scene s;
    const ecs::Entity floor = s.Floor();
    const Guid grass = Guid::New();
    MeshRenderer mr;
    mr.materials.PushBack(grass);
    s.world.Add(floor, mr);
    const ecs::Entity walker = scene::CreateNode(s.world, "Walker", Transform(Vec3(0, 0, 0)));
    s.world.Add(walker, CharacterController{});
    s.world.Add(walker, Script{String("Scripts/Walker.zs"), String()});
    const ecs::Entity ball = scene::CreateNode(s.world, "Ball", Transform(Vec3(3, 0.3f, 0)));
    s.world.Add(ball, SphereCollider{Vec3(), 0.3f});
    s.world.Add(ball, RigidBody{});
    s.world.Add(ball, Script{String("Scripts/Ball.zs"), String()});
    scene::UpdateWorldTransforms(s.world);
    PhysicsSystem p;
    p.Begin(s.world, nullptr);
    ScriptSystem scripts;
    scripts.SetContentRoot(root);
    scripts.physics = &p;
    scripts.materialName = [&](asset::AssetId id) { return id == grass ? String("M_Grass") : String(); };
    String log;
    scripts.onLog = [&](StringView msg, bool) {
        log += msg;
        log += '\n';
    };
    for (u32 i = 0; i < 90; ++i) {
        scene::UpdateWorldTransforms(s.world);
        scripts.Tick(s.world, 1.0f / 60.0f, nullptr);
        scene::UpdateWorldTransforms(s.world);
        p.Tick(s.world, 1.0f / 60.0f);
        scripts.DispatchPhysicsEvents(s.world, p.Events());
    }
    scripts.End();
    p.End();
    platform::DeleteDirectoryTree(root);
    ZX_EXPECT(log.Contains("under Grass"));
    ZX_EXPECT(log.Contains("ray Grass"));
    ZX_EXPECT(log.Contains("stayed"));
}
