#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Engine/ZGraph.h"
#include "Engine/Project.h"
#include "Engine/ViewportRenderer.h"
#include "Render/MaterialGraph.h"
#include "RHI/EngineProfile.h"
#include "Script/ZeqouScript.h"

using namespace zx;

namespace {

String TempRoot(StringView name)
{
    const String dir = Path::Join(Path::Normalize(platform::GetTempDirectory()), String::Format("zx_project_test/{}", name));
    platform::DeleteDirectoryTree(dir);
    platform::CreateDirectoryTree(dir);
    return dir;
}

} // namespace

ZX_TEST(Project, CreateLoadAndValidate)
{
    ZX_EXPECT(ValidateProjectName("MyGame").Empty());
    ZX_EXPECT(ValidateProjectName("My_Game-2").Empty());
    ZX_EXPECT(!ValidateProjectName("").Empty());
    ZX_EXPECT(!ValidateProjectName("2Fast").Empty());
    ZX_EXPECT(!ValidateProjectName("My Game").Empty());
    ZX_EXPECT(!ValidateProjectName("Игра").Empty());

    const String parent = TempRoot("create");
    Result<ProjectInfo> p = CreateProject(parent, "Demo");
    ZX_EXPECT(p.IsOk());
    if (!p.IsOk())
        return;
    ZX_EXPECT_EQ(p->descriptor.name, String("Demo"));
    ZX_EXPECT_EQ(p->root, Path::Join(parent, "Demo"));
    ZX_EXPECT(platform::DirectoryExists(p->contentDir + "/Scenes"));
    ZX_EXPECT(platform::FileExists(p->root + "/Demo.zproj"));

    // Existing non-empty folder is refused.
    ZX_EXPECT(!CreateProject(parent, "Demo").IsOk());
    ZX_EXPECT(!CreateProject(parent, "Bad Name").IsOk());

    // Round trip of the descriptor.
    ProjectDescriptor d = p->descriptor;
    d.startupScene = "Content/Scenes/Main.zscene";
    ZX_EXPECT(SaveProjectDescriptor(p->descriptorPath, d));
    Result<ProjectInfo> again = LoadProject(p->descriptorPath);
    ZX_EXPECT(again.IsOk() && again->descriptor.startupScene == "Content/Scenes/Main.zscene");

    fs::WriteTextFile(parent + "/broken.zproj", "{ \"format\": \"other\" }");
    ZX_EXPECT(!LoadProject(parent + "/broken.zproj").IsOk());
    ZX_EXPECT(!LoadProject(parent + "/missing.zproj").IsOk());
    platform::DeleteDirectoryTree(parent);
}

ZX_TEST(Project, RecentProjects)
{
    const String dir = TempRoot("recent");
    const String file = dir + "/Recent.json";
    ProjectInfo a, b;
    a.descriptorPath = "C:/Projects/A/A.zproj";
    a.descriptor.name = "A";
    b.descriptorPath = "C:/Projects/B/B.zproj";
    b.descriptor.name = "B";
    TouchRecentProject(a, file);
    TouchRecentProject(b, file);
    TouchRecentProject(a, file); // moves A back to the front, no duplicate
    Array<RecentProject> list = LoadRecentProjects(file);
    ZX_EXPECT_EQ(list.Size(), usize(2));
    ZX_EXPECT(list.Size() == 2 && list[0].name == "A" && list[1].name == "B" && list[0].lastOpened > 0);
    RemoveRecentProject("C:/Projects/A/A.zproj", file);
    list = LoadRecentProjects(file);
    ZX_EXPECT(list.Size() == 1 && list[0].name == "B");
    platform::DeleteDirectoryTree(dir);
}

ZX_TEST(Project, CopySourceWithDependencies)
{
    const String root = TempRoot("copy");
    const String src = root + "/src";
    platform::CreateDirectoryTree(src + "/bin");
    fs::WriteTextFile(src + "/model.gltf",
                      "{ \"buffers\": [ { \"uri\": \"bin/data.bin\" }, { \"uri\": \"data:application/octet-stream;base64,AAAA\" } ],"
                      "  \"images\": [ { \"uri\": \"tex%20a.png\" } ] }");
    fs::WriteTextFile(src + "/bin/data.bin", "bytes");
    fs::WriteTextFile(src + "/tex a.png", "png");
    fs::WriteTextFile(src + "/model.gltf.zmeta", "{ \"guid\": \"x\" }");
    Result<String> copied = CopySourceWithDependencies(src + "/model.gltf", root + "/dst/Model");
    ZX_EXPECT(copied.IsOk());
    ZX_EXPECT(copied.IsOk() && *copied == root + "/dst/Model/model.gltf");
    ZX_EXPECT(platform::FileExists(root + "/dst/Model/bin/data.bin"));
    ZX_EXPECT(platform::FileExists(root + "/dst/Model/tex a.png"));
    ZX_EXPECT(platform::FileExists(root + "/dst/Model/model.gltf.zmeta"));

    // References outside the source folder are refused.
    fs::WriteTextFile(src + "/escape.gltf", "{ \"buffers\": [ { \"uri\": \"../outside.bin\" } ] }");
    ZX_EXPECT(!CopySourceWithDependencies(src + "/escape.gltf", root + "/dst/Escape").IsOk());

    // OBJ: material library and its textures.
    fs::WriteTextFile(src + "/box.obj", "mtllib box.mtl\nv 0 0 0\n");
    fs::WriteTextFile(src + "/box.mtl", "newmtl m\nmap_Kd -bm 1.0 bin/albedo.png\n");
    fs::WriteTextFile(src + "/bin/albedo.png", "png");
    ZX_EXPECT(CopySourceWithDependencies(src + "/box.obj", root + "/dst/Box").IsOk());
    ZX_EXPECT(platform::FileExists(root + "/dst/Box/box.mtl"));
    ZX_EXPECT(platform::FileExists(root + "/dst/Box/bin/albedo.png"));
    platform::DeleteDirectoryTree(root);
}

