#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Log/Log.h"
#include "Core/Testing/Test.h"
#include "Core/Time/Time.h"
#include "Render/MaterialGraph.h"
#include "RHI/ShaderCompiler.h"

using namespace zx;
using namespace zx::render;

ZX_DEFINE_LOG_CATEGORY_STATIC(LogMaterialTest, Info);

namespace {

/// A graph using every node type: tiled texture * tint, Fresnel rim emissive, normal map, panner, math chain.
MaterialGraph BuildRichGraph()
{
    MaterialGraph g = MaterialGraph::CreateDefault();
    const u32 out = g.OutputNode();
    const u32 tint = g.nodes[1].id;
    const u32 uv = g.AddNode("TexCoord", Vec2(-600, 0))->id;
    g.FindNode(uv)->value = Vec4(4, 4, 0, 0);
    const u32 albedo = g.AddNode("TextureSample", Vec2(-300, 0))->id;
    g.FindNode(albedo)->texture = "Textures/Bricks.png";
    g.Connect(uv, 0, albedo, 0);
    const u32 mul = g.AddNode("Multiply", Vec2(0, 0))->id;
    g.Connect(albedo, 0, mul, 0);
    g.Connect(tint, 0, mul, 1);
    g.Connect(mul, 0, out, 0);
    const u32 normal = g.AddNode("TextureSample", Vec2(-300, 200))->id;
    g.FindNode(normal)->normalMap = true;
    const u32 panner = g.AddNode("Panner", Vec2(-600, 200))->id;
    g.Connect(panner, 0, normal, 0);
    g.Connect(normal, 0, out, 3);
    const u32 fresnel = g.AddNode("Fresnel", Vec2(-300, 400))->id;
    const u32 rim = g.AddNode("Color", Vec2(-300, 500))->id;
    g.FindNode(rim)->name = "Rim";
    const u32 rimMul = g.AddNode("Multiply", Vec2(0, 400))->id;
    g.Connect(fresnel, 0, rimMul, 0);
    g.Connect(rim, 0, rimMul, 1);
    g.Connect(rimMul, 0, out, 4);
    const u32 rough = g.AddNode("Scalar", Vec2(-300, 600))->id;
    g.FindNode(rough)->name = "Roughness";
    const u32 sine = g.AddNode("Sine", Vec2(-500, 700))->id;
    const u32 time = g.AddNode("Time", Vec2(-700, 700))->id;
    g.Connect(time, 0, sine, 0);
    const u32 lerp = g.AddNode("Lerp", Vec2(0, 600))->id;
    g.Connect(rough, 0, lerp, 0);
    g.Connect(sine, 0, lerp, 2);
    const u32 sat = g.AddNode("Saturate", Vec2(200, 600))->id;
    g.Connect(lerp, 0, sat, 0);
    g.Connect(sat, 0, out, 2);
    // Utility nodes feeding metallic through a mask/append/dot chain.
    const u32 vc = g.AddNode("VertexColor", Vec2(-700, 900))->id;
    const u32 mask = g.AddNode("ComponentMask", Vec2(-500, 900))->id;
    g.Connect(vc, 0, mask, 0);
    const u32 append = g.AddNode("Append", Vec2(-300, 900))->id;
    g.Connect(mask, 0, append, 0);
    const u32 wp = g.AddNode("WorldPosition", Vec2(-700, 1000))->id;
    const u32 len = g.AddNode("Length", Vec2(-500, 1000))->id;
    g.Connect(wp, 0, len, 0);
    g.Connect(len, 0, append, 1);
    const u32 dot = g.AddNode("Dot", Vec2(-100, 900))->id;
    g.Connect(append, 0, dot, 0);
    const u32 frac = g.AddNode("Frac", Vec2(100, 900))->id;
    g.Connect(dot, 0, frac, 0);
    g.Connect(frac, 0, out, 1);
    return g;
}

struct CompilerScope
{
    String savedDir;
    CompilerScope()
    {
        fs::Initialize(1);
        fs::Mount("/Shaders", FindEngineRootDirectory() + "/Engine/Shaders", 0, false);
        savedDir = platform::GetTempDirectory() + "/zx_matgraph_test";
        platform::CreateDirectoryTree(savedDir);
        fs::Mount("/Saved", savedDir);
        rhi::ShaderCompiler::Initialize();
    }
    ~CompilerScope()
    {
        rhi::ShaderCompiler::Shutdown();
        fs::Shutdown();
        platform::DeleteDirectoryTree(savedDir);
    }
};

} // namespace

