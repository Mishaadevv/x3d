#include "Engine/TimeOfDay.h"
#include "Engine/ViewportRenderer.h"
#include "Engine/WaterPhysics.h"
#include "Engine/Wind.h"
#include "Engine/Particles.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Profiler/Profiler.h"
#include "Engine/PostProcessVolume.h"

namespace zx {

using namespace rhi;
using namespace render;

ViewportRenderer::ViewportRenderer(Device& device, RenderAssets& assets)
    : m_device(device), m_assets(assets), m_scene(device, assets), m_sky(device), m_water(device), m_particleRenderer(device), m_volumetric(device), m_gi(device, assets), m_clouds(device), m_fluids(device), m_taa(device), m_reflections(device), m_captureScene(device, assets), m_post(device), m_outline(device)
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
    s.scene.nano.pixelError = lite ? 2.0f : 1.0f;
    s.scene.nano.triangleBudget = lite ? 1500000u : 3000000u;
    // Virtual shadow maps are a High-end feature: on by default in Full; Lite (cvar) uses coarser texels and fewer pages.
    s.scene.shadows.virtualShadows = !lite;
    s.scene.shadows.vsmResolution = lite ? 0.5f : 1.0f;
    s.scene.shadows.vsmPagesPerFrame = lite ? 24u : 48u;
    if (lite) {
        s.scene.shadows.cascades = 3;
        s.scene.shadows.resolution = 1024;
        s.scene.shadows.distance = 50.0f;
        s.scene.shadows.filter = 1;
    } else {
        s.scene.shadows.cascades = 4;
        s.scene.shadows.resolution = 2048;
        s.scene.shadows.distance = 120.0f;
        s.scene.shadows.filter = 3; // PCSS
    }
    s.scene.ao.halfResolution = lite;
    s.scene.contactShadows.steps = lite ? 6u : 10u;
    s.scene.deferred = !lite; // Medium+ tiers: G-buffer + one lighting pass (TZ 6.4.5)
    s.scene.ao.slices = lite ? 2 : 3;
    s.scene.ao.steps = lite ? 4 : 6;
    s.taa.catmullRom = !lite;
    s.post.bloomLevels = lite ? 4 : 6;
    s.post.maxBlur = lite ? 10.0f : 16.0f;
    s.post.dofQuality = lite ? 1.5f : 1.0f;
    s.post.motionBlurSamples = lite ? 6 : 10;
    s.renderScale = lite ? 0.7f : 1.0f;
    s.reflections.probeResolution = lite ? 64 : 128;
    s.reflections.maxProbes = lite ? 8 : 16;
    s.reflections.ssr = !lite; // Lite: probes + sky only (the iGPU is bandwidth bound)
    s.volumetricFog.tileSize = lite ? 16u : 10u;
    // Dynamic GI: Lite traces fewer probes per frame (the light settles over a few more frames).
    s.gi.probeCounts = lite ? UVec3(20, 10, 20) : UVec3(28, 12, 28);
    s.gi.probeSpacing = lite ? 1.6f : 1.4f;
    s.gi.raysPerProbe = 64;
    s.gi.probeStride = lite ? 8u : 2u;
    s.gi.mode = lite ? render::GIMode::Baked : render::GIMode::Dynamic; // Baked: when the scene has a bake
    s.gi.screenSpace = !lite; // needs the deferred G-buffer normals (Full)
    s.volumetricFog.slices = lite ? 48u : 64u;
    return m_scene.Init(m_hdrFormat, DepthFormat()) && m_captureScene.Init(m_hdrFormat, DepthFormat()) && m_reflections.Init() && m_gi.Init() && m_sky.Init(m_hdrFormat, DepthFormat()) && m_post.Init(outputFormat) &&
           m_taa.Init() && m_outline.Init(outputFormat) && m_water.Init(m_hdrFormat, DepthFormat(), lite) &&
           m_particleRenderer.Init(m_hdrFormat) && m_volumetric.Init(m_hdrFormat) &&
           m_clouds.Init(m_hdrFormat, lite) && m_fluids.Init(m_hdrFormat);
}

