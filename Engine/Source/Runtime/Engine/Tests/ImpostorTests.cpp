// Impostors: every frame direction maps back to its frame, a baked card shows where (and in which color) the model is
// seen from each side, the asset survives serialization, and the real starter pine bakes into a plausible atlas.

#include "Asset/AssetManager.h"
#include "Core/CoreInit.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Log/Log.h"
#include "Core/Image/Png.h"
#include "Core/Image/TextureTools.h"
#include "Core/Testing/Test.h"
#include "Core/Threading/Thread.h"
#include "Engine/Impostor.h"

using namespace zx;

ZX_DEFINE_LOG_CATEGORY_STATIC(LogImpostorTest, Info);

namespace {

Image Decode(const asset::CookedTexture& t)
{
    if (t.format == asset::TextureFormat::RGBA8 || t.format == asset::TextureFormat::RGBA8_SRGB) {
        Image img = Image::Create(t.width, t.height, ImageFormat::RGBA8);
        MemCopy(img.pixels.Data(), t.mips[0].Data(), img.pixels.Size());
        return img;
    }
    const texture::BlockFormat f = t.format == asset::TextureFormat::BC5 ? texture::BlockFormat::BC5
                                   : t.format == asset::TextureFormat::BC1 || t.format == asset::TextureFormat::BC1_SRGB ? texture::BlockFormat::BC1
                                   : t.format == asset::TextureFormat::BC3 || t.format == asset::TextureFormat::BC3_SRGB ? texture::BlockFormat::BC3
                                                                                                                         : texture::BlockFormat::BC7;
    return texture::DecompressBlocks(t.mips[0].View(), t.width, t.height, f);
}

/// Covered share of a frame (alpha >= 128) and the mean color of the covered texels.
f32 Coverage(const Image& img, u32 fx, u32 fy, u32 size, Vec3* mean = nullptr)
{
    u32 covered = 0;
    Vec3 sum;
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            const u8* p = img.Row(fy * size + y) + usize(fx * size + x) * 4;
            if (p[3] >= 128) {
                ++covered;
                sum += Vec3(p[0], p[1], p[2]);
            }
        }
    if (mean)
        *mean = covered ? sum / f32(covered) : Vec3();
    return f32(covered) / f32(size * size);
}

struct TempAssets
{
    String ddc = platform::GetTempDirectory() + String::Format("/zx_impostor_test_{}", Thread::CurrentId());
    UniquePtr<asset::AssetManager> assets;
    TempAssets()
    {
        asset::AssetManagerSettings s;
        s.ddcDirectory = ddc;
        s.writeMetaFiles = false;
        assets.Reset(new asset::AssetManager(s));
    }
    ~TempAssets()
    {
        assets.Reset();
        platform::DeleteDirectoryTree(ddc);
    }
};

} // namespace

ZX_TEST(Impostor, FrameDirectionsMapBackToTheirFrames)
{
    for (u32 frames : {4u, 8u, 12u})
        for (u32 y = 0; y < frames; ++y)
            for (u32 x = 0; x < frames; ++x) {
                const Vec3 d = ImpostorFrameDirection(x, y, frames);
                ZX_EXPECT_NEAR(Length(d), 1.0f, 1e-4f);
                ZX_EXPECT_GE(d.y, 0.0f);
                u32 bx = 99, by = 99;
                ImpostorFrameOf(d, frames, bx, by);
                ZX_EXPECT(bx == x && by == y);
                Vec3 right, up;
                ImpostorFrameAxes(d, right, up);
                ZX_EXPECT_NEAR(Dot(right, d), 0.0f, 1e-4f);
                ZX_EXPECT_NEAR(Dot(up, d), 0.0f, 1e-4f);
                ZX_EXPECT_GE(up.y, -1e-4f); // the image is never upside down
            }
    // Straight above: the middle of the atlas; below the horizon: on it.
    u32 x, y;
    ImpostorFrameOf(Vec3(0, 1, 0), 8, x, y);
    ZX_EXPECT((x == 3 || x == 4) && (y == 3 || y == 4));
    u32 hx, hy, lx, ly;
    ImpostorFrameOf(Vec3(1, 0, 0), 8, hx, hy);
    ImpostorFrameOf(Vec3(1, -0.7f, 0), 8, lx, ly);
    ZX_EXPECT(hx == lx && hy == ly);
}

