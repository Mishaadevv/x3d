// ZGraph: the node library generates valid ZeqouScript for every node, .zgraph files round-trip, graphs run on
// entities through the Script component (events, variables as props, flow nodes, Delay), errors name their node.

#include "Core/CoreInit.h"
#include "Core/Log/Log.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Engine/Scripting.h"
#include "Platform/Input.h"
#include "Engine/ZGraph.h"
#include "Scene/Hierarchy.h"
#include "Script/ZeqouScript.h"

using namespace zx;

ZX_DEFINE_LOG_CATEGORY_STATIC(LogZGraphTest, Info);

namespace {

bool CompilesAsScript(const String& source, String* error = nullptr)
{
    script::CompileError ce;
    Result<script::Program*> p = script::Compile(source, "test.zgraph", &ce);
    if (!p) {
        if (error)
            *error = String::Format("{}:{}: {}\n{}", ce.line, ce.column, ce.message, source);
        return false;
    }
    script::DestroyProgram(*p);
    return true;
}

GraphNode& AddNode(Graph& g, StringView type)
{
    GraphNode n;
    n.id = g.NextId();
    n.type = String(type);
    g.nodes.PushBack(move(n));
    return g.nodes.Back();
}

} // namespace

ZX_TEST(ZGraph, EveryNodeGeneratesValidScript)
{
    // One graph per node type: On Start -> the node (actions / flow), or On Start -> Print(the node's outputs) for values.
    u32 tested = 0;
    for (const GraphNodeDef& d : GraphNodeLibrary()) {
        Graph g;
        g.name = String(d.id);
        g.variables.PushBack(GraphVariable{"v", GraphPinType::Number, "1", true, "A variable"});
        GraphNode& start = AddNode(g, "event.start");
        const u32 startId = start.id;
        if (d.kind == GraphNodeKind::Event) {
            GraphNode& e = AddNode(g, d.id);
            if (StringView(d.id) == "event.custom" || StringView(d.id) == "action.callEvent")
                e.settings.InsertOrAssign("event", "Ping");
            const u32 eid = e.id;
            GraphNode& print = AddNode(g, "action.print");
            g.links.PushBack(GraphLink{eid, "then", print.id, "in"});
        } else if (d.kind == GraphNodeKind::Pure) {
            GraphNode& n = AddNode(g, d.id);
            n.settings.InsertOrAssign("variable", "v");
            const u32 nid = n.id;
            u32 prev = startId;
            const char* prevPin = "then";
            for (u32 o = 0; o < d.OutputCount(); ++o) {
                GraphNode& print = AddNode(g, "action.print");
                g.links.PushBack(GraphLink{prev, prevPin, print.id, "in"});
                g.links.PushBack(GraphLink{nid, d.outputs[o].name, print.id, "text"});
                prev = print.id;
                prevPin = "then";
            }
        } else {
            GraphNode& n = AddNode(g, d.id);
            n.settings.InsertOrAssign("variable", "v");
            n.settings.InsertOrAssign("event", "Ping");
            const u32 nid = n.id;
            g.links.PushBack(GraphLink{startId, "then", nid, "in"});
            // Every exec output continues into a Print.
            for (u32 o = 0; o < d.OutputCount(); ++o)
                if (d.outputs[o].type == GraphPinType::Exec) {
                    GraphNode& print = AddNode(g, "action.print");
                    g.links.PushBack(GraphLink{nid, d.outputs[o].name, print.id, "in"});
                }
            if (StringView(d.id) == "action.callEvent")
                AddNode(g, "event.custom").settings.InsertOrAssign("event", "Ping");
        }
        GraphCompileResult r;
        const bool ok = CompileGraph(g, r);
        ZX_EXPECT(ok);
        if (!ok)
            ZX_LOG_ERROR(LogZGraphTest, "{}: {}", d.id, r.errors[0]);
        String error;
        const bool compiles = CompilesAsScript(r.source, &error);
        ZX_EXPECT(compiles);
        if (!compiles)
            ZX_LOG_ERROR(LogZGraphTest, "{}: {}", d.id, error);
        ++tested;
    }
    ZX_EXPECT(tested > 70u);
}