void ViewportRenderer::ResolveHdriSky(ecs::World& world)
{
    render::HdriSkySettings& h = m_settings.sky.hdri;
    h = render::HdriSkySettings();
    m_hdriInfo = asset::HdrInfo();
    m_hdriHasSun = false;
    const HdriSky* sky = m_settings.useSky && decalTextures ? FindHdriSky(world) : nullptr;
    if (!sky || sky->texture.Empty())
        return;
    const asset::AssetId id = decalTextures(sky->texture, false);
    Texture* texture = m_assets.GetTexture(id);
    if (!texture)
        return; // loading: the physical sky until the panorama is resident
    if (const asset::HdrInfo* info = m_assets.GetTextureHdrInfo(id))
        m_hdriInfo = *info;
    h.texture = texture->GetSrvIndex();
    h.width = texture->GetDesc().width;
    h.intensity = Max(sky->intensity, 0.0f);
    h.rotation = DegToRad(sky->rotation);
    m_hdriSun = sky->sun;
    m_hdriHasSun = m_hdriInfo.hasSun && sky->sun == HdriSunMode::FromHDRI;
    if (m_hdriHasSun) {
        // The sun leaves the sky lighting and becomes the directional light.
        h.clampDirection = HdriToWorld(m_hdriInfo.sunDirection, h.rotation);
        h.clampCos = m_hdriInfo.sunCosRadius;
        h.clampLuminance = m_hdriInfo.sunClamp;
    }
    u64 key = 0x48445249u;
    for (u64 v : {u64(h.texture), u64(h.width), u64(std::bit_cast<u32>(h.intensity)), u64(std::bit_cast<u32>(h.rotation)), u64(sky->sun)})
        key = key * 0x100000001B3ull ^ v;
    h.key = key;
}

