#include "Core/Testing/Test.h"
#include "Engine/GameModule.h"
#include "Engine/InputActions.h"
#include "Engine/SceneRendering.h"
#include "Platform/Input.h"
#include "Scene/Hierarchy.h"

using namespace zx;

// --- A test "game module" built in-process with the Game SDK macros ------------------------------------------------------

namespace {

struct Counters
{
    int started = 0, stopped = 0, reloaded = 0, updates = 0;
} g_counters;

class TestMover : public game::ScriptComponent
{
public:
    float speed = 1.0f;
    int steps = 2;
    bool enabled = true;
    game::Vec3 direction = game::Vec3(1, 0, 0);

    void OnStart() override { ++g_counters.started; }
    void OnStop() override { ++g_counters.stopped; }
    void OnReloaded() override { ++g_counters.reloaded; }
    void OnUpdate(float dt) override
    {
        ++g_counters.updates;
        if (enabled)
            SetPosition(Position() + direction * (speed * dt));
    }
};

class TestSpawner : public game::ScriptComponent
{
public:
    void OnStart() override
    {
        const game::EntityId cube = host->Spawn("Cube", game::Vec3(0, 5, 0));
        host->Destroy(host->Find("Victim"));
        host->Log(host->GetName(cube), false);
    }
};

} // namespace

ZX_GAME_COMPONENT(TestMover, "Moves along a direction")
    ZX_GAME_PROP(speed, 0, 10, "Meters per second")
    ZX_GAME_PROP(steps, 0, 0, "")
    ZX_GAME_PROP(enabled, 0, 0, "")
    ZX_GAME_PROP(direction, 0, 0, "")
ZX_GAME_COMPONENT_END()

ZX_GAME_COMPONENT(TestSpawner, "Spawns a cube and destroys Victim")
ZX_GAME_COMPONENT_END()

namespace {

// "Version 2" of TestMover (what a rebuilt DLL would contain): same props, moves up instead.
class TestMoverV2 : public game::ScriptComponent
{
public:
    float speed = 1.0f;
    int steps = 2;
    bool enabled = true;
    game::Vec3 direction = game::Vec3(1, 0, 0);
    float extra = 7.0f; // a new prop

    void OnStart() override { ++g_counters.started; }
    void OnReloaded() override { ++g_counters.reloaded; }
    void OnStop() override { ++g_counters.stopped; }
    void OnUpdate(float dt) override { SetPosition(Position() + game::Vec3(0, speed * dt, 0)); }
};

const game::PropDesc V2Props[] = {
    {"speed", game::PropType::Float, u32(offsetof(TestMoverV2, speed)), 0.0f, 10.0f, ""},
    {"extra", game::PropType::Float, u32(offsetof(TestMoverV2, extra)), 0.0f, 0.0f, ""},
};
game::ScriptComponent* CreateV2() { return new TestMoverV2(); }
void DestroyV2(game::ScriptComponent* c) { delete c; }
const game::ComponentDesc V2Desc = {"TestMover", "v2", &CreateV2, &DestroyV2, V2Props, 2, nullptr};
const game::ModuleDesc V2Module = {game::ApiVersion, "TestGameV2", &V2Desc};

game::ModuleDesc V1Module() { return game::ModuleDesc{game::ApiVersion, "TestGame", game::detail::Head()}; }

} // namespace

ZX_TEST(GameModule, ReflectsComponentsAndProps)
{
    const game::ModuleDesc v1 = V1Module();
    GameModuleSystem game;
    ZX_REQUIRE(game.LoadFromDesc(&v1));
    ZX_EXPECT(game.ModuleName() == "TestGame");
    const game::ComponentDesc* mover = game.FindComponent("TestMover");
    ZX_REQUIRE(mover != nullptr);
    ZX_EXPECT(game.FindComponent("TestSpawner") != nullptr);
    ZX_EXPECT(game.FindComponent("Nope") == nullptr);
    ZX_EXPECT_GE(game.Components().Size(), 2u);
    ZX_EXPECT_EQ(mover->propCount, 4u);

    const JsonValue defaults = GameModuleSystem::DefaultProps(*mover);
    ZX_EXPECT_NEAR(defaults.Get("speed").AsFloat(), 1.0f, 1e-6f);
    ZX_EXPECT_EQ(defaults.Get("steps").AsInt(), 2);
    ZX_EXPECT(defaults.Get("enabled").AsBool());
    ZX_EXPECT_EQ(defaults.Get("direction").Size(), 3u);

    // Write: clamped to [min, max], ints rounded, vec3 from arrays, unknown names ignored.
    JsonValue values;
    ZX_REQUIRE(ParseJson(R"({"speed": 50, "steps": 2.6, "enabled": false, "direction": [0, 0, 2], "unknown": 1})", values));
    game::ScriptComponent* object = mover->create();
    GameModuleSystem::WriteProps(*mover, object, values);
    const JsonValue back = GameModuleSystem::ReadProps(*mover, object);
    mover->destroy(object);
    ZX_EXPECT_NEAR(back.Get("speed").AsFloat(), 10.0f, 1e-6f);
    ZX_EXPECT_EQ(back.Get("steps").AsInt(), 3);
    ZX_EXPECT(!back.Get("enabled").AsBool(true));
    ZX_EXPECT_NEAR(back.Get("direction")[2].AsFloat(), 2.0f, 1e-6f);

    // NativeScript stores only overrides.
    NativeScript ns{String("TestMover"), String()};
    SetNativeScriptProperty(ns, *mover, "speed", JsonValue(3.0f));
    ZX_EXPECT_NEAR(ParseNativeScriptProperties(ns).Get("speed").AsFloat(), 3.0f, 1e-6f);
    SetNativeScriptProperty(ns, *mover, "speed", JsonValue(1.0f));
    ZX_EXPECT(ns.properties.Empty());
}

