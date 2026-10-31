// FBX importer: every export variant of the generated rig (Tools/gen_fbx_testdata.py -> Tests/Data/Models/Fbx) is
// compared with the glTF export of the same Blender scene (posed world-space bounds within 2 mm, skeleton, clips, materials), a real
// asset against its glTF conversion, and corrupt input.

#include "Asset/SceneImport.h"
#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Math/Random.h"
#include "Core/Testing/Test.h"
#include "Core/Time/Time.h"

#include <cstdio>
#include <cstdlib>

using namespace zx;
using namespace zx::asset;

namespace {

String FbxPath(StringView name) { return FindEngineRootDirectory() + "/Tests/Data/Models/Fbx/" + name; }

Mat4 NodeGlobal(const ImportedScene& s, i32 node)
{
    Mat4 m = Mat4::Identity();
    for (i32 n = node; n >= 0; n = s.nodes[usize(n)].parent)
        m = s.nodes[usize(n)].local.ToMatrix() * m;
    return m;
}

i32 FindClip(const ImportedScene& s, StringView name)
{
    for (usize i = 0; i < s.animations.Size(); ++i)
        if (s.animations[i].name == name)
            return i32(i);
    return -1;
}

/// World-space bounds of every mesh; skinned meshes posed by `clip` at `time` (clip < 0: bind pose).
AABB PosedBounds(const ImportedScene& s, i32 clip, f32 time)
{
    AABB box;
    for (usize n = 0; n < s.nodes.Size(); ++n) {
        const ImportedNode& node = s.nodes[n];
        if (node.mesh < 0)
            continue;
        const Mat4 world = NodeGlobal(s, i32(n));
        const MeshData& mesh = s.meshes[usize(node.mesh)];
        Array<Mat4> palette;
        if (node.skin >= 0) {
            const Skeleton& sk = s.skeletons[usize(node.skin)];
            Array<Transform> local(sk.bindPose.View());
            if (clip >= 0 && s.animationSkeleton[usize(clip)] == u32(node.skin))
                anim::SampleClip(s.animations[usize(clip)], sk, time, false, local.View());
            Array<Mat4> model;
            model.Resize(sk.BoneCount());
            anim::LocalToModel(sk, local.View(), model.View());
            palette.Resize(sk.BoneCount());
            anim::SkinningMatrices(sk, model.View(), node.skinToNode, palette.View());
        }
        for (const MeshVertex& v : mesh.vertices) {
            Vec3 p = v.position;
            if (!palette.Empty()) {
                p = Vec3(0.0f, 0.0f, 0.0f);
                for (u32 k = 0; k < 4; ++k) {
                    const u32 j = (v.joints >> (k * 8)) & 0xFFu;
                    const f32 w = f32((v.weights >> (k * 8)) & 0xFFu) / 255.0f;
                    if (w > 0.0f && j < palette.Size())
                        p = p + palette[j].TransformPoint(v.position) * w;
                }
            }
            box.Expand(world.TransformPoint(p));
        }
    }
    return box;
}

bool BoundsNear(const AABB& a, const AABB& b, f32 tolerance)
{
    return Length(a.min - b.min) < tolerance && Length(a.max - b.max) < tolerance;
}

const ImportedMaterial* FindMaterial(const ImportedScene& s, StringView name)
{
    for (const ImportedMaterial& m : s.materials)
        if (m.name == name)
            return &m;
    return nullptr;
}

} // namespace