void ViewportRenderer::ApplyEnvironment(const ViewportEnvironment& env)
{
    SceneLighting& l = m_lighting;
    m_dirToSun = atmosphere::SunDirection(env.night && !env.timeOfDay ? DegToRad(-12.0f) : env.sunElevation, env.sunAzimuth);
    l.ambientFloor = 0.0f;
    m_settings.sky.moonIlluminance = 0.0f;
    m_settings.sky.starLuminance = 0.0f;
    m_settings.sky.nightLuminance = 0.0f;
    if (env.timeOfDay) {
        // Day -> dusk -> night: the sun's light fades through the atmosphere by itself; ambient light and exposure
        // follow the sun's height down to a moonlit night, where the moon becomes the directional light.
        const f32 sinSun = Sin(env.sunElevation);
        // The atmosphere tables stop at the horizon: after sunset the sky's own glow fades out (about 7 % with the
        // sun 5 degrees down, nothing left by astronomical night).
        m_settings.sky.sunIlluminanceTop *= std::exp(Min(sinSun, 0.0f) * 30.0f);
        SkyRenderer::ApplySun(l, m_dirToSun, m_settings.sky);
        l.sunIlluminance *= Saturate((sinSun + 0.015f) / 0.03f); // the disk sets below the horizon
        const f32 day = Saturate(sinSun * 2.0f);
        const f32 twilight = Saturate((sinSun + 0.12f) / 0.12f); // 1 with the sun up, 0 about 7 degrees below
        const f32 nightAmbient = 0.4f + 1.5f * env.moonLight;
        const f32 dayAmbient = 300.0f + 13700.0f * day;
        l.skyLuminance = nightAmbient * Pow(dayAmbient / nightAmbient, twilight);
        l.skyColor = Lerp(Vec3(0.25f, 0.35f, 0.6f), Vec3(0.45f, 0.6f, 0.9f), twilight);
        l.exposureEV100 = Lerp(1.0f, 9.0f + 4.5f * day, twilight);
        l.ambientFloor = 1.0f - twilight;
        // Auto exposure must not turn the night into day: its darkest setting rises as the sun goes down.
        m_settings.post.minEV100 = Max(m_settings.post.minEV100, Lerp(0.5f, m_settings.post.minEV100, twilight));
        if (twilight < 0.5f && env.moonLight > 0.0f) {
            l.sunDirection = -env.toMoon;
            l.sunColor = Vec3(0.6f, 0.7f, 1.0f);
            l.sunIlluminance = env.moonLight * Saturate(1.0f - twilight * 2.0f);
        }
        m_settings.sky.moonDirection = env.toMoon;
        m_settings.sky.moonIlluminance = env.toMoon.y > -0.02f ? Max(env.moonLight, 0.05f) : 0.0f;
        m_settings.sky.starLuminance = 40.0f * env.starBrightness * Saturate(1.0f - twilight * 1.5f);
        m_settings.sky.nightLuminance = 0.03f * (1.0f - twilight);
    } else if (env.night) {
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
    const render::HdriSkySettings& hdri = m_settings.sky.hdri;
    if (hdri.IsActive()) {
        // Ambient and manual exposure from the panorama's average; the directional light per the sun mode.
        auto luminance = [](Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; };
        const f32 average = m_hdriInfo.valid ? luminance(m_hdriInfo.average) : 1.0f;
        l.skyLuminance = average * hdri.intensity;
        l.skyColor = m_hdriInfo.valid && average > 0.0f ? m_hdriInfo.average / average : Vec3(1, 1, 1);
        l.exposureEV100 = Log2(Max(l.skyLuminance, 1e-4f) * 8.0f); // EV100 = log2(L * 100 / 12.5)
        if (m_hdriHasSun) {
            m_dirToSun = HdriToWorld(m_hdriInfo.sunDirection, hdri.rotation);
            const f32 sun = luminance(m_hdriInfo.sunIlluminance);
            l.sunDirection = -m_dirToSun;
            l.sunIlluminance = sun * hdri.intensity;
            l.sunColor = sun > 0.0f ? m_hdriInfo.sunIlluminance / sun : Vec3(1, 1, 1);
        } else if (m_hdriSun != HdriSunMode::SceneSun) {
            l.sunIlluminance = 0.0f; // None, or no distinct sun in the panorama
        }
    }
    m_settings.post.autoExposure = env.autoExposure;
    if (env.autoExposure && m_post.HasAutoExposure())
        l.exposureEV100 = m_post.GetAutoExposureEV100();
}

StringView GraphicsEraId(GraphicsEra era)
{
    switch (era) {
    case GraphicsEra::Y2017: return "2017";
    case GraphicsEra::Y2011: return "2011";
    case GraphicsEra::Y2008: return "2008";
    default: return "modern";
    }
}

GraphicsEra ParseGraphicsEra(StringView id)
{
    if (id == "2017")
        return GraphicsEra::Y2017;
    if (id == "2011")
        return GraphicsEra::Y2011;
    if (id == "2008")
        return GraphicsEra::Y2008;
    return GraphicsEra::Modern;
}

void ApplyGraphicsEra(ViewportSettings& s, ViewportFeatures& f, GraphicsEra era)
{
    if (era == GraphicsEra::Modern)
        return;
    render::ShadowSettings& sh = s.scene.shadows;
    f.fluids = false; // real-time GPU fluid simulation came later
    if (era != GraphicsEra::Y2017)
        f.globalIllumination = false; // real-time bounced light: precomputed / probe GI from the mid 2010s
    sh.filter = Min(sh.filter, 2u); // contact-hardening (PCSS) shadows: late 2010s
    if (era == GraphicsEra::Y2017) {
        // Battlefield 1, Horizon Zero Dawn, UE 4.18: all of it, a little coarser.
        sh.resolution = Min(sh.resolution, 2048u);
        sh.cascades = Min(sh.cascades, 3u);
        s.volumetricFog.slices = Min(s.volumetricFog.slices, 48u);
        s.post.tonemapper = render::Tonemapper::ACES;
        return;
    }
    // 2011 and older: no temporal anti-aliasing (FXAA, 2009-), no screen-space reflections, no volumetric lighting or
    // 3D clouds (skydomes), no contact shadows, filmic tonemapping (Uncharted 2), native resolution.
    f.temporalAA = false;
    f.screenSpaceReflections = false;
    f.volumetricFog = false;
    f.clouds = false;
    s.reflections.ssr = false;
    s.scene.contactShadows.enabled = false;
    s.post.tonemapper = render::Tonemapper::Filmic;
    s.post.fxaa = true;
    s.renderScale = 1.0f;
    s.dynamicResolution.enabled = false;
    s.scene.ao.halfResolution = true;
    if (era == GraphicsEra::Y2011) {
        // Crysis 2 (DX11), Battlefield 3, Skyrim: deferred, SSAO, three PCF cascades, bokeh depth of field.
        sh.cascades = Min(sh.cascades, 3u);
        sh.resolution = Min(sh.resolution, 1024u);
        sh.distance = Min(sh.distance, 80.0f);
        sh.filter = Min(sh.filter, 1u);
        s.scene.ao.slices = Min(s.scene.ao.slices, 2u);
        s.scene.ao.steps = Min(s.scene.ao.steps, 4u);
        s.reflections.probeResolution = Min(s.reflections.probeResolution, 64u);
        s.post.bloomIntensity = Max(s.post.bloomIntensity, 0.06f);
        return;
    }
    // 2008: Crysis (DX10, Very High), GTA IV, Fallout 3: forward shading, no anti-aliasing, the first SSAO (noisy,
    // short), two cascades of hard shadows, sky reflections, strong bloom.
    f.reflections = true;
    s.scene.deferred = false;
    s.post.fxaa = false;
    sh.cascades = Min(sh.cascades, 2u);
    sh.resolution = Min(sh.resolution, 1024u);
    sh.distance = Min(sh.distance, 50.0f);
    sh.filter = 0;
    s.scene.ao.slices = 1;
    s.scene.ao.steps = Min(s.scene.ao.steps, 3u);
    s.scene.ao.radius = Min(s.scene.ao.radius, 0.8f);
    s.reflections.probes = false;
    s.scene.maxAnisotropy = Min(s.scene.maxAnisotropy, 4u);
    s.scene.lodBias = Min(s.scene.lodBias, 0.7f);
    s.post.bloomIntensity = Max(s.post.bloomIntensity, 0.12f);
    s.post.saturation = Min(s.post.saturation, 0.95f);
}

void ViewportRenderer::AddPasses(RenderGraph& graph, ecs::World& world, const ViewportCamera& camera, const ViewportEnvironment& env,
                                 RGTexture output, UVec2 outputSize, f32 deltaTime,
                                 Function<void(CommandList&, const RenderView&, f32)> overlay)
{
    if (graphicsEra != GraphicsEra::Modern) {
        // The era's limits for this frame only (the profile's settings stay as they are for the next).
        const ViewportSettings savedSettings = m_settings;
        const ViewportFeatures savedFeatures = features;
        ApplyGraphicsEra(m_settings, features, graphicsEra);
        const GraphicsEra era = graphicsEra;
        graphicsEra = GraphicsEra::Modern;
        AddPasses(graph, world, camera, env, output, outputSize, deltaTime, move(overlay));
        graphicsEra = era;
        m_settings = savedSettings;
        features = savedFeatures;
        return;
    }
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
    if (features.postProcessVolumes) {
        m_settings.post = EvaluatePostProcessVolumes(world, camera.position, m_settings.post);
        // Color grading LUT (an image in the project, loaded like decal textures).
        f32 lutIntensity = 0.0f;
        const String lut = FindColorLut(world, camera.position, lutIntensity);
        if (!lut.Empty() && decalTextures) {
            m_settings.post.colorLut = m_assets.GetTextureSrv(decalTextures(lut, false));
            m_settings.post.colorLutIntensity = lutIntensity;
        }
    }
    m_settings.post.depthOfField &= features.depthOfField;
    m_settings.post.motionBlur &= features.motionBlur;
    m_settings.reflections.sky &= features.reflections;
    m_settings.reflections.probes &= features.reflections;
    m_settings.reflections.ssr &= features.screenSpaceReflections;
    m_settings.scene.normalOutput = m_settings.reflections.ssr;
    if (camera.cut) {
        m_taa.Reset();
        m_volumetric.ResetHistory();
        m_scene.ResetOcclusion();
    }
    ResolveHdriSky(world);
    // Height Fog component (without one: the default fog, volumetric).
    bool volumetric = features.volumetricFog && m_settings.volumetricFog.enabled && m_settings.useSky;
    f32 fogHeight = 0.0f;
    if (const HeightFog* hf = FindHeightFog(world, &fogHeight)) {
        render::FogSettings& f = m_settings.sky.fog;
        f.density = Max(hf->density, 0.0f);
        f.falloff = Max(hf->falloff, 1e-5f);
        f.baseHeight = fogHeight;
        f.albedo = hf->color;
        f.sunAnisotropy = hf->sunAnisotropy;
        f.maxDistance = hf->maxDistance;
        m_settings.volumetricFog.distance = hf->volumetricDistance;
        m_settings.volumetricFog.scattering = hf->extinctionScale;
        m_settings.volumetricFog.localLights = hf->localLights;
        volumetric &= hf->volumetric;
    }
    m_settings.sky.fog.startDistance = volumetric ? m_settings.volumetricFog.distance : 0.0f;
    m_settings.reflections.skyResolution = m_settings.sky.hdri.IsActive() ? 128u : 64u;
    // Directional Light: the sun follows the entity (its forward axis is the direction the light travels).
    ViewportEnvironment sceneEnv = env;
    Vec3 sunTravel;
    const TimeOfDay* clock = FindTimeOfDay(world);
    if (clock) {
        // Time of Day: the sun and the moon from the clock (over the Directional Light's rotation).
        const CelestialState sky = ComputeCelestial(*clock);
        sceneEnv.timeOfDay = true;
        sceneEnv.night = false;
        sceneEnv.sunElevation = Asin(Clamp(sky.toSun.y, -1.0f, 1.0f));
        sceneEnv.sunAzimuth = Atan2(sky.toSun.x, sky.toSun.z);
        sceneEnv.toMoon = sky.toMoon;
        sceneEnv.moonLight = 1.0f * Max(clock->moonBrightness, 0.0f) * (0.1f + 0.9f * sky.moonPhase) * Saturate(sky.toMoon.y * 10.0f);
        sceneEnv.starBrightness = Max(clock->starBrightness, 0.0f);
    }
    if (const DirectionalLight* sun = FindDirectionalLight(world, &sunTravel)) {
        const Vec3 toSun = -sunTravel;
        if (!clock) {
            sceneEnv.sunElevation = Asin(Clamp(toSun.y, -1.0f, 1.0f));
            sceneEnv.sunAzimuth = Atan2(toSun.x, toSun.z);
        }
        m_settings.sky.sunIlluminanceTop *= Max(sun->intensity, 0.0f);
        m_settings.sky.sunColor = m_settings.sky.sunColor * sun->color;
        render::ShadowSettings& sh = m_settings.scene.shadows;
        if (!sun->castShadows)
            sh.enabled = false;
        sh.sourceAngle = Clamp(sun->sourceAngle, 0.0f, 10.0f);
        if (sun->shadowDistance > 0.0f)
            sh.distance = sh.vsmDistance = Clamp(sun->shadowDistance, 5.0f, 2000.0f);
        sh.normalBias *= Clamp(sun->shadowBias, 0.0f, 8.0f);
        sh.vsmNormalBias *= Clamp(sun->shadowBias, 0.0f, 8.0f);
        m_settings.scene.contactShadows.enabled &= sun->contactShadows;
        m_settings.scene.contactShadows.length = Clamp(sun->contactShadowLength, 0.01f, 2.0f);
    }
    if (const SkyLight* skyLight = FindSkyLight(world))
        m_settings.sky.skyBrightness *= Max(skyLight->skyBrightness, 0.0f);
    ApplyEnvironment(sceneEnv);
    m_lights.Clear();
    GatherLights(world, m_lights);
    if (m_particleSource) {
        m_particleSource->ModulateLights(m_lights.View());
        m_particleSource->AppendLights(m_lights); // fire on burning objects
    }
    m_lighting.lights = m_lights.View();

    const f32 scale = Clamp(m_settings.renderScale, 0.25f, 2.0f);
    m_lastScale = scale;
    // ZTSR needs more jitter positions per output pixel the lower the render scale.
    if (m_settings.taa.upscale && scale < 0.999f)
        m_settings.taa.jitterSamples = Max(m_settings.taa.jitterSamples, u32(Ceil(8.0f / (scale * scale))));
    const UVec2 renderSize(Max(1u, u32(f32(outputSize.x) * scale)), Max(1u, u32(f32(outputSize.y) * scale)));
    RenderView& view = m_view;
    view.view = camera.view;
    const Mat4 projection = Mat4::PerspectiveInfiniteReversedZ(camera.fovY, f32(renderSize.x) / f32(renderSize.y), camera.nearPlane);
    m_viewProjNoJitter = projection * camera.view;
    const Vec2 jitter = m_taa.NextJitter(renderSize.x, renderSize.y, m_settings.taa);
    view.projection = TemporalAA::ApplyJitter(projection, jitter);
    view.viewProj = view.projection * view.view;
    view.position = camera.position;
    view.fovY = camera.fovY;
    view.nearPlane = camera.nearPlane;
    view.width = renderSize.x;
    view.height = renderSize.y;

    {
        ZX_PROFILE_SCOPE("GatherInstances");
        m_instances.Clear();
        GatherRenderInstances(world, m_instances, m_animation);
    }
    {
        // Object motion: pair each instance with its world matrix of last frame (none after a cut).
        ZX_PROFILE_SCOPE("PairPrevWorlds");
        // One pass over the instances (memory bound: 100k instances are ~15 MB): the gather order is stable, so the
        // instance at the same index usually is the same entity. Mismatches (entities added / removed) are paired
        // through an id -> index map of last frame afterwards.
        const Array<u64>& prevIds = m_motionIds[m_motionCurrent];
        const Array<Mat4>& prevWorlds = m_motionWorlds[m_motionCurrent];
        m_motionCurrent ^= 1;
        Array<u64>& ids = m_motionIds[m_motionCurrent];
        Array<Mat4>& worlds = m_motionWorlds[m_motionCurrent];
        const u32 count = u32(m_instances.Size());
        const u32 prevCount = camera.cut ? 0u : u32(prevIds.Size());
        ids.ResizeUninitialized(count);
        worlds.ResizeUninitialized(count);
        std::atomic<u32> mismatches{0};
        std::atomic<u32> movedCount{0};
        m_movedIndices.ResizeUninitialized(count);
        auto pair = [&](u32 begin, u32 end) {
            u32 missed = 0;
            for (u32 i = begin; i < end; ++i) {
                render::RenderInstance& inst = m_instances[i];
                ids[i] = inst.id;
                worlds[i] = inst.world;
                if (i < prevCount && prevIds[i] == inst.id) {
                    if (MemCompare(&prevWorlds[i], &inst.world, sizeof(Mat4)) != 0) {
                        inst.prevWorld = &prevWorlds[i];
                        m_movedIndices[movedCount.fetch_add(1, std::memory_order_relaxed)] = i;
                    }
                } else {
                    ++missed;
                }
            }
            if (missed)
                mismatches.fetch_add(missed, std::memory_order_relaxed);
        };
        if (jobs::IsInitialized() && count > 4096)
            jobs::ParallelFor(count, 4096, pair);
        else
            pair(0, count);
        if (mismatches.load() > 0 && prevCount > 0) {
            HashMap<u64, u32> prevIndex;
            prevIndex.Reserve(prevCount);
            for (u32 k = 0; k < prevCount; ++k)
                prevIndex.InsertOrAssign(prevIds[k], k);
            for (u32 i = 0; i < count; ++i) {
                render::RenderInstance& inst = m_instances[i];
                if (i < prevCount && prevIds[i] == inst.id)
                    continue;
                if (const u32* j = prevIndex.Find(inst.id); j && MemCompare(&prevWorlds[*j], &inst.world, sizeof(Mat4)) != 0) {
                    inst.prevWorld = &prevWorlds[*j];
                    m_movedIndices[movedCount.fetch_add(1, std::memory_order_relaxed)] = i;
                }
            }
        }
        m_movedIndices.Resize(movedCount.load());
        m_scene.SetMovedInstances(m_movedIndices.View());
    }
    m_scene.SetMotionMatrices(m_viewProjNoJitter, m_hasPrevView && !camera.cut ? m_prevViewProjNoJitter : m_viewProjNoJitter);
    m_prevViewProjNoJitter = m_viewProjNoJitter;
    m_hasPrevView = true;
    const f32 exposure = ExposureFromEV100(m_lighting.exposureEV100);
    if (m_settings.useSky)
        m_sky.AddPrePasses(graph, view, m_dirToSun, exposure, m_settings.sky, m_lighting);
    // Sky Light: scales (and tints) the ambient light from the sky.
    if (const SkyLight* sky = FindSkyLight(world)) {
        const Vec3 k = sky->tint * Max(sky->intensity, 0.0f);
        m_lighting.skyIrradianceScale = m_lighting.skyIrradianceScale * k;
        m_lighting.skyLuminance *= Max(sky->intensity, 0.0f);
        m_lighting.skyColor = m_lighting.skyColor * sky->tint;
    }
    m_time += f64(deltaTime);
    m_scene.SetTime(f32(std::fmod(m_time, 3600.0))); // wraps hourly: keeps float precision for animated materials
    // Reflections: probes are recaptured when the scene content, the sun or the sky changes.
    m_probes.Clear();
    GatherReflectionProbes(world, m_probes, SceneContentKey(m_instances.View(), m_lights.View()) ^ (m_assets.ContentVersion() * 0x9E3779B97F4A7C15ull));
    u64 environmentKey = (m_settings.useSky ? 1u : 2u) ^ m_settings.sky.hdri.key;
    for (f32 c : {m_dirToSun.x, m_dirToSun.y, m_dirToSun.z, env.night ? 1.0f : 0.0f})
        environmentKey = environmentKey * 31u + u64(i64(c * 1000.0f));
    m_reflections.Prepare(m_probes.View(), view, exposure, m_settings.reflections, environmentKey, m_lighting);
    m_decals.Clear();
    if (features.decals)
        GatherDecals(world, m_decals, [this](StringView path, bool normalMap) {
            return decalTextures ? m_assets.GetTextureSrv(decalTextures(path, normalMap)) : InvalidDescriptor;
        });
    m_lighting.decals = m_decals.View();
    // Cloud shadows from last frame's map (none without clouds).
    m_lighting.cloudShadow = m_clouds.ShadowMap();
    m_lighting.cloudShadowOrigin = m_clouds.ShadowOrigin();
    m_lighting.cloudShadowExtent = render::CloudRenderer::ShadowExtent;
    // Dynamic GI: the probes as last updated light this frame's surfaces (updated again at the end of the frame).
    {
        render::GISettings gi = m_settings.gi;
        gi.enabled &= features.globalIllumination;
        gi.screenSpace &= m_settings.scene.debugMode == 0; // it gathers the lit frame: debug views would feed back into it
        m_gi.Prepare(m_instances.View(), view.position, exposure, gi, m_lighting);
    }
    m_scene.Prepare(m_instances.View(), view, m_lighting, m_settings.scene);
    // One face of a reflection probe per frame, rendered by a second scene renderer (sky reflections only, no SSR/AO).
    RGTexture faceColor, faceDepth;
    RenderView faceView;
    if (m_reflections.CaptureView(faceView)) {
        SceneLighting captureLighting = m_lighting;
        captureLighting.probeBuffer = InvalidDescriptor;
        captureLighting.probeCount = 0;
        captureLighting.ssrTexture = InvalidDescriptor;
        captureLighting.ssgiTexture = InvalidDescriptor; // traced from the main view
        SceneRenderSettings cs = m_settings.scene;
        cs.normalOutput = false;
        cs.deferred = false;   // small one-off faces: forward is cheaper
        cs.ao.enabled = false;
        cs.debugMode = 0;
        cs.lodBias = 0.5f;
        cs.nano.pixelError = Max(cs.nano.pixelError, 4.0f);
        cs.shadows.localEnabled = false;
        cs.shadows.cascades = Min(cs.shadows.cascades, 2u);
        cs.shadows.cache = false;
        cs.occlusionCulling = false; // six unrelated face views: no previous depth to test against
        m_captureScene.SetTime(f32(std::fmod(m_time, 3600.0)));
        m_captureScene.Prepare(m_instances.View(), faceView, captureLighting, cs);
        TextureDesc fd;
        fd.width = faceView.width;
        fd.height = faceView.height;
        fd.format = m_hdrFormat;
        fd.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        fd.debugName = "ProbeFace";
        faceColor = graph.CreateTexture(fd);
        fd.format = DepthFormat();
        fd.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
        fd.debugName = "ProbeFaceDepth";
        faceDepth = graph.CreateTexture(fd);
        m_captureScene.AddPasses(graph, faceColor, faceDepth, LinearColor(0, 0, 0, 1));
    }

    TextureDesc hdrDesc;
    hdrDesc.width = renderSize.x;
    hdrDesc.height = renderSize.y;
    hdrDesc.format = m_hdrFormat;
    hdrDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    hdrDesc.debugName = "SceneColor";
    RGTexture sceneColor = graph.CreateTexture(hdrDesc); // replaced by the heat-haze composite when there is one
    TextureDesc depthDesc = hdrDesc;
    depthDesc.format = DepthFormat();
    depthDesc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
    depthDesc.debugName = "SceneDepth";
    const RGTexture sceneDepth = graph.CreateTexture(depthDesc);
    m_scene.AddPasses(graph, sceneColor, sceneDepth, LinearColor(0, 0, 0, 1),
                      [this, &view](RenderGraph& g, RGTexture color, RGTexture depth, RGTexture normals) {
                          m_reflections.AddSsrPass(g, color, depth, normals, view);
                          m_gi.AddScreenPass(g, color, depth, normals, view);
                      });
    if (m_settings.useSky)
        m_sky.AddSkyPass(graph, sceneColor, sceneDepth);
    // Volumetric clouds over the sky (and their shadow map for the next frame).
    {
        render::CloudSettings clouds = FindClouds(world);
        clouds.enabled &= features.clouds && m_settings.useSky && !m_settings.sky.hdri.IsActive();
        const f64 cloudClock = EnvironmentTime(world); // game time: pausing the game stops the clouds too
        m_clouds.AddPasses(graph, sceneColor, sceneDepth, view, m_viewProjNoJitter, m_lighting, clouds, m_settings.useSky ? &m_sky : nullptr,
                           exposure, f32(std::fmod(cloudClock >= 0.0 ? cloudClock : m_time, 3600.0)));
    }
    // Water after the sky (its refraction sees the sky) and before the fog (which then covers it too).
    m_waterSurfaces.Clear();
    if (features.water)
        GatherWater(world, m_waterSurfaces);
    // Waves run on the environment clock (game time) so floating objects ride the same waves (Engine/WaterPhysics.h).
    const f64 waterClock = EnvironmentTime(world);
    m_water.AddPasses(graph, sceneColor, sceneDepth, view, m_waterSurfaces.View(), m_lighting, &m_scene.GetLightingInputs(),
                      m_settings.useSky ? &m_sky : nullptr, WaterTime(waterClock >= 0.0 ? waterClock : m_time), exposure);
    if (m_settings.useSky)
        m_sky.AddFogPass(graph, sceneColor, sceneDepth, m_lighting, m_settings.sky);
    // Volumetric fog over the near part (the analytic fog above started where it ends).
    {
        render::VolumetricFogSettings vs = m_settings.volumetricFog;
        vs.enabled = volumetric;
        m_fogVolumes.Clear();
        if (volumetric) {
            GatherFogVolumes(world, m_fogVolumes);
            if (m_particleSource)
                m_particleSource->AppendFogVolumes(m_fogVolumes); // smoke over burning objects
        } else {
            m_volumetric.ResetHistory();
        }
        m_volumetric.AddPasses(graph, sceneColor, sceneDepth, view, m_viewProjNoJitter, m_lighting, m_scene.GetLightingInputs(),
                               m_settings.sky.fog, vs, m_fogVolumes.View(), exposure, f32(std::fmod(m_time, 3600.0)), &m_sky);
    }
    // Simulated smoke and fire (Fluid Volumes), over the fogged scene.
    m_fluidVolumes.Clear();
    if (features.fluids)
        GatherFluidVolumes(world, m_fluidVolumes, env.playing);
    m_fluids.AddPasses(graph, sceneColor, sceneDepth, view, m_viewProjNoJitter, m_lighting, m_fluidVolumes.View(), exposure, deltaTime,
                       f32(std::fmod(m_time, 3600.0)));
    // Under water the water's own fog (over the atmosphere's).
    m_water.AddUnderwaterPass(graph, sceneColor, sceneDepth, view);
    // Particles over the fogged scene (they don't write depth, so the fog couldn't place them).
    m_particleList.Clear();
    if (features.particles && m_particleSource) {
        const Function<u32(StringView), 32> textures = [this](StringView path) {
            return decalTextures ? m_assets.GetTextureSrv(decalTextures(path, false)) : InvalidDescriptor;
        };
        m_particleSource->Gather(view.position, m_particleList, &textures);
        m_particleSource->GatherGpu(m_gpuEmitters, m_gpuParticleFrame, &textures);
    } else {
        m_gpuEmitters.Clear();
        m_gpuParticleFrame.emitters = {};
    }
    render::ParticleRenderer::FogVolumes particleFog;
    if (m_volumetric.IsActive()) {
        particleFog.lighting = m_volumetric.LightingVolume();
        particleFog.integrated = m_volumetric.IntegratedVolume();
        particleFog.distance = m_volumetric.Distance();
        particleFog.slices = m_volumetric.Slices();
    }
    sceneColor = m_particleRenderer.AddPass(graph, sceneColor, sceneDepth, view, m_particleList.View(), m_lighting, exposure, f32(std::fmod(m_time, 3600.0)),
                               particleFog, &m_gpuParticleFrame);
    // After every pass that samples the cubes: sky cube update, the probe face, prefiltering.
    m_reflections.AddUpdatePasses(graph, m_settings.useSky ? m_sky.SkyViewTexture() : RGTexture(), m_sky.SkyRadianceScale(), faceColor,
                                  faceDepth, exposure, m_settings.useSky ? &m_settings.sky.hdri : nullptr);
    m_gi.AddPasses(graph, m_lighting, m_scene.GetLightingInputs(), m_scene.PreparedLightBuffer(), m_scene.PreparedLightCount(), m_settings.useSky ? &m_sky : nullptr);
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
                                   ? m_taa.AddPass(graph, sceneColor, sceneDepth, m_viewProjNoJitter, exposure, m_settings.taa, outputSize, jitter,
                                                  m_scene.VelocityTexture())
                                   : sceneColor;
    PostView postView;
    postView.viewProj = m_viewProjNoJitter;
    postView.fovY = camera.fovY;
    postView.nearPlane = camera.nearPlane;
    postView.cut = camera.cut;
    postView.velocity = m_scene.VelocityTexture();
    m_post.AddPasses(graph, resolved, sceneDepth, output, m_lighting.exposureEV100, deltaTime, m_settings.post, &postView);
    m_lastPost = m_settings.post;
    m_clouds.AddShadowPass(graph); // last: this frame's passes read the previous map

    m_outlineItems.Clear();
    for (ecs::Entity e : m_outlined) {
        if (!world.IsAlive(e))
            continue;
        const MeshRenderer* mr = world.Get<const MeshRenderer>(e);
        const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(e);
        if (mr && wt && mr->visible)
            if (const GpuMesh* mesh = m_assets.GetMesh(mr->mesh))
                m_outlineItems.PushBack(OutlineItem{mesh, wt->matrix, m_scene.SkinnedVertexBuffer(e.Pack())});
    }
    if (!m_outlineItems.Empty())
        m_outline.AddPasses(graph, m_outlineItems.View(), m_viewProjNoJitter, camera.nearPlane, sceneDepth, renderSize, output, outputSize,
                            m_outlineSettings);
}

void ViewportRenderer::UpdateDynamicResolution(f32 gpuFrameMs, f32 deltaTime)
{
    DynamicResolutionSettings& d = m_settings.dynamicResolution;
    if (!d.enabled || gpuFrameMs <= 0.0f)
        return;
    m_dynamicTimer += deltaTime;
    if (m_dynamicTimer < 0.25f)
        return;
    m_dynamicTimer = 0.0f;
    m_settings.renderScale = NextDynamicResolutionScale(m_settings.renderScale, gpuFrameMs, d);
}

f32 NextDynamicResolutionScale(f32 currentScale, f32 gpuFrameMs, const DynamicResolutionSettings& d)
{
    const f32 lo = Clamp(Min(d.minScale, d.maxScale), 0.25f, 1.0f), hi = Clamp(Max(d.minScale, d.maxScale), lo, 2.0f);
    if (gpuFrameMs <= 0.0f)
        return Clamp(currentScale, lo, hi);
    // GPU cost grows with the pixel count (scale squared); move half of the way, in 5 % steps.
    const f32 headroom = Clamp(d.targetMs / gpuFrameMs, 0.7f, 1.3f);
    const f32 next = currentScale * Sqrt(Lerp(1.0f, headroom, 0.5f));
    return Clamp(Round(next * 20.0f) / 20.0f, lo, hi);
}

} // namespace zx