ZX_TEST(GameModule, RunsComponentsAndHotReloadsWithProps)
{
    g_counters = {};
    const game::ModuleDesc v1 = V1Module();
    GameModuleSystem game;
    String log;
    u32 errors = 0;
    game.onLog = [&](StringView s, bool isError) {
        log += s;
        log += '\n';
        errors += isError;
    };
    ZX_REQUIRE(game.LoadFromDesc(&v1));

    ecs::World world;
    const ecs::Entity a = world.Create(scene::EntityName{String("A")}, Transform::Identity(), NativeScript{String("TestMover"), String(R"({"speed": 4})")});
    const ecs::Entity missing = world.Create(Transform::Identity(), NativeScript{String("DoesNotExist"), String()});
    (void)missing;

    game.Begin(world);
    game.Tick(world, 0.5f, {});
    game.Tick(world, 0.5f, {});
    ZX_EXPECT_EQ(g_counters.started, 1);
    ZX_EXPECT_EQ(g_counters.updates, 2);
    ZX_EXPECT_EQ(game.RunningComponents(), 1u);
    ZX_EXPECT_NEAR(world.Get<Transform>(a)->position.x, 4.0f, 1e-5f);
    ZX_EXPECT_EQ(errors, 1u); // DoesNotExist, reported once
    ZX_EXPECT(log.Contains("DoesNotExist"));

    // A prop changed at runtime survives the reload (here: written through the running object).
    JsonValue props = game.RunningProps(a);
    ZX_EXPECT_NEAR(props.Get("speed").AsFloat(), 4.0f, 1e-6f);

    // Hot reload to "version 2": same entity, props carried by name, OnReloaded instead of OnStart.
    const u32 generation = game.Generation();
    ZX_REQUIRE(game.LoadFromDesc(&V2Module));
    ZX_EXPECT_EQ(game.Generation(), generation + 1);
    ZX_EXPECT_EQ(g_counters.reloaded, 1);
    ZX_EXPECT_EQ(g_counters.stopped, 0);
    ZX_EXPECT_EQ(game.RunningComponents(), 1u);
    props = game.RunningProps(a);
    ZX_EXPECT_NEAR(props.Get("speed").AsFloat(), 4.0f, 1e-6f);
    ZX_EXPECT_NEAR(props.Get("extra").AsFloat(), 7.0f, 1e-6f);
    game.Tick(world, 0.25f, {});
    ZX_EXPECT_NEAR(world.Get<Transform>(a)->position.x, 4.0f, 1e-5f); // v2 no longer moves along x
    ZX_EXPECT_NEAR(world.Get<Transform>(a)->position.y, 1.0f, 1e-5f);
    ZX_EXPECT_EQ(g_counters.started, 1);

    // Removing the component stops it; End stops the rest.
    world.Remove<NativeScript>(a);
    game.Tick(world, 0.1f, {});
    ZX_EXPECT_EQ(game.RunningComponents(), 0u);
    ZX_EXPECT_EQ(g_counters.stopped, 1);
    game.End();
    ZX_EXPECT(!game.IsRunning());
}

ZX_TEST(GameModule, HostSpawnsFindsAndDestroys)
{
    const game::ModuleDesc v1 = V1Module();
    GameModuleSystem game;
    String log;
    game.onLog = [&](StringView s, bool) { log += s; };
    ZX_REQUIRE(game.LoadFromDesc(&v1));
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    const ecs::Entity victim = scene::CreateNode(world, "Victim");
    world.Create(Transform::Identity(), NativeScript{String("TestSpawner"), String()});
    game.Tick(world, 0.016f, {});
    ZX_EXPECT(!world.IsAlive(victim));
    ZX_EXPECT(log.Contains("Cube"));
    u32 cubes = 0;
    world.Query<const MeshRenderer, const Transform>().Each([&](ecs::Entity, const MeshRenderer&, const Transform& t) {
        cubes += t.position.y == 5.0f;
    });
    ZX_EXPECT_EQ(cubes, 1u);
    game.End();
}

