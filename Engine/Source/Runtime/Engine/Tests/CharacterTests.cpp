#include "Core/Testing/Test.h"
#include "Engine/Gameplay.h"
#include "Engine/SceneRendering.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

const Guid UnitCube = Guid::FromName(Guid(), "test-unit-cube"); // bounds -0.5..0.5

/// A static box collider: a unit-cube mesh scaled/placed to [min, max].
ecs::Entity Box(ecs::World& w, Vec3 min, Vec3 max)
{
    Transform t((min + max) * 0.5f);
    t.scale = max - min;
    const ecs::Entity e = scene::CreateNode(w, "Box", t);
    MeshRenderer mr;
    mr.mesh = UnitCube;
    w.Add(e, mr);
    return e;
}

struct Level
{
    ecs::World world;
    ecs::Entity player;
    MeshBoundsFn bounds = [](asset::AssetId id, AABB& out) {
        if (id != UnitCube)
            return false;
        out = AABB(Vec3(-0.5f), Vec3(0.5f));
        return true;
    };
    Level()
    {
        scene::InstallHierarchyHooks(world);
        Box(world, Vec3(-20, -1, -20), Vec3(20, 0, 20)); // floor (top at y = 0)
        player = scene::CreateNode(world, "Player", Transform(Vec3(0, 0.5f, 0)));
        world.Add(player, CharacterController{});
    }
    void Step(Vec3 move, u32 frames, f32 jump = 0.0f)
    {
        for (u32 i = 0; i < frames; ++i) {
            if (!world.Has<CharacterState>(player))
                world.Add(player, CharacterState{});
            world.Get<CharacterState>(player)->desiredMove = move;
            if (jump > 0.0f && i == 0)
                world.Get<CharacterState>(player)->jumpSpeed = jump;
            scene::UpdateWorldTransforms(world);
            TickCharacters(world, 1.0f / 60.0f, bounds);
        }
    }
    Vec3 Pos() { return world.Get<Transform>(player)->position; }
    bool Grounded() { return world.Get<CharacterState>(player)->grounded; }
};

} // namespace

ZX_TEST(Character, FallsAndLandsOnTheFloor)
{
    Level l;
    l.Step(Vec3(0.0f), 60);
    ZX_EXPECT_NEAR(l.Pos().y, 0.0f, 0.01f);
    ZX_EXPECT(l.Grounded());
}

ZX_TEST(Character, WallsBlockAndStepsAreClimbed)
{
    Level l;
    Box(l.world, Vec3(2, 0, -5), Vec3(3, 3, 5));        // wall in +X
    Box(l.world, Vec3(-3, 0, -1), Vec3(-2, 0.25f, 1));  // low step in -X
    l.Step(Vec3(0.0f), 30);
    l.Step(Vec3(4, 0, 0), 120); // walk into the wall
    ZX_EXPECT_NEAR(l.Pos().x, 2.0f - 0.35f, 0.01f);
    ZX_EXPECT_NEAR(l.Pos().y, 0.0f, 0.01f);
    l.Step(Vec3(-4, 0, 0), 60); // onto the step
    ZX_EXPECT_NEAR(l.Pos().y, 0.25f, 0.01f);
    ZX_EXPECT(l.Grounded());
    l.Step(Vec3(-4, 0, 0), 60); // and down again on the far side
    ZX_EXPECT_LT(l.Pos().x, -3.5f);
    ZX_EXPECT_NEAR(l.Pos().y, 0.0f, 0.01f);
}

ZX_TEST(Character, JumpsOntoAPlatform)
{
    Level l;
    Box(l.world, Vec3(1, 0, -2), Vec3(4, 0.8f, 2)); // too high to step on, low enough to jump on
    l.Step(Vec3(0.0f), 30);
    l.Step(Vec3(3, 0, 0), 60);
    ZX_EXPECT_LT(l.Pos().x, 1.0f); // blocked
    l.Step(Vec3(3, 0, 0), 60, 6.0f);
    ZX_EXPECT_NEAR(l.Pos().y, 0.8f, 0.02f);
    ZX_EXPECT_GT(l.Pos().x, 1.0f);
    // Walking off the edge falls back to the floor.
    l.Step(Vec3(3, 0, 0), 120);
    ZX_EXPECT_NEAR(l.Pos().y, 0.0f, 0.01f);
    ZX_EXPECT_GT(l.Pos().x, 4.0f);
}

ZX_TEST(Character, OwnMeshesDontBlockAndCollisionCanBeOff)
{
    Level l;
    const ecs::Entity body = Box(l.world, Vec3(-0.3f, 0.5f, -0.3f), Vec3(0.3f, 2.3f, 0.3f));
    scene::SetParent(l.world, body, l.player, true);
    const ecs::Entity ghost = Box(l.world, Vec3(1, 0, -1), Vec3(2, 2, 1));
    l.world.Get<MeshRenderer>(ghost)->collision = false;
    l.Step(Vec3(0.0f), 30);
    ZX_EXPECT_NEAR(l.Pos().y, 0.0f, 0.01f); // not standing on its own body
    l.Step(Vec3(3, 0, 0), 60);
    ZX_EXPECT_GT(l.Pos().x, 2.5f); // walked through the non-colliding box
}
