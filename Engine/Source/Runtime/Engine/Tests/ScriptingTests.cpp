#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Core/Threading/Thread.h"
#include "Engine/Scripting.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

String ScriptRoot(StringView name)
{
    const String dir = Path::Join(Path::Normalize(platform::GetTempDirectory()), String::Format("zx_script_test/{}", name));
    platform::DeleteDirectoryTree(dir);
    platform::CreateDirectoryTree(dir + "/Scripts");
    return dir;
}

ecs::Entity Spawn(ecs::World& world, StringView name, StringView file, StringView props = {})
{
    return world.Create(scene::EntityName{String(name)}, Transform::Identity(), Script{String(file), String(props)});
}

} // namespace

ZX_TEST(Scripting, RunsStartAndUpdate)
{
    const String root = ScriptRoot("run");
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Mover.zs", R"zs(
        script Mover {
            prop speed = 2.0
            prop label = "a"
            var started = 0
            fn start() { started += 1; log("start " + label) }
            fn update(dt) {
                transform.position.x += speed * dt
                if input.key("Space") { entity.position = vec3(0) }
                if time.frame == 3 and label == "killer" { find("Victim").destroy() }
            }
        }
    )zs"));
    ScriptSystem scripts;
    scripts.SetContentRoot(root);
    String log;
    u32 errors = 0;
    scripts.onLog = [&](StringView s, bool isError) {
        log += s;
        log += '\n';
        errors += isError;
    };

    ecs::World world;
    const ecs::Entity a = Spawn(world, "A", "Scripts/Mover.zs");
    const ecs::Entity b = Spawn(world, "B", "Scripts/Mover.zs", R"({"speed": 10, "label": "killer"})");
    const ecs::Entity victim = world.Create(scene::EntityName{String("Victim")}, Transform::Identity());

    const script::Program* program = scripts.GetProgram("Scripts/Mover.zs");
    ZX_REQUIRE(program != nullptr);
    ZX_EXPECT_EQ(program->properties.Size(), usize(2));

    scripts.Begin(world);
    for (u32 i = 0; i < 4; ++i)
        scripts.Tick(world, 0.5f, nullptr);
    ZX_EXPECT_EQ(errors, 0u);
    ZX_EXPECT_EQ(scripts.InstanceCount(), 2u);
    ZX_EXPECT_NEAR(world.Get<Transform>(a)->position.x, 4.0f, 1e-5f);  // 4 * 0.5 * 2
    ZX_EXPECT_NEAR(world.Get<Transform>(b)->position.x, 20.0f, 1e-5f); // per-entity prop override
    ZX_EXPECT(!world.IsAlive(victim));
    ZX_EXPECT(log.Contains("start a") && log.Contains("start killer"));

    // Entities created while playing start on the next tick; removed ones stop.
    const ecs::Entity c = Spawn(world, "C", "Scripts/Mover.zs");
    world.Destroy(a);
    scripts.Tick(world, 0.5f, nullptr);
    ZX_EXPECT_EQ(scripts.InstanceCount(), 2u);
    ZX_EXPECT_NEAR(world.Get<Transform>(c)->position.x, 1.0f, 1e-5f);
    scripts.End();
    ZX_EXPECT_EQ(scripts.InstanceCount(), 0u);
}

ZX_TEST(Scripting, HotReloadKeepsState)
{
    const String root = ScriptRoot("reload");
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Counter.zs", "script Counter { var n = 0\n fn update(dt) { n += 1\n transform.position.y = n } }"));
    ScriptSystem scripts;
    scripts.SetContentRoot(root);
    u32 errors = 0;
    scripts.onLog = [&](StringView, bool isError) { errors += isError; };
    ecs::World world;
    const ecs::Entity e = Spawn(world, "E", "Scripts/Counter.zs");
    scripts.Begin(world);
    scripts.Tick(world, 0.1f, nullptr);
    scripts.Tick(world, 0.1f, nullptr);
    ZX_EXPECT_NEAR(world.Get<Transform>(e)->position.y, 2.0f, 1e-5f);

    // New version counts by 10; n survives the reload.
    Thread::SleepMs(20);
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Counter.zs", "script Counter { var n = 0\n fn update(dt) { n += 10\n transform.position.y = n } }"));
    scripts.CheckForChanges();
    scripts.Tick(world, 0.1f, nullptr);
    ZX_EXPECT_NEAR(world.Get<Transform>(e)->position.y, 12.0f, 1e-5f);

    // A broken edit keeps the running version and reports the error once.
    Thread::SleepMs(20);
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Counter.zs", "script Counter { fn update(dt) { n += } }"));
    scripts.CheckForChanges();
    scripts.Tick(world, 0.1f, nullptr);
    ZX_EXPECT_EQ(errors, 1u);
    ZX_EXPECT_NEAR(world.Get<Transform>(e)->position.y, 22.0f, 1e-5f);
    String error;
    ZX_EXPECT(scripts.GetProgram("Scripts/Counter.zs", &error) == nullptr);
    ZX_EXPECT(error.Contains("Counter.zs:1:"));
    scripts.End();
}

ZX_TEST(Scripting, RuntimeErrorStopsOnlyThatInstance)
{
    const String root = ScriptRoot("errors");
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Bad.zs", "script Bad {\n fn update(dt) {\n  entity.nope = 1\n }\n}"));
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Good.zs", "script Good { fn update(dt) { entity.rotate(vec3(0, 90, 0)) } }"));
    ScriptSystem scripts;
    scripts.SetContentRoot(root);
    Array<String> errors;
    scripts.onLog = [&](StringView s, bool isError) {
        if (isError)
            errors.PushBack(String(s));
    };
    ecs::World world;
    Spawn(world, "Bad", "Scripts/Bad.zs");
    const ecs::Entity good = Spawn(world, "Good", "Scripts/Good.zs");
    Spawn(world, "Missing", "Scripts/Missing.zs");
    scripts.Begin(world);
    for (u32 i = 0; i < 3; ++i)
        scripts.Tick(world, 0.1f, nullptr);
    ZX_REQUIRE(errors.Size() == 2u); // each reported once
    ZX_EXPECT(errors[0].Contains("Bad.zs:3") || errors[1].Contains("Bad.zs:3"));
    ZX_EXPECT(errors[0].Contains("not found") || errors[1].Contains("not found"));
    const Vec3 f = world.Get<Transform>(good)->rotation.GetForward(); // 3 x 90 deg = 270 deg: forward -> -X
    ZX_EXPECT(IsNearlyEqual(f, Vec3(-1, 0, 0), 1e-4f));
    scripts.End();
}

ZX_TEST(Scripting, PropertyOverrides)
{
    script::Property p;
    p.name = "speed";
    p.kind = script::PropertyKind::Number;
    p.defaultValue = script::Value(1.0);
    Script s;
    SetScriptProperty(s, p, script::Value(3.5));
    ZX_EXPECT_EQ(s.properties, String(R"({"speed":3.5})"));
    ZX_EXPECT_EQ(JsonToScriptValue(ParseScriptProperties(s)["speed"], p).AsNumber(), 3.5);
    SetScriptProperty(s, p, script::Value(1.0)); // back to default: removed
    ZX_EXPECT(s.properties.Empty());
    ZX_EXPECT(ScriptTemplate("My Mover").Contains("script MyMover {"));
}
