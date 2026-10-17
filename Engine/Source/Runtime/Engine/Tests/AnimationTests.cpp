// AnimationSystem on the Starter Content character: Animator from the imported prefab, poses over time, clip switch
// with cross-fade, bind pose while editing without preview.

#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Testing/Test.h"
#include "Core/Threading/Thread.h"
#include "Engine/Animation.h"
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