ZX_TEST(FbxImport, RigMatchesGltf)
{
    Result<ImportedScene> reference = ImportScene(FbxPath("Rig.glb"));
    ZX_REQUIRE(reference.IsOk());
    ZX_REQUIRE(reference->skeletons.Size() == 1);
    for (StringView file : {"Rig.fbx", "Rig_ZUp.fbx", "Rig_7500.fbx"}) {
        Result<ImportedScene> scene = ImportScene(FbxPath(file));
        if (!scene) {
            test::ReportFailure(__FILE__, __LINE__, String::Format("{}: {}", file, scene.GetError().message));
            continue;
        }
        ZX_EXPECT_EQ(scene->TotalTriangles(), reference->TotalTriangles());
        ZX_REQUIRE(scene->skeletons.Size() == 1);
        const Skeleton& sk = scene->skeletons[0];
        ZX_EXPECT_EQ(sk.BoneCount(), reference->skeletons[0].BoneCount());
        ZX_EXPECT(sk.FindBone("Root") >= 0 && sk.FindBone("Mid") >= 0 && sk.FindBone("Tip") >= 0);
        for (const MeshData& m : scene->meshes) {
            ZX_EXPECT(m.Validate().Empty());
            ZX_EXPECT((m.attributes & MeshAttributes::Skin) != MeshAttributes::None);
            ZX_EXPECT((m.attributes & MeshAttributes::UV0) != MeshAttributes::None);
        }

        // Bind pose and poses along both clips match the glTF export of the same scene (2 mm).
        const AABB bind = PosedBounds(*scene, -1, 0.0f), bindRef = PosedBounds(*reference, -1, 0.0f);
        if (!BoundsNear(bind, bindRef, 0.002f))
            test::ReportFailure(__FILE__, __LINE__,
                                String::Format("{}: bind bounds ({} {} {})-({} {} {}) vs glTF ({} {} {})-({} {} {})", file, bind.min.x, bind.min.y,
                                               bind.min.z, bind.max.x, bind.max.y, bind.max.z, bindRef.min.x, bindRef.min.y, bindRef.min.z,
                                               bindRef.max.x, bindRef.max.y, bindRef.max.z));
        for (StringView clipName : {"Bend", "Lift"}) {
            const i32 clip = FindClip(*scene, clipName), clipRef = FindClip(*reference, clipName);
            if (clip < 0 || clipRef < 0) {
                test::ReportFailure(__FILE__, __LINE__, String::Format("{}: clip '{}' missing", file, clipName));
                continue;
            }
            // The FBX clip starts at the first frame; Blender's glTF export counts from frame 0 (one frame earlier).
            const f32 offset = reference->animations[usize(clipRef)].duration - scene->animations[usize(clip)].duration;
            ZX_EXPECT(offset > -1e-3f && offset < 1.0f / 24.0f + 1e-3f);
            for (f32 t : {0.0f, 0.3f, 0.6f, 1.0f}) {
                const AABB posed = PosedBounds(*scene, clip, t), posedRef = PosedBounds(*reference, clipRef, t + offset);
                if (!BoundsNear(posed, posedRef, 0.002f))
                    test::ReportFailure(__FILE__, __LINE__,
                                        String::Format("{}: {} at {} s: ({} {} {})-({} {} {}) vs glTF ({} {} {})-({} {} {})", file, clipName, t,
                                                       posed.min.x, posed.min.y, posed.min.z, posed.max.x, posed.max.y, posed.max.z, posedRef.min.x,
                                                       posedRef.min.y, posedRef.min.z, posedRef.max.x, posedRef.max.y, posedRef.max.z));
            }
        }

        // Materials: plain color + texture (embedded in Rig.fbx, a file next to Rig_ZUp.fbx).
        const ImportedMaterial* red = FindMaterial(*scene, "Red");
        const ImportedMaterial* checker = FindMaterial(*scene, "Checker");
        ZX_REQUIRE(red && checker);
        ZX_EXPECT(Length(red->baseColor.XYZ() - Vec3(0.8f, 0.05f, 0.05f)) < 0.01f);
        ZX_EXPECT_NEAR(red->roughness, 0.4f, 0.02f);
        ZX_REQUIRE(checker->baseColorTexture.IsValid());
        const ImportedTexture& tex = scene->textures[usize(checker->baseColorTexture.texture)];
        ZX_EXPECT(tex.srgb);
        if (StringView(file) == "Rig_ZUp.fbx")
            ZX_EXPECT(tex.path.View().EndsWith("RigTexture.png") && fs::Exists(tex.path));
        else
            ZX_EXPECT(!tex.data.Empty() && tex.mimeType == "image/png");
    }
}

ZX_TEST(FbxImport, TextureFilesAreListed)
{
    const Array<String> files = ListFbxTextureFiles(FbxPath("Rig_ZUp.fbx"));
    ZX_REQUIRE(files.Size() == 1);
    ZX_EXPECT(files[0].View().EndsWith("Fbx/RigTexture.png"));
    ZX_EXPECT(ListFbxTextureFiles(FbxPath("Rig.fbx")).Empty()); // embedded
}

ZX_TEST(FbxImport, QuaterniusAxeMatchesPack)
{
    // The CC0 Survival Pack ships both an FBX (Blender 2.79, -90 degree root rotation with a 11.18 scale in centimeters)
    // and the .blend our glTF pack was converted from: the same model must come out in the same orientation.
    Result<ImportedScene> fbx = ImportScene(FbxPath("Axe.fbx"));
    Result<ImportedScene> gltf = ImportScene(FindEngineRootDirectory() + "/Engine/Content/Packs/SurvivalPack/Models/Axe.glb");
    ZX_REQUIRE(fbx.IsOk() && gltf.IsOk());
    ZX_EXPECT_EQ(fbx->materials.Size(), usize(3));
    // The pack was rescaled to a real-world size: compare the proportions.
    const AABB a = PosedBounds(*fbx, -1, 0.0f), b = PosedBounds(*gltf, -1, 0.0f);
    if (Length(a.Size() / Length(a.Size()) - b.Size() / Length(b.Size())) > 0.02f)
        test::ReportFailure(__FILE__, __LINE__,
                            String::Format("Axe size {} {} {} vs glTF {} {} {}", a.Size().x, a.Size().y, a.Size().z, b.Size().x, b.Size().y, b.Size().z));
}

