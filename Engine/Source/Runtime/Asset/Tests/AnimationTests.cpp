// Skeletons, clips and skinning data: sampling math, skin packing, glTF skin / animation import (Starter Content
// character), cooked round trips.

#include "Asset/Animation.h"
#include "Asset/CookedAssets.h"
#include "Asset/SceneImport.h"
#include "Core/CoreInit.h"
#include "Core/Testing/Test.h"

using namespace zx;
using namespace zx::asset;

namespace {

String CharacterPath() { return FindEngineRootDirectory() + "/Engine/Content/StarterContent/Characters/SK_Human.glb"; }

/// Two bones (root, child 1 m up) and a clip moving the root along +X and turning the child 90 degrees about Y.
void MakeTestRig(Skeleton& sk, AnimationClip& clip)
{
    sk.boneNames = {String("Root"), String("Child")};
    sk.parents = {-1, 0};
    sk.bindPose = {Transform(), Transform(Vec3(0, 1, 0))};
    sk.inverseBind = {Mat4::Identity(), Mat4::Translation(Vec3(0, -1, 0))};
    clip.name = "Test";
    clip.duration = 2.0f;
    clip.boneCount = 2;
    AnimationTrack& root = clip.tracks.AddDefaulted();
    root.bone = 0;
    root.positionTimes = {0.0f, 2.0f};
    root.positions = {Vec3(0, 0, 0), Vec3(4, 0, 0)};
    AnimationTrack& child = clip.tracks.AddDefaulted();
    child.bone = 1;
    child.rotationTimes = {0.0f, 2.0f};
    child.rotations = {Quat::Identity(), Quat::FromAxisAngle(Vec3(0, 1, 0), HalfPi)};
}

} // namespace

ZX_TEST(Animation, PackSkinWeightsSumTo255)
{
    const u32 joints[4] = {3, 7, 300, 1};
    const f32 weights[4] = {0.5f, 0.3f, 0.15f, 0.05f};
    u32 pj = 0, pw = 0;
    anim::PackSkin(joints, weights, pj, pw);
    u32 sum = 0;
    for (u32 k = 0; k < 4; ++k)
        sum += (pw >> (k * 8)) & 0xFF;
    ZX_EXPECT_EQ(sum, 255u);
    ZX_EXPECT_EQ(pj & 0xFF, 3u);
    ZX_EXPECT_EQ((pj >> 16) & 0xFF, 255u); // clamped
    const f32 none[4] = {0, 0, 0, 0};
    anim::PackSkin(joints, none, pj, pw);
    ZX_EXPECT_EQ(pw, 255u); // no weights: all on the first joint
}

ZX_TEST(Animation, SampleClipInterpolatesAndLoops)
{
    Skeleton sk;
    AnimationClip clip;
    MakeTestRig(sk, clip);
    Transform pose[2];
    anim::SampleClip(clip, sk, 0.5f, true, Span<Transform>(pose, 2));
    ZX_EXPECT_NEAR(pose[0].position.x, 1.0f, 1e-5f);
    ZX_EXPECT_NEAR(pose[1].position.y, 1.0f, 1e-5f); // untracked channel: bind pose
    // Looping wraps 2.5 s to 0.5 s; clamping holds the last key.
    anim::SampleClip(clip, sk, 2.5f, true, Span<Transform>(pose, 2));
    ZX_EXPECT_NEAR(pose[0].position.x, 1.0f, 1e-5f);
    anim::SampleClip(clip, sk, 2.5f, false, Span<Transform>(pose, 2));
    ZX_EXPECT_NEAR(pose[0].position.x, 4.0f, 1e-5f);
    // Halfway the child has turned 45 degrees.
    anim::SampleClip(clip, sk, 1.0f, false, Span<Transform>(pose, 2));
    const Vec3 fwd = pose[1].rotation * Vec3(0, 0, 1);
    ZX_EXPECT_NEAR(fwd.x, Sin(HalfPi * 0.5f), 1e-4f);
    // Model matrices chain the parent: the child sits 1 m above the moved root.
    Mat4 model[2];
    anim::LocalToModel(sk, Span<const Transform>(pose, 2), Span<Mat4>(model, 2));
    const Vec3 childPos = model[1].GetTranslation();
    ZX_EXPECT_NEAR(childPos.x, 2.0f, 1e-5f);
    ZX_EXPECT_NEAR(childPos.y, 1.0f, 1e-5f);
    // Blend halfway between two poses.
    Transform a[2], b[2], m[2];
    anim::SampleClip(clip, sk, 0.0f, false, Span<Transform>(a, 2));
    anim::SampleClip(clip, sk, 2.0f, false, Span<Transform>(b, 2));
    anim::BlendPoses(Span<const Transform>(a, 2), Span<const Transform>(b, 2), 0.25f, Span<Transform>(m, 2));
    ZX_EXPECT_NEAR(m[0].position.x, 1.0f, 1e-5f);
}

