// Retargeting: bone classification across naming schemes, a Mixamo-style T-pose rig (Y-up, three spine bones) driving
// an Unreal-style A-pose rig (Z-up skeleton space, two spine bones, other proportions, arbitrary bone axes).

#include "Asset/Retarget.h"
#include "Asset/SceneImport.h"
#include "Core/CoreInit.h"
#include "Core/Testing/Test.h"

using namespace zx;
using namespace zx::asset;
using namespace zx::asset::anim;

namespace {

struct BoneDef
{
    const char* name;
    const char* parent;
    Vec3 position; // skeleton space, Y-up, facing +Z (left = -X)
};

/// Builds a skeleton from skeleton-space joint positions; every bone gets a pseudo-random orientation (the local axes
/// of real rigs differ between tools), `space` rotates the whole rig inside its model (e.g. a Z-up armature).
Skeleton MakeSkeleton(Span<const BoneDef> defs, const Quat& space, f32 scale, u32 seed)
{
    Skeleton sk;
    Array<Quat> global;
    Array<Vec3> position;
    for (const BoneDef& d : defs) {
        i32 parent = -1;
        if (d.parent)
            parent = sk.FindBone(StringView(d.parent));
        seed = seed * 1664525u + 1013904223u;
        const Vec3 axis = Normalize(Vec3(f32(seed & 255) - 127.5f, f32((seed >> 8) & 255) - 127.5f, f32((seed >> 16) & 255) - 127.5f));
        const Quat r = space * Quat::FromAxisAngle(axis, f32((seed >> 24) & 255) / 255.0f * 3.0f);
        const Vec3 p = space * (d.position * scale);
        Transform local;
        if (parent >= 0) {
            const Quat pr = global[u32(parent)];
            local.rotation = Normalize(Inverse(pr) * r);
            local.position = Inverse(pr) * (p - position[u32(parent)]);
        } else {
            local.rotation = r;
            local.position = p;
        }
        sk.boneNames.PushBack(String(d.name));
        sk.parents.PushBack(parent);
        sk.bindPose.PushBack(local);
        sk.inverseBind.PushBack(Mat4::Identity());
        global.PushBack(r);
        position.PushBack(p);
    }
    sk.toModel = Transform(Vec3(), Inverse(space)); // the model stands upright, facing +Z
    return sk;
}

const BoneDef g_mixamo[] = {
    {"mixamorig:Hips", nullptr, Vec3(0, 1.0f, 0)},
    {"mixamorig:Spine", "mixamorig:Hips", Vec3(0, 1.1f, 0)},
    {"mixamorig:Spine1", "mixamorig:Spine", Vec3(0, 1.25f, 0)},
    {"mixamorig:Spine2", "mixamorig:Spine1", Vec3(0, 1.4f, 0)},
    {"mixamorig:Neck", "mixamorig:Spine2", Vec3(0, 1.55f, 0)},
    {"mixamorig:Head", "mixamorig:Neck", Vec3(0, 1.65f, 0)},
    {"mixamorig:LeftShoulder", "mixamorig:Spine2", Vec3(-0.05f, 1.5f, 0)},
    {"mixamorig:LeftArm", "mixamorig:LeftShoulder", Vec3(-0.2f, 1.5f, 0)},
    {"mixamorig:LeftForeArm", "mixamorig:LeftArm", Vec3(-0.45f, 1.5f, 0)},
    {"mixamorig:LeftHand", "mixamorig:LeftForeArm", Vec3(-0.7f, 1.5f, 0)},
    {"mixamorig:LeftHandIndex1", "mixamorig:LeftHand", Vec3(-0.8f, 1.5f, 0)},
    {"mixamorig:RightShoulder", "mixamorig:Spine2", Vec3(0.05f, 1.5f, 0)},
    {"mixamorig:RightArm", "mixamorig:RightShoulder", Vec3(0.2f, 1.5f, 0)},
    {"mixamorig:RightForeArm", "mixamorig:RightArm", Vec3(0.45f, 1.5f, 0)},
    {"mixamorig:RightHand", "mixamorig:RightForeArm", Vec3(0.7f, 1.5f, 0)},
    {"mixamorig:LeftUpLeg", "mixamorig:Hips", Vec3(-0.1f, 0.95f, 0)},
    {"mixamorig:LeftLeg", "mixamorig:LeftUpLeg", Vec3(-0.1f, 0.5f, 0.02f)},
    {"mixamorig:LeftFoot", "mixamorig:LeftLeg", Vec3(-0.1f, 0.08f, 0)},
    {"mixamorig:LeftToeBase", "mixamorig:LeftFoot", Vec3(-0.1f, 0.0f, 0.12f)},
    {"mixamorig:RightUpLeg", "mixamorig:Hips", Vec3(0.1f, 0.95f, 0)},
    {"mixamorig:RightLeg", "mixamorig:RightUpLeg", Vec3(0.1f, 0.5f, 0.02f)},
    {"mixamorig:RightFoot", "mixamorig:RightLeg", Vec3(0.1f, 0.08f, 0)},
    {"mixamorig:RightToeBase", "mixamorig:RightFoot", Vec3(0.1f, 0.0f, 0.12f)},
};

// Shorter legs, longer arms held in an A-pose (45 degrees down), two spine bones, a root under the pelvis.
const BoneDef g_unreal[] = {
    {"root", nullptr, Vec3(0, 0, 0)},
    {"pelvis", "root", Vec3(0, 0.8f, 0)},
    {"spine_01", "pelvis", Vec3(0, 0.95f, 0)},
    {"spine_02", "spine_01", Vec3(0, 1.2f, 0.03f)},
    {"neck_01", "spine_02", Vec3(0, 1.4f, 0)},
    {"head", "neck_01", Vec3(0, 1.5f, 0)},
    {"clavicle_l", "spine_02", Vec3(-0.06f, 1.35f, 0)},
    {"upperarm_l", "clavicle_l", Vec3(-0.2f, 1.35f, 0)},
    {"upperarm_twist_01_l", "upperarm_l", Vec3(-0.3f, 1.25f, 0)},
    {"lowerarm_l", "upperarm_l", Vec3(-0.44f, 1.11f, 0)},
    {"hand_l", "lowerarm_l", Vec3(-0.65f, 0.9f, 0)},
    {"index_01_l", "hand_l", Vec3(-0.72f, 0.83f, 0)},
    {"clavicle_r", "spine_02", Vec3(0.06f, 1.35f, 0)},
    {"upperarm_r", "clavicle_r", Vec3(0.2f, 1.35f, 0)},
    {"lowerarm_r", "upperarm_r", Vec3(0.44f, 1.11f, 0)},
    {"hand_r", "lowerarm_r", Vec3(0.65f, 0.9f, 0)},
    {"thigh_l", "pelvis", Vec3(-0.09f, 0.75f, 0)},
    {"calf_l", "thigh_l", Vec3(-0.09f, 0.4f, 0.02f)},
    {"foot_l", "calf_l", Vec3(-0.09f, 0.07f, 0)},
    {"ball_l", "foot_l", Vec3(-0.09f, 0.0f, 0.1f)},
    {"thigh_r", "pelvis", Vec3(0.09f, 0.75f, 0)},
    {"calf_r", "thigh_r", Vec3(0.09f, 0.4f, 0.02f)},
    {"foot_r", "calf_r", Vec3(0.09f, 0.07f, 0)},
    {"ball_r", "foot_r", Vec3(0.09f, 0.0f, 0.1f)},
    {"ik_foot_l", "root", Vec3(-0.09f, 0.07f, 0)},
};

void Model(const Skeleton& sk, Span<const Transform> local, Array<Transform>& out)
{
    out.Resize(sk.BoneCount());
    for (u32 b = 0; b < sk.BoneCount(); ++b)
        out[b] = sk.parents[b] >= 0 ? out[u32(sk.parents[b])] * local[b] : local[b];
}

Vec3 Dir(const Array<Transform>& model, const Skeleton& sk, const char* from, const char* to)
{
    return Normalize(model[u32(sk.FindBone(StringView(to)))].position - model[u32(sk.FindBone(StringView(from)))].position);
}

Quat ZUp() { return Quat::FromAxisAngle(Vec3(1, 0, 0), -HalfPi); }

/// Skeleton space -> model root space of the (only) skinned node.
Mat4 SkinToRoot(const ImportedScene& scene)
{
    for (const ImportedNode& n : scene.nodes) {
        if (n.skin < 0)
            continue;
        Mat4 m = n.skinToNode;
        for (i32 p = i32(&n - scene.nodes.Data()); p >= 0; p = scene.nodes[usize(p)].parent)
            m = scene.nodes[usize(p)].local.ToMatrix() * m;
        return m;
    }
    return Mat4::Identity();
}

Vec3 RootDir(const Mat4& toRoot, Span<const Mat4> model, const Skeleton& sk, const char* from, const char* to)
{
    const Vec3 a = toRoot.TransformPoint(model[u32(sk.FindBone(StringView(from)))].GetTranslation());
    const Vec3 b = toRoot.TransformPoint(model[u32(sk.FindBone(StringView(to)))].GetTranslation());
    return Normalize(b - a);
}

} // namespace