ZX_TEST(FbxImport, CorruptInputFailsCleanly)
{
    Array<u8> bytes;
    ZX_REQUIRE(fs::ReadFile(FbxPath("Rig.fbx"), bytes));
    const String dir = FindEngineRootDirectory() + "/Tests/Data/Models/Fbx";
    // Truncated files: an error (or a partial scene), never a crash.
    for (usize size : {usize(10), usize(27), usize(100), bytes.Size() / 3, bytes.Size() / 2, bytes.Size() - 300})
        (void)ImportFbxFromMemory(ByteSpan(bytes.Data(), size), dir, "Truncated.fbx");
    ZX_EXPECT(!ImportFbxFromMemory(ByteSpan(bytes.Data(), 27), dir, "Header.fbx"));
    // Random byte damage.
    Random rng(1234);
    for (u32 round = 0; round < 200; ++round) {
        Array<u8> damaged(bytes.View());
        for (u32 k = 0; k < 8; ++k)
            damaged[usize(rng.NextU32() % u32(damaged.Size()))] = u8(rng.NextU32());
        (void)ImportFbxFromMemory(damaged.View(), dir, "Damaged.fbx");
    }
    const char ascii[] = "; FBX 7.4.0 project file\nFBXHeaderExtension:  {\n}\n";
    Result<ImportedScene> text = ImportFbxFromMemory(ByteSpan(reinterpret_cast<const u8*>(ascii), sizeof(ascii) - 1), dir, "Ascii.fbx");
    ZX_REQUIRE(!text);
    ZX_EXPECT(text.GetError().code == ErrorCode::Unsupported);
}

// Manual: an FBX against a glTF of the same scene (posed bounds per clip, import time):
// ZX_FBX_FILE=<fbx> ZX_GLTF_FILE=<gltf/glb> ZXTests ManualFbx.*
ZX_TEST(ManualFbx, CompareWithGltf)
{
    const char* fbxPath = std::getenv("ZX_FBX_FILE");
    const char* gltfPath = std::getenv("ZX_GLTF_FILE");
    ZX_REQUIRE(fbxPath && gltfPath);
    Stopwatch timer;
    Result<ImportedScene> fbx = ImportScene(fbxPath);
    const f64 ms = timer.ElapsedMilliseconds();
    if (!fbx) {
        test::ReportFailure(__FILE__, __LINE__, fbx.GetError().message);
        return;
    }
    Result<ImportedScene> gltf = ImportScene(gltfPath);
    ZX_REQUIRE(gltf.IsOk());
    std::printf("FBX: %zu meshes, %u triangles, %zu materials, %zu textures, %zu skeletons, %zu clips, %.1f ms\n", fbx->meshes.Size(),
                fbx->TotalTriangles(), fbx->materials.Size(), fbx->textures.Size(), fbx->skeletons.Size(), fbx->animations.Size(), ms);
    std::printf("glTF: %zu meshes, %u triangles, %zu skeletons, %zu clips\n", gltf->meshes.Size(), gltf->TotalTriangles(), gltf->skeletons.Size(),
                gltf->animations.Size());
    auto report = [](StringView what, const AABB& a, const AABB& b) {
        std::printf("%s: (%.3f %.3f %.3f)-(%.3f %.3f %.3f) vs (%.3f %.3f %.3f)-(%.3f %.3f %.3f), max error %.4f m\n", String(what).CStr(), a.min.x,
                    a.min.y, a.min.z, a.max.x, a.max.y, a.max.z, b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z,
                    Max(Length(a.min - b.min), Length(a.max - b.max)));
    };
    report("bind", PosedBounds(*fbx, -1, 0.0f), PosedBounds(*gltf, -1, 0.0f));
    for (usize c = 0; c < fbx->animations.Size(); ++c) {
        const AnimationClip& clip = fbx->animations[c];
        const i32 ref = FindClip(*gltf, clip.name);
        if (ref < 0) {
            std::printf("clip %s (%.3f s): no glTF counterpart\n", clip.name.CStr(), clip.duration);
            continue;
        }
        const f32 offset = gltf->animations[usize(ref)].duration - clip.duration;
        for (f32 f : {0.0f, 0.5f, 1.0f})
            report(String::Format("{} at {} s (offset {})", clip.name, clip.duration * f, offset), PosedBounds(*fbx, i32(c), clip.duration * f),
                   PosedBounds(*gltf, ref, clip.duration * f + Max(offset, 0.0f)));
    }
}