ZX_TEST(Project, SettingsAndStarterContent)
{
    const String parent = TempRoot("settings");
    ProjectDescriptor settings;
    settings.language = "ZeqouScript";
    Result<ProjectInfo> p = CreateProject(parent, "Colored", settings);
    ZX_REQUIRE(p.IsOk());
    ZX_EXPECT(p->descriptor.color != 0u); // a random tile color, stored in the .zproj
    ZX_EXPECT_EQ(p->descriptor.language, String("ZeqouScript"));
    Result<ProjectInfo> again = LoadProject(p->descriptorPath);
    ZX_REQUIRE(again.IsOk());
    ZX_EXPECT_EQ(again->descriptor.color, p->descriptor.color);
    ZX_EXPECT_EQ(ProjectTileColor(again->descriptor), p->descriptor.color);

    // Starter content: every material parses, every script compiles.
    ZX_EXPECT_GE(WriteStarterContent(p->contentDir), 10u);
    Array<platform::DirectoryEntry> files;
    platform::ListDirectory(p->contentDir + "/StarterContent/Materials", files);
    ZX_EXPECT_GE(files.Size(), usize(5));
    for (const platform::DirectoryEntry& f : files) {
        String text;
        JsonValue json;
        render::MaterialGraph g;
        ZX_EXPECT(fs::ReadTextFile(p->contentDir + "/StarterContent/Materials/" + f.name, text) && ParseJson(text, json) && g.FromJson(json));
        ZX_EXPECT(render::CompileMaterialGraph(g).errors.Empty());
    }
    files.Clear();
    platform::ListDirectory(p->contentDir + "/StarterContent/Scripts", files);
    ZX_EXPECT_GE(files.Size(), usize(4));
    for (const platform::DirectoryEntry& f : files) {
        String text;
        ZX_EXPECT(fs::ReadTextFile(p->contentDir + "/StarterContent/Scripts/" + f.name, text));
        if (Path::HasExtension(f.name, ".zgraph")) { // script graphs compile to ZeqouScript first
            Graph graph;
            GraphCompileResult generated;
            ZX_EXPECT(ParseGraph(text, graph) && CompileGraph(graph, generated));
            text = generated.source;
        }
        script::CompileError e;
        Result<script::Program*> program = script::Compile(text, f.name, &e);
        if (!program)
            test::ReportFailure(__FILE__, __LINE__, String::Format("{}:{}: {}", f.name, e.line, e.message));
        else
            script::DestroyProgram(*program);
    }
    platform::DeleteDirectoryTree(parent);
}

ZX_TEST(Project, AssetPacks)
{
    // A fake engine with one pack: listed with its Pack.json, copied (models + license) into Content/Packs/<folder>.
    const String engine = TempRoot("packs_engine");
    const String pack = engine + "/Engine/Content/Packs/TestPack";
    platform::CreateDirectoryTree(pack + "/Models/Textures");
    fs::WriteTextFile(pack + "/Pack.json", R"({"name": "Test Pack", "description": "d", "author": "a", "license": "CC0 1.0", "models": 1})");
    fs::WriteTextFile(pack + "/License.txt", "CC0");
    fs::WriteTextFile(pack + "/Models/Rock.gltf", "{}");
    fs::WriteTextFile(pack + "/Models/Textures/Rock.png", "png");
    platform::CreateDirectoryTree(engine + "/Engine/Content/Packs/NotAPack"); // no Pack.json: ignored

    const Array<AssetPackInfo> packs = ListAssetPacks(engine);
    ZX_EXPECT_EQ(packs.Size(), usize(1));
    if (packs.Size() != 1)
        return;
    ZX_EXPECT_EQ(packs[0].folder, String("TestPack"));
    ZX_EXPECT_EQ(packs[0].name, String("Test Pack"));
    ZX_EXPECT_EQ(packs[0].models, 1u);
    ZX_EXPECT_EQ(packs[0].bytes, u64(5)); // "{}" + "png"

    const String content = TempRoot("packs_project");
    ZX_EXPECT_EQ(CopyAssetPack(engine, "TestPack", content), 3u);
    ZX_EXPECT(platform::FileExists(content + "/Packs/TestPack/Rock.gltf"));
    ZX_EXPECT(platform::FileExists(content + "/Packs/TestPack/Textures/Rock.png"));
    ZX_EXPECT(platform::FileExists(content + "/Packs/TestPack/License.txt"));
    ZX_EXPECT(!platform::FileExists(content + "/Packs/TestPack/Pack.json"));
    ZX_EXPECT_EQ(CopyAssetPack(engine, "Missing", content), 0u);
    ZX_EXPECT_EQ(CopyAssetPack(engine, "../Packs/TestPack", content), 0u);
}