ZX_TEST(GameModule, HostReadsInputAndActions)
{
    InputActionMap map = InputActionMap::Defaults();
    InputActionState actions;
    InputState input;
    input.SetKey(Key::W, true);
    input.SetKey(Key::Space, true);
    actions.Update(map, &input);

    struct Probe : game::ScriptComponent
    {
        float forward = 0.0f;
        bool jump = false, w = false, shift = true;
        void OnUpdate(float) override
        {
            forward = host->Axis("MoveForward");
            jump = host->ActionPressed("Jump");
            w = host->KeyDown("W");
            shift = host->KeyDown("Shift");
        }
    };
    static Probe* s_probe = nullptr;
    static game::ComponentDesc probeDesc = {"Probe", "", [] { return static_cast<game::ScriptComponent*>(s_probe = new Probe()); },
                                            [](game::ScriptComponent* c) { delete c; }, nullptr, 0, nullptr};
    const game::ModuleDesc module = {game::ApiVersion, "Probe", &probeDesc};
    GameModuleSystem game;
    ZX_REQUIRE(game.LoadFromDesc(&module));
    ecs::World world;
    world.Create(Transform::Identity(), NativeScript{String("Probe"), String()});
    GameTickContext ctx;
    ctx.input = &input;
    ctx.actions = &actions;
    game.Tick(world, 0.016f, ctx);
    ZX_REQUIRE(s_probe != nullptr);
    ZX_EXPECT_NEAR(s_probe->forward, 1.0f, 1e-6f);
    ZX_EXPECT(s_probe->jump);
    ZX_EXPECT(s_probe->w);
    ZX_EXPECT(!s_probe->shift);
    game.End();
}

// --- Input Actions ---------------------------------------------------------------------------------------------------------

ZX_TEST(InputActions, DefaultsAndJsonRoundTrip)
{
    const InputActionMap map = InputActionMap::Defaults();
    ZX_REQUIRE(map.Find("jump") != nullptr); // case-insensitive
    ZX_EXPECT(!map.Find("Jump")->axis);
    ZX_REQUIRE(map.Find("MoveForward") != nullptr);
    ZX_EXPECT(map.Find("MoveForward")->axis);

    InputActionMap copy;
    ZX_REQUIRE(copy.FromJson(map.ToJson()));
    ZX_EXPECT_EQ(copy.actions.Size(), map.actions.Size());
    for (usize i = 0; i < map.actions.Size(); ++i) {
        ZX_EXPECT(copy.actions[i].name == map.actions[i].name);
        ZX_EXPECT_EQ(copy.actions[i].axis, map.actions[i].axis);
        ZX_EXPECT_EQ(copy.actions[i].bindings.Size(), map.actions[i].bindings.Size());
    }
    ZX_EXPECT_NEAR(copy.Find("MoveForward")->bindings[1].scale, -1.0f, 1e-6f);
    ZX_EXPECT(!copy.FromJson(JsonValue::MakeObject()));
    ZX_EXPECT_GT(InputControlNames().Size(), 40u);
}

ZX_TEST(InputActions, ButtonsAndAxes)
{
    const InputActionMap map = InputActionMap::Defaults();
    InputActionState actions;
    InputState input;

    input.SetKey(Key::Space, true);
    input.SetKey(Key::W, true);
    input.SetKey(Key::Up, true); // W + Up: clamped to 1
    input.SetKey(Key::RightShift, true);
    actions.Update(map, &input);
    ZX_EXPECT(actions.Down("Jump"));
    ZX_EXPECT(actions.Pressed("Jump"));
    ZX_EXPECT(actions.Down("Sprint")); // "Shift" matches either side
    ZX_EXPECT_NEAR(actions.Axis("MoveForward"), 1.0f, 1e-6f);
    ZX_EXPECT_NEAR(actions.Axis("MoveRight"), 0.0f, 1e-6f);

    // Held: no longer "pressed".
    actions.Update(map, &input);
    ZX_EXPECT(actions.Down("Jump"));
    ZX_EXPECT(!actions.Pressed("Jump"));

    // Opposite keys cancel; release is reported once.
    input.SetKey(Key::Space, false);
    input.SetKey(Key::Up, false);
    input.SetKey(Key::S, true);
    actions.Update(map, &input);
    ZX_EXPECT(actions.Released("Jump"));
    ZX_EXPECT_NEAR(actions.Axis("MoveForward"), 0.0f, 1e-6f);
    actions.Update(map, &input);
    ZX_EXPECT(!actions.Released("Jump"));

    // Gamepad stick and unknown actions.
    GamepadState pad;
    pad.connected = true;
    pad.leftStick = Vec2(-0.5f, 0.0f);
    input.SetGamepad(0, pad);
    actions.Update(map, &input);
    ZX_EXPECT_NEAR(actions.Axis("MoveRight"), -0.5f, 1e-6f);
    ZX_EXPECT(!actions.Down("NoSuchAction"));
    ZX_EXPECT_NEAR(actions.Axis("NoSuchAction"), 0.0f, 1e-6f);

    // Null input releases everything.
    actions.Update(map, nullptr);
    ZX_EXPECT(!actions.Down("Sprint"));
    ZX_EXPECT(actions.Released("Sprint"));
}
