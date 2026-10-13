// MeshViewer (M4 sample): asynchronous glTF/OBJ loading through the asset manager (cook -> DDC -> GPU streaming),
// ECS scene with transform hierarchy, frustum culling + LODs, forward PBR with one directional light.
//
// Controls: RMB + mouse look, WASD/QE move, Shift fast; F2 post view (final / HDR / depth / EV heatmap / bloom);
// F3 AgX/ACES; F4 auto exposure;
// F5 material debug view (lit / base color / normals / roughness-metallic / LOD colors); F6 depth pre-pass on/off;
// F7 force LOD (auto, 0..3); F8 sun shadows; F9 day/night; F10 AO; T TAA/FXAA; PgUp/PgDn sun height; F11 fullscreen. Views: 5 cascades, 6 light clusters.
// F1 toggles the ZUI overlay (stats, GPU pass timings, render settings); -noui hides it at start.
// Command line: -scene=<path> (default Samples/Content/Sponza/Sponza.gltf), -lite/-full, -novsync, -warp,
// -debuglayer, -autotest[=frames] (counted after the scene is resident), -screenshot=<path>, -lod=<n>, -debugview=<n>.

#include "Asset/AssetManager.h"
#include "Core/Console/Console.h"
#include "Core/Containers/HashMap.h"
#include "Core/CoreInit.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Image/Png.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Log/Log.h"
#include "Core/Math/Random.h"
#include "Core/Profiler/Profiler.h"
#include "Core/String/Parse.h"
#include "Core/String/Path.h"
#include "Core/Threading/Thread.h"
#include "Core/Time/Time.h"
#include "Engine/FlyCamera.h"
#include "Engine/SceneRendering.h"
#include "Platform/Platform.h"
#include "Platform/Window.h"
#include "Render/PostProcess.h"
#include "Render/RenderGraph.h"
#include "Render/Sky.h"
#include "Render/TemporalAA.h"
#include "RHI/EngineProfile.h"
#include "RHI/ShaderCompiler.h"
#include "RHI_D3D12/D3D12Backend.h"
#include "UI/UI.h"
#include "UI/UIRenderer.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogMeshViewer, Info);

using namespace zx;
using namespace zx::rhi;
using namespace zx::render;