ZX_TEST(Retarget, ClassifiesCommonNamingSchemes)
{
    struct Case
    {
        const char* name;
        HumanBone bone;
    };
    const Case cases[] = {
        {"mixamorig:Hips", HumanBone::Hips},           {"mixamorig:LeftUpLeg", HumanBone::LeftUpperLeg},
        {"mixamorig:RightLeg", HumanBone::RightLowerLeg}, {"mixamorig:LeftForeArm", HumanBone::LeftLowerArm},
        {"mixamorig:RightArm", HumanBone::RightUpperArm}, {"mixamorig:LeftShoulder", HumanBone::LeftShoulder},
        {"mixamorig:LeftToeBase", HumanBone::LeftToes},   {"mixamorig:LeftHandIndex1", HumanBone::None},
        {"HeadTop_End", HumanBone::None},                {"pelvis", HumanBone::Hips},
        {"thigh_l", HumanBone::LeftUpperLeg},            {"calf_r", HumanBone::RightLowerLeg},
        {"ball_l", HumanBone::LeftToes},                 {"clavicle_r", HumanBone::RightShoulder},
        {"upperarm_l", HumanBone::LeftUpperArm},         {"lowerarm_r", HumanBone::RightLowerArm},
        {"upperarm_twist_01_l", HumanBone::None},        {"ik_foot_l", HumanBone::None},
        {"neck_01", HumanBone::Neck},                    {"spine_03", HumanBone::Spine},
        {"root", HumanBone::Root},                       {"Bip01 L Thigh", HumanBone::LeftUpperLeg},
        {"Bip01 R Forearm", HumanBone::RightLowerArm},   {"Bip01 Pelvis", HumanBone::Hips},
        {"upper_arm.L", HumanBone::LeftUpperArm},        {"forearm.R", HumanBone::RightLowerArm},
        {"shin.L", HumanBone::LeftLowerLeg},             {"DEF-thigh.R", HumanBone::RightUpperLeg},
        {"LThigh", HumanBone::LeftUpperLeg},             {"HandR", HumanBone::RightHand},
        {"Leg_Upper_Right", HumanBone::RightUpperLeg},   {"LeftEye", HumanBone::None},
        {"Head", HumanBone::Head},                       {"Chest", HumanBone::Spine},
    };
    for (const Case& c : cases)
        ZX_EXPECT_EQ(String(HumanBoneName(ClassifyBone(StringView(c.name)))), String(HumanBoneName(c.bone)));
    ZX_EXPECT_EQ(NormalizeBoneName("mixamorig:Left_Hand.001"), String("lefthand001"));
}