ZX_TEST(Impostor, BakedCardShowsTheModelFromEachSide)
{
    // A red card 2 m wide and 4 m tall facing +Z (two-sided): seen from the front it fills its share of the frame, seen
    // edge-on (from +X) almost nothing.
    TempAssets t;
    asset::CookedMesh card;
    const Vec3 corners[4] = {Vec3(-1, 4, 0), Vec3(1, 4, 0), Vec3(1, 0, 0), Vec3(-1, 0, 0)};
    for (const Vec3& c : corners) {
        asset::MeshVertex v;
        v.position = c;
        v.normal = Vec3(0, 0, 1);
        v.tangent = Vec4(1, 0, 0, 1);
        card.vertices.PushBack(asset::PackVertex(v));
    }
    const u16 idx[6] = {0, 1, 2, 0, 2, 3};
    card.index16 = true;
    card.indexData.ResizeUninitialized(sizeof(idx));
    MemCopy(card.indexData.Data(), idx, sizeof(idx));
    asset::CookedSectionLod lod;
    lod.indexCount = 6;
    card.sectionLods.PushBack(lod);
    card.sections.PushBack(asset::CookedMeshSection{});
    card.materialSlotCount = 1;
    const Guid meshId = Guid::New(), matId = Guid::New();
    t.assets->AddMemoryMesh(meshId, move(card));
    asset::MaterialAsset red;
    red.baseColor = Vec4(1, 0, 0, 1);
    red.doubleSided = true;
    t.assets->AddMemoryMaterial(matId, red);
    FoliagePart part;
    part.mesh = meshId;
    part.materials.PushBack(matId);
    ImpostorAsset imp;
    ZX_REQUIRE(BakeImpostor(*t.assets, Span<const FoliagePart>(&part, 1), imp, 8, 32));
    ZX_EXPECT_NEAR(imp.centerY, 2.0f, 1e-4f);
    ZX_EXPECT_NEAR(imp.radius, Sqrt(5.0f) * 1.02f, 1e-3f);
    const Image albedo = Decode(imp.albedo);
    ZX_REQUIRE(albedo.width == 256u);
    u32 fx, fy;
    ImpostorFrameOf(Vec3(0, 0, 1), 8, fx, fy);
    Vec3 color;
    const f32 front = Coverage(albedo, fx, fy, 32, &color);
    const f32 expected = 8.0f / Square(2.0f * imp.radius) * Abs(ImpostorFrameDirection(fx, fy, 8).z); // tilted a little
    ZX_EXPECT_NEAR(front, expected, 0.06f);
    ZX_EXPECT_GT(color.x, 200.0f);
    ZX_EXPECT_LT(color.y, 40.0f);
    ImpostorFrameOf(Vec3(0, 0, -1), 8, fx, fy);
    ZX_EXPECT_NEAR(Coverage(albedo, fx, fy, 32), expected, 0.06f); // the back of a two-sided card
    ImpostorFrameOf(Vec3(1, 0, 0), 8, fx, fy);
    ZX_EXPECT_LT(Coverage(albedo, fx, fy, 32), 0.08f);
    // The front view's normal faces the viewer (z up in the frame's space).
    ImpostorFrameOf(Vec3(0, 0, 1), 8, fx, fy);
    const Image normal = Decode(imp.normal);
    const u8* n = normal.Row(fy * 32 + 16) + usize(fx * 32 + 16) * 4;
    ZX_EXPECT_NEAR(f32(n[0]) / 127.5f - 1.0f, 0.0f, 0.1f);
    // Serialization keeps it.
    Array<u8> bytes;
    SerializeImpostor(imp, bytes);
    ImpostorAsset back;
    ZX_REQUIRE(DeserializeImpostor(bytes.View(), back));
    ZX_EXPECT(back.frames == 8u && back.frameSize == 32u && back.radius == imp.radius && back.albedo.mips.Size() == imp.albedo.mips.Size());
    // Registered: a quad as big as the sphere, an alpha-tested material with both atlases.
    const ImpostorIds ids = RegisterImpostor(*t.assets, imp, "TestCard");
    const asset::CookedMesh* quad = t.assets->GetMesh(ids.mesh);
    const asset::MaterialAsset* mat = t.assets->GetMaterial(ids.material);
    ZX_REQUIRE(quad && mat);
    ZX_EXPECT_NEAR(quad->bounds.max.y, imp.centerY + imp.radius, 1e-4f);
    ZX_EXPECT(mat->alphaMode == asset::AlphaMode::Mask && mat->baseColorTexture == ids.albedo && mat->normalTexture == ids.normal);
    ZX_EXPECT(RegisterImpostor(*t.assets, imp, "TestCard").mesh == ids.mesh);
}

