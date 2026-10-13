#include "Asset/AssetManager.h"
#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Core/Threading/Thread.h"

using namespace zx;
using namespace zx::asset;

namespace {

struct TempProject
{
    String root;
    TempProject()
    {
        root = platform::GetTempDirectory() + String::Format("/zx_asset_test_{}", Thread::CurrentId());
        fs::Delete(root);
        fs::CreateDirectories(root + "/Content");
        const String data = FindEngineRootDirectory() + "/Tests/Data/";
        for (StringView f : {"Models/cube.obj", "Models/cube.mtl", "Models/BoxTextured.glb", "Images/rgba8.png"})
            fs::Copy(data + f, root + "/Content/" + Path::FileName(f));
    }
    ~TempProject() { fs::Delete(root); }
};

struct JobsScope
{
    JobsScope() { jobs::Initialize({.workerCount = 3}); }
    ~JobsScope() { jobs::Shutdown(); }
};

} // namespace

ZX_TEST(CookedAssets, PackedVertexRoundTrip)
{
    MeshVertex v;
    v.position = Vec3(1.5f, -2.0f, 3.25f);
    v.normal = Normalize(Vec3(0.3f, 0.8f, -0.5f));
    v.tangent = Vec4(Normalize(Cross(v.normal, Vec3::Right())), -1.0f);
    v.uv0 = Vec2(12.345f, -0.5f);
    v.uv1 = Vec2(0.25f, 0.75f);
    v.color = 0x80FF4020;
    const MeshVertex u = UnpackVertex(PackVertex(v));
    ZX_EXPECT(u.position == v.position && u.uv0 == v.uv0 && u.color == v.color);
    ZX_EXPECT(Length(u.normal - v.normal) < 1e-3f);
    ZX_EXPECT(Length(u.tangent.XYZ() - v.tangent.XYZ()) < 1e-3f && u.tangent.w == -1.0f);
    ZX_EXPECT(Length(u.uv1 - v.uv1) < 1e-3f);
}

ZX_TEST(CookedAssets, MeshAndTextureSerialization)
{
    Result<ImportedScene> scene = ImportScene(FindEngineRootDirectory() + "/Tests/Data/Models/cube.obj");
    ZX_REQUIRE(scene.IsOk());
    CookedMesh mesh = CookMesh(scene->meshes[0], false);
    ZX_EXPECT(mesh.index16 && mesh.IndexCount() == 36 && mesh.materialSlotCount == 2);
    Array<u8> bytes;
    Serialize(mesh, bytes);
    ZX_EXPECT(PeekAssetType(bytes.View()) == AssetType::Mesh);
    CookedMesh back;
    ZX_REQUIRE(Deserialize(bytes.View(), back));
    ZX_EXPECT(back.vertices.Size() == mesh.vertices.Size() && back.IndexCount() == 36 && back.TriangleCount(0) == 12);
    ZX_EXPECT(back.bounds.max == mesh.bounds.max && back.sections.Size() == 2);
    bytes.Resize(bytes.Size() - 5);
    ZX_EXPECT(!Deserialize(bytes.View(), back)); // truncated

    Image img = Image::Create(256, 128, ImageFormat::RGBA8);
    for (usize i = 0; i < img.pixels.Size(); ++i)
        img.pixels[i] = u8(i * 7);
    for (usize i = 3; i < img.pixels.Size(); i += 4)
        img.pixels[i] = 255;
    TextureCookOptions o;
    o.maxSize = 64;
    Result<CookedTexture> tex = CookTexture(img, o);
    ZX_REQUIRE(tex.IsOk());
    ZX_EXPECT(tex->width == 64 && tex->height == 32 && tex->format == TextureFormat::BC1_SRGB);
    ZX_EXPECT_EQ(tex->mips.Size(), usize(7)); // 64x32 ... 1x1
    bytes.Clear();
    Serialize(*tex, bytes);
    CookedTexture tback;
    ZX_REQUIRE(Deserialize(bytes.View(), tback));
    ZX_EXPECT(tback.MemorySize() == tex->MemorySize());

    MaterialAsset mat;
    mat.name = "Brick";
    mat.baseColorTexture = Guid::New();
    mat.alphaMode = AlphaMode::Mask;
    bytes.Clear();
    Serialize(mat, bytes);
    MaterialAsset mback;
    ZX_REQUIRE(Deserialize(bytes.View(), mback));
    ZX_EXPECT(mback.name == "Brick" && mback.baseColorTexture == mat.baseColorTexture && mback.alphaMode == AlphaMode::Mask);
}