ZX_TEST(Retarget, SameSkeletonIsIdentity)
{
    const Skeleton a = MakeSkeleton(Span<const BoneDef>(g_mixamo), Quat(), 1.0f, 7);
    const RetargetMap map = BuildRetargetMap(a, a);
    ZX_EXPECT(map.identity);
    ZX_EXPECT_EQ(map.mapped, a.BoneCount());
}

ZX_TEST(Retarget, MapsHumanoidSlotsAndSpineChains)
{
    const Skeleton src = MakeSkeleton(Span<const BoneDef>(g_mixamo), Quat(), 1.0f, 1);
    const Skeleton dst = MakeSkeleton(Span<const BoneDef>(g_unreal), ZUp(), 1.0f, 2);
    const RetargetMap map = BuildRetargetMap(src, dst);
    ZX_EXPECT(!map.identity);
    auto sourceOf = [&](const char* bone) {
        const i32 s = map.source[u32(dst.FindBone(StringView(bone)))];
        return s >= 0 ? src.boneNames[u32(s)] : String("-");
    };
    ZX_EXPECT_EQ(sourceOf("pelvis"), String("mixamorig:Hips"));
    ZX_EXPECT_EQ(sourceOf("spine_01"), String("mixamorig:Spine1")); // 3 source spine bones over 2
    ZX_EXPECT_EQ(sourceOf("spine_02"), String("mixamorig:Spine2"));
    ZX_EXPECT_EQ(sourceOf("thigh_l"), String("mixamorig:LeftUpLeg"));
    ZX_EXPECT_EQ(sourceOf("calf_r"), String("mixamorig:RightLeg"));
    ZX_EXPECT_EQ(sourceOf("upperarm_l"), String("mixamorig:LeftArm"));
    ZX_EXPECT_EQ(sourceOf("hand_r"), String("mixamorig:RightHand"));
    ZX_EXPECT_EQ(sourceOf("ball_l"), String("mixamorig:LeftToeBase"));
    ZX_EXPECT_EQ(sourceOf("root"), String("-"));
    ZX_EXPECT_EQ(sourceOf("upperarm_twist_01_l"), String("-"));
    ZX_EXPECT_EQ(sourceOf("ik_foot_l"), String("-"));
    // Z-up target space: aligning maps the source's up onto the target's.
    ZX_EXPECT_LT(Length(map.align * Vec3(0, 1, 0) - ZUp() * Vec3(0, 1, 0)), 1e-3f);
    ZX_EXPECT_NEAR(map.scale, 0.73f / 0.92f, 1e-3f); // hip heights above the feet
}