ZX_TEST(Project, EnginePacksShipWithTheEngine)
{
    const Array<AssetPackInfo> packs = ListAssetPacks(FindEngineRootDirectory());
    u32 found = 0;
    for (const AssetPackInfo& p : packs) {
        if (p.folder == "SurvivalPack" || p.folder == "MedievalVillage" || p.folder == "StylizedNature")
            ++found;
        ZX_EXPECT(p.models > 0);
        ZX_EXPECT(p.license == "CC0 1.0");
    }
    ZX_EXPECT_EQ(found, 3u);
}

ZX_TEST(Project, EngineProfilePreference)
{
    const String file = TempRoot("profile") + "/EngineProfile.txt";
    ZX_EXPECT_EQ(LoadEngineProfilePreference(file), String("auto")); // no file yet
    ZX_EXPECT(SaveEngineProfilePreference("full", file));
    ZX_EXPECT_EQ(LoadEngineProfilePreference(file), String("full"));
    ZX_EXPECT(SaveEngineProfilePreference("lite", file));
    ZX_EXPECT_EQ(LoadEngineProfilePreference(file), String("lite"));
    fs::WriteTextFile(file, "  FULL \r\n");
    ZX_EXPECT_EQ(LoadEngineProfilePreference(file), String("full"));
    fs::WriteTextFile(file, "ultra");
    ZX_EXPECT_EQ(LoadEngineProfilePreference(file), String("auto"));
}

ZX_TEST(Project, GraphicsEra)
{
    for (GraphicsEra e : {GraphicsEra::Modern, GraphicsEra::Y2017, GraphicsEra::Y2011, GraphicsEra::Y2008})
        ZX_EXPECT(ParseGraphicsEra(GraphicsEraId(e)) == e);
    ZX_EXPECT(ParseGraphicsEra("nonsense") == GraphicsEra::Modern);

    ViewportSettings base;
    base.scene.shadows.filter = 3; // PCSS
    base.scene.shadows.cascades = 4;
    base.scene.deferred = true;
    ViewportFeatures features;
    ViewportSettings s = base;
    ApplyGraphicsEra(s, features, GraphicsEra::Modern);
    ZX_EXPECT_EQ(s.scene.shadows.filter, 3u);

    ViewportFeatures f2017;
    ViewportSettings s2017 = base;
    ApplyGraphicsEra(s2017, f2017, GraphicsEra::Y2017);
    ZX_EXPECT(s2017.scene.shadows.filter <= 2u); // no PCSS
    ZX_EXPECT(f2017.temporalAA && f2017.volumetricFog && f2017.clouds && !f2017.fluids);

    ViewportFeatures f2011;
    ViewportSettings s2011 = base;
    ApplyGraphicsEra(s2011, f2011, GraphicsEra::Y2011);
    ZX_EXPECT(!f2011.temporalAA && !f2011.volumetricFog && !f2011.clouds && !f2011.screenSpaceReflections);
    ZX_EXPECT(s2011.post.fxaa && s2011.scene.deferred);
    ZX_EXPECT(s2011.post.tonemapper == render::Tonemapper::Filmic);

    ViewportFeatures f2008;
    ViewportSettings s2008 = base;
    ApplyGraphicsEra(s2008, f2008, GraphicsEra::Y2008);
    ZX_EXPECT(!s2008.scene.deferred && !s2008.post.fxaa && !f2008.temporalAA);
    ZX_EXPECT(s2008.scene.shadows.cascades <= 2u && s2008.scene.shadows.filter == 0u);

    // Saved with the project.
    const String parent = TempRoot("era");
    ProjectDescriptor settings;
    settings.graphicsEra = "2011";
    Result<ProjectInfo> p = CreateProject(parent, "Old", settings);
    ZX_REQUIRE(p.IsOk());
    Result<ProjectInfo> loaded = LoadProject(p->descriptorPath);
    ZX_REQUIRE(loaded.IsOk());
    ZX_EXPECT_EQ(loaded->descriptor.graphicsEra, String("2011"));
}