ZX_TEST(Animation, ImportsStarterCharacterSkinAndClips)
{
    Result<ImportedScene> scene = ImportGltf(CharacterPath());
    ZX_REQUIRE(scene.IsOk());
    ZX_REQUIRE(scene->skeletons.Size() == 1);
    const Skeleton& sk = scene->skeletons[0];
    ZX_EXPECT_GE(sk.BoneCount(), 30u);
    for (u32 b = 0; b < sk.BoneCount(); ++b)
        ZX_EXPECT_LT(sk.parents[b], i32(b)); // parents precede children
    ZX_EXPECT_GE(sk.FindBone("LeftFoot"), 0);
    ZX_EXPECT_GE(sk.FindBone("RightUpLeg"), 0);
    // Clips by name, all for that skeleton, with sensible lengths.
    for (StringView name : {StringView("Idle"), StringView("Walk"), StringView("Run"), StringView("Jump")}) {
        bool found = false;
        for (usize a = 0; a < scene->animations.Size(); ++a)
            if (scene->animations[a].name.View() == name) {
                found = true;
                ZX_EXPECT_EQ(scene->animationSkeleton[a], 0u);
                ZX_EXPECT_GT(scene->animations[a].duration, 0.3f);
                ZX_EXPECT_EQ(scene->animations[a].boneCount, sk.BoneCount());
            }
        ZX_EXPECT(found);
    }
    // The skinned mesh: weights sum to 255, joints index real bones; the node knows its skin.
    const ImportedNode* skinned = nullptr;
    for (const ImportedNode& n : scene->nodes)
        if (n.skin >= 0)
            skinned = &n;
    ZX_REQUIRE(skinned != nullptr);
    const MeshData& mesh = scene->meshes[usize(skinned->mesh)];
    ZX_EXPECT((mesh.attributes & MeshAttributes::Skin) != MeshAttributes::None);
    for (const MeshVertex& v : mesh.vertices) {
        u32 sum = 0;
        for (u32 k = 0; k < 4; ++k) {
            sum += (v.weights >> (k * 8)) & 0xFF;
            if ((v.weights >> (k * 8)) & 0xFF)
                ZX_EXPECT_LT((v.joints >> (k * 8)) & 0xFF, sk.BoneCount());
        }
        ZX_EXPECT_EQ(sum, 255u);
    }
    // Bind pose: every skinning matrix is (close to) identity, i.e. the mesh, the inverse bind matrices, the bone
    // hierarchy and skinToNode agree on the spaces involved.
    Array<Transform> local;
    local.Resize(sk.BoneCount());
    for (u32 b = 0; b < sk.BoneCount(); ++b)
        local[b] = sk.bindPose[b];
    Array<Mat4> model, palette;
    model.Resize(sk.BoneCount());
    palette.Resize(sk.BoneCount());
    anim::LocalToModel(sk, local.View(), model.View());
    anim::SkinningMatrices(sk, model.View(), skinned->skinToNode, palette.View());
    f32 worst = 0.0f;
    for (const Mat4& m : palette)
        for (u32 c = 0; c < 4; ++c)
            for (u32 r = 0; r < 4; ++r)
                worst = Max(worst, Abs(m.columns[c][r] - (c == r ? 1.0f : 0.0f)));
    ZX_EXPECT_LT(worst, 1e-3f);
}