ZX_TEST(AssetManager, CookLoadAndDdcHits)
{
    JobsScope jobsScope;
    TempProject project;
    AssetManagerSettings settings;
    settings.ddcDirectory = project.root + "/Intermediate/DDC";

    AssetId cubeId, glbId, pngId;
    {
        AssetManager assets(settings);
        cubeId = assets.LoadPath(project.root + "/Content/cube.obj");
        glbId = assets.LoadPath(project.root + "/Content/BoxTextured.glb");
        pngId = assets.LoadPath(project.root + "/Content/rgba8.png");
        ZX_REQUIRE(cubeId.IsValid() && glbId.IsValid() && pngId.IsValid());
        ZX_EXPECT(fs::Exists(project.root + "/Content/cube.obj.zmeta"));
        assets.WaitIdle();
        ZX_EXPECT(assets.GetState(cubeId) == AssetState::Loaded);
        const PrefabAsset* prefab = assets.GetPrefab(glbId);
        ZX_REQUIRE(prefab != nullptr);
        ZX_EXPECT(!prefab->nodes.Empty());
        // Find the mesh node and load its sub-assets.
        AssetId meshId, materialId;
        for (const PrefabNode& n : prefab->nodes)
            if (n.mesh.IsValid()) {
                meshId = n.mesh;
                materialId = n.materials[0];
            }
        ZX_REQUIRE(meshId.IsValid());
        assets.Load(meshId);
        assets.Load(materialId);
        assets.WaitIdle();
        const CookedMesh* mesh = assets.GetMesh(meshId);
        const MaterialAsset* material = assets.GetMaterial(materialId);
        ZX_REQUIRE(mesh && material);
        ZX_EXPECT_EQ(mesh->TriangleCount(0), 12u);
        ZX_REQUIRE(material->baseColorTexture.IsValid());
        assets.Load(material->baseColorTexture);
        assets.WaitIdle();
        const CookedTexture* tex = assets.GetTexture(material->baseColorTexture);
        ZX_REQUIRE(tex != nullptr);
        ZX_EXPECT(tex->width > 0 && tex->mips.Size() > 1);
        ZX_EXPECT(assets.GetTexture(pngId) != nullptr);
        ZX_EXPECT(assets.GetStats().cookedThisSession >= 3);
        ZX_EXPECT_EQ(assets.GetStats().failed, 0u);
    }
    {
        // Second session: same GUIDs from .zmeta, everything comes from the DDC.
        AssetManager assets(settings);
        ZX_EXPECT(assets.LoadPath(project.root + "/Content/cube.obj") == cubeId);
        ZX_EXPECT(assets.LoadPath(project.root + "/Content/rgba8.png") == pngId);
        assets.WaitIdle();
        ZX_EXPECT(assets.GetPrefab(cubeId) != nullptr);
        ZX_EXPECT(assets.GetTexture(pngId) != nullptr);
        ZX_EXPECT_EQ(assets.GetStats().cookedThisSession, 0u);
        ZX_EXPECT(assets.GetStats().ddcHits >= 2);
    }
    {
        // Editing the source re-cooks it.
        String text;
        ZX_REQUIRE(fs::ReadTextFile(project.root + "/Content/cube.obj", text));
        text += "\n# edited\n";
        ZX_REQUIRE(fs::WriteTextFile(project.root + "/Content/cube.obj", text.View()));
        AssetManager assets(settings);
        assets.LoadPath(project.root + "/Content/cube.obj");
        assets.WaitIdle();
        ZX_EXPECT_EQ(assets.GetStats().cookedThisSession, 1u);
    }
}

ZX_TEST(AssetManager, MissingAndUnsupported)
{
    TempProject project;
    AssetManager assets({.ddcDirectory = project.root + "/DDC"});
    ZX_EXPECT(!assets.RegisterSource(project.root + "/Content/nope.gltf").IsOk());
    ZX_EXPECT(!assets.RegisterSource(project.root + "/Content/cube.mtl").IsOk());
    const AssetId unknown = Guid::New();
    assets.Load(unknown);
    ZX_EXPECT(assets.GetState(unknown) == AssetState::Queued); // waits for a source that never comes
    ZX_EXPECT(assets.GetMesh(unknown) == nullptr);
}

ZX_TEST(AssetManager, CopiedSourceGetsItsOwnCook)
{
    // Regression: two copies of the same file (e.g. imported into two projects) used to share one DDC manifest, so the
    // second copy's sub-asset ids (derived from its own GUID) never appeared and its prefab stayed Queued.
    JobsScope jobsScope;
    TempProject project;
    fs::CreateDirectories(project.root + "/Content/Copy");
    fs::Copy(project.root + "/Content/BoxTextured.glb", project.root + "/Content/Copy/BoxTextured.glb");
    AssetManager assets({.ddcDirectory = project.root + "/DDC"});
    const AssetId a = assets.LoadPath(project.root + "/Content/BoxTextured.glb");
    assets.WaitIdle();
    const AssetId b = assets.LoadPath(project.root + "/Content/Copy/BoxTextured.glb");
    assets.WaitIdle();
    ZX_REQUIRE(a.IsValid() && b.IsValid() && a != b);
    ZX_EXPECT(assets.GetState(a) == AssetState::Loaded);
    ZX_EXPECT(assets.GetState(b) == AssetState::Loaded);
    const PrefabAsset* pa = assets.GetPrefab(a);
    const PrefabAsset* pb = assets.GetPrefab(b);
    ZX_REQUIRE(pa && pb);
    ZX_EXPECT_EQ(pa->nodes.Size(), pb->nodes.Size());
    ZX_EXPECT_EQ(assets.GetSourcePath(b), Path::Normalize(project.root + "/Content/Copy/BoxTextured.glb"));
    for (const PrefabNode& n : pb->nodes)
        if (n.mesh.IsValid()) {
            assets.Load(n.mesh);
            assets.WaitIdle();
            ZX_EXPECT(assets.GetMesh(n.mesh) != nullptr);
            ZX_EXPECT_EQ(assets.GetSourcePath(n.mesh), assets.GetSourcePath(b));
        }
}