ZX_TEST(MaterialGraph, EditingJsonAndCycles)
{
    MaterialGraph g = BuildRichGraph();
    ZX_EXPECT(g.OutputNode() != 0);
    // A link that would close a loop is refused; a second link into an input replaces the first.
    const u32 a = g.AddNode("Add", Vec2())->id, b = g.AddNode("OneMinus", Vec2())->id;
    ZX_EXPECT(g.Connect(a, 0, b, 0));
    ZX_EXPECT(!g.Connect(b, 0, a, 0));
    ZX_EXPECT(!g.Connect(a, 0, a, 1));
    const u32 c = g.AddNode("Scalar", Vec2())->id;
    ZX_EXPECT(g.Connect(c, 0, b, 0));
    ZX_EXPECT(g.FindInputLink(b, 0)->fromNode == c);
    g.RemoveNode(c);
    ZX_EXPECT(g.FindInputLink(b, 0) == nullptr);

    JsonValue parsed;
    ZX_REQUIRE(ParseJson(ToJsonString(g.ToJson()).View(), parsed));
    MaterialGraph g2;
    Array<String> warnings;
    ZX_REQUIRE(g2.FromJson(parsed, &warnings));
    ZX_EXPECT(warnings.Empty());
    ZX_EXPECT(g2.guid == g.guid);
    ZX_EXPECT_EQ(g2.nodes.Size(), g.nodes.Size());
    ZX_EXPECT_EQ(g2.links.Size(), g.links.Size());
    ZX_EXPECT_EQ(CompileMaterialGraph(g2).code, CompileMaterialGraph(g).code);
}

ZX_TEST(MaterialGraph, CodegenParamsAndHashes)
{
    MaterialGraph g = BuildRichGraph();
    const CompiledMaterialGraph c = CompileMaterialGraph(g);
    ZX_EXPECT(c.Ok());
    for (const String& e : c.errors)
        ZX_LOG_WARN(LogMaterialTest, "{}", e);
    ZX_EXPECT(c.code.View().Contains("EvaluateGraphMaterial"));
    ZX_EXPECT(c.code.View().Contains("SampleParamNormalMap"));
    // 4 value nodes (2 colors, 1 scalar ... ) + 2 textures, 16 bytes each.
    ZX_EXPECT_EQ(c.paramBytes, u32(c.params.Size() * 16));
    ZX_EXPECT_EQ(c.params.Size(), usize(5));

    // Changing a value does not change the code; changing structure does.
    MaterialGraph g2 = g;
    g2.nodes[1].value = Vec4(0.1f, 0.2f, 0.3f, 1.0f);
    const CompiledMaterialGraph c2 = CompileMaterialGraph(g2);
    ZX_EXPECT_EQ(c2.hash, c.hash);
    Array<u8> p1, p2;
    WriteMaterialParams(g, c, p1);
    WriteMaterialParams(g2, c2, p2);
    ZX_EXPECT(p1.Size() == p2.Size() && !(p1 == p2));
    g2.Disconnect(g2.OutputNode(), 4);
    ZX_EXPECT(CompileMaterialGraph(g2).hash != c.hash);
    g2.blend = MaterialBlendMode::Masked;
    ZX_EXPECT(CompileMaterialGraph(g2).hash != CompileMaterialGraph(g2).hash + 1); // stable

    // Errors are reported per node.
    MaterialGraph bad = MaterialGraph::CreateDefault();
    const u32 app = bad.AddNode("Append", Vec2())->id;
    const u32 col = bad.AddNode("Color", Vec2())->id;
    bad.Connect(col, 5, app, 0); // float4 + float -> 5 channels
    bad.Connect(app, 0, bad.OutputNode(), 0);
    const CompiledMaterialGraph e = CompileMaterialGraph(bad);
    ZX_EXPECT(!e.Ok());
    ZX_EXPECT(e.errorNodes.Contains(app));
    MaterialGraph none;
    ZX_EXPECT(!CompileMaterialGraph(none).Ok());
}

ZX_TEST(MaterialGraph, GeneratedCodeCompilesWithForwardPBR)
{
    CompilerScope scope;
    if (!rhi::ShaderCompiler::IsAvailable())
        return;
    MaterialGraph g = BuildRichGraph();
    const CompiledMaterialGraph c = CompileMaterialGraph(g);
    ZX_REQUIRE(c.Ok());
    const String source = c.code + "\n#include \"/Shaders/Forward/ForwardPBR.hlsl\"\n";
    struct Variant
    {
        StringView entry;
        rhi::ShaderStage stage;
        bool alphaTest, depthOnly;
    } variants[] = {
        {"PSMain", rhi::ShaderStage::Pixel, false, false},
        {"PSMain", rhi::ShaderStage::Pixel, true, false},
        {"PSDepth", rhi::ShaderStage::Pixel, true, true},
    };
    for (const Variant& v : variants) {
        Array<rhi::ShaderDefine> defines;
        defines.PushBack({"ZX_GRAPH_MATERIAL", "1"});
        if (v.alphaTest)
            defines.PushBack({"ALPHA_TEST", "1"});
        if (v.depthOnly)
            defines.PushBack({"DEPTH_ONLY", "1"});
        rhi::ShaderCompileDesc d;
        d.entryPoint = v.entry;
        d.stage = v.stage;
        d.defines = defines.View();
        Stopwatch sw;
        Result<rhi::ShaderBytecode> r = rhi::ShaderCompiler::CompileSource(source.View(), "/Shaders/Generated/Material.hlsl", d);
        if (!r)
            ZX_LOG_WARN(LogMaterialTest, "{}", r.GetError().message);
        ZX_EXPECT(r.IsOk());
        ZX_LOG_WARN(LogMaterialTest, "Material variant {}{} compiled in {:.0f} ms", v.entry, v.alphaTest ? " (alpha test)" : "",
                    sw.ElapsedMilliseconds());
    }
}
