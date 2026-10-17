// AnimationSystem on the Starter Content character: Animator from the imported prefab, poses over time, clip switch
// with cross-fade, bind pose while editing without preview.

#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Testing/Test.h"
#include "Core/Threading/Thread.h"
#include "Engine/Animation.h"
#include "Engine/Gameplay.h"
#include "Engine/SceneRendering.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

struct AnimatedCharacter
{
    String ddc;
    UniquePtr<asset::AssetManager> assets;
    ecs::World world;
    ecs::Entity skinned;

    AnimatedCharacter()
    {
        scene::InstallHierarchyHooks(world);
        ddc = platform::GetTempDirectory() + String::Format("/zx_anim_test_{}", Thread::CurrentId());
        asset::AssetManagerSettings settings;
        settings.ddcDirectory = ddc;
        settings.writeMetaFiles = false;
        assets.Reset(new asset::AssetManager(settings));
        const asset::AssetId id = assets->LoadPath(FindEngineRootDirectory() + "/Engine/Content/StarterContent/Characters/SK_Human.glb");
        assets->WaitIdle();
        if (const asset::PrefabAsset* prefab = assets->GetPrefab(id)) {
            InstantiatePrefab(world, *prefab, {}, {}, assets.Get());
            world.Query<const Animator>().Each([&](ecs::Entity e, const Animator&) { skinned = e; });
        }
        scene::UpdateWorldTransforms(world);
    }
    ~AnimatedCharacter()
    {
        assets.Reset();
        fs::Delete(ddc);
    }
    /// Ticks until the skeleton and clips are loaded (they are requested on first use).
    void Warm(AnimationSystem& anim, bool playing)
    {
        for (u32 i = 0; i < 20 && anim.Palette(skinned).Empty(); ++i) {
            anim.Tick(world, 0.0f, playing);
            assets->WaitIdle();
        }
        anim.Tick(world, 0.0f, playing);
        assets->WaitIdle();
    }
};

f32 MaxDifference(Span<const Mat4> a, Span<const Mat4> b)
{
    f32 d = 0.0f;
    for (usize i = 0; i < Min(a.Size(), b.Size()); ++i)
        for (u32 c = 0; c < 4; ++c)
            for (u32 r = 0; r < 4; ++r)
                d = Max(d, Abs(a[i].columns[c][r] - b[i].columns[c][r]));
    return d;
}

} // namespace

ZX_TEST(AnimationSystem, ImportedCharacterAnimatesAndCrossFades)
{
    AnimatedCharacter ch;
    ZX_REQUIRE(bool(ch.skinned));
    const Animator* a = ch.world.Get<Animator>(ch.skinned);
    ZX_REQUIRE(a != nullptr);
    ZX_EXPECT(a->skeleton.IsValid());
    ZX_EXPECT(a->clips.Size() >= 7);
    ZX_EXPECT(ch.assets->GetName(a->clip) == "Idle"); // the default clip
    const MeshRenderer* mr = ch.world.Get<MeshRenderer>(ch.skinned);
    ZX_REQUIRE(mr != nullptr);
    ZX_EXPECT(!mr->collision); // animated meshes are no static obstacles

    AnimationSystem anim(*ch.assets);
    ch.Warm(anim, true);
    const Span<const Mat4> start = anim.Palette(ch.skinned);
    ZX_REQUIRE(!start.Empty());
    Array<Mat4> idle0;
    idle0.Append(start);
    for (u32 i = 0; i < 30; ++i)
        anim.Tick(ch.world, 1.0f / 60.0f, true);
    ZX_EXPECT_NEAR(anim.Time(ch.skinned), 0.5f, 1e-4f);
    Array<Mat4> idleHalf;
    idleHalf.Append(anim.Palette(ch.skinned));
    ZX_EXPECT_GT(MaxDifference(idle0.View(), idleHalf.View()), 1e-3f); // the pose moves

    // Switch to Run: the first frames blend from Idle (closer to the Idle pose than the pure Run pose is).
    ZX_REQUIRE(anim.Play(ch.world, ch.skinned, "run"));
    ch.assets->Load(ch.world.Get<Animator>(ch.skinned)->clip);
    ch.assets->WaitIdle();
    anim.Tick(ch.world, 1.0f / 60.0f, true);
    ch.assets->WaitIdle();
    anim.Tick(ch.world, 1.0f / 60.0f, true);
    Array<Mat4> blending;
    blending.Append(anim.Palette(ch.skinned));
    for (u32 i = 0; i < 60; ++i) // well past the 0.25 s blend
        anim.Tick(ch.world, 1.0f / 60.0f, true);
    Array<Mat4> running;
    running.Append(anim.Palette(ch.skinned));
    ZX_EXPECT_LT(MaxDifference(blending.View(), idleHalf.View()), MaxDifference(running.View(), idleHalf.View()));
    ZX_EXPECT(!anim.Play(ch.world, ch.skinned, "NoSuchClip"));
}