ZX_TEST(Animation, CookedSkeletonClipAndSkinnedMeshRoundTrip)
{
    Result<ImportedScene> scene = ImportGltf(CharacterPath());
    ZX_REQUIRE(scene.IsOk() && !scene->animations.Empty());
    Array<u8> bytes;
    Serialize(scene->skeletons[0], bytes);
    ZX_EXPECT_EQ(PeekAssetType(bytes.View()), AssetType::Skeleton);
    Skeleton sk;
    ZX_REQUIRE(Deserialize(bytes.View(), sk));
    ZX_EXPECT_EQ(sk.BoneCount(), scene->skeletons[0].BoneCount());
    ZX_EXPECT(sk.boneNames[3] == scene->skeletons[0].boneNames[3]);
    bytes.Clear();
    Serialize(scene->animations[0], bytes);
    AnimationClip clip;
    ZX_REQUIRE(Deserialize(bytes.View(), clip));
    ZX_EXPECT_EQ(clip.tracks.Size(), scene->animations[0].tracks.Size());
    ZX_EXPECT_NEAR(clip.duration, scene->animations[0].duration, 1e-6f);
    // The cooked mesh keeps one skin entry per vertex through splitting, LODs and the vertex-cache reordering.
    for (const ImportedNode& n : scene->nodes)
        if (n.skin >= 0) {
            CookedMesh cooked = CookMesh(scene->meshes[usize(n.mesh)], true);
            ZX_EXPECT(cooked.IsSkinned());
            ZX_EXPECT_EQ(cooked.skin.Size(), cooked.vertices.Size() * 2);
            ZX_EXPECT_LE(cooked.boneCount, sk.BoneCount());
            ZX_EXPECT_GT(cooked.boneCount, 10u);
            bytes.Clear();
            Serialize(cooked, bytes);
            CookedMesh back;
            ZX_REQUIRE(Deserialize(bytes.View(), back));
            ZX_EXPECT_EQ(back.skin.Size(), cooked.skin.Size());
            ZX_EXPECT_EQ(back.boneCount, cooked.boneCount);
        }
}

ZX_TEST(Animation, TwoBoneIKReachesTheTarget)
{
    // Hip at the origin, knee 0.5 m down (slightly forward: a bent knee), foot 0.5 m below the knee.
    Skeleton sk;
    sk.boneNames = {String("Hip"), String("Knee"), String("Foot")};
    sk.parents = {-1, 0, 1};
    sk.bindPose = {Transform(), Transform(Vec3(0, -0.5f, 0.05f)), Transform(Vec3(0, -0.5f, -0.05f))};
    sk.inverseBind = {Mat4::Identity(), Mat4::Identity(), Mat4::Identity()};
    for (const Vec3 target : {Vec3(0.1f, -0.8f, 0.1f), Vec3(0.0f, -0.6f, 0.3f), Vec3(-0.2f, -0.7f, 0.0f)}) {
        Transform pose[3] = {sk.bindPose[0], sk.bindPose[1], sk.bindPose[2]};
        anim::SolveTwoBoneIK(sk, Span<Transform>(pose, 3), 0, 1, 2, target, Vec3(1, 0, 0));
        Mat4 model[3];
        anim::LocalToModel(sk, Span<const Transform>(pose, 3), Span<Mat4>(model, 3));
        ZX_EXPECT_LT(Length(model[2].GetTranslation() - target), 2e-3f);
        // Bone lengths are kept (only rotations change).
        ZX_EXPECT_NEAR(Length(model[1].GetTranslation() - model[0].GetTranslation()), Length(sk.bindPose[1].position), 1e-4f);
    }
    // Out of reach: the chain points straight at the target.
    Transform pose[3] = {sk.bindPose[0], sk.bindPose[1], sk.bindPose[2]};
    anim::SolveTwoBoneIK(sk, Span<Transform>(pose, 3), 0, 1, 2, Vec3(0, -5, 0), Vec3(1, 0, 0));
    Mat4 model[3];
    anim::LocalToModel(sk, Span<const Transform>(pose, 3), Span<Mat4>(model, 3));
    ZX_EXPECT_LT(model[2].GetTranslation().y, -0.99f);
}