ZX_TEST(ZGraph, FileRoundTripAndErrors)
{
    const Graph g = MakeExampleGraph();
    Graph back;
    String error;
    ZX_REQUIRE(ParseGraph(WriteGraph(g), back, &error));
    ZX_EXPECT(back.name == g.name);
    ZX_EXPECT_EQ(back.nodes.Size(), g.nodes.Size());
    ZX_EXPECT_EQ(back.links.Size(), g.links.Size());
    ZX_EXPECT_EQ(back.variables.Size(), 2u);
    ZX_EXPECT(back.variables[0].exposed);
    ZX_EXPECT(*back.FindNode(3)->settings.Find("variable") == "speed");
    GraphCompileResult r;
    ZX_REQUIRE(CompileGraph(back, r));
    ZX_EXPECT(r.source.Contains("prop speed = 90"));
    ZX_EXPECT(CompilesAsScript(r.source));
    // Errors: unknown nodes, type mismatches, exec loops - each names its node.
    Graph bad;
    AddNode(bad, "event.start");
    AddNode(bad, "action.nope");
    AddNode(bad, "action.setPosition"); // #3
    AddNode(bad, "pure.getName");       // #4: text into a vector
    bad.links.PushBack(GraphLink{1, "then", 3, "in"});
    bad.links.PushBack(GraphLink{4, "name", 3, "position"});
    AddNode(bad, "action.print"); // #5 -> #6 -> #5
    AddNode(bad, "action.print");
    bad.links.PushBack(GraphLink{3, "then", 5, "in"});
    bad.links.PushBack(GraphLink{5, "then", 6, "in"});
    bad.links.PushBack(GraphLink{6, "then", 5, "in"});
    GraphCompileResult br;
    ZX_EXPECT(!CompileGraph(bad, br));
    String all;
    for (const String& e : br.errors)
        all += e + "\n";
    ZX_EXPECT(all.Contains("unknown node type 'action.nope'"));
    ZX_EXPECT(all.Contains("Node 'Set Position' (#3): 'position' expects vec3 but gets string"));
    ZX_EXPECT(all.Contains("loop back"));
    ZX_EXPECT(!ParseGraph("{\"format\": \"zscene\"}", back, &error));
}

ZX_TEST(ZGraph, RunsOnEntities)
{
    const String root = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_zgraph_test");
    platform::DeleteDirectoryTree(root);
    // The example spinner, and a graph using Do Once + Delay + a variable.
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Spin.zgraph", WriteGraph(MakeExampleGraph())));
    Graph g;
    g.name = "Mover";
    g.variables.PushBack(GraphVariable{"height", GraphPinType::Number, "3", true, {}});
    // (ids, not references: AddNode grows the array)
    AddNode(g, "event.update");                                // #1
    AddNode(g, "flow.doOnce");                                 // #2
    AddNode(g, "flow.delay").values.InsertOrAssign("seconds", "0.05"); // #3
    AddNode(g, "action.setPosition");                          // #4
    AddNode(g, "pure.makeVec3");                               // #5
    AddNode(g, "pure.getVariable").settings.InsertOrAssign("variable", "height"); // #6
    g.links.PushBack(GraphLink{1, "then", 2, "in"});
    g.links.PushBack(GraphLink{2, "then", 3, "in"});
    g.links.PushBack(GraphLink{3, "completed", 4, "in"});
    g.links.PushBack(GraphLink{6, "value", 5, "y"});
    g.links.PushBack(GraphLink{5, "vector", 4, "position"});
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Mover.zgraph", WriteGraph(g)));

    ecs::World world;
    scene::InstallHierarchyHooks(world);
    const ecs::Entity spinner = scene::CreateNode(world, "Spinner");
    world.Add(spinner, Script{String("Scripts/Spin.zgraph"), String("{\"speed\": 180}")}); // the exposed variable as a prop
    const ecs::Entity mover = scene::CreateNode(world, "Mover");
    world.Add(mover, Script{String("Scripts/Mover.zgraph"), String()});
    ScriptSystem scripts;
    scripts.SetContentRoot(root);
    scripts.Begin(world);
    for (u32 i = 0; i < 10; ++i)
        scripts.Tick(world, 0.05f, nullptr);
    // 180 degrees per second for 0.5 s: a quarter turn around Y.
    const Vec3 forward = world.Get<const Transform>(spinner)->rotation.GetForward();
    ZX_EXPECT(Abs(Abs(forward.x) - 1.0f) < 0.05f);
    // The delayed Set Position ran once with the variable.
    ZX_EXPECT(Abs(world.Get<const Transform>(mover)->position.y - 3.0f) < 1e-4f);
    scripts.End();
    platform::DeleteDirectoryTree(root);
}