ZX_TEST(AnimationSystem, EditModeWithoutPreviewShowsBindPose)
{
    AnimatedCharacter ch;
    ZX_REQUIRE(bool(ch.skinned));
    ch.world.Get<Animator>(ch.skinned)->previewInEditor = false;
    AnimationSystem anim(*ch.assets);
    ch.Warm(anim, false);
    for (u32 i = 0; i < 20; ++i)
        anim.Tick(ch.world, 1.0f / 60.0f, false);
    // Bind pose: every skinning matrix is identity (the importer moved the vertices into node space).
    const Span<const Mat4> palette = anim.Palette(ch.skinned);
    ZX_REQUIRE(!palette.Empty());
    Array<Mat4> identity;
    identity.Resize(palette.Size(), Mat4::Identity());
    ZX_EXPECT_LT(MaxDifference(palette, identity.View()), 2e-3f);
    ZX_EXPECT_NEAR(anim.Time(ch.skinned), 0.0f, 1e-6f);
}

ZX_TEST(AnimGraph, WritesAndParsesTheLocomotionGraph)
{
    const AnimGraph g = MakeLocomotionGraph();
    const String text = WriteAnimGraph(g);
    AnimGraph back;
    String error;
    ZX_REQUIRE(ParseAnimGraph(text.View(), back, &error));
    ZX_EXPECT_EQ(back.states.Size(), g.states.Size());
    ZX_EXPECT_EQ(back.transitions.Size(), g.transitions.Size());
    ZX_EXPECT_EQ(back.params.Size(), g.params.Size());
    const i32 loco = back.FindState("Locomotion");
    ZX_REQUIRE(loco >= 0);
    ZX_EXPECT_EQ(back.states[usize(loco)].blend.Size(), usize(3));
    ZX_EXPECT(back.states[usize(loco)].blendParam == "Speed");
    ZX_EXPECT(!back.states[usize(back.FindState("Jump"))].loop);
    ZX_EXPECT_EQ(back.transitions[0].conditions[0].op, AnimGraph::Op::IsFalse);
    // Errors name what is wrong.
    ZX_EXPECT(!ParseAnimGraph(R"({"states":[{"name":"A","clip":"Idle"}],"transitions":[{"from":"A","to":"B"}]})", back, &error));
    ZX_EXPECT(error.View().Contains("unknown state"));
    ZX_EXPECT(!ParseAnimGraph(R"({"states":[]})", back, &error));
    ZX_EXPECT(!ParseAnimGraph(R"({"states":[{"name":"A","clip":"Idle"}],"transitions":[{"from":"*","to":"A","conditions":[{"param":"X","op":">"}]}]})",
                              back, &error));
    ZX_EXPECT(error.View().Contains("unknown parameter"));
}