ZX_TEST(Animation, AdditiveDeltaRoundTrips)
{
    using namespace asset::anim;
    const Transform reference(Vec3(0.1f, 1.0f, 0.0f), Quat::FromAxisAngle(Vec3(0, 1, 0), 0.3f), Vec3(1.0f));
    const Transform pose(Vec3(0.2f, 1.1f, -0.1f), Quat::FromAxisAngle(Vec3(1, 0, 0), 0.5f) * reference.rotation, Vec3(1.2f, 1.0f, 1.0f));
    const Transform delta = AdditiveDelta(reference, pose);
    // Applied to the reference with full weight: the pose again; weight 0: unchanged.
    const Transform back = ApplyAdditive(reference, delta, 1.0f);
    ZX_EXPECT_LT(Length(back.position - pose.position), 1e-5f);
    ZX_EXPECT_GT(Abs(Dot(back.rotation, pose.rotation)), 1.0f - 1e-5f);
    ZX_EXPECT_LT(Length(back.scale - pose.scale), 1e-5f);
    const Transform base(Vec3(3.0f, 0.0f, 0.0f), Quat::FromAxisAngle(Vec3(0, 0, 1), 1.0f), Vec3(2.0f));
    const Transform none = ApplyAdditive(base, delta, 0.0f);
    ZX_EXPECT_LT(Length(none.position - base.position), 1e-6f);
    ZX_EXPECT_GT(Abs(Dot(none.rotation, base.rotation)), 1.0f - 1e-6f);
    // On another base, the same rotation change is added on top.
    const Transform added = ApplyAdditive(base, delta, 1.0f);
    ZX_EXPECT_GT(Abs(Dot(added.rotation, Quat::FromAxisAngle(Vec3(1, 0, 0), 0.5f) * base.rotation)), 1.0f - 1e-5f);
    ZX_EXPECT_LT(Length(added.scale - Vec3(2.4f, 2.0f, 2.0f)), 1e-5f);
}

ZX_TEST(Animation, RootMotionDeltaAcrossLoops)
{
    Skeleton sk;
    AnimationClip clip;
    MakeTestRig(sk, clip); // the root moves 2 m/s along x for 2 s
    ZX_EXPECT_NEAR(anim::SampleBonePosition(clip, sk, 0, 0.5f).x, 1.0f, 1e-5f);
    ZX_EXPECT_NEAR(anim::RootMotionDelta(clip, sk, 0, 0.25f, 0.75f, true).x, 1.0f, 1e-5f);
    // Passing the loop point: the wrap does not jump back, each cycle adds its displacement.
    ZX_EXPECT_NEAR(anim::RootMotionDelta(clip, sk, 0, 1.5f, 2.5f, true).x, 2.0f, 1e-5f);
    ZX_EXPECT_NEAR(anim::RootMotionDelta(clip, sk, 0, 0.5f, 4.5f, true).x, 8.0f, 1e-4f);
    // Without looping the motion stops at the end.
    ZX_EXPECT_NEAR(anim::RootMotionDelta(clip, sk, 0, 1.5f, 2.5f, false).x, 1.0f, 1e-5f);
    ZX_EXPECT_NEAR(anim::RootMotionDelta(clip, sk, 0, 1.5f, 2.5f, true).y, 0.0f, 1e-6f);
}

namespace {

String BlobPath() { return FindEngineRootDirectory() + "/Engine/Content/StarterContent/Characters/SK_Blob.glb"; }

/// Highest point of the mesh with morph target `t` applied at `weight` (cooked data).
f32 TopWithMorph(const CookedMesh& m, u32 t, f32 weight)
{
    f32 top = -1e30f;
    for (usize v = 0; v < m.vertices.Size(); ++v) {
        Vec3 d;
        MemCopy(&d, &m.morphDeltas[(t * m.vertices.Size() + v) * 4], 12);
        top = Max(top, m.vertices[v].position.y + d.y * weight);
    }
    return top;
}

} // namespace

