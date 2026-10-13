#pragma once

// Full frame of one 3D view (scene, sky, fog, TAA, post) recorded into a render graph. Shared by the editor viewports
// and the samples.
//
//   ViewportRenderer viewport(device, renderAssets);
//   viewport.Init(lite, Format::R8G8B8A8_UNORM);
//   per frame:
//     ViewportCamera cam{camera.ViewMatrix(), camera.position, camera.fovY, camera.nearPlane};
//     viewport.AddPasses(graph, world, cam, environment, output, outputSize, dt);

#include "Engine/SceneRendering.h"
#include "Render/PostProcess.h"
#include "Render/RenderGraph.h"
#include "Render/SelectionOutline.h"
#include "Render/Sky.h"
#include "Render/TemporalAA.h"

namespace zx {

struct ViewportCamera
{
    Mat4 view = Mat4::Identity();
    Vec3 position;
    f32 fovY = DegToRad(60.0f);
    f32 nearPlane = 0.05f;
    bool cut = false; // the view jumped (camera switch): no motion blur / history this frame
};

/// Sun / sky / exposure of a scene (until these become scene components).
struct ViewportEnvironment
{
    bool night = false;
    f32 sunElevation = DegToRad(60.0f); // radians above the horizon
    f32 sunAzimuth = DegToRad(80.0f);   // radians, 0 = +Z, pi/2 = +X
    bool autoExposure = true;
};

/// Subsystems switched off for the project (Features window); applied on top of ViewportSettings each frame.
struct ViewportFeatures
{
    bool shadows = true;
    bool ambientOcclusion = true;
    bool sky = true; // physical sky + height fog
    bool bloom = true;
    bool temporalAA = true;
    bool water = true;
    bool postProcessVolumes = true;
    bool depthOfField = true;
    bool motionBlur = true;
};

struct ViewportSettings
{
    render::SceneRenderSettings scene;
    render::SkySettings sky;
    render::PostSettings post;
    render::TAASettings taa;
    bool useSky = true; // physical sky + fog
    f32 renderScale = 1.0f;
};

class ViewportRenderer
{
public:
    ViewportRenderer(rhi::Device& device, render::RenderAssets& assets);
    ~ViewportRenderer();
    ZX_NON_COPYABLE(ViewportRenderer)

    /// `lite` selects the low-end defaults (R11G11B10 HDR, 70 % render scale, half-res AO, cheaper shadows).
    bool Init(bool lite, rhi::Format outputFormat);

    /// Gathers instances/lights from `world` (WorldTransforms must be up to date) and records the whole frame into
    /// `output` (outputSize pixels, imported/created by the caller). Optional `overlay` runs in a pass bound to the
    /// HDR scene color + depth (render resolution) after the sky - editor grid, gizmo helpers.
    void AddPasses(render::RenderGraph& graph, ecs::World& world, const ViewportCamera& camera, const ViewportEnvironment& env,
                   render::RGTexture output, UVec2 outputSize, f32 deltaTime,
                   Function<void(rhi::CommandList&, const render::RenderView&, f32 exposure)> overlay = nullptr);

    ViewportSettings& Settings() { return m_settings; }
    ViewportFeatures features;
    render::SceneRenderer& Scene() { return m_scene; }
    const render::SceneLighting& Lighting() const { return m_lighting; }
    const render::RenderView& LastView() const { return m_view; }
    UVec2 RenderSize() const { return UVec2(m_view.width, m_view.height); }
    rhi::Format HdrFormat() const { return m_hdrFormat; }
    rhi::Format DepthFormat() const { return rhi::Format::D32_FLOAT; }
    /// Unjittered view-projection of the last frame (picking, gizmos).
    const Mat4& ViewProjection() const { return m_viewProjNoJitter; }
    /// Post-processing settings of the last frame after Post Process Volumes and feature toggles.
    const render::PostSettings& LastPostSettings() const { return m_lastPost; }
    void ResetHistory() { m_taa.Reset(); }

    /// Entities whose meshes get the selection outline (editor). Pass the selected entity and its descendants.
    void SetOutlinedEntities(Span<const ecs::Entity> entities) { m_outlined.Clear(); m_outlined.Append(entities); }
    render::OutlineSettings& Outline() { return m_outlineSettings; }

private:
    void ApplyEnvironment(const ViewportEnvironment& env);

    rhi::Device& m_device;
    render::RenderAssets& m_assets;
    render::SceneRenderer m_scene;
    render::SkyRenderer m_sky;
    render::WaterRenderer m_water;
    Array<render::WaterSurface> m_waterSurfaces;
    render::TemporalAA m_taa;
    render::PostProcess m_post;
    render::SelectionOutline m_outline;
    render::OutlineSettings m_outlineSettings;
    Array<ecs::Entity> m_outlined;
    Array<render::OutlineItem> m_outlineItems;
    f64 m_time = 0.0; // material graph time
    ViewportSettings m_settings;
    rhi::Format m_hdrFormat = rhi::Format::R16G16B16A16_FLOAT;
    render::SceneLighting m_lighting;
    Vec3 m_dirToSun;
    render::RenderView m_view;
    Mat4 m_viewProjNoJitter = Mat4::Identity();
    render::PostSettings m_lastPost;
    Array<render::RenderInstance> m_instances;
    Array<render::LightDesc> m_lights;
};

} // namespace zx
