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
#include "Render/Particles.h"
#include "Render/PostProcess.h"
#include "Render/Reflections.h"
#include "Render/RenderGraph.h"
#include "Render/SelectionOutline.h"
#include "Render/Sky.h"
#include "Render/TemporalAA.h"
#include "Render/VolumetricFog.h"

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
    bool playing = false; // Play-in-Editor / game: every Fluid Volume simulates
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
    bool reflections = true;            // sky cube + reflection probes
    bool screenSpaceReflections = true;
    bool decals = true;
    bool particles = true;
    bool volumetricFog = true;
    bool clouds = true;
    bool fluids = true;
};

/// Dynamic resolution: the render scale follows the GPU frame time towards a target (5 % steps, 4 times a second).
struct DynamicResolutionSettings
{
    bool enabled = false;
    f32 targetMs = 16.6f;
    f32 minScale = 0.5f;
    f32 maxScale = 1.0f;
};

/// Next render scale for a measured GPU frame time (pure: the step dynamic resolution takes 4 times a second).
f32 NextDynamicResolutionScale(f32 currentScale, f32 gpuFrameMs, const DynamicResolutionSettings& settings);

struct ViewportSettings
{
    render::SceneRenderSettings scene;
    render::SkySettings sky;
    render::PostSettings post;
    render::TAASettings taa;
    render::ReflectionSettings reflections;
    bool useSky = true; // physical sky + fog
    render::VolumetricFogSettings volumetricFog;
    f32 renderScale = 1.0f; // below 1 with TAA: ZTSR temporal upscaling to the output resolution
    DynamicResolutionSettings dynamicResolution;
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
    /// Feeds the measured GPU frame time to dynamic resolution (changes Settings().renderScale when enabled).
    void UpdateDynamicResolution(f32 gpuFrameMs, f32 deltaTime);
    /// Render scale actually used for the last frame (dynamic resolution, clamping).
    f32 LastRenderScale() const { return m_lastScale; }
    bool IsUpscaling() const { return m_taa.IsUpscaling(); }
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
    render::ReflectionRenderer& Reflections() { return m_reflections; }
    /// Content-relative texture path (+ normal map flag) -> texture asset, for decals and the HDRI Sky (the editor resolves
    /// it through the project's material library). Unset = solid-color decals, no HDRI sky.
    Function<asset::AssetId(StringView path, bool normalMap)> decalTextures;
    /// The HDRI sky of the last frame (inactive = physical sky) and whether its sun lights the scene.
    const render::HdriSkySettings& LastHdri() const { return m_sky.Hdri(); }
    bool HdriSunFound() const { return m_hdriHasSun; }

    /// Entities whose meshes get the selection outline (editor). Pass the selected entity and its descendants.
    void SetOutlinedEntities(Span<const ecs::Entity> entities) { m_outlined.Clear(); m_outlined.Append(entities); }
    /// Particles drawn (and flickering lights modulated) each frame; null = none.
    void SetParticleSystem(const class ParticleSystem* particles) { m_particleSource = particles; }
    /// Poses of skinned meshes (none: bind pose).
    void SetAnimationSystem(const class AnimationSystem* animation) { m_animation = animation; }
    render::OutlineSettings& Outline() { return m_outlineSettings; }

private:
    void ResolveHdriSky(ecs::World& world);
    void ApplyEnvironment(const ViewportEnvironment& env);

    rhi::Device& m_device;
    render::RenderAssets& m_assets;
    render::SceneRenderer m_scene;
    render::SkyRenderer m_sky;
    render::WaterRenderer m_water;
    render::ParticleRenderer m_particleRenderer;
    render::VolumetricFogRenderer m_volumetric;
    render::CloudRenderer m_clouds;
    render::FluidRenderer m_fluids;
    Array<render::FluidVolumeDesc> m_fluidVolumes;
    Array<render::FogVolumeDesc> m_fogVolumes;
    const class ParticleSystem* m_particleSource = nullptr;
    const class AnimationSystem* m_animation = nullptr;
    Array<render::GpuParticle> m_particleList;
    Array<render::GpuEmitter> m_gpuEmitters;
    render::GpuParticleFrame m_gpuParticleFrame;
    Array<render::WaterSurface> m_waterSurfaces;
    render::TemporalAA m_taa;
    render::ReflectionRenderer m_reflections;
    render::SceneRenderer m_captureScene; // reflection probe faces
    Array<render::ReflectionProbeDesc> m_probes;
    Array<render::DecalDesc> m_decals;
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
    f32 m_lastScale = 1.0f;
    // HDRI Sky of this frame (resolved before the environment: its sun is the directional light).
    asset::HdrInfo m_hdriInfo;
    HdriSunMode m_hdriSun = HdriSunMode::SceneSun;
    bool m_hdriHasSun = false;
    f32 m_dynamicTimer = 0.0f;
    Array<render::RenderInstance> m_instances;
    // Object motion: instance ids + world matrices of this and last frame, in gather order (RenderInstance::prevWorld
    // points into last frame's matrices).
    Array<u64> m_motionIds[2];
    Array<Mat4> m_motionWorlds[2];
    u32 m_motionCurrent = 0;
    Array<u32> m_movedIndices; // this frame's instances whose world matrix changed (shadow cache)                 // instance id -> last frame's world (object motion)
    Mat4 m_prevViewProjNoJitter = Mat4::Identity();
    bool m_hasPrevView = false;
    Array<render::LightDesc> m_lights;
};

} // namespace zx
