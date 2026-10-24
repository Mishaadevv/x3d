// Behaviour trees: the .zbt form round-trips and rejects broken trees, composites and decorators follow their rules
// (sequence guards abort, selectors switch to a higher-priority branch, cooldowns, repeats), and the starter Guard tree
// patrols its waypoints, chases a target that walks into view and reports catching it. Nav components are registered
// (Add Component, scenes).

#include "Core/HAL/PlatformFile.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Engine/BehaviorTree.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/Navigation.h"
#include "Engine/Terrain.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

struct Fixture
{
    ecs::World world;
    BehaviorSystem bt;
    NavigationSystem nav;
    String dir;
    ecs::Entity agent;

    Fixture()
    {
        RegisterEngineComponents();
        dir = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_bt_test");
        platform::CreateDirectoryTree(dir);
        bt.contentRoot = dir;
        bt.navigation = &nav;
    }
    ~Fixture() { platform::DeleteDirectoryTree(dir); }

    void Write(StringView name, const BehaviorTreeAsset& tree) { fs::WriteTextFile(Path::Join(dir, name), WriteBehaviorTree(tree)); }
    void Ground()
    {
        const ecs::Entity g = scene::CreateNode(world, "Ground");
        Terrain t;
        t.size = 60.0f;
        t.resolution = 61;
        t.hills = false;
        world.Add(g, move(t));
        const ecs::Entity b = scene::CreateNode(world, "Nav");
        NavMeshBounds nb;
        nb.size = Vec3(50, 10, 50);
        world.Add(b, nb);
        scene::UpdateWorldTransforms(world);
        nav.Build(world, nullptr);
    }
    ecs::Entity Agent(StringView tree, Vec3 at)
    {
        agent = scene::CreateNode(world, "Guard", Transform(at));
        world.Add(agent, NavMeshAgent{});
        BehaviorTree b;
        b.tree = String(tree);
        world.Add(agent, b);
        scene::UpdateWorldTransforms(world);
        return agent;
    }
    void Step(u32 frames = 1)
    {
        for (u32 i = 0; i < frames; ++i) {
            bt.Tick(world, 1.0f / 30.0f);
            nav.Tick(world, 1.0f / 30.0f);
            scene::UpdateWorldTransforms(world);
        }
    }
    BehaviorTree& Tree() { return *world.Get<BehaviorTree>(agent); }
    Vec3 Position() const { return world.Get<const Transform>(agent)->position; }
};

u32 Add(BehaviorTreeAsset& t, BTNodeType type, StringView key = {}, Span<const u32> children = {})
{
    BTNode n;
    n.type = type;
    n.key = String(key);
    n.children.Append(children);
    t.nodes.PushBack(move(n));
    return u32(t.nodes.Size() - 1);
}

} // namespace

ZX_TEST(BehaviorTree, FileRoundTripAndValidation)
{
    const BehaviorTreeAsset guard = MakeGuardTree();
    BehaviorTreeAsset back;
    String error;
    ZX_REQUIRE(ParseBehaviorTree(WriteBehaviorTree(guard).View(), back, &error));
    ZX_EXPECT_EQ(back.nodes.Size(), guard.nodes.Size());
    ZX_EXPECT_EQ(back.defaults.Size(), guard.defaults.Size());
    for (usize i = 0; i < guard.nodes.Size(); ++i) {
        ZX_EXPECT(back.nodes[i].type == guard.nodes[i].type);
        ZX_EXPECT(back.nodes[i].children.Size() == guard.nodes[i].children.Size());
        ZX_EXPECT(back.nodes[i].key == guard.nodes[i].key);
    }
    ZX_EXPECT(!ParseBehaviorTree("{\"format\":\"zbt\",\"nodes\":[{\"type\":\"Dance\"}]}", back, &error));
    ZX_EXPECT(!ParseBehaviorTree("{\"format\":\"zbt\",\"nodes\":[{\"type\":\"Sequence\",\"children\":[0]}]}", back, &error)); // cycle
    ZX_EXPECT(!ParseBehaviorTree("{\"format\":\"zbt\",\"nodes\":[{\"type\":\"Inverter\"}]}", back, &error));                 // no child
}