ZX_TEST(AnimationSystem, LocomotionGraphFollowsTheCharacter)
{
    AnimatedCharacter ch;
    ZX_REQUIRE(bool(ch.skinned));
    const String graphPath = ch.ddc + "/Locomotion.zanimgraph";
    ZX_REQUIRE(fs::WriteTextFile(graphPath, WriteAnimGraph(MakeLocomotionGraph()).View()));
    ch.world.Get<Animator>(ch.skinned)->graph = "Locomotion.zanimgraph";
    // A character controller on the model's root drives the parameters.
    ecs::Entity root = ch.skinned;
    while (const scene::Parent* p = ch.world.Get<scene::Parent>(root))
        root = p->entity;
    CharacterState st;
    st.grounded = true;
    st.velocity = Vec3(5.0f, 0.0f, 0.0f);
    ch.world.Add(root, st);
    AnimationSystem anim(*ch.assets);
    anim.contentRoot = ch.ddc;
    ch.Warm(anim, true);
    for (u32 i = 0; i < 10; ++i) {
        anim.Tick(ch.world, 1.0f / 60.0f, true);
        ch.assets->WaitIdle();
    }
    ZX_EXPECT(anim.CurrentState(ch.skinned) == "Locomotion");
    ZX_EXPECT_NEAR(anim.GetParam(ch.skinned, "Speed"), 5.0f, 1e-4f);
    ZX_EXPECT_NEAR(anim.GetParam(ch.skinned, "Grounded"), 1.0f, 1e-6f);
    Array<Mat4> running;
    running.Append(anim.Palette(ch.skinned));
    // Standing still blends to Idle: a different pose.
    ch.world.Get<CharacterState>(root)->velocity = Vec3(0.0f);
    for (u32 i = 0; i < 30; ++i)
        anim.Tick(ch.world, 1.0f / 60.0f, true);
    Array<Mat4> idle;
    idle.Append(anim.Palette(ch.skinned));
    ZX_EXPECT_GT(MaxDifference(running.View(), idle.View()), 1e-3f);
    // A jump (rising, off the ground) enters Jump at once; landing returns.
    CharacterState* c = ch.world.Get<CharacterState>(root);
    c->grounded = false;
    c->velocity = Vec3(0.0f, 6.0f, 0.0f);
    anim.Tick(ch.world, 1.0f / 60.0f, true);
    anim.Tick(ch.world, 1.0f / 60.0f, true);
    ZX_EXPECT(anim.CurrentState(ch.skinned) == "Jump");
    c->grounded = true;
    c->velocity = Vec3(0.0f);
    anim.Tick(ch.world, 1.0f / 60.0f, true);
    ZX_EXPECT(anim.CurrentState(ch.skinned) == "Locomotion");
    // Stepping off a stair (briefly airborne, not rising) is no jump.
    c->grounded = false;
    for (u32 i = 0; i < 4; ++i)
        anim.Tick(ch.world, 1.0f / 60.0f, true);
    ZX_EXPECT(anim.CurrentState(ch.skinned) == "Locomotion");
}

namespace {

/// World position of a bone from the palette (palette = skinToNode * model * inverseBind).
Vec3 BoneWorld(AnimatedCharacter& ch, AnimationSystem& anim, StringView bone)
{
    const Animator* a = ch.world.Get<Animator>(ch.skinned);
    const asset::Skeleton* sk = ch.assets->GetSkeleton(a->skeleton);
    const i32 b = sk ? sk->FindBone(bone) : -1;
    const Span<const Mat4> palette = anim.Palette(ch.skinned);
    if (b < 0 || palette.Size() <= usize(b))
        return Vec3();
    const Mat4 node = palette[usize(b)] * Inverse(sk->inverseBind[usize(b)]);
    return ch.world.Get<scene::WorldTransform>(ch.skinned)->matrix.TransformPoint(node.GetTranslation());
}

ecs::Entity RootOf(ecs::World& world, ecs::Entity e)
{
    while (const scene::Parent* p = world.Get<scene::Parent>(e))
        e = p->entity;
    return e;
}

} // namespace