ZX_TEST(ZGraph, TracesNodesForTheDebugger)
{
    // Traced graphs report every event and statement as it runs (the editor highlights them, breakpoints pause).
    GraphCompileResult traced;
    ZX_REQUIRE(CompileGraph(MakeExampleGraph(), traced, true));
    ZX_EXPECT(traced.source.Contains("__trace(1)"));
    ZX_EXPECT(CompilesAsScript(traced.source));
    const String root = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_zgraph_trace");
    platform::DeleteDirectoryTree(root);
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Spin.zgraph", WriteGraph(MakeExampleGraph())));
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    const ecs::Entity e = scene::CreateNode(world, "Spinner");
    world.Add(e, Script{String("Scripts/Spin.zgraph"), String()});
    ScriptSystem scripts;
    scripts.SetContentRoot(root);
    scripts.traceGraphs = true;
    Array<u32> nodes;
    scripts.onGraphTrace = [&](StringView file, ecs::Entity entity, u32 node) {
        if (file == "Scripts/Spin.zgraph" && entity == e)
            nodes.PushBack(node);
    };
    scripts.Begin(world);
    scripts.Tick(world, 0.016f, nullptr);
    ZX_EXPECT(nodes.Contains(1u)); // On Update
    ZX_EXPECT(nodes.Contains(2u)); // Rotate
    ZX_EXPECT(!nodes.Contains(7u)); // Set Variable: only on Space
    Array<ecs::Entity> running;
    scripts.InstancesOf("Scripts/Spin.zgraph", running);
    ZX_EXPECT(running.Size() == 1u && running[0] == e);
    String speed;
    ZX_EXPECT(scripts.ReadField(e, "Scripts/Spin.zgraph", "speed", speed) && speed == "90");
    scripts.End();
    platform::DeleteDirectoryTree(root);
}

ZX_TEST(ZGraph, ThirdPersonMiniGame)
{
    // The acceptance demo's graphs compile; a coin next to the player is collected, the gate opens with enough coins.
    for (const Graph& g : {MakeCoinGraph(), MakeGateGraph(), MakeGameHUDGraph()}) {
        GraphCompileResult r;
        const bool ok = CompileGraph(g, r);
        ZX_EXPECT(ok);
        if (!ok)
            ZX_LOG_ERROR(LogZGraphTest, "{}: {}", g.name, r.errors[0]);
        ZX_EXPECT(r.warnings.Empty());
        String error;
        const bool compiles = CompilesAsScript(r.source, &error);
        ZX_EXPECT(compiles);
        if (!compiles)
            ZX_LOG_ERROR(LogZGraphTest, "{}: {}", g.name, error);
    }
    const String root = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_zgraph_demo");
    platform::DeleteDirectoryTree(root);
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Coin.zgraph", WriteGraph(MakeCoinGraph())));
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Gate.zgraph", WriteGraph(MakeGateGraph())));
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    scene::CreateNode(world, "Player", Transform(Vec3(0, 0, 0)));
    const ecs::Entity nearCoin = scene::CreateNode(world, "Coin 1", Transform(Vec3(0.5f, 0.7f, 0)));
    world.Add(nearCoin, Script{String("Scripts/Coin.zgraph"), String()});
    const ecs::Entity farCoin = scene::CreateNode(world, "Coin 2", Transform(Vec3(10, 0.7f, 0)));
    world.Add(farCoin, Script{String("Scripts/Coin.zgraph"), String()});
    const ecs::Entity gate = scene::CreateNode(world, "Gate", Transform(Vec3(0, 1.6f, 2)));
    world.Add(gate, Script{String("Scripts/Gate.zgraph"), String("{\"coinsNeeded\": 1}")});
    ScriptSystem scripts;
    scripts.SetContentRoot(root);
    scripts.Begin(world);
    scripts.Tick(world, 0.016f, nullptr);
    scripts.Tick(world, 0.016f, nullptr);
    ZX_EXPECT(!world.IsAlive(nearCoin)); // collected
    ZX_EXPECT(world.IsAlive(farCoin));
    const script::Value* coins = scripts.GameValue("coins");
    ZX_EXPECT(coins && coins->AsFloat() == 1.0f);
    // E at the gate: it slides up 3.4 m over 1.6 s.
    InputState input;
    input.BeginFrame();
    input.SetKey(Key::E, true);
    scripts.Tick(world, 0.016f, &input);
    input.BeginFrame();
    input.SetKey(Key::E, false);
    for (u32 i = 0; i < 120; ++i)
        scripts.Tick(world, 0.02f, &input);
    ZX_EXPECT(Abs(world.Get<const Transform>(gate)->position.y - 5.0f) < 0.01f);
    scripts.End();
    platform::DeleteDirectoryTree(root);
}