namespace {


Result<ShaderBytecode> Compile(StringView path, StringView entry, ShaderStage stage)
{
    ShaderCompileDesc sd;
    sd.path = path;
    sd.entryPoint = entry;
    sd.stage = stage;
    return ShaderCompiler::Compile(sd);
}

int Run()
{
    WindowDesc wd;
    wd.title = "ZeqouX3D - MeshViewer";
    UniquePtr<Window> window(Window::Create(wd));
    if (!window)
        return 1;
    DeviceDesc dd;
    dd.enableDebugLayer = ZX_BUILD_DEBUG || CommandLine::HasFlag("debuglayer");
    dd.useWarp = CommandLine::HasFlag("warp");
    UniquePtr<Device> device = d3d12::CreateD3D12Device(dd);
    if (!device)
        return 1;
    const EngineProfile profile = SelectEngineProfile(device->GetCaps());
    const bool lite = profile == EngineProfile::Lite;
    StringView resolutionArg, heightPart;
    if (CommandLine::GetValue("resolution", resolutionArg)) { // physical pixels, e.g. -resolution=1600x900
        StringView widthPart;
        if (resolutionArg.Split('x', widthPart, heightPart))
            window->SetClientSize(u32(ParseInt(widthPart).ValueOr(1600)), u32(ParseInt(heightPart).ValueOr(900)));
    }
    UVec2 size = window->GetClientSize();
    UniquePtr<SwapChain> swapChain = device->CreateSwapChain(
        SwapChainDesc{window->GetNativeHandle(), size.x, size.y, 3, Format::R8G8B8A8_UNORM, !CommandLine::HasFlag("novsync")});

    // Lite: 4-byte HDR target (the iGPU is memory-bandwidth bound).
    const Format hdrFormat = lite ? Format::R11G11B10_FLOAT : Format::R16G16B16A16_FLOAT;
    const Format depthFormat = Format::D32_FLOAT;

    // --- Assets ---
    const String root = FindEngineRootDirectory();
    asset::AssetManagerSettings as;
    as.ddcDirectory = root + "/Intermediate/DDC";
    as.textureMaxSize = lite ? 1024 : 0;
    asset::AssetManager assets(as);
    RenderAssets renderAssets(*device, assets);
    SceneRenderer sceneRenderer(*device, renderAssets);
    SkyRenderer sky(*device);
    SkySettings skySettings;
    skySettings.fog.enabled = !CommandLine::HasFlag("nofog");
    TemporalAA taa(*device);
    TAASettings taaSettings;
    taaSettings.enabled = !CommandLine::HasFlag("notaa");
    taaSettings.catmullRom = !lite;
    PostProcess post(*device);
    PostSettings postSettings;
    postSettings.bloomLevels = lite ? 4 : 6;
    if (!renderAssets.Init() || !sceneRenderer.Init(hdrFormat, depthFormat) ||
        !sky.Init(hdrFormat, depthFormat) || !post.Init(swapChain->GetFormat()) || !taa.Init())
        return 1;

    // --- UI overlay ---
    ui::FontSystem fonts;
    ui::Context uiContext(fonts);
    ui::UIRenderer uiRenderer(*device);
    if (!uiContext.LoadDefaultFonts() || !uiRenderer.Init(swapChain->GetFormat()))
        return 1;
    ui::SetCurrentContext(&uiContext);
    bool showUi = !CommandLine::HasFlag("noui");

    StringView scenePathArg;
    const String scenePath = CommandLine::GetValue("scene", scenePathArg) ? String(scenePathArg)
                                                                          : root + "/Samples/Content/Sponza/Sponza.gltf";
    Stopwatch loadTimer;
    const asset::AssetId sceneId = assets.LoadPath(scenePath);
    if (!sceneId.IsValid()) {
        ZX_LOG_ERROR(LogMeshViewer, "Can't load '{}' (run Tools/fetch_samples.ps1 to download the sample content)", scenePath);
        return 1;
    }

    ecs::World world;
    scene::InstallHierarchyHooks(world);
    ecs::Entity sceneRoot;
    bool residentLogged = false;

    TransientResourcePool pool(*device);
    FlyCamera camera;
    camera.position = Vec3(-9.0f, 1.8f, -0.4f);
    camera.LookAt(Vec3(5.0f, 2.5f, 0.0f));
    camera.moveSpeed = 4.0f;
    StringView camArg;
    if (CommandLine::GetValue("cam", camArg)) { // -cam=x,y,z,yawDeg,pitchDeg
        f32 v[5] = {};
        u32 n = 0;
        for (StringView rest = camArg; n < 5 && !rest.Empty();) {
            StringView part;
            if (!rest.Split(',', part, rest)) // no separator: `part` holds the remainder
                rest = {};
            v[n++] = ParseFloat(part.Trim()).ValueOr(0.0f);
        }
        camera.position = Vec3(v[0], v[1], v[2]);
        camera.yaw = DegToRad(v[3]);
        camera.pitch = DegToRad(v[4]);
    }

    SceneLighting lighting;
    lighting.sunDirection = Normalize(Vec3(0.35f, -1.0f, 0.3f));
    // Physical units (lux, nits, lumens) + manual exposure; F9 toggles day/night.
    bool night = CommandLine::HasFlag("night");
    f32 sunElevation = DegToRad(60.0f), sunAzimuth = DegToRad(80.0f);
    StringView elevationArg;
    if (CommandLine::GetValue("sun", elevationArg)) // -sun=<elevation degrees>
        sunElevation = DegToRad(ParseFloat(elevationArg).ValueOr(55.0f));
    StringView azimuthArg;
    if (CommandLine::GetValue("sunaz", azimuthArg)) // -sunaz=<azimuth degrees>, 0 = +Z, 90 = +X
        sunAzimuth = DegToRad(ParseFloat(azimuthArg).ValueOr(35.0f));
    Vec3 dirToSun;
    auto applyTimeOfDay = [&] {
        dirToSun = atmosphere::SunDirection(night ? DegToRad(-12.0f) : sunElevation, sunAzimuth);
        if (night) {
            lighting.sunDirection = Normalize(Vec3(-0.4f, -1.0f, -0.2f)); // moon
            lighting.sunColor = Vec3(0.6f, 0.7f, 1.0f);
            lighting.sunIlluminance = 0.4f;
            lighting.skyColor = Vec3(0.25f, 0.35f, 0.6f);
            lighting.skyLuminance = 0.25f;
            lighting.exposureEV100 = 5.0f;
        } else {
            SkyRenderer::ApplySun(lighting, dirToSun, skySettings);
            lighting.skyColor = Vec3(0.45f, 0.6f, 0.9f);
            // Until IBL: ambient and exposure follow the sun height (bright noon .. dim dusk).
            const f32 day = Saturate(Sin(sunElevation) * 2.0f);
            lighting.skyLuminance = 300.0f + 13700.0f * day;
            lighting.exposureEV100 = 9.0f + 4.5f * day;
        }
    };
    applyTimeOfDay();
    // 50 point lights along the atrium (M5 acceptance): warm lanterns on both sides and the upper gallery.
    const u32 lightCount = CommandLine::HasFlag("nolights") ? 0 : 50;
    Array<ecs::Entity> lanterns;
    Random lightRng(42);
    for (u32 i = 0; i < lightCount; ++i) {
        const u32 row = i % 4;
        const f32 x = -12.0f + 24.0f * f32(i / 4) / 12.0f;
        const Vec3 p(x, row < 2 ? 1.4f : 6.5f, (row % 2) ? 2.6f : -2.6f);
        const ecs::Entity e = scene::CreateNode(world, String::Format("Lantern{}", i).View(), Transform(p));
        PointLight l;
        const f32 hue = lightRng.Range(0.02f, 0.12f) + (i % 7 == 0 ? 0.5f : 0.0f);
        const LinearColor c = LinearColor::FromHSV(hue, 0.7f, 1.0f);
        l.color = Vec3(c.r, c.g, c.b);
        l.lumens = 3000.0f;
        l.range = 7.0f;
        l.castShadows = true;
        world.Add(e, l);
        lanterns.PushBack(e);
    }
    Array<LightDesc> lights;
    SceneRenderSettings settings;
    settings.lodBias = lite ? 0.75f : 1.0f;
    settings.maxAnisotropy = lite ? 4 : 16;
    if (lite) {
        settings.shadows.cascades = 3;
        settings.shadows.resolution = 1024;
        settings.shadows.distance = 50.0f;
        settings.shadows.filter = 1;
    } else {
        settings.shadows.cascades = 4;
        settings.shadows.resolution = 2048;
        settings.shadows.distance = 120.0f;
        settings.shadows.filter = 2;
    }
    settings.shadows.enabled = !CommandLine::HasFlag("noshadows");
    settings.shadows.localEnabled = !CommandLine::HasFlag("nolocalshadows");
    settings.ao.enabled = !CommandLine::HasFlag("noao");
    settings.ao.halfResolution = lite;
    settings.ao.slices = lite ? 2 : 3;
    settings.ao.steps = lite ? 4 : 6;
    settings.depthPrepass = !CommandLine::HasFlag("noprepass");
    StringView lodArg, debugViewArg;
    if (CommandLine::GetValue("lod", lodArg))
        settings.forcedLod = i32(ParseInt(lodArg).ValueOr(-1));
    if (CommandLine::GetValue("debugview", debugViewArg))
        settings.debugMode = u32(ParseInt(debugViewArg).ValueOr(0));

    StringView autotestValue, screenshotPath;
    const bool autotest = CommandLine::HasFlag("autotest") || CommandLine::GetValue("autotest", autotestValue);
    const u64 autotestFrames = autotestValue.Empty() ? 120 : u64(Max<i64>(1, ParseInt(autotestValue).ValueOr(120)));
    const bool takeScreenshot = CommandLine::GetValue("screenshot", screenshotPath);
    const bool flythrough = CommandLine::HasFlag("flythrough");
    const bool useSky = !CommandLine::HasFlag("nosky");
    Buffer* screenshotBuffer = nullptr;
    u64 screenshotPitch = 0;
    UVec2 screenshotSize;

    InputState input;
    Array<WindowEvent> events;
    FrameTimer timer;
    u32 viewMode = 0;
    StringView postViewArg;
    if (CommandLine::GetValue("postview", postViewArg))
        viewMode = u32(ParseInt(postViewArg).ValueOr(0));
    f64 titleTimer = 0.0, cpuSum = 0.0, gpuSum = 0.0, workSum = 0.0;
    u64 frames = 0, residentFrames = 0, statFrames = 0;
    Stopwatch workTimer;
    StringView scaleArg;
    const f32 renderScale = CommandLine::GetValue("scale", scaleArg) ? Clamp(ParseFloat(scaleArg).ValueOr(1.0f), 0.25f, 2.0f)
                            : lite                                    ? 0.7f // TZ M4 acceptance: 900p @ 70 % on the Lite tier
                                                                      : 1.0f;
    Array<RenderInstance> instances;
    HashMap<const char*, f64> passTimes; // interned timer names

    while (!window->ShouldClose() && !device->IsDeviceLost()) {
        if (!platform::PumpMessages())
            break;
        profiler::BeginFrame();
        workTimer.Restart();
        const f32 dt = timer.Tick();
        events.Clear();
        platform::UpdateInput(*window, input, events);
        for (const WindowEvent& e : events) {
            if (e.type != WindowEventType::KeyDown || e.repeat || uiContext.WantsKeyboard())
                continue;
            switch (e.key) {
            case Key::F1: showUi = !showUi; break;
            case Key::F2: viewMode = (viewMode + 1) % 5; break;
            case Key::F3:
                postSettings.tonemapper = postSettings.tonemapper == Tonemapper::AgX ? Tonemapper::ACES : Tonemapper::AgX;
                break;
            case Key::F4: postSettings.autoExposure = !postSettings.autoExposure; break;
            case Key::F5: settings.debugMode = (settings.debugMode + 1) % 9; break;
            case Key::F10: settings.ao.enabled = !settings.ao.enabled; break;
            case Key::T:
                taaSettings.enabled = !taaSettings.enabled;
                taa.Reset();
                break;
            case Key::F8: settings.shadows.enabled = !settings.shadows.enabled; break;
            case Key::F9:
                night = !night;
                applyTimeOfDay();
                break;
            case Key::PageUp:
            case Key::PageDown:
                sunElevation = Clamp(sunElevation + DegToRad(e.key == Key::PageUp ? 5.0f : -5.0f), DegToRad(-5.0f), DegToRad(89.0f));
                applyTimeOfDay();
                break;
            case Key::F6: settings.depthPrepass = !settings.depthPrepass; break;
            case Key::F7: settings.forcedLod = settings.forcedLod >= 3 ? -1 : settings.forcedLod + 1; break;
            case Key::F11:
                window->SetMode(window->GetMode() == WindowMode::Windowed ? WindowMode::BorderlessFullscreen : WindowMode::Windowed);
                break;
            case Key::Escape: window->RequestClose(); break;
            default: break;
            }
        }
        // The camera ignores the mouse while it is over the overlay (unless already flying).
        if (!uiContext.WantsMouse() || window->IsCursorLocked())
            window->SetCursorLocked(camera.Update(input, dt));
        if (flythrough && residentFrames > 0) {
            // Deterministic path through the atrium (benchmarks and cascade-cache stress test).
            const f32 t = f32(residentFrames) / 60.0f;
            camera.position = Vec3(9.0f * Sin(t * 0.3f), 1.8f + 3.0f * (0.5f + 0.5f * Sin(t * 0.23f)), 0.8f * Sin(t * 0.31f));
            camera.yaw = DegToRad(90.0f) + 0.7f * Sin(t * 0.5f);
            camera.pitch = 0.1f * Sin(t * 0.37f);
        }

        // Instantiate the scene once its prefab arrives; meshes/materials/textures keep streaming afterwards.
        if (!sceneRoot) {
            if (const asset::PrefabAsset* prefab = assets.GetPrefab(sceneId)) {
                sceneRoot = InstantiatePrefab(world, *prefab);
                ZX_LOG_INFO(LogMeshViewer, "Prefab '{}' ready after {:.0f} ms ({} nodes)", prefab->name, loadTimer.ElapsedMilliseconds(),
                            prefab->nodes.Size());
            } else if (assets.GetState(sceneId) == asset::AssetState::Failed) {
                ZX_LOG_ERROR(LogMeshViewer, "Scene failed to load: {}", assets.GetErrorMessage(sceneId));
                return 1;
            }
        }
        // Lanterns bob gently (exercises per-frame light upload and clustering).
        for (usize i = 0; i < lanterns.Size(); ++i)
            if (Transform* t = world.Get<Transform>(lanterns[i]))
                t->position.y = (i % 4 < 2 ? 1.4f : 6.5f) + 0.25f * Sin(f32(timer.TotalSeconds()) * 1.3f + f32(i));
        scene::UpdateWorldTransforms(world);
        // Auto exposure (GPU histogram, read back) drives the pre-exposure; the time-of-day EV is the start value.
        if (postSettings.autoExposure && post.HasAutoExposure())
            lighting.exposureEV100 = post.GetAutoExposureEV100();
        lights.Clear();
        GatherLights(world, lights);
        lighting.lights = lights.View();

        const UVec2 client = window->GetClientSize();
        if (client.x == 0 || client.y == 0 || window->IsMinimized()) {
            Thread::SleepMs(16);
            continue;
        }
        if (client != swapChain->GetSize())
            swapChain->Resize(client.x, client.y);
        const UVec2 outSize = swapChain->GetSize();
        const UVec2 renderSize(Max(1u, u32(f32(outSize.x) * renderScale)), Max(1u, u32(f32(outSize.y) * renderScale)));

        // --- UI frame (overlay) ---
        {
            ui::FrameInput fi;
            fi.displaySize = Vec2(f32(outSize.x), f32(outSize.y));
            fi.deltaTime = dt;
            fi.dpiScale = window->GetDpiScale();
            fi.input = window->IsCursorLocked() ? nullptr : &input;
            fi.events = events.View();
            fi.hasFocus = window->IsFocused();
            uiContext.BeginFrame(fi);
            if (showUi) {
                ui::SetNextWindowPos(Vec2(12.0f, 12.0f));
                ui::SetNextWindowSize(Vec2(330.0f, 560.0f));
                if (ui::Begin(ZX_ICON_GAUGE "  MeshViewer", &showUi)) {
                    const SceneRenderer::Stats& st = sceneRenderer.GetStats();
                    f64 gpuMs = 0.0;
                    for (const GpuTiming& t : device->GetGpuTimings())
                        if (StringView(t.name) == "Frame")
                            gpuMs = t.milliseconds;
                    ui::LabelText("Profile", EngineProfileName(profile));
                    ui::LabelText("Frame (CPU)", String::Format("{:.2f} ms  ({:.0f} FPS)", timer.SmoothedDeltaSeconds() * 1000.0f,
                                                               1.0f / Max(timer.SmoothedDeltaSeconds(), 1e-4f)));
                    ui::LabelText("GPU", String::Format("{:.2f} ms", gpuMs));
                    ui::LabelText("Render", String::Format("{}x{} ({}%)", renderSize.x, renderSize.y, i32(renderScale * 100.0f + 0.5f)));
                    ui::LabelText("Draw calls", String::Format("{} (+{} shadow)", st.draws, st.shadowDraws));
                    ui::LabelText("Triangles", String::Format("{:.0f}k", f64(st.triangles) / 1000.0));
                    ui::LabelText("Lights", String::Format("{} visible, {} shadow faces", st.lights, st.localShadowFaces));
                    if (ui::CollapsingHeader("GPU passes", ui::TreeNodeFlags::DefaultOpen)) {
                        if (ui::BeginTable("passes", 2, ui::TableFlags::RowBg)) {
                            ui::TableSetupColumn("Pass");
                            ui::TableSetupColumn("ms", 64.0f);
                            for (const GpuTiming& t : device->GetGpuTimings()) {
                                if (StringView(t.name) == "Frame")
                                    continue;
                                ui::TableNextRow();
                                ui::TableNextColumn();
                                ui::Text(t.name);
                                ui::TableNextColumn();
                                ui::TextColored(t.milliseconds > 3.0 ? uiContext.GetStyle().colors.warning : uiContext.GetStyle().colors.text,
                                                String::Format("{:.2f}", t.milliseconds));
                            }
                            ui::EndTable();
                        }
                    }
                    if (ui::CollapsingHeader("Settings", ui::TreeNodeFlags::DefaultOpen)) {
                        ui::Toggle("Sun shadows (F8)", &settings.shadows.enabled);
                        ui::Toggle("Local shadows", &settings.shadows.localEnabled);
                        ui::Toggle("GTAO (F10)", &settings.ao.enabled);
                        if (ui::Toggle("TAA (T)", &taaSettings.enabled))
                            taa.Reset();
                        ui::Toggle("Height fog", &skySettings.fog.enabled);
                        ui::Toggle("Depth pre-pass (F6)", &settings.depthPrepass);
                        ui::Toggle("Auto exposure (F4)", &postSettings.autoExposure);
                        if (ui::Toggle("Night (F9)", &night))
                            applyTimeOfDay();
                        f32 elevation = RadToDeg(sunElevation), azimuth = RadToDeg(sunAzimuth);
                        if (ui::SliderFloat("Sun elevation", &elevation, -5.0f, 89.0f, "{:.0f}°")) {
                            sunElevation = DegToRad(elevation);
                            applyTimeOfDay();
                        }
                        if (ui::SliderFloat("Sun azimuth", &azimuth, 0.0f, 360.0f, "{:.0f}°")) {
                            sunAzimuth = DegToRad(azimuth);
                            applyTimeOfDay();
                        }
                        static const StringView Tonemappers[] = {"ACES", "AgX"};
                        i32 tm = i32(postSettings.tonemapper);
                        if (ui::Combo("Tonemapper", &tm, Span<const StringView>(Tonemappers, 2)))
                            postSettings.tonemapper = Tonemapper(tm);
                        static const StringView DebugViews[] = {"Lit", "Base color", "Normals", "Rough/Metal", "LOD",
                                                                "Cascades", "Clusters", "AO", "Local shadows"};
                        i32 dv = i32(settings.debugMode);
                        if (ui::Combo("Debug view", &dv, Span<const StringView>(DebugViews, 9)))
                            settings.debugMode = u32(dv);
                    }
                }
                ui::End();
            }
            uiContext.EndFrame();
            if (!window->IsCursorLocked())
                window->SetCursorShape(uiContext.GetCursorShape());
        }

        f64 workMs = workTimer.ElapsedMilliseconds();
        device->BeginFrame();
        workTimer.Restart();
        CommandList* cmd = device->BeginCommandList();
        cmd->BeginTimer("Frame");
        renderAssets.Update(*cmd);

        RenderView view;
        view.view = camera.ViewMatrix();
        const Mat4 projectionNoJitter = camera.ProjectionMatrix(f32(renderSize.x) / f32(renderSize.y));
        const Mat4 viewProjNoJitter = projectionNoJitter * view.view;
        view.projection = TemporalAA::ApplyJitter(projectionNoJitter, taa.NextJitter(renderSize.x, renderSize.y, taaSettings));
        view.viewProj = view.projection * view.view;
        view.position = camera.position;
        view.fovY = camera.fovY;
        view.nearPlane = camera.nearPlane;
        view.width = renderSize.x;
        view.height = renderSize.y;
        instances.Clear();
        GatherRenderInstances(world, instances);

        RenderGraph graph(*device, pool);
        if (useSky)
            sky.AddPrePasses(graph, view, dirToSun, ExposureFromEV100(lighting.exposureEV100), skySettings, lighting);
        sceneRenderer.Prepare(instances.View(), view, lighting, settings); // requests the meshes/materials it needs

        const RenderAssets::Stats rs = renderAssets.GetStats();
        const bool resident = sceneRoot && assets.IsIdle() && rs.pendingUploads == 0;
        if (resident && !residentLogged) {
            residentLogged = true;
            ZX_LOG_INFO(LogMeshViewer, "Scene resident after {:.0f} ms: {} meshes ({:.1f} MB), {} textures ({:.1f} MB), {} materials",
                        loadTimer.ElapsedMilliseconds(), rs.meshes, f64(rs.meshBytes) / 1048576.0, rs.textures,
                        f64(rs.textureBytes) / 1048576.0, rs.materials);
        }
        const bool captureNow = takeScreenshot && resident && !screenshotBuffer &&
                                (autotest ? residentFrames + 1 == autotestFrames : residentFrames == 30);

        Texture* backBuffer = swapChain->GetCurrentBackBuffer();
        const RGTexture output = graph.ImportTexture(backBuffer, captureNow ? ResourceState::CopySource : ResourceState::Present);
        TextureDesc hdrDesc;
        hdrDesc.width = renderSize.x;
        hdrDesc.height = renderSize.y;
        hdrDesc.format = hdrFormat;
        hdrDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        hdrDesc.debugName = "SceneColor";
        const RGTexture sceneColor = graph.CreateTexture(hdrDesc);
        TextureDesc depthDesc = hdrDesc;
        depthDesc.format = depthFormat;
        depthDesc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
        depthDesc.debugName = "SceneDepth";
        const RGTexture sceneDepth = graph.CreateTexture(depthDesc);
        sceneRenderer.AddPasses(graph, sceneColor, sceneDepth, LinearColor(0, 0, 0, 1));
        if (useSky) {
            sky.AddSkyPass(graph, sceneColor, sceneDepth);
            sky.AddFogPass(graph, sceneColor, sceneDepth, lighting, skySettings);
        }
        postSettings.viewMode = viewMode;
        postSettings.fxaa = !taaSettings.enabled;
        const RGTexture resolved = taaSettings.enabled ? taa.AddPass(graph, sceneColor, sceneDepth, viewProjNoJitter,
                                                                     ExposureFromEV100(lighting.exposureEV100), taaSettings)
                                                       : sceneColor;
        post.AddPasses(graph, resolved, sceneDepth, output, lighting.exposureEV100, dt, postSettings);
        uiRenderer.Prepare(*cmd, fonts, uiContext.GetDrawLists()); // atlas uploads: before the graph's render passes
        if (uiRenderer.GetStats().primitives > 0)
            uiRenderer.AddPass(graph, output, outSize);
        graph.Execute(*cmd);
        if (captureNow) {
            screenshotSize = outSize;
            screenshotBuffer = device->CreateBuffer(BufferDesc{AlignUp(u64(outSize.x) * 4, u64(256)) * outSize.y, BufferUsage::CopyDest,
                                                               MemoryType::Readback, 0, "Screenshot"});
            screenshotPitch = cmd->CopyTextureToBuffer(screenshotBuffer, 0, backBuffer);
            cmd->Barrier(backBuffer, ResourceState::Present);
        }
        cmd->EndTimer();
        device->Submit(cmd);
        workMs += workTimer.ElapsedMilliseconds();
        swapChain->Present();
        device->EndFrame();

        if (resident) {
            ++residentFrames;
            if (residentFrames > 10) {
                cpuSum += f64(timer.UnscaledDeltaSeconds()) * 1000.0;
                workSum += workMs;
                for (const GpuTiming& t : device->GetGpuTimings()) {
                    if (StringView(t.name) == "Frame")
                        gpuSum += t.milliseconds;
                    f64* pass = passTimes.Find(t.name);
                    if (!pass)
                        pass = &passTimes.InsertOrAssign(t.name, 0.0);
                    *pass += t.milliseconds;
                }
                ++statFrames;
            }
        }
        titleTimer += dt;
        if (titleTimer > 0.5) {
            titleTimer = 0.0;
            f64 gpuMs = 0.0;
            for (const GpuTiming& t : device->GetGpuTimings())
                if (StringView(t.name) == "Frame")
                    gpuMs += t.milliseconds;
            const SceneRenderer::Stats& st = sceneRenderer.GetStats();
            window->SetTitle(String::Format(
                "ZeqouX3D - MeshViewer [{}] | {}x{} (render {}x{}) | CPU {:.2f} ms | GPU {:.2f} ms | {} draws, {:.0f}k tris | "
                "LOD {}/{}/{}/{} | {}",
                EngineProfileName(profile), outSize.x, outSize.y, renderSize.x, renderSize.y, timer.SmoothedDeltaSeconds() * 1000.0f,
                gpuMs, st.draws, f64(st.triangles) / 1000.0, st.lodHistogram[0], st.lodHistogram[1], st.lodHistogram[2],
                st.lodHistogram[3], resident ? String("resident") : String::Format("streaming ({} pending)", rs.pendingUploads)));
        }
        ++frames;
        if (autotest && (residentFrames >= autotestFrames || frames > 20000))
            window->RequestClose();
    }

    device->WaitIdle();
    if (screenshotBuffer) {
        const bool ok = png::WriteFile(screenshotPath, static_cast<const u8*>(screenshotBuffer->GetMappedData()), screenshotSize.x,
                                       screenshotSize.y, 4, usize(screenshotPitch));
        ZX_LOG_INFO(LogMeshViewer, "Screenshot {} -> {}", ok ? "saved" : "FAILED", screenshotPath);
        device->Destroy(screenshotBuffer);
    }
    const u32 debugErrors = d3d12::GetDebugLayerErrorCount(*device);
    const SceneRenderer::Stats& st = sceneRenderer.GetStats();
    ZX_LOG_INFO(LogMeshViewer, "Lights: {} visible of {}, local shadow faces {} ({} updated last frame)", st.lights,
                lights.Size(), st.localShadowFaces, st.localShadowUpdates);
    ZX_LOG_INFO(LogMeshViewer, "Exited after {} frames ({} resident), debug layer errors: {}", frames, residentFrames, debugErrors);
    ZX_LOG_INFO(LogMeshViewer, "Last frame: {} instances, {} culled, {} draws, {} triangles; shadows: {} draws, {} triangles, {} cascades updated",
                st.instances, st.culled, st.draws, st.triangles, st.shadowDraws, st.shadowTriangles, st.cascadesUpdated);
    if (statFrames)
        ZX_LOG_INFO(LogMeshViewer, "Average frame: {:.3f} ms (CPU work {:.3f} ms), GPU {:.3f} ms", cpuSum / f64(statFrames),
                    workSum / f64(statFrames), gpuSum / f64(statFrames));
    for (const auto& kv : passTimes)
        ZX_LOG_INFO(LogMeshViewer, "  GPU {:<20} {:.3f} ms", kv.first, kv.second / f64(Max<u64>(statFrames, 1)));
    pool.Clear();
    ui::SetCurrentContext(nullptr);
    return debugErrors == 0 && (!autotest || residentFrames >= autotestFrames) ? 0 : 3;
}

} // namespace

int main()
{
    if (!CoreInit(CoreInitParams{.appName = "MeshViewer"}))
        return 1;
    platform::InitializePlatform();
    ShaderCompiler::Initialize();
    int result = 0;
    {
        result = Run();
    }
    ShaderCompiler::Shutdown();
    platform::ShutdownPlatform();
    const u64 leaks = CoreShutdown();
    return result != 0 ? result : (leaks == 0 ? 0 : 2);
}