ZX_TEST(AnimationSystem, FootIKFollowsTheGround)
{
    AnimatedCharacter ch;
    ZX_REQUIRE(bool(ch.skinned));
    ch.world.Get<Animator>(ch.skinned)->playing = false; // a still pose: only the IK moves the feet
    const ecs::Entity root = RootOf(ch.world, ch.skinned);
    CharacterState st;
    st.grounded = true;
    ch.world.Add(root, st);
    AnimationSystem anim(*ch.assets);
    // Flat ground first: reference foot heights.
    f32 groundLeft = 0.0f, groundRight = 0.0f;
    f32 leftX = 0.0f;
    anim.raycast = [&](Vec3 from, Vec3, ecs::Entity ignore, Vec3& point, Vec3& normal) {
        ZX_EXPECT(ignore == root); // the character itself is skipped
        point = Vec3(from.x, (from.x - leftX) * (from.x - leftX) < 1e-6f ? groundLeft : groundRight, from.z);
        normal = Vec3(0, 1, 0);
        return true;
    };
    ch.Warm(anim, true);
    auto settle = [&] {
        for (u32 i = 0; i < 90; ++i)
            anim.Tick(ch.world, 1.0f / 60.0f, true);
    };
    settle();
    const Vec3 left0 = BoneWorld(ch, anim, "LeftFoot"), right0 = BoneWorld(ch, anim, "RightFoot");
    ZX_REQUIRE(LengthSq(left0 - right0) > 1e-4f);
    leftX = left0.x;
    // A 0.2 m step under the left foot: it rises, the right one stays.
    groundLeft = 0.2f;
    settle();
    const Vec3 left1 = BoneWorld(ch, anim, "LeftFoot"), right1 = BoneWorld(ch, anim, "RightFoot");
    ZX_EXPECT_NEAR(left1.y - left0.y, 0.2f, 0.03f);
    ZX_EXPECT_NEAR(right1.y, right0.y, 0.03f);
    // A 0.2 m dip under the right foot instead: the hips drop, the right foot reaches down, the left stays.
    groundLeft = 0.0f;
    groundRight = -0.2f;
    settle();
    const Vec3 left2 = BoneWorld(ch, anim, "LeftFoot"), right2 = BoneWorld(ch, anim, "RightFoot");
    ZX_EXPECT_NEAR(right2.y - right0.y, -0.2f, 0.03f);
    ZX_EXPECT_NEAR(left2.y, left0.y, 0.03f);
    ZX_EXPECT_LT(BoneWorld(ch, anim, "Hips").y, BoneWorld(ch, anim, "LeftFoot").y + 10.0f); // finite
    // In the air (or without a controller) the feet go back to the animation.
    ch.world.Get<CharacterState>(root)->grounded = false;
    settle();
    ZX_EXPECT_NEAR(BoneWorld(ch, anim, "RightFoot").y, right0.y, 0.03f);
}

ZX_TEST(AnimationSystem, WalkingReportsFootsteps)
{
    AnimatedCharacter ch;
    ZX_REQUIRE(bool(ch.skinned));
    AnimationSystem anim(*ch.assets);
    ZX_REQUIRE(anim.Play(ch.world, ch.skinned, "Walk"));
    const ecs::Entity root = RootOf(ch.world, ch.skinned);
    CharacterState st;
    st.grounded = true;
    st.velocity = Vec3(0.0f, 0.0f, 2.0f);
    ch.world.Add(root, st);
    ch.Warm(anim, true);
    u32 steps = 0;
    for (u32 i = 0; i < 180; ++i) { // 3 s of a 1 s walk cycle: about six steps
        anim.Tick(ch.world, 1.0f / 60.0f, true);
        for (const AnimEvent& ev : anim.Events())
            steps += ev.name == "Footstep" ? 1u : 0u;
    }
    ZX_EXPECT_GE(steps, 4u);
    ZX_EXPECT_LE(steps, 8u);
    // Standing still: no footsteps.
    ch.world.Get<CharacterState>(root)->velocity = Vec3(0.0f);
    steps = 0;
    for (u32 i = 0; i < 120; ++i) {
        anim.Tick(ch.world, 1.0f / 60.0f, true);
        steps += u32(anim.Events().Size());
    }
    ZX_EXPECT_EQ(steps, 0u);
}

ZX_TEST(AnimationSystem, LookAtTurnsTheHead)
{
    AnimatedCharacter ch;
    ZX_REQUIRE(bool(ch.skinned));
    ch.world.Get<Animator>(ch.skinned)->playing = false;
    AnimationSystem anim(*ch.assets);
    ch.Warm(anim, true);
    const asset::Skeleton* sk = ch.assets->GetSkeleton(ch.world.Get<Animator>(ch.skinned)->skeleton);
    ZX_REQUIRE(sk != nullptr);
    const usize head = usize(sk->FindBone("Head"));
    auto headMatrix = [&] { return anim.Palette(ch.skinned)[head] * Inverse(sk->inverseBind[head]); };
    for (u32 i = 0; i < 10; ++i)
        anim.Tick(ch.world, 1.0f / 60.0f, true);
    const Mat4 ahead = headMatrix();
    // A target to the side (+X, at head height): after the smoothing the head has turned.
    const Vec3 headWorld = ch.world.Get<scene::WorldTransform>(ch.skinned)->matrix.TransformPoint(ahead.GetTranslation());
    anim.SetLookAt(ch.skinned, headWorld + Vec3(3.0f, 0.0f, 1.0f), 1.0f);
    for (u32 i = 0; i < 90; ++i)
        anim.Tick(ch.world, 1.0f / 60.0f, true);
    const Mat4 turned = headMatrix();
    f32 change = 0.0f;
    for (u32 c = 0; c < 3; ++c)
        change = Max(change, Length(SafeNormalize(turned.columns[c].XYZ(), Vec3(1, 0, 0)) - SafeNormalize(ahead.columns[c].XYZ(), Vec3(1, 0, 0))));
    ZX_EXPECT_GT(change, 0.3f); // turned by tens of degrees
    // Weight 0: back to the animation.
    anim.SetLookAt(ch.skinned, Vec3(), 0.0f);
    for (u32 i = 0; i < 120; ++i)
        anim.Tick(ch.world, 1.0f / 60.0f, true);
    const Mat4 back = headMatrix();
    f32 rest = 0.0f;
    for (u32 c = 0; c < 3; ++c)
        rest = Max(rest, Length(SafeNormalize(back.columns[c].XYZ(), Vec3(1, 0, 0)) - SafeNormalize(ahead.columns[c].XYZ(), Vec3(1, 0, 0))));
    ZX_EXPECT_LT(rest, 0.05f);
}