ZX_TEST(Retarget, PoseKeepsDirectionsAcrossRigs)
{
    const Skeleton src = MakeSkeleton(Span<const BoneDef>(g_mixamo), Quat(), 1.0f, 3);
    const Skeleton dst = MakeSkeleton(Span<const BoneDef>(g_unreal), ZUp(), 1.0f, 4);
    const RetargetMap map = BuildRetargetMap(src, dst);
    const Quat back = Inverse(map.align);

    // At rest the A-pose arms come up to the source's T-pose.
    Array<Transform> local, srcModel, dstModel;
    local.Resize(dst.BoneCount());
    RetargetPose(map, src, src.bindPose.View(), dst, local.View());
    Model(src, src.bindPose.View(), srcModel);
    Model(dst, local.View(), dstModel);
    ZX_EXPECT_LT(Length(back * Dir(dstModel, dst, "upperarm_l", "lowerarm_l") -
                        Dir(srcModel, src, "mixamorig:LeftArm", "mixamorig:LeftForeArm")), 1e-3f);

    // A pose: arm down 60 degrees, knee bent, head turned, walked 0.5 m forward.
    Array<Transform> pose(src.bindPose.View());
    RotateGlobal(src, pose.View(), u32(src.FindBone("mixamorig:LeftArm")), Quat::FromAxisAngle(Vec3(0, 0, 1), DegToRad(60.0f)));
    RotateGlobal(src, pose.View(), u32(src.FindBone("mixamorig:RightLeg")), Quat::FromAxisAngle(Vec3(1, 0, 0), DegToRad(-70.0f)));
    const Quat headTurn = Quat::FromAxisAngle(Vec3(0, 1, 0), DegToRad(40.0f));
    RotateGlobal(src, pose.View(), u32(src.FindBone("mixamorig:Head")), headTurn);
    pose[u32(src.FindBone("mixamorig:Hips"))].position += Vec3(0, 0, 0.5f); // root bone: skeleton space
    RetargetPose(map, src, pose.View(), dst, local.View());
    Model(src, pose.View(), srcModel);
    Model(dst, local.View(), dstModel);

    const struct
    {
        const char *srcFrom, *srcTo, *dstFrom, *dstTo;
    } limbs[] = {
        {"mixamorig:LeftArm", "mixamorig:LeftForeArm", "upperarm_l", "lowerarm_l"},
        {"mixamorig:LeftForeArm", "mixamorig:LeftHand", "lowerarm_l", "hand_l"},
        {"mixamorig:RightUpLeg", "mixamorig:RightLeg", "thigh_r", "calf_r"},
        {"mixamorig:RightLeg", "mixamorig:RightFoot", "calf_r", "foot_r"},
        {"mixamorig:RightFoot", "mixamorig:RightToeBase", "foot_r", "ball_r"},
    };
    for (const auto& l : limbs)
        ZX_EXPECT_LT(Length(back * Dir(dstModel, dst, l.dstFrom, l.dstTo) - Dir(srcModel, src, l.srcFrom, l.srcTo)), 1e-3f);

    // The head turns by the same rotation (no child to aim: pure change from rest).
    const u32 head = u32(dst.FindBone("head"));
    const Quat dstDelta = back * dstModel[head].rotation * Inverse(map.targetRest[head].rotation) * map.align;
    ZX_EXPECT(IsNearlyEqual(dstDelta, headTurn, 1e-3f));

    // The pelvis walks forward, scaled by the hip heights.
    const u32 pelvis = u32(dst.FindBone("pelvis"));
    const Vec3 moved = back * (dstModel[pelvis].position - map.targetRest[pelvis].position);
    ZX_EXPECT_LT(Length(moved - Vec3(0, 0, 0.5f * map.scale)), 1e-3f);
    // Bones keep the target's lengths.
    ZX_EXPECT_NEAR(Length(dstModel[u32(dst.FindBone("calf_r"))].position - dstModel[u32(dst.FindBone("thigh_r"))].position), 0.35057f, 1e-3f);
}