ZX_TEST(Animation, ImportsMorphTargetsAndTheirAnimation)
{
    Result<ImportedScene> scene = ImportGltf(BlobPath());
    ZX_REQUIRE(scene.IsOk());
    const ImportedNode* skinned = nullptr;
    for (const ImportedNode& n : scene->nodes)
        if (n.skin >= 0)
            skinned = &n;
    ZX_REQUIRE(skinned != nullptr);
    MeshData& mesh = scene->meshes[usize(skinned->mesh)];
    ZX_REQUIRE(mesh.morphs.Size() == 3);
    ZX_EXPECT(mesh.morphs[0].name == "Squash");
    ZX_EXPECT(mesh.morphs[2].name == "Blink");
    for (const MorphTarget& m : mesh.morphs) {
        ZX_EXPECT_EQ(m.positions.Size(), mesh.vertices.Size());
        ZX_EXPECT_EQ(m.normals.Size(), mesh.vertices.Size());
    }
    // Blink moves the eyes only: a small part of the vertices.
    usize blinking = 0;
    f32 low = 1e30f, high = -1e30f;
    for (usize v = 0; v < mesh.vertices.Size(); ++v)
        if (LengthSq(mesh.morphs[2].positions[v]) > 1e-8f) {
            ++blinking;
            low = Min(low, mesh.vertices[v].position.y);
            high = Max(high, mesh.vertices[v].position.y);
        }
    ZX_EXPECT_GT(blinking, 10u);
    ZX_EXPECT_LT(blinking, mesh.vertices.Size());
    ZX_EXPECT_GT(low, 0.35f); // eye height (the body around them stays)
    ZX_EXPECT_LT(high, 0.75f);
    // The clips carry the morph weights next to the bone tracks: Bounce starts squashed.
    bool bounce = false;
    for (const AnimationClip& c : scene->animations)
        if (c.name == "Bounce") {
            bounce = true;
            ZX_REQUIRE(c.morphTracks.Size() == 1);
            ZX_EXPECT_EQ(c.morphTracks[0].targets, 3u);
            f32 w[3];
            anim::SampleMorphWeights(c.morphTracks[0], c.duration, 0.0f, true, Span<f32>(w, 3));
            ZX_EXPECT_NEAR(w[0], 0.9f, 0.02f);
            // Serialized and back.
            Array<u8> bytes;
            Serialize(c, bytes);
            AnimationClip back;
            ZX_REQUIRE(Deserialize(bytes.View(), back));
            ZX_REQUIRE(back.morphTracks.Size() == 1);
            ZX_EXPECT_EQ(back.morphTracks[0].weights.Size(), c.morphTracks[0].weights.Size());
        }
    ZX_EXPECT(bounce);
    // Cooking keeps the offsets with their vertices (splitting, LODs, vertex reordering): squash lowers the top,
    // stretch raises it.
    const CookedMesh cooked = CookMesh(mesh, true);
    ZX_REQUIRE(cooked.MorphCount() == 3);
    ZX_EXPECT_EQ(cooked.morphDeltas.Size(), cooked.vertices.Size() * 3 * 4);
    const f32 top = TopWithMorph(cooked, 0, 0.0f);
    ZX_EXPECT_LT(TopWithMorph(cooked, 0, 1.0f), top - 0.1f);
    ZX_EXPECT_GT(TopWithMorph(cooked, 1, 1.0f), top + 0.1f);
    ZX_EXPECT_GE(cooked.bounds.max.y, TopWithMorph(cooked, 1, 1.0f) - 1e-4f); // bounds cover the stretched shape
    Array<u8> bytes;
    Serialize(cooked, bytes);
    CookedMesh back;
    ZX_REQUIRE(Deserialize(bytes.View(), back));
    ZX_EXPECT(back.morphNames[1] == "Stretch");
    ZX_EXPECT_EQ(back.morphDeltas.Size(), cooked.morphDeltas.Size());
    // Packed normal offsets.
    const Vec3 n(0.5f, -1.25f, 1.99f);
    ZX_EXPECT_LT(Length(UnpackMorphNormal(PackMorphNormal(n)) - n), 0.01f);
}
