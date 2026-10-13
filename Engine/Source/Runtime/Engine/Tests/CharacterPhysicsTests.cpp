// Character Controller on physics (M11 acceptance: walks up stairs and slopes, blocked by steep ones and walls,
// rides platforms, pushes light bodies, not heavy ones).

#include "Core/Testing/Test.h"
#include "Engine/Gameplay.h"
#include "Engine/PhysicsSystem.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

struct Course
{
    ecs::World world;
    PhysicsSystem physics;
    ecs::Entity player;

    Course()
    {
        scene::InstallHierarchyHooks(world);
        Box(Vec3(0, -0.5f, 0), Vec3(60, 1, 60)); // floor, top at y = 0
    }
    ecs::Entity Box(Vec3 center, Vec3 size, Quat rotation = {})
    {
        const ecs::Entity e = scene::CreateNode(world, "Box", Transform(center, rotation));
        world.Add(e, BoxCollider{Vec3(), size});
        return e;
    }
    void Spawn(Vec3 feet)
    {
        player = scene::CreateNode(world, "Player", Transform(feet));
        world.Add(player, CharacterController{});
        scene::UpdateWorldTransforms(world);
        physics.Begin(world, nullptr);
    }
    /// Walks with `velocity` for `seconds` (60 Hz frames: characters, then physics, like the editor).
    void Walk(Vec3 velocity, f32 seconds, bool jump = false)
    {
        for (u32 i = 0; i < u32(seconds * 60.0f); ++i) {
            CharacterState& st = *world.Get<CharacterState>(player) ;
            st.desiredMove = velocity;
            if (jump && i == 0)
                st.jumpSpeed = 6.0f;
            scene::UpdateWorldTransforms(world);
            TickCharacters(world, 1.0f / 60.0f, physics);
            scene::UpdateWorldTransforms(world);
            physics.Tick(world, 1.0f / 60.0f);
        }
        scene::UpdateWorldTransforms(world);
    }
    void Settle()
    {
        if (!world.Has<CharacterState>(player))
            world.Add(player, CharacterState{});
        Walk(Vec3(), 0.5f);
    }
    Vec3 Feet() const { return world.Get<const Transform>(player)->position; }
    bool Grounded() const { return world.Get<const CharacterState>(player)->grounded; }
};

} // namespace

ZX_TEST(CharacterPhysics, ClimbsStairsAndComesDown)
{
    Course c;
    // 8 steps of 0.25 m (each 0.5 m deep) up along +Z from z = 2, then a landing.
    for (u32 i = 0; i < 8; ++i)
        c.Box(Vec3(0, 0.125f + f32(i) * 0.125f, 2.25f + f32(i) * 0.5f), Vec3(3, 0.25f + f32(i) * 0.25f, 0.5f));
    c.Box(Vec3(0, 1.0f, 13.0f), Vec3(3, 2.0f, 14.0f)); // landing (z 6..20), top at y = 2
    c.Spawn(Vec3(0, 0, 0));
    c.Settle();
    ZX_EXPECT(c.Grounded());
    c.Walk(Vec3(0, 0, 3), 4.0f);
    if (Abs(c.Feet().y - 2.0f) > 0.05f || c.Feet().z < 7.0f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("stairs: feet at ({:.2f}, {:.2f}, {:.2f})", c.Feet().x, c.Feet().y, c.Feet().z));
    ZX_EXPECT(c.Grounded());
    // And back down without flying off the steps.
    c.Walk(Vec3(0, 0, -3), 4.0f);
    ZX_EXPECT(Abs(c.Feet().y) < 0.05f && c.Feet().z < 1.0f);
    ZX_EXPECT(c.Grounded());
}

ZX_TEST(CharacterPhysics, WalkableAndSteepSlopes)
{
    for (f32 degrees : {30.0f, 60.0f}) {
        Course c;
        // A ramp rising along +Z starting at z = 2.
        const f32 a = DegToRad(degrees), length = 10.0f;
        const Vec3 center(0.0f, Sin(a) * length * 0.5f - Cos(a) * 0.25f, 2.0f + Cos(a) * length * 0.5f + Sin(a) * 0.25f);
        c.Box(center, Vec3(4, 0.5f, length), Quat::FromEulerDegrees(Vec3(-degrees, 0, 0)));
        c.Spawn(Vec3(0, 0, 0));
        c.Settle();
        c.Walk(Vec3(0, 0, 3), 3.0f);
        const f32 y = c.Feet().y;
        if (degrees < 45.0f ? y < 2.0f : y > 0.6f)
            test::ReportFailure(__FILE__, __LINE__, String::Format("{} degree slope: feet at y {:.2f}", degrees, y));
    }
}