ZX_TEST(Impostor, BakesTheStarterPine)
{
    TempAssets t;
    const String path = FindEngineRootDirectory() + "/Engine/Content/Packs/StylizedNature/Models/Pine_3.gltf";
    const asset::AssetId id = t.assets->LoadPath(path);
    t.assets->WaitIdle();
    const asset::PrefabAsset* prefab = t.assets->GetPrefab(id);
    ZX_REQUIRE(prefab != nullptr);
    Foliage f;
    f.model = "Packs/StylizedNature/Models/Pine_3.gltf";
    ZX_REQUIRE(FoliagePartsFromPrefab(*prefab, f.parts));
    for (const FoliagePart& p : f.parts) {
        t.assets->Load(p.mesh);
        for (const asset::AssetId& m : p.materials)
            t.assets->Load(m);
    }
    t.assets->WaitIdle();
    for (const FoliagePart& p : f.parts)
        for (const asset::AssetId& m : p.materials) {
            const asset::MaterialAsset* mat = t.assets->GetMaterial(m);
            ZX_EXPECT(mat != nullptr);
            ZX_EXPECT(mat && mat->baseColorTexture.IsValid());
            ZX_EXPECT(mat && !t.assets->GetSourcePath(mat->baseColorTexture).Empty());
        }
    ZX_EXPECT(WantsImpostor(f));
    ImpostorAsset imp;
    ZX_REQUIRE(BakeImpostor(*t.assets, f.parts.View(), imp));
    const Image albedo = Decode(imp.albedo);
    // A pine from the side: a good part of the frame is needles and bark, and they are green-brown, not white or black.
    u32 fx, fy;
    ImpostorFrameOf(Vec3(0.3f, 0.1f, 1.0f), 8, fx, fy);
    Vec3 color;
    const f32 side = Coverage(albedo, fx, fy, 64, &color);
    ZX_EXPECT_GT(side, 0.12f);
    ZX_EXPECT_LT(side, 0.8f);
    ZX_EXPECT_GT(color.y, color.z); // green over blue
    ZX_EXPECT_LT(color.x + color.y + color.z, 600.0f);
    ZX_EXPECT_GT(color.x + color.y + color.z, 40.0f);
    // A preview for people (temp folder).
    const String preview = platform::GetTempDirectory() + "/zx_impostor_Pine_3.png";
    png::WriteFile(preview, albedo.pixels.Data(), albedo.width, albedo.height, 4);
    const Image normal = Decode(imp.normal);
    png::WriteFile(platform::GetTempDirectory() + "/zx_impostor_Pine_3_normal.png", normal.pixels.Data(), normal.width, normal.height, 4);
}