ZX_TEST(AnimGraph, BlendSpace2DWeights)
{
    // Idle at the origin, forward / right / left walks around it.
    const AnimGraph::BlendSample samples[] = {{String("Idle"), 0.0f, 0.0f}, {String("Fwd"), 0.0f, 2.0f}, {String("Right"), 2.0f, 0.0f},
                                              {String("Left"), -2.0f, 0.0f}};
    Array<f32> w;
    auto sum = [&] {
        f32 t = 0.0f;
        for (f32 x : w)
            t += x;
        return t;
    };
    // On a sample: that sample alone.
    BlendSpace2DWeights(Span<const AnimGraph::BlendSample>(samples, 4), 2.0f, 0.0f, w);
    ZX_EXPECT_NEAR(w[2], 1.0f, 1e-5f);
    ZX_EXPECT_NEAR(sum(), 1.0f, 1e-5f);
    // Half way to Forward: Idle and Forward share, the sides stay out.
    BlendSpace2DWeights(Span<const AnimGraph::BlendSample>(samples, 4), 0.0f, 1.0f, w);
    ZX_EXPECT_NEAR(w[0], 0.5f, 1e-4f);
    ZX_EXPECT_NEAR(w[1], 0.5f, 1e-4f);
    ZX_EXPECT_NEAR(w[2] + w[3], 0.0f, 1e-4f);
    // Anywhere (also outside the samples): non-negative and normalized.
    for (f32 x = -3.0f; x <= 3.0f; x += 0.7f)
        for (f32 y = -1.0f; y <= 3.0f; y += 0.6f) {
            BlendSpace2DWeights(Span<const AnimGraph::BlendSample>(samples, 4), x, y, w);
            ZX_EXPECT_NEAR(sum(), 1.0f, 1e-4f);
            for (f32 v : w)
                ZX_EXPECT_GE(v, 0.0f);
        }
}

ZX_TEST(AnimGraph, LayersAnd2DBlendSpacesRoundTrip)
{
    AnimGraph g = MakeLocomotionGraph();
    g.params.PushBack(AnimGraph::Param{"Direction", AnimGraph::ParamType::Float, 0.0f});
    AnimGraph::State strafe;
    strafe.name = "Strafe";
    strafe.blendParam = "Speed";
    strafe.blendParamY = "Direction";
    strafe.blend = {AnimGraph::BlendSample{"Idle", 0.0f, 0.0f}, AnimGraph::BlendSample{"Walk", 0.0f, 1.0f}, AnimGraph::BlendSample{"Run", 3.0f, -1.0f}};
    g.states.PushBack(strafe);
    g.layers[0].weightParam = "Speed";
    AnimGraph::Layer breathe;
    breathe.name = "Breathe";
    breathe.additive = true;
    breathe.weight = 0.5f;
    AnimGraph::State idle;
    idle.name = "Idle";
    idle.clip = "Idle";
    breathe.states.PushBack(idle);
    g.layers.PushBack(breathe);

    AnimGraph back;
    String error;
    ZX_REQUIRE(ParseAnimGraph(WriteAnimGraph(g).View(), back, &error));
    ZX_REQUIRE(back.layers.Size() == 2);
    const AnimGraph::Layer& upper = back.layers[0];
    ZX_EXPECT(upper.name == "UpperBody");
    ZX_REQUIRE(upper.mask.Size() == 1);
    ZX_EXPECT(upper.mask[0] == "Spine");
    ZX_EXPECT(upper.weightParam == "Speed");
    ZX_EXPECT(!upper.additive);
    ZX_EXPECT(!upper.states[usize(upper.FindState("None"))].HasMotion()); // silent states are fine in layers
    ZX_EXPECT_EQ(upper.transitions[0].from, -1);
    ZX_EXPECT_EQ(upper.transitions[1].exitTime, 0.85f);
    ZX_EXPECT(back.layers[1].additive);
    ZX_EXPECT_NEAR(back.layers[1].weight, 0.5f, 1e-6f);
    ZX_EXPECT(back.layers[1].mask.Empty());
    const AnimGraph::State& s2 = back.states[usize(back.FindState("Strafe"))];
    ZX_EXPECT(s2.Is2D());
    ZX_EXPECT(s2.blendParamY == "Direction");
    ZX_EXPECT_NEAR(s2.blend[2].value, 3.0f, 1e-6f);
    ZX_EXPECT_NEAR(s2.blend[2].y, -1.0f, 1e-6f); // 2D samples keep their order (no sorting by value)
    // The base machine still needs motion in every state; layer errors name the layer.
    ZX_EXPECT(!ParseAnimGraph(R"({"states":[{"name":"A"}]})", back, &error));
    ZX_EXPECT(!ParseAnimGraph(R"({"states":[{"name":"A","clip":"Idle"}],"layers":[{"name":"Arms","states":[{"name":"X"}],"transitions":[{"from":"X","to":"Y"}]}]})",
                              back, &error));
    ZX_EXPECT(error.View().Contains("Arms"));
}