ZX_TEST(CharacterPhysics, SlidesAlongWallsAndJumps)
{
    Course c;
    c.Box(Vec3(0, 1, 5), Vec3(40, 2, 1)); // wall across +Z
    c.Spawn(Vec3(0, 0, 0));
    c.Settle();
    // Diagonal into the wall: stopped in Z, keeps sliding in X.
    c.Walk(Vec3(3, 0, 3), 2.0f);
    if (!(c.Feet().z < 4.5f - 0.3f && c.Feet().z > 4.0f) || c.Feet().x < 4.0f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("wall: feet at ({:.3f}, {:.3f}, {:.3f})", c.Feet().x, c.Feet().y, c.Feet().z));
    // Jump: leaves the ground and lands again.
    c.Walk(Vec3(), 0.25f, true);
    ZX_EXPECT(c.Feet().y > 0.5f && !c.Grounded());
    c.Walk(Vec3(), 2.0f);
    ZX_EXPECT(Abs(c.Feet().y) < 0.02f && c.Grounded());
}

ZX_TEST(CharacterPhysics, RidesPlatformsAndPushesBodies)
{
    Course c;
    // A kinematic platform moving along +X at 1 m/s.
    const ecs::Entity platform = c.Box(Vec3(0, 0.5f, 10), Vec3(3, 1, 3));
    RigidBody kinematic;
    kinematic.type = RigidBodyType::Kinematic;
    c.world.Add(platform, kinematic);
    // A light (20 kg) and a heavy (2000 kg) crate ahead of the player's second position.
    const ecs::Entity light = c.Box(Vec3(-10, 0.5f, 3), Vec3(1));
    RigidBody lightBody;
    lightBody.mass = 20.0f;
    c.world.Add(light, lightBody);
    const ecs::Entity heavy = c.Box(Vec3(10, 0.5f, 3), Vec3(1));
    RigidBody heavyBody;
    heavyBody.mass = 2000.0f;
    c.world.Add(heavy, heavyBody);
    c.Spawn(Vec3(0, 1.0f, 10));
    c.Settle();
    ZX_EXPECT(Abs(c.Feet().y - 1.0f) < 0.05f);
    for (u32 i = 0; i < 120; ++i) {
        c.world.Get<Transform>(platform)->position.x += 1.0f / 60.0f;
        c.Walk(Vec3(), 1.0f / 60.0f);
    }
    ZX_EXPECT(Abs(c.Feet().x - 2.0f) < 0.1f); // carried 2 m
    ZX_EXPECT(Abs(c.Feet().y - 1.0f) < 0.05f);

    // Push the light crate (-X side) and lean on the heavy one (+X side).
    Course p;
    const ecs::Entity crate = p.Box(Vec3(2, 0.5f, 0), Vec3(1));
    RigidBody crateBody;
    crateBody.mass = 20.0f;
    p.world.Add(crate, crateBody);
    const ecs::Entity boulder = p.Box(Vec3(-2, 0.5f, 0), Vec3(1));
    RigidBody boulderBody;
    boulderBody.mass = 2000.0f;
    p.world.Add(boulder, boulderBody);
    p.Spawn(Vec3(0, 0, 0));
    p.Settle();
    p.Walk(Vec3(3, 0, 0), 2.0f);
    const f32 crateX = p.world.Get<Transform>(crate)->position.x;
    const f32 playerAfterPush = p.Feet().x;
    p.Walk(Vec3(-3, 0, 0), 4.0f);
    const f32 boulderX = p.world.Get<Transform>(boulder)->position.x;
    // The light crate is pushed along, the heavy one barely moves and blocks the player.
    if (crateX < 4.0f || boulderX < -2.3f || p.Feet().x < -1.6f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("push: crate {:.2f} (player {:.2f}), boulder {:.2f}, player {:.2f} y {:.2f}", crateX,
                                                               playerAfterPush, boulderX, p.Feet().x, p.Feet().y));
    (void)light;
    (void)heavy;
}


