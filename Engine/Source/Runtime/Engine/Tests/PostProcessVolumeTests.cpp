#include "Core/Testing/Test.h"

#include <cmath>
#include "Engine/ComponentRegistry.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SceneSerialization.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

ecs::Entity AddVolume(ecs::World& world, Vec3 position, const PostProcessVolume& v, Vec3 scale = Vec3(1.0f))
{
    Transform t(position);
    t.scale = scale;
    const ecs::Entity e = scene::CreateNode(world, "Volume", t);
    world.Add(e, v);
    return e;
}

} // namespace

ZX_TEST(PostProcessVolume, WeightInsideBlendAndOutside)
{
    PostProcessVolume v;
    v.extent = Vec3(2.0f, 2.0f, 2.0f);
    v.blendRadius = 4.0f;
    const Mat4 world = Mat4::TRS(Vec3(10, 0, 0), Quat(), Vec3(1.0f));
    ZX_EXPECT_NEAR(PostProcessVolumeWeight(v, world, Vec3(10, 0, 0)), 1.0f, 1e-6f); // center
    ZX_EXPECT_NEAR(PostProcessVolumeWeight(v, world, Vec3(11.9f, 1, -1)), 1.0f, 1e-6f); // inside
    ZX_EXPECT_NEAR(PostProcessVolumeWeight(v, world, Vec3(14, 0, 0)), 0.5f, 1e-5f);    // 2 m out of 4
    ZX_EXPECT_NEAR(PostProcessVolumeWeight(v, world, Vec3(17, 0, 0)), 0.0f, 1e-6f);    // beyond the radius
    // Scale grows the box: x extent 2 * 3 = 6 m.
    const Mat4 scaled = Mat4::TRS(Vec3(10, 0, 0), Quat(), Vec3(3, 1, 1));
    ZX_EXPECT_NEAR(PostProcessVolumeWeight(v, scaled, Vec3(15.5f, 0, 0)), 1.0f, 1e-6f);
    // Unbound, blend weight, disabled.
    v.unbound = true;
    v.blendWeight = 0.3f;
    ZX_EXPECT_NEAR(PostProcessVolumeWeight(v, world, Vec3(1000, 0, 0)), 0.3f, 1e-6f);
    v.enabled = false;
    ZX_EXPECT_NEAR(PostProcessVolumeWeight(v, world, Vec3(10, 0, 0)), 0.0f, 1e-6f);
}

ZX_TEST(PostProcessVolume, BlendsOverridesByPriority)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    render::PostSettings base;
    base.exposureCompensation = 0.0f;
    base.saturation = 1.0f;
    base.depthOfField = false;

    // A global volume: saturation 0 (only that is overridden).
    PostProcessVolume global;
    global.unbound = true;
    global.overrideSaturation = true;
    global.saturation = 0.0f;
    global.contrast = 3.0f; // not overridden: ignored
    AddVolume(world, Vec3(0.0f), global);
    // A local, higher-priority volume: exposure +2, saturation 2, depth of field on; 2 m blend radius.
    PostProcessVolume local;
    local.priority = 5;
    local.extent = Vec3(1.0f);
    local.blendRadius = 2.0f;
    local.overrideExposureCompensation = true;
    local.exposureCompensation = 2.0f;
    local.overrideSaturation = true;
    local.saturation = 2.0f;
    local.overrideDepthOfField = true;
    local.depthOfField = true;
    local.overrideFocusDistance = true;
    local.focusDistance = 1.5f;
    AddVolume(world, Vec3(20, 0, 0), local);
    scene::UpdateWorldTransforms(world);

    // Far from the local volume: only the global one.
    render::PostSettings s = EvaluatePostProcessVolumes(world, Vec3(0.0f), base);
    ZX_EXPECT_NEAR(s.saturation, 0.0f, 1e-6f);
    ZX_EXPECT_NEAR(s.contrast, base.contrast, 1e-6f);
    ZX_EXPECT_NEAR(s.exposureCompensation, 0.0f, 1e-6f);
    ZX_EXPECT(!s.depthOfField);

    // Inside the local volume: it wins (applied after the global one).
    s = EvaluatePostProcessVolumes(world, Vec3(20, 0, 0), base);
    ZX_EXPECT_NEAR(s.saturation, 2.0f, 1e-6f);
    ZX_EXPECT_NEAR(s.exposureCompensation, 2.0f, 1e-6f);
    ZX_EXPECT(s.depthOfField);
    ZX_EXPECT_NEAR(s.focusDistance, 1.5f, 1e-6f);

    // In the blend region of the local volume (box half size 1 m, blend radius 2 m).
    s = EvaluatePostProcessVolumes(world, Vec3(22, 0, 0), base); // 1 m outside -> weight 0.5
    ZX_EXPECT_NEAR(s.saturation, 1.0f, 1e-5f);                   // lerp(0, 2, 0.5)
    ZX_EXPECT_NEAR(s.exposureCompensation, 1.0f, 1e-5f);
    ZX_EXPECT(s.depthOfField); // switches flip at weight >= 0.5
    s = EvaluatePostProcessVolumes(world, Vec3(22.5f, 0, 0), base); // weight 0.25
    ZX_EXPECT_NEAR(s.exposureCompensation, 0.5f, 1e-5f);
    ZX_EXPECT(!s.depthOfField);

    // Swapping priorities: the global volume now wins inside the local one.
    world.Query<PostProcessVolume>().Each([](PostProcessVolume& v) { v.priority = v.unbound ? 10 : 0; });
    s = EvaluatePostProcessVolumes(world, Vec3(20, 0, 0), base);
    ZX_EXPECT_NEAR(s.saturation, 0.0f, 1e-6f);
    ZX_EXPECT_NEAR(s.exposureCompensation, 2.0f, 1e-6f); // only the local volume sets exposure
}