ZX_TEST(BehaviorTree, CompositesAndDecorators)
{
    Fixture f;
    // Selector: [Sequence(Condition alarm, Wait 10 s, SetValue result=chased), SetValue result=idle + Wait 10 s].
    BehaviorTreeAsset t;
    const u32 root = Add(t, BTNodeType::Selector);
    const u32 cond = Add(t, BTNodeType::Condition, "alarm");
    t.nodes[cond].op = "==";
    t.nodes[cond].value = BBValue::Bool(true);
    const u32 longWait = Add(t, BTNodeType::Wait);
    t.nodes[longWait].seconds = 10.0f;
    const u32 setChased = Add(t, BTNodeType::SetValue, "result");
    t.nodes[setChased].value = BBValue::Text("chased");
    const u32 seqA = Add(t, BTNodeType::Sequence, {}, Span<const u32>({cond, longWait, setChased}));
    const u32 setIdle = Add(t, BTNodeType::SetValue, "result");
    t.nodes[setIdle].value = BBValue::Text("idle");
    const u32 idleWait = Add(t, BTNodeType::Wait);
    t.nodes[idleWait].seconds = 10.0f;
    const u32 seqB = Add(t, BTNodeType::Sequence, {}, Span<const u32>({setIdle, idleWait}));
    t.nodes[root].children = {seqA, seqB};
    f.Write("sel.zbt", t);
    f.Agent("sel.zbt", Vec3());
    f.Step(3);
    BehaviorTree& b = f.Tree();
    ZX_EXPECT(b.blackboard.Find(String("result")) && b.blackboard.Find(String("result"))->text == "idle");
    ZX_EXPECT(b.states[idleWait].status == BTStatus::Running);
    // The alarm goes up: the higher-priority branch takes over (aborting the idle wait).
    b.blackboard.InsertOrAssign(String("alarm"), BBValue::Bool(true));
    f.Step(1);
    ZX_EXPECT(b.states[longWait].status == BTStatus::Running);
    ZX_EXPECT(b.states[idleWait].status != BTStatus::Running);
    // ... and back down: the sequence guard fails, the wait is aborted, the selector falls back to idle.
    b.blackboard.InsertOrAssign(String("alarm"), BBValue::Bool(false));
    f.Step(1);
    ZX_EXPECT(b.states[longWait].status != BTStatus::Running);
    ZX_EXPECT(b.states[idleWait].status == BTStatus::Running);

    // Repeat 3 x (counter += via script-like SetValue is not additive: count events instead) and Cooldown.
    BehaviorTreeAsset r;
    const u32 rep = Add(r, BTNodeType::Repeat);
    r.nodes[rep].count = 3;
    const u32 ev = Add(r, BTNodeType::Event);
    r.nodes[ev].text = "tick";
    r.nodes[rep].children = {ev};
    f.Write("rep.zbt", r);
    BehaviorTreeAsset cd;
    const u32 cool = Add(cd, BTNodeType::Cooldown);
    cd.nodes[cool].seconds = 1.0f;
    const u32 ev2 = Add(cd, BTNodeType::Event);
    cd.nodes[ev2].text = "cool";
    cd.nodes[cool].children = {ev2};
    f.Write("cool.zbt", cd);
    f.Agent("rep.zbt", Vec3(5, 0, 0));
    const ecs::Entity repAgent = f.agent;
    f.Agent("cool.zbt", Vec3(-5, 0, 0));
    const ecs::Entity coolAgent = f.agent;
    u32 ticks = 0, cools = 0;
    for (u32 frame = 0; frame < 60; ++frame) { // two seconds
        f.Step(1);
        for (const BehaviorSystem::AIEvent& e : f.bt.Events()) {
            ticks += e.entity == repAgent && e.name == "tick";
            cools += e.entity == coolAgent && e.name == "cool";
        }
    }
    ZX_EXPECT(ticks >= 3u * 15u); // the repeat finishes every 3 ticks and the root restarts it
    ZX_EXPECT(cools >= 2u && cools <= 3u);
}

ZX_TEST(BehaviorTree, GuardPatrolsAndChases)
{
    Fixture f;
    f.Ground();
    f.Write("Guard.zbt", MakeGuardTree());
    // Waypoints in a square; the player far away behind the guard.
    const ecs::Entity group = scene::CreateNode(f.world, "Waypoints");
    const Vec3 corners[3] = {Vec3(-8, 0, -8), Vec3(8, 0, -8), Vec3(8, 0, 8)};
    for (const Vec3& c : corners)
        scene::CreateNode(f.world, "Point", Transform(c), group);
    const ecs::Entity player = scene::CreateNode(f.world, "Player", Transform(Vec3(0, 0, 22)));
    f.Agent("Guard.zbt", Vec3(-8, 0, -6));
    f.world.Get<BehaviorTree>(f.agent)->sightRange = 12.0f;
    // Patrol: it visits the first waypoints in order.
    bool reached[3] = {false, false, false};
    for (u32 frame = 0; frame < 30 * 20; ++frame) {
        f.Step(1);
        for (u32 k = 0; k < 3; ++k)
            reached[k] |= Distance(Vec2(f.Position().x, f.Position().z), Vec2(corners[k].x, corners[k].z)) < 0.8f;
        ZX_EXPECT(!f.Tree().blackboard.Find(String("targetVisible"))->Truthy());
    }
    ZX_EXPECT(reached[0] && reached[1] && reached[2]);
    // The player walks up close in front of it: it chases and catches.
    const Vec3 guard = f.Position();
    const Vec3 forward = f.world.Get<const Transform>(f.agent)->rotation * Vec3(0, 0, 1);
    f.world.Get<Transform>(player)->position = guard + Vec3(forward.x, 0, forward.z) * 6.0f;
    f.world.Get<Transform>(player)->position.y = 0.0f;
    scene::UpdateWorldTransforms(f.world);
    bool caught = false;
    for (u32 frame = 0; frame < 30 * 6 && !caught; ++frame) {
        f.Step(1);
        for (const BehaviorSystem::AIEvent& e : f.bt.Events())
            caught |= e.name == "Caught";
    }
    ZX_EXPECT(caught);
    ZX_EXPECT(f.Tree().blackboard.Find(String("search"))->Truthy());
    // The player vanishes far away: the guard searches the last seen spot, then patrols again.
    f.world.Get<Transform>(player)->position = Vec3(0, 0, 200);
    scene::UpdateWorldTransforms(f.world);
    f.Step(30 * 8);
    ZX_EXPECT(!f.Tree().blackboard.Find(String("search"))->Truthy());
}

ZX_TEST(BehaviorTree, NavComponentsAreRegistered)
{
    RegisterEngineComponents();
    ZX_EXPECT(FindComponentType("NavMeshBounds") != nullptr);
    ZX_EXPECT(FindComponentType("NavMeshAgent") != nullptr);
    ZX_EXPECT(FindComponentType("BehaviorTree") != nullptr);
}