ZX_TEST(Retarget, ClipIsResampledForTheTarget)
{
    const Skeleton src = MakeSkeleton(Span<const BoneDef>(g_mixamo), Quat(), 1.0f, 5);
    const Skeleton dst = MakeSkeleton(Span<const BoneDef>(g_unreal), ZUp(), 0.01f, 6); // centimeter-sized rig
    AnimationClip clip;
    clip.name = "Wave";
    clip.duration = 1.0f;
    clip.boneCount = src.BoneCount();
    const u32 arm = u32(src.FindBone("mixamorig:LeftArm"));
    AnimationTrack& t = clip.tracks.AddDefaulted();
    t.bone = arm;
    Array<Transform> up(src.bindPose.View());
    RotateGlobal(src, up.View(), arm, Quat::FromAxisAngle(Vec3(0, 0, 1), DegToRad(-50.0f)));
    t.rotationTimes = {0.0f, 1.0f};
    t.rotations = {src.bindPose[arm].rotation, up[arm].rotation};

    const RetargetMap map = BuildRetargetMap(src, dst);
    const AnimationClip out = RetargetClip(clip, src, dst, map, 30.0f);
    ZX_EXPECT_EQ(out.name, String("Wave"));
    ZX_EXPECT_NEAR(out.duration, 1.0f, 1e-6f);
    ZX_EXPECT_EQ(out.boneCount, dst.BoneCount());
    ZX_EXPECT_EQ(out.tracks.Size(), usize(map.mapped));
    ZX_EXPECT_NEAR(map.scale, 0.01f * 0.73f / 0.92f, 1e-5f);

    // Sampled halfway, the retargeted clip matches retargeting the source pose halfway.
    Array<Transform> srcPose, expected, got;
    srcPose.Resize(src.BoneCount());
    expected.Resize(dst.BoneCount());
    got.Resize(dst.BoneCount());
    SampleClip(clip, src, 0.5f, false, srcPose.View());
    RetargetPose(map, src, srcPose.View(), dst, expected.View());
    SampleClip(out, dst, 0.5f, false, got.View());
    for (u32 b = 0; b < dst.BoneCount(); ++b) {
        ZX_EXPECT(IsNearlyEqual(got[b].rotation, expected[b].rotation, 1e-3f));
        ZX_EXPECT_LT(Length(got[b].position - expected[b].position), 1e-5f);
    }
}

