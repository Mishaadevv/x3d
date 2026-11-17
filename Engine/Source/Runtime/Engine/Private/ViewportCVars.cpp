// Render console variables (r.*) of a ViewportRenderer: the editor binds them to its viewport, a packaged game
// (ZXGame) to its own, so the console, the command line and .zproj startupCommands work in both.

#include "Engine/ViewportCVars.h"

#include "Core/Console/Console.h"
#include "Engine/ViewportRenderer.h"

namespace zx {

namespace {

CVar<f32> CVarScreenPercentage("r.ScreenPercentage", 100.0f, "Viewport render resolution in percent (25..200)");
CVar<bool> CVarTAA("r.TAA", true, "Temporal anti-aliasing (FXAA when off)");
CVar<bool> CVarAO("r.AO", true, "Ground-truth ambient occlusion");
CVar<bool> CVarShadows("r.Shadows", true, "Sun cascaded shadows");
CVar<bool> CVarSky("r.Sky", true, "Physical sky and height fog");
CVar<bool> CVarBloom("r.Bloom", true, "Bloom");
CVar<bool> CVarTSR("r.TSR", true, "ZTSR temporal upscaling when r.ScreenPercentage is below 100");
CVar<bool> CVarDynRes("r.DynamicResolution", false, "Render scale follows the GPU frame time (r.DynamicResolution.TargetMs)");
CVar<f32> CVarDynResTarget("r.DynamicResolution.TargetMs", 16.6f, "Dynamic resolution target GPU frame time (ms)");
CVar<f32> CVarDynResMin("r.DynamicResolution.MinPercentage", 50.0f, "Lowest render scale of dynamic resolution (percent)");
CVar<bool> CVarSSR("r.SSR", false, "Screen-space reflections (on by default in the Full profile)");
CVar<bool> CVarGpuDriven("r.GPUDriven", true, "GPU culling + ExecuteIndirect for opaque geometry (CPU draw lists when off)");
CVar<bool> CVarVsm("r.Shadows.Virtual", false, "Virtual shadow maps for the sun: sharp shadows near and far, pages cached until something moves");
CVar<f32> CVarVsmNormalBias("r.Shadows.Virtual.NormalBias", 1.5f, "Virtual shadow receiver offset along the normal, texels");
CVar<f32> CVarVsmDistance("r.Shadows.Virtual.Distance", 250.0f, "Reach of the virtual shadow maps around the camera, meters (a Directional Light's Shadow Distance wins)");
CVar<f32> CVarVsmResolution("r.Shadows.Virtual.Resolution", 1.0f, "Virtual shadow texels per pixel footprint (< 1 coarser, Lite 0.5)");
CVar<f32> CVarNanoShadowError("r.Nano.ShadowPixelError", 2.0f, "Virtual geometry LOD error allowed in shadow maps, texels");
CVar<bool> CVarNano("r.Nano", true, "Virtual geometry: meshes cooked with clusters are drawn cluster by cluster (fallback LODs when off)");
CVar<f32> CVarNanoPixelError("r.Nano.PixelError", 1.0f, "Virtual geometry LOD error allowed on screen, pixels (Lite: 2)");
CVar<i32> CVarNanoBudget("r.Nano.TriangleBudget", 3000, "Virtual geometry triangle budget of the main view, thousands (over it detail drops)");
CVar<bool> CVarOcclusion("r.OcclusionCulling", true, "Two-phase HiZ occlusion culling (GPU-driven path)");
CVar<i32> CVarShadowFilter("r.ShadowFilter", 1, "Sun shadow filter: 0 hard, 1 4 taps, 2 9 taps, 3 PCSS (soft, contact-hardening)");
CVar<bool> CVarContactShadows("r.ContactShadows", true, "Screen-space contact shadows of the sun");
CVar<bool> CVarContactSkip("r.ContactShadows.SkipShadowed", true, "Contact shadows skip pixels the sun's cascades already shadow");
CVar<i32> CVarRendererPath("r.Renderer.Path", 0, "Opaque rendering path: 0 forward+ (clustered), 1 deferred (G-buffer + lighting pass)");
CVar<i32> CVarGIMode("r.GI.Mode", 0, "0 dynamic probes (traced every frame), 1 baked probes (gi.bake; no GPU time; dynamic when the scene has none)");
CVar<bool> CVarGIScreen("r.GI.ScreenSpace", false, "Screen-space GI: short-range bounce from what is on screen (Full profile; needs the deferred normals)");
CVar<bool> CVarGILumen("r.GI.Lumen", false, "Lumen-lite prototype: per-pixel GI rays (screen, then the distance field) over the probes - high-end GPUs (deferred path)");
CVar<bool> CVarGI("r.GI", true, "Dynamic global illumination: light bounces off surfaces (probes traced through a distance field of the scene)");
CVar<f32> CVarGIIntensity("r.GI.Intensity", 1.0f, "Multiplies the bounced (indirect) diffuse light");
CVar<i32> CVarGIRays("r.GI.Rays", 64, "Rays per GI probe update (16..256)");
CVar<i32> CVarGIStride("r.GI.Stride", 2, "One of every N GI probes is updated per frame (1 = all: fastest response, most GPU time)");
CVar<f32> CVarGISpacing("r.GI.Spacing", 1.4f, "Meters between GI probes");
CVar<bool> CVarProbes("r.ReflectionProbes", true, "Reflection probes and the sky reflection cube");
CVar<f32> CVarExposure("r.ExposureCompensation", 1.0f, "Exposure compensation in EV (+ = brighter)");
CVar<i32> CVarViewMode("r.ViewMode", 0, "0 lit, 1 base color, 2 normals, 3 roughness/metallic, 4 LODs, 5 cascades, 6 clusters, 7 AO, 8 local shadows, 9 indirect light (GI), 10 lightmap UV density");
CVar<i32> CVarForceLod("r.ForceLOD", -1, "Force a mesh LOD (-1 = automatic)");

ViewportRenderer* g_viewport = nullptr;

/// Initializes the CVar from the current setting, then forwards console changes to `apply`. A value set with a higher
/// priority before (e.g. +r.TAA=0 on the command line, a project's startupCommands) wins and is applied right away.
template <typename T>
void Bind(CVar<T>& cvar, T current, void (*apply)(T))
{
    cvar.SetOnChanged(nullptr);
    if (!cvar.Set(current, CVarSetBy::Code))
        apply(cvar.Get());
    cvar.SetOnChanged([&cvar, apply](CVarBase&) {
        if (g_viewport)
            apply(cvar.Get());
    });
}

} // namespace

void BindViewportCVars(ViewportRenderer* viewport)
{
    g_viewport = viewport;
    if (!viewport) {
        for (CVarBase* c : {static_cast<CVarBase*>(&CVarScreenPercentage), static_cast<CVarBase*>(&CVarTAA), static_cast<CVarBase*>(&CVarAO), static_cast<CVarBase*>(&CVarShadows), static_cast<CVarBase*>(&CVarSky), static_cast<CVarBase*>(&CVarBloom), static_cast<CVarBase*>(&CVarTSR), static_cast<CVarBase*>(&CVarDynRes), static_cast<CVarBase*>(&CVarDynResTarget), static_cast<CVarBase*>(&CVarDynResMin), static_cast<CVarBase*>(&CVarSSR), static_cast<CVarBase*>(&CVarGpuDriven), static_cast<CVarBase*>(&CVarVsm), static_cast<CVarBase*>(&CVarVsmNormalBias), static_cast<CVarBase*>(&CVarVsmDistance), static_cast<CVarBase*>(&CVarVsmResolution), static_cast<CVarBase*>(&CVarNanoShadowError), static_cast<CVarBase*>(&CVarNano), static_cast<CVarBase*>(&CVarNanoPixelError), static_cast<CVarBase*>(&CVarNanoBudget), static_cast<CVarBase*>(&CVarOcclusion), static_cast<CVarBase*>(&CVarShadowFilter), static_cast<CVarBase*>(&CVarContactShadows), static_cast<CVarBase*>(&CVarContactSkip), static_cast<CVarBase*>(&CVarRendererPath), static_cast<CVarBase*>(&CVarGIMode), static_cast<CVarBase*>(&CVarGIScreen), static_cast<CVarBase*>(&CVarGILumen), static_cast<CVarBase*>(&CVarGI), static_cast<CVarBase*>(&CVarGIIntensity), static_cast<CVarBase*>(&CVarGIRays), static_cast<CVarBase*>(&CVarGIStride), static_cast<CVarBase*>(&CVarGISpacing), static_cast<CVarBase*>(&CVarProbes), static_cast<CVarBase*>(&CVarExposure), static_cast<CVarBase*>(&CVarViewMode), static_cast<CVarBase*>(&CVarForceLod)})
            c->SetOnChanged(nullptr);
        return;
    }
    ViewportSettings& s = viewport->Settings();
    Bind<f32>(CVarScreenPercentage, s.renderScale * 100.0f, [](f32 v) {
        g_viewport->Settings().renderScale = Clamp(v, 25.0f, 200.0f) / 100.0f;
        g_viewport->ResetHistory();
    });
    Bind<bool>(CVarTAA, s.taa.enabled, [](bool v) {
        g_viewport->Settings().taa.enabled = v;
        g_viewport->ResetHistory();
    });
    Bind<bool>(CVarAO, s.scene.ao.enabled, [](bool v) { g_viewport->Settings().scene.ao.enabled = v; });
    Bind<bool>(CVarShadows, s.scene.shadows.enabled, [](bool v) { g_viewport->Settings().scene.shadows.enabled = v; });
    Bind<bool>(CVarSky, s.useSky, [](bool v) { g_viewport->Settings().useSky = v; });
    Bind<bool>(CVarBloom, s.post.bloom, [](bool v) { g_viewport->Settings().post.bloom = v; });
    Bind<bool>(CVarTSR, s.taa.upscale, [](bool v) {
        g_viewport->Settings().taa.upscale = v;
        g_viewport->ResetHistory();
    });
    Bind<bool>(CVarDynRes, s.dynamicResolution.enabled, [](bool v) { g_viewport->Settings().dynamicResolution.enabled = v; });
    Bind<f32>(CVarDynResTarget, s.dynamicResolution.targetMs,
              [](f32 v) { g_viewport->Settings().dynamicResolution.targetMs = Clamp(v, 4.0f, 100.0f); });
    Bind<f32>(CVarDynResMin, s.dynamicResolution.minScale * 100.0f,
              [](f32 v) { g_viewport->Settings().dynamicResolution.minScale = Clamp(v, 25.0f, 100.0f) / 100.0f; });
    Bind<bool>(CVarSSR, s.reflections.ssr, [](bool v) { g_viewport->Settings().reflections.ssr = v; });
    Bind<bool>(CVarGpuDriven, s.scene.gpuDriven, [](bool v) { g_viewport->Settings().scene.gpuDriven = v; });
    Bind<bool>(CVarVsm, s.scene.shadows.virtualShadows, [](bool v) { g_viewport->Settings().scene.shadows.virtualShadows = v; });
    Bind<f32>(CVarVsmNormalBias, s.scene.shadows.vsmNormalBias, [](f32 v) { g_viewport->Settings().scene.shadows.vsmNormalBias = Clamp(v, 0.0f, 16.0f); });
    Bind<f32>(CVarVsmDistance, s.scene.shadows.vsmDistance, [](f32 v) { g_viewport->Settings().scene.shadows.vsmDistance = Clamp(v, 5.0f, 2000.0f); });
    Bind<f32>(CVarVsmResolution, s.scene.shadows.vsmResolution, [](f32 v) { g_viewport->Settings().scene.shadows.vsmResolution = Clamp(v, 0.125f, 4.0f); });
    Bind<f32>(CVarNanoShadowError, s.scene.nano.shadowPixelError, [](f32 v) { g_viewport->Settings().scene.nano.shadowPixelError = Clamp(v, 0.25f, 32.0f); });
    Bind<bool>(CVarNano, s.scene.nano.enabled, [](bool v) { g_viewport->Settings().scene.nano.enabled = v; });
    Bind<f32>(CVarNanoPixelError, s.scene.nano.pixelError,
              [](f32 v) { g_viewport->Settings().scene.nano.pixelError = Clamp(v, 0.25f, 32.0f); });
    Bind<i32>(CVarNanoBudget, i32(s.scene.nano.triangleBudget / 1000),
              [](i32 v) { g_viewport->Settings().scene.nano.triangleBudget = u32(Clamp(v, 100, 20000)) * 1000u; });
    Bind<bool>(CVarOcclusion, s.scene.occlusionCulling, [](bool v) { g_viewport->Settings().scene.occlusionCulling = v; });
    Bind<i32>(CVarShadowFilter, i32(s.scene.shadows.filter), [](i32 v) { g_viewport->Settings().scene.shadows.filter = u32(Clamp(v, 0, 3)); });
    Bind<bool>(CVarContactShadows, s.scene.contactShadows.enabled, [](bool v) { g_viewport->Settings().scene.contactShadows.enabled = v; });
    Bind<bool>(CVarContactSkip, s.scene.contactShadows.skipShadowed, [](bool v) { g_viewport->Settings().scene.contactShadows.skipShadowed = v; });
    Bind<i32>(CVarRendererPath, s.scene.deferred ? 1 : 0, [](i32 v) { g_viewport->Settings().scene.deferred = v == 1; });
    Bind<i32>(CVarGIMode, i32(s.gi.mode), [](i32 v) { g_viewport->Settings().gi.mode = v == 1 ? render::GIMode::Baked : render::GIMode::Dynamic; });
    Bind<bool>(CVarGIScreen, s.gi.screenSpace, [](bool v) { g_viewport->Settings().gi.screenSpace = v; });
    Bind<bool>(CVarGILumen, s.gi.lumen, [](bool v) { g_viewport->Settings().gi.lumen = v; });
    Bind<bool>(CVarGI, s.gi.enabled, [](bool v) { g_viewport->Settings().gi.enabled = v; });
    Bind<f32>(CVarGIIntensity, s.gi.intensity, [](f32 v) { g_viewport->Settings().gi.intensity = Clamp(v, 0.0f, 10.0f); });
    Bind<i32>(CVarGIRays, i32(s.gi.raysPerProbe), [](i32 v) { g_viewport->Settings().gi.raysPerProbe = u32(Clamp(v, 16, 256)); });
    Bind<i32>(CVarGIStride, i32(s.gi.probeStride), [](i32 v) { g_viewport->Settings().gi.probeStride = u32(Clamp(v, 1, 16)); });
    Bind<f32>(CVarGISpacing, s.gi.probeSpacing, [](f32 v) { g_viewport->Settings().gi.probeSpacing = Clamp(v, 0.25f, 16.0f); });
    Bind<bool>(CVarProbes, s.reflections.probes, [](bool v) {
        g_viewport->Settings().reflections.probes = v;
        g_viewport->Settings().reflections.sky = v;
    });
    Bind<f32>(CVarExposure, s.post.exposureCompensation, [](f32 v) { g_viewport->Settings().post.exposureCompensation = v; });
    Bind<i32>(CVarViewMode, i32(s.scene.debugMode), [](i32 v) { g_viewport->Settings().scene.debugMode = u32(Clamp(v, 0, 10)); });
    Bind<i32>(CVarForceLod, s.scene.forcedLod, [](i32 v) { g_viewport->Settings().scene.forcedLod = v; });
}

} // namespace zx