ZX_TEST(AnimationSystem, UpperBodyLayerPunchesWhileTheLegsRun)
{
    AnimatedCharacter ch;
    ZX_REQUIRE(bool(ch.skinned));
    ZX_REQUIRE(fs::WriteTextFile(ch.ddc + "/Locomotion.zanimgraph", WriteAnimGraph(MakeLocomotionGraph()).View()));
    Animator* a = ch.world.Get<Animator>(ch.skinned);
    a->graph = "Locomotion.zanimgraph";
    a->footIK = false;
    // Two systems over the same character: one punches, the other only runs.
    AnimationSystem punching(*ch.assets), running(*ch.assets);
    punching.contentRoot = running.contentRoot = ch.ddc;
    for (AnimationSystem* anim : {&punching, &running}) {
        anim->SetParam(ch.skinned, "Speed", 6.0f); // warm up with the Run clip loaded
        ch.Warm(*anim, true);
    }
    const asset::Skeleton* sk = ch.assets->GetSkeleton(a->skeleton);
    ZX_REQUIRE(sk != nullptr);
    const i32 hand = sk->FindBone("RightHand"), foot = sk->FindBone("LeftFoot");
    ZX_REQUIRE(hand >= 0 && foot >= 0);
    for (u32 i = 0; i < 10; ++i) {
        punching.Tick(ch.world, 1.0f / 60.0f, true);
        running.Tick(ch.world, 1.0f / 60.0f, true);
    }
    ZX_EXPECT(punching.CurrentState(ch.skinned, 1) == "None");
    punching.SetTrigger(ch.skinned, "Punch");
    f32 handDiff = 0.0f, footDiff = 0.0f;
    for (u32 i = 0; i < 20; ++i) {
        punching.Tick(ch.world, 1.0f / 60.0f, true);
        running.Tick(ch.world, 1.0f / 60.0f, true);
        const Span<const Mat4> p = punching.Palette(ch.skinned), r = running.Palette(ch.skinned);
        handDiff = Max(handDiff, MaxDifference(p.SubSpan(usize(hand), 1), r.SubSpan(usize(hand), 1)));
        footDiff = Max(footDiff, MaxDifference(p.SubSpan(usize(foot), 1), r.SubSpan(usize(foot), 1)));
    }
    ZX_EXPECT(punching.CurrentState(ch.skinned, 1) == "Punch");
    ZX_EXPECT(punching.CurrentState(ch.skinned) == "Locomotion");
    ZX_EXPECT_GT(handDiff, 1e-2f); // the arm punches...
    ZX_EXPECT_LT(footDiff, 1e-4f); // ...the legs keep running exactly as without the layer
    // The punch plays once and the layer falls silent again.
    for (u32 i = 0; i < 180; ++i)
        punching.Tick(ch.world, 1.0f / 60.0f, true);
    ZX_EXPECT(punching.CurrentState(ch.skinned, 1) == "None");
}