ZX_TEST(Retarget, StarterBotWalksWithTheHumanClips)
{
    const String dir = FindEngineRootDirectory() + "/Engine/Content/StarterContent/Characters/";
    Result<ImportedScene> human = ImportGltf(dir + "SK_Human.glb");
    Result<ImportedScene> bot = ImportGltf(dir + "SK_Bot.glb");
    ZX_REQUIRE(human.IsOk() && bot.IsOk());
    ZX_REQUIRE(human->skeletons.Size() == 1 && bot->skeletons.Size() == 1);
    ZX_EXPECT(bot->animations.Empty()); // it borrows the human's
    const Skeleton& hs = human->skeletons[0];
    const Skeleton& bs = bot->skeletons[0];
    const RetargetMap map = BuildRetargetMap(hs, bs);
    ZX_EXPECT(!map.identity);
    ZX_EXPECT_EQ(map.mapped, bs.BoneCount() - 1); // all but the root
    ZX_EXPECT_LT(map.scale, 1.0f);

    const AnimationClip* walk = nullptr;
    for (const AnimationClip& c : human->animations)
        if (c.name == "Walk")
            walk = &c;
    ZX_REQUIRE(walk != nullptr);
    const Mat4 humanToRoot = SkinToRoot(*human), botToRoot = SkinToRoot(*bot);
    Array<Transform> hp, bp;
    Array<Mat4> hm, bm;
    hp.Resize(hs.BoneCount());
    bp.Resize(bs.BoneCount());
    hm.Resize(hs.BoneCount());
    bm.Resize(bs.BoneCount());
    const struct
    {
        const char *humanFrom, *humanTo, *botFrom, *botTo;
    } limbs[] = {
        {"LeftUpLeg", "LeftLeg", "thigh_l", "calf_l"},
        {"RightLeg", "RightFoot", "calf_r", "foot_r"},
        {"LeftFoot", "LeftToeBase", "foot_l", "ball_l"},
        {"RightArm", "RightForeArm", "upperarm_r", "lowerarm_r"},
        {"LeftForeArm", "LeftHand", "lowerarm_l", "hand_l"},
        {"Spine2", "Neck", "spine_02", "neck_01"},
    };
    f32 worst = 0.0f;
    for (f32 phase : {0.0f, 0.25f, 0.5f, 0.75f}) {
        SampleClip(*walk, hs, phase * walk->duration, true, hp.View());
        RetargetPose(map, hs, hp.View(), bs, bp.View());
        LocalToModel(hs, hp.View(), hm.View());
        LocalToModel(bs, bp.View(), bm.View());
        for (const auto& l : limbs)
            worst = Max(worst, Length(RootDir(botToRoot, bm.View(), bs, l.botFrom, l.botTo) -
                                      RootDir(humanToRoot, hm.View(), hs, l.humanFrom, l.humanTo)));
    }
    // In the model space (where both face +Z while walking) every limb points the same way, although the human's rest
    // pose faces -X and its skeleton is Z-up in centimeters.
    ZX_EXPECT_LT(worst, 0.01f);
}
