#include "Engine/ViewportRenderer.h"
#include "Core/Profiler/Profiler.h"
#include "Engine/PostProcessVolume.h"

namespace zx {

using namespace rhi;
using namespace render;

ViewportRenderer::ViewportRenderer(Device& device, RenderAssets& assets)
    : m_device(device), m_assets(assets), m_scene(device, assets), m_sky(device), m_water(device), m_taa(device), m_post(device), m_outline(device)
{
}

ViewportRenderer::~ViewportRenderer() = default;

bool ViewportRenderer::Init(bool lite, Format outputFormat)
{
    // Lite: 4-byte HDR target (the iGPU is memory-bandwidth bound).
    m_hdrFormat = lite ? Format::R11G11B10_FLOAT : Format::R16G16B16A16_FLOAT;
    ViewportSettings& s = m_settings;
    s.scene.lodBias = lite ? 0.75f : 1.0f;
    s.scene.maxAnisotropy = lite ? 4 : 16;
    if (lite) {
        s.scene.shadows.cascades = 3;
        s.scene.shadows.resolution = 1024;
        s.scene.shadows.distance = 50.0f;
        s.scene.shadows.filter = 1;
    } else {
        s.scene.shadows.cascades = 4;
        s.scene.shadows.resolution = 2048;
        s.scene.shadows.distance = 120.0f;
        s.scene.shadows.filter = 2;
    }
    s.scene.ao.halfResolution = lite;
    s.scene.ao.slices = lite ? 2 : 3;
    s.scene.ao.steps = lite ? 4 : 6;
    s.taa.catmullRom = !lite;
    s.post.bloomLevels = lite ? 4 : 6;
    s.post.maxBlur = lite ? 10.0f : 16.0f;
    s.post.dofQuality = lite ? 1.5f : 1.0f;
    s.post.motionBlurSamples = lite ? 6 : 10;
    s.renderScale = lite ? 0.7f : 1.0f;
    return m_scene.Init(m_hdrFormat, DepthFormat()) && m_sky.Init(m_hdrFormat, DepthFormat()) && m_post.Init(outputFormat) &&
           m_taa.Init() && m_outline.Init(outputFormat) && m_water.Init(m_hdrFormat, DepthFormat(), lite);
}

void ViewportRenderer::ApplyEnvironment(const ViewportEnvironment& env)
{
    SceneLighting& l = m_lighting;
    m_dirToSun = atmosphere::SunDirection(env.night ? DegToRad(-12.0f) : env.sunElevation, env.sunAzimuth);
    if (env.night) {
        l.sunDirection = Normalize(Vec3(-0.4f, -1.0f, -0.2f)); // moon
        l.sunColor = Vec3(0.6f, 0.7f, 1.0f);
        l.sunIlluminance = 0.4f;
        l.skyColor = Vec3(0.25f, 0.35f, 0.6f);
        l.skyLuminance = 0.25f;
        l.exposureEV100 = 5.0f;
    } else {
        SkyRenderer::ApplySun(l, m_dirToSun, m_settings.sky);
        l.skyColor = Vec3(0.45f, 0.6f, 0.9f);
        // Until IBL probes: ambient and exposure follow the sun height (bright noon .. dim dusk).
        const f32 day = Saturate(Sin(env.sunElevation) * 2.0f);
        l.skyLuminance = 300.0f + 13700.0f * day;
        l.exposureEV100 = 9.0f + 4.5f * day;
    }
    m_settings.post.autoExposure = env.autoExposure;
    if (env.autoExposure && m_post.HasAutoExposure())
        l.exposureEV100 = m_post.GetAutoExposureEV100();
}

void ViewportRenderer::AddPasses(RenderGraph& graph, ecs::World& world, const ViewportCamera& camera, const ViewportEnvironment& env,
                                 RGTexture output, UVec2 outputSize, f32 deltaTime,
                                 Function<void(CommandList&, const RenderView&, f32)> overlay)
{
    ZX_PROFILE_SCOPE("ViewportRenderer");
    // Feature toggles override the settings for this frame only (the settings stay as the user/CVars set them).
    const ViewportSettings userSettings = m_settings;
    m_settings.scene.shadows.enabled &= features.shadows;
    m_settings.scene.shadows.localEnabled &= features.shadows;
    m_settings.scene.ao.enabled &= features.ambientOcclusion;
    m_settings.useSky &= features.sky;
    m_settings.post.bloom &= features.bloom;
    m_settings.taa.enabled &= features.temporalAA;
    struct Restore
    {
        ViewportSettings& target;
        const ViewportSettings& saved;
        ~Restore() { target = saved; }
    } restore{m_settings, userSettings};
    // Post Process Volumes around the camera, then the feature toggles on top.
    if (features.postProcessVolumes)
        m_settings.post = EvaluatePostProcessVolumes(world, camera.position, m_settings.post);
    m_settings.post.depthOfField &= features.depthOfField;
    m_settings.post.motionBlur &= features.motionBlur;
    if (camera.cut)
        m_taa.Reset();
    ApplyEnvironment(env);
    m_lights.Clear();
    GatherLights(world, m_lights);
    m_lighting.lights = m_lights.View();

    const f32 scale = Clamp(m_settings.renderScale, 0.25f, 2.0f);
    const UVec2 renderSize(Max(1u, u32(f32(outputSize.x) * scale)), Max(1u, u32(f32(outputSize.y) * scale)));
    RenderView& view = m_view;
    view.view = camera.view;
    const Mat4 projection = Mat4::PerspectiveInfiniteReversedZ(camera.fovY, f32(renderSize.x) / f32(renderSize.y), camera.nearPlane);
    m_viewProjNoJitter = projection * camera.view;
    view.projection = TemporalAA::ApplyJitter(projection, m_taa.NextJitter(renderSize.x, renderSize.y, m_settings.taa));
    view.viewProj = view.projection * view.view;
    view.position = camera.position;
    view.fovY = camera.fovY;
    view.nearPlane = camera.nearPlane;
    view.width = renderSize.x;
    view.height = renderSize.y;

    m_instances.Clear();
    GatherRenderInstances(world, m_instances);
    const f32 exposure = ExposureFromEV100(m_lighting.exposureEV100);
    if (m_settings.useSky)
        m_sky.AddPrePasses(graph, view, m_dirToSun, exposure, m_settings.sky, m_lighting);
    m_time += f64(deltaTime);
    m_scene.SetTime(f32(std::fmod(m_time, 3600.0))); // wraps hourly: keeps float precision for animated materials
    m_scene.Prepare(m_instances.View(), view, m_lighting, m_settings.scene);

    TextureDesc hdrDesc;
    hdrDesc.width = renderSize.x;
    hdrDesc.height = renderSize.y;
    hdrDesc.format = m_hdrFormat;
    hdrDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    hdrDesc.debugName = "SceneColor";
    const RGTexture sceneColor = graph.CreateTexture(hdrDesc);
    TextureDesc depthDesc = hdrDesc;
    depthDesc.format = DepthFormat();
    depthDesc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
    depthDesc.debugName = "SceneDepth";
    const RGTexture sceneDepth = graph.CreateTexture(depthDesc);
    m_scene.AddPasses(graph, sceneColor, sceneDepth, LinearColor(0, 0, 0, 1));
    if (m_settings.useSky)
        m_sky.AddSkyPass(graph, sceneColor, sceneDepth);
    // Water after the sky (its refraction sees the sky) and before the fog (which then covers it too).
    m_waterSurfaces.Clear();
    if (features.water)
        GatherWater(world, m_waterSurfaces);
    m_water.AddPasses(graph, sceneColor, sceneDepth, view, m_waterSurfaces.View(), m_lighting, m_settings.useSky ? &m_sky : nullptr,
                      f32(std::fmod(m_time, 3600.0)), exposure);
    if (m_settings.useSky)
        m_sky.AddFogPass(graph, sceneColor, sceneDepth, m_lighting, m_settings.sky);
    if (overlay) {
        graph.AddPass(
            "Overlay", PassType::Graphics,
            [&](PassBuilder& b) {
                b.RenderTarget(sceneColor, LoadOp::Load);
                b.DepthStencil(sceneDepth, LoadOp::Load, 0.0f, true);
            },
            [overlay = move(overlay), view, exposure](PassContext& ctx) mutable {
                ctx.cmd.SetViewport(Viewport{0, 0, f32(view.width), f32(view.height), 0, 1});
                ctx.cmd.SetScissor(0, 0, view.width, view.height);
                overlay(ctx.cmd, view, exposure);
            });
    }
    m_settings.post.fxaa = !m_settings.taa.enabled;
    const RGTexture resolved = m_settings.taa.enabled
                                   ? m_taa.AddPass(graph, sceneColor, sceneDepth, m_viewProjNoJitter, exposure, m_settings.taa)
                                   : sceneColor;
    PostView postView;
    postView.viewProj = m_viewProjNoJitter;
    postView.fovY = camera.fovY;
    postView.nearPlane = camera.nearPlane;
    postView.cut = camera.cut;
    m_post.AddPasses(graph, resolved, sceneDepth, output, m_lighting.exposureEV100, deltaTime, m_settings.post, &postView);
    m_lastPost = m_settings.post;

    m_outlineItems.Clear();
    for (ecs::Entity e : m_outlined) {
        if (!world.IsAlive(e))
            continue;
        const MeshRenderer* mr = world.Get<const MeshRenderer>(e);
        const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(e);
        if (mr && wt && mr->visible)
            if (const GpuMesh* mesh = m_assets.GetMesh(mr->mesh))
                m_outlineItems.PushBack(OutlineItem{mesh, wt->matrix});
    }
    if (!m_outlineItems.Empty())
        m_outline.AddPasses(graph, m_outlineItems.View(), m_viewProjNoJitter, camera.nearPlane, sceneDepth, renderSize, output, outputSize,
                            m_outlineSettings);
}

} // namespace zx