ZX_TEST(PostProcessVolume, SerializesWithTheScene)
{
    RegisterEngineComponents();
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    PostProcessVolume v;
    v.priority = 3;
    v.overrideFStop = true;
    v.fStop = 1.4f;
    v.colorFilter = Vec3(1.0f, 0.5f, 0.25f);
    const ecs::Entity e = AddVolume(world, Vec3(1, 2, 3), v);
    EnsureEntityGuid(world, e);
    const JsonValue json = SaveScene(world, SceneIOOptions{});
    ecs::World loaded;
    scene::InstallHierarchyHooks(loaded);
    ZX_REQUIRE(LoadScene(json, loaded, SceneIOOptions{}));
    u32 found = 0;
    loaded.Query<const PostProcessVolume>().Each([&](const PostProcessVolume& p) {
        ++found;
        ZX_EXPECT_EQ(p.priority, 3);
        ZX_EXPECT(p.overrideFStop);
        ZX_EXPECT(!p.overrideFocusDistance);
        ZX_EXPECT_NEAR(p.fStop, 1.4f, 1e-6f);
        ZX_EXPECT_NEAR(p.colorFilter.y, 0.5f, 1e-6f);
    });
    ZX_EXPECT_EQ(found, 1u);
}

ZX_TEST(PostProcess, CircleOfConfusion)
{
    const f32 fov = 2.0f * std::atan(12.0f / 50.0f); // 50 mm lens on a 24 mm sensor
    // In focus: 0. Nearer: negative. Farther: positive, growing towards infinity.
    ZX_EXPECT_NEAR(render::CircleOfConfusion(3.0f, 3.0f, 2.0f, fov, 1080.0f), 0.0f, 1e-4f);
    const f32 near = render::CircleOfConfusion(1.0f, 3.0f, 2.0f, fov, 1080.0f);
    const f32 far = render::CircleOfConfusion(10.0f, 3.0f, 2.0f, fov, 1080.0f);
    const f32 inf = render::CircleOfConfusion(1e6f, 3.0f, 2.0f, fov, 1080.0f);
    ZX_EXPECT_LT(near, 0.0f);
    ZX_EXPECT_GT(far, 0.0f);
    ZX_EXPECT_GT(inf, far);
    // Thin lens: 50 mm f/2 focused at 3 m, infinity -> 2500 / (2 * 2950) mm = 0.4237 mm = 19.07 px of 1080 on 24 mm.
    ZX_EXPECT_NEAR(inf, 19.07f, 0.05f);
    // Stopping down to f/8 quarters the blur.
    ZX_EXPECT_NEAR(render::CircleOfConfusion(1e6f, 3.0f, 8.0f, fov, 1080.0f), inf * 0.25f, 0.01f);
}
