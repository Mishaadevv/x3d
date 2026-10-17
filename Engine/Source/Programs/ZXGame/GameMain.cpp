// ZXGame: the runtime of a packaged game. Next to the executable: Game.json (name, startup scene, window), Content.zpak
// (the project's Content, cooked data, shaders) and optionally the C++ game module DLL. It mounts the package, loads the
// startup scene and runs the same game loop as Play-in-Editor (Engine/GameRuntime.h) in its own window.
//
// Command line: -windowed, -resolution=WxH, -autotest=<frames> (quit after N frames once loaded; exit code 3 on
// D3D12 debug-layer errors), -screenshot=<png> (with -autotest), -pak=<path> (another package), -warp, -noaudio.

#include "Asset/AssetManager.h"
#include "Audio/AudioEngine.h"
#include "Core/Console/Console.h"
#include "Core/CoreInit.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/FileSystem/Pak.h"
#include "Core/Image/Png.h"
#include "Core/Log/Log.h"
#include "Core/Serialization/Json.h"
#include "Core/String/Parse.h"
#include "Core/String/Path.h"
#include "Core/Threading/Thread.h"
#include "Core/Time/Time.h"
#include "Engine/AudioSystem.h"
#include "Engine/BuiltinAssets.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/Fire.h"
#include "Engine/GameModule.h"
#include "Engine/GameRuntime.h"
#include "Engine/Gameplay.h"
#include "Engine/InputActions.h"
#include "Engine/Materials.h"
#include "Engine/Animation.h"
#include "Engine/Particles.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/Prefab.h"
#include "Engine/SceneRendering.h"
#include "Engine/SceneSerialization.h"
#include "Engine/Scripting.h"
#include "Engine/Terrain.h"
#include "Engine/ViewportRenderer.h"
#include "Platform/Platform.h"
#include "Platform/Window.h"
#include "RHI/EngineProfile.h"
#include "RHI/ShaderCompiler.h"
#include "RHI_D3D12/D3D12Backend.h"
#include "Render/RenderAssets.h"
#include "Render/RenderGraph.h"
#include "Scene/Hierarchy.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogGame, Info);

using namespace zx;
using namespace zx::rhi;
using namespace zx::render;

namespace {

/// Game.json next to the executable (written by the editor's Package Project).
struct GameConfig
{
    String name = "Game";
    String scene;            // virtual path, e.g. /Game/Scenes/Main.zscene
    String module;           // C++ game module DLL next to the executable (empty = none)
    u32 textureMaxSize = 0;  // the cook setting (DDC keys depend on it)
    Array<String> disabledFeatures;
    bool fullscreen = true;
    UVec2 resolution = UVec2(1600, 900);
};

bool ReadConfig(const String& dir, GameConfig& c)
{
    platform::File f;
    if (!f.Open(dir + "/Game.json", platform::FileMode::Read))
        return false;
    String text;
    text.Resize(usize(f.Size()));
    f.Read(text.Data(), text.Size());
    JsonValue j;
    if (!ParseJson(text.View(), j))
        return false;
    c.name = String(j.Get("name").AsString("Game"));
    c.scene = String(j.Get("scene").AsString());
    c.module = String(j.Get("module").AsString());
    c.textureMaxSize = u32(j.Get("textureMaxSize").AsInt(0));
    c.fullscreen = j.Get("fullscreen").AsBool(true);
    const JsonValue& res = j.Get("resolution");
    if (res.Size() == 2)
        c.resolution = UVec2(u32(Max<i64>(res[0].AsInt(1600), 320)), u32(Max<i64>(res[1].AsInt(900), 240)));
    for (const JsonValue& f2 : j.Get("disabledFeatures").Elements())
        c.disabledFeatures.PushBack(String(f2.AsString()));
    return true;
}

/// Characters allowed in a folder name (the per-user save directory).
String SafeName(StringView name)
{
    String out;
    for (char ch : name)
        out.Append((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ? ch : '_');
    return out.Empty() ? String("Game") : out;
}

class Game
{
public:
    bool Initialize(const String& exeDir, const GameConfig& config);
    bool Frame();
    void Shutdown();

    u32 debugLayerErrors = 0;

private:
    void Render();
    bool FeatureEnabled(StringView f) const
    {
        for (const String& d : m_config.disabledFeatures)
            if (d == f)
                return false;
        return true;
    }

    GameConfig m_config;
    UniquePtr<Window> m_window;
    UniquePtr<Device> m_device;
    UniquePtr<SwapChain> m_swapChain;
    UniquePtr<asset::AssetManager> m_assets;
    UniquePtr<RenderAssets> m_renderAssets;
    UniquePtr<MaterialLibrary> m_materials;
    UniquePtr<PrefabLibrary> m_prefabs;
    UniquePtr<audio::AudioEngine> m_audioEngine;
    UniquePtr<AudioSystem> m_audio;
    UniquePtr<ScriptSystem> m_scripts;
    UniquePtr<GameModuleSystem> m_module;
    UniquePtr<ViewportRenderer> m_viewport;
    UniquePtr<TransientResourcePool> m_pool;
    PhysicsSystem m_physics;
    ParticleSystem m_particles;
    UniquePtr<AnimationSystem> m_animation;
    FireSystem m_fire;
    TerrainSystem m_terrains;
    InputActionMap m_inputActions;
    InputActionState m_inputActionState;
    ecs::World m_world;
    InputState m_input;
    Array<WindowEvent> m_events;
    FrameTimer m_timer;
    ViewportEnvironment m_environment;
    ViewportCamera m_sceneCamera; // when the scene has no Camera
    bool m_started = false;   // assets loaded, game running
    u32 m_readyFrames = 0;
    f64 m_loadStart = 0.0;
    bool m_captured = false;  // mouse locked to the game
    u64 m_frame = 0;
    u64 m_startFrame = 0;
    u32 m_autotest = 0;
    String m_screenshot;
    Buffer* m_screenshotBuffer = nullptr;
    u32 m_screenshotPitch = 0;
    UVec2 m_screenshotSize;
    bool m_quit = false;
};

bool Game::Initialize(const String& exeDir, const GameConfig& config)
{
    m_config = config;
    m_loadStart = Clock::NowSeconds();
    RegisterEngineComponents();
    scene::InstallHierarchyHooks(m_world);
    StringView arg;
    if (CommandLine::GetValue("autotest", arg))
        m_autotest = u32(ParseInt(arg).ValueOr(120));
    if (CommandLine::GetValue("screenshot", arg))
        m_screenshot = arg;

    // Content: the package (or a loose development directory with the same layout).
    String pakPath = exeDir + "/Content.zpak";
    if (CommandLine::GetValue("pak", arg))
        pakPath = arg;
    RefPtr<fs::PakArchive> pak = fs::OpenPak(pakPath);
    if (!pak) {
        platform::ShowErrorMessageBox(m_config.name.CStr(), "Content.zpak is missing or damaged. Reinstall the game.");
        return false;
    }
    fs::MountSourceAt("/Game", fs::MakePakSource(pak, "Game"));
    fs::MountSourceAt("/DDC", fs::MakePakSource(pak, "DDC"));
    fs::MountSourceAt("/Project", fs::MakePakSource(pak, "Project"));
    fs::MountSourceAt("/Shaders", fs::MakePakSource(pak, "Shaders"), 10);
    fs::MountSourceAt("/Saved/ShaderCache", fs::MakePakSource(pak, "ShaderCache"), -10); // compiled with the game; misses compile and land in /Saved

    WindowDesc wd;
    wd.title = m_config.name;
    const bool windowed = !m_config.fullscreen || CommandLine::HasFlag("windowed") || m_autotest > 0;
    wd.mode = windowed ? WindowMode::Windowed : WindowMode::BorderlessFullscreen;
    wd.width = i32(m_config.resolution.x);
    wd.height = i32(m_config.resolution.y);
    if (CommandLine::GetValue("resolution", arg)) {
        StringView w, h;
        if (arg.Split('x', w, h)) {
            wd.width = i32(ParseInt(w).ValueOr(wd.width));
            wd.height = i32(ParseInt(h).ValueOr(wd.height));
        }
    }
    m_window.Reset(Window::Create(wd));
    if (!m_window)
        return false;

    DeviceDesc dd;
    dd.enableDebugLayer = ZX_BUILD_DEBUG || CommandLine::HasFlag("debuglayer");
    dd.useWarp = CommandLine::HasFlag("warp");
    m_device = d3d12::CreateD3D12Device(dd);
    if (!m_device) {
        platform::ShowErrorMessageBox(m_config.name.CStr(), "Failed to create a Direct3D 12 device.");
        return false;
    }
    const bool lite = SelectEngineProfile(m_device->GetCaps()) == EngineProfile::Lite;
    const UVec2 size = m_window->GetClientSize();
    m_swapChain = m_device->CreateSwapChain(SwapChainDesc{m_window->GetNativeHandle(), Max(size.x, 1u), Max(size.y, 1u), 3, Format::R8G8B8A8_UNORM,
                                                          !CommandLine::HasFlag("novsync")});
    if (!m_swapChain)
        return false;

    asset::AssetManagerSettings as;
    as.ddcDirectory = "/DDC";                    // cooked by the editor for exactly these paths
    as.textureMaxSize = m_config.textureMaxSize; // must match the cook
    as.writeMetaFiles = false;
    m_assets.Reset(new asset::AssetManager(as));
    RegisterBuiltinAssets(*m_assets);
    m_animation.Reset(new AnimationSystem(*m_assets));
    m_renderAssets.Reset(new RenderAssets(*m_device, *m_assets));
    m_prefabs.Reset(new PrefabLibrary(m_assets.Get(), "/Game"));
    m_materials.Reset(new MaterialLibrary(*m_assets, *m_renderAssets, "/Game"));

    audio::AudioSettings audioSettings;
    if (CommandLine::HasFlag("noaudio"))
        audioSettings.backend = audio::AudioSettings::Backend::Null;
    m_audioEngine.Reset(new audio::AudioEngine());
    m_audioEngine->Initialize(audioSettings);
    m_audio.Reset(new AudioSystem(*m_audioEngine));
    m_audio->SetContentRoot("/Game");
    m_scripts.Reset(new ScriptSystem());
    m_scripts->SetContentRoot("/Game");
    m_animation->contentRoot = "/Game";
    m_scripts->audio = m_audio.Get();
    m_scripts->materialName = [this](asset::AssetId id) {
        const MaterialLibrary::Material* m = m_materials ? m_materials->Find(id) : nullptr;
        return m ? String(Path::Stem(m->path)) : String();
    };
    m_scripts->onLog = [](StringView message, bool isError) {
        if (isError)
            ZX_LOG_ERROR(LogGame, "{}", message);
        else
            ZX_LOG_INFO(LogGame, "{}", message);
    };
    m_module.Reset(new GameModuleSystem());
    if (!m_config.module.Empty()) {
        String error;
        if (!m_module->LoadDll(exeDir + "/" + m_config.module, &error))
            ZX_LOG_ERROR(LogGame, "C++ game module: {}", error);
    }
    m_inputActions = LoadInputActions("/Project");
    m_particles.raycast = [this](Vec3 from, Vec3 to, Vec3& point, Vec3& normal) {
        const Vec3 d = to - from;
        const f32 dist = Length(d);
        PhysicsHit hit;
        if (dist < 1e-3f || !m_physics.IsRunning() || !m_physics.Raycast(from, d / dist, dist, hit))
            return false;
        point = hit.point;
        normal = hit.normal;
        return true;
    };

    m_viewport.Reset(new ViewportRenderer(*m_device, *m_renderAssets));
    m_viewport->decalTextures = [this](StringView path, bool normalMap) { return m_materials->TextureAsset(path, normalMap); };
    m_viewport->SetParticleSystem(&m_particles);
    m_viewport->SetAnimationSystem(m_animation.Get());
    m_pool.Reset(new TransientResourcePool(*m_device));
    if (!m_renderAssets->Init() || !m_viewport->Init(lite, Format::R8G8B8A8_UNORM))
        return false;
    m_materials->ScanContent();

    // The startup scene and its environment.
    JsonValue settings;
    if (m_config.scene.Empty() || !LoadSceneFile(m_config.scene, m_world, SceneIOOptions{m_assets.Get(), "/Game", m_prefabs.Get()}, &settings)) {
        platform::ShowErrorMessageBox(m_config.name.CStr(), "The startup scene could not be loaded.");
        return false;
    }
    const JsonValue& env = settings.Get("environment");
    m_environment.night = env.Get("night").AsBool(false);
    m_environment.sunElevation = DegToRad(env.Get("sunElevation").AsFloat(60.0f));
    m_environment.sunAzimuth = DegToRad(env.Get("sunAzimuth").AsFloat(80.0f));
    m_environment.autoExposure = env.Get("autoExposure").AsBool(true);
    const JsonValue& cam = settings.Get("editorCamera");
    const JsonValue& p = cam.Get("position");
    const Vec3 camPos = p.Size() == 3 ? Vec3(p[0].AsFloat(), p[1].AsFloat(), p[2].AsFloat()) : Vec3(0, 2, -5);
    const f32 yaw = DegToRad(cam.Get("yaw").AsFloat(0.0f)), pitch = DegToRad(cam.Get("pitch").AsFloat(0.0f));
    const Vec3 fwd(Sin(yaw) * Cos(pitch), -Sin(pitch), Cos(yaw) * Cos(pitch));
    m_sceneCamera = ViewportCamera{Mat4::LookAt(camPos, camPos + fwd, Vec3::Up()), camPos, DegToRad(60.0f), 0.05f};
    ZX_LOG_INFO(LogGame, "{}: scene {} ({} entities)", m_config.name, m_config.scene, m_world.EntityCount());
    return true;
}

bool Game::Frame()
{
    if (!platform::PumpMessages() || m_device->IsDeviceLost() || m_quit || m_window->ShouldClose())
        return false;
    const f32 dt = Min(f32(m_timer.Tick()), 0.1f);
    m_events.Clear();
    platform::UpdateInput(*m_window, m_input, m_events);
    const UVec2 client = m_window->GetClientSize();
    if (client.x == 0 || client.y == 0 || m_window->IsMinimized()) {
        Thread::SleepMs(16);
        return true;
    }
    if (client != m_swapChain->GetSize())
        m_swapChain->Resize(client.x, client.y);
    ++m_frame;

    // Start once everything the scene needs is resident: meshes uploaded (static mesh colliders need them), materials
    // resolved, material shaders compiled - a black screen until then.
    bool ready = !m_started && m_assets->IsIdle() && m_frame > 2;
    if (!m_started) {
        m_world.Query<const MeshRenderer>().Each([&](const MeshRenderer& mr) {
            if (!mr.visible || !mr.mesh.IsValid())
                return;
            ready &= m_renderAssets->GetMesh(mr.mesh) != nullptr;
            for (const asset::AssetId& m : mr.materials)
                m_renderAssets->GetMaterialIndex(m);
        });
        ready &= m_renderAssets->GetStats().pendingUploads == 0 && m_renderAssets->Shaders().PendingCount() == 0;
        m_readyFrames = ready ? m_readyFrames + 1 : 0;
        ready = m_readyFrames >= 3 || (m_frame > 3000 && m_assets->IsIdle()); // give up waiting on something broken
    }
    if (!m_started && ready) {
        GameSystems s{m_scripts.Get(), m_module.Get(), m_audio.Get(), &m_physics, &m_particles, &m_fire, m_renderAssets.Get(),
                      &m_inputActions, &m_inputActionState, [this](StringView f) { return FeatureEnabled(f); }, m_animation.Get()};
        BeginGame(m_world, s);
        m_started = true;
        m_startFrame = m_frame;
        m_captured = FindGameCamera(m_world) != ecs::NullEntity && m_autotest == 0;
        ZX_LOG_INFO(LogGame, "Game started (loaded in {:.1f} s)", Clock::NowSeconds() - m_loadStart);
    }
    // Esc frees the mouse, a click in the window takes it again.
    if (m_input.WasKeyPressed(Key::Escape))
        m_captured = false;
    else if (!m_captured && m_started && m_input.WasMousePressed(MouseButton::Left) && m_window->IsFocused() && m_autotest == 0)
        m_captured = true;
    m_window->SetCursorLocked(m_captured && m_window->IsFocused());

    if (m_started) {
        GameSystems s{m_scripts.Get(), m_module.Get(), m_audio.Get(), &m_physics, &m_particles, &m_fire, m_renderAssets.Get(),
                      &m_inputActions, &m_inputActionState, [this](StringView f) { return FeatureEnabled(f); }, m_animation.Get()};
        ListenerPose fallback;
        fallback.position = m_sceneCamera.position;
        TickGame(m_world, s, dt, m_captured || m_autotest ? &m_input : nullptr, GameListener(m_world, fallback));
    }
    m_terrains.Update(m_world, *m_assets, m_renderAssets.Get());
    scene::UpdateWorldTransforms(m_world);
    m_particles.Tick(m_world, dt, m_started);
    m_animation->Tick(m_world, dt, m_started);
    Render();
    if (m_autotest && m_started && m_frame >= m_startFrame + m_autotest)
        return false;
    if (m_autotest && !m_started && m_frame > 4000) {
        ZX_LOG_ERROR(LogGame, "Assets did not finish loading");
        return false;
    }
    return true;
}

void Game::Render()
{
    const UVec2 size = m_swapChain->GetSize();
    m_device->BeginFrame();
    CommandList* cmd = m_device->BeginCommandList();
    m_renderAssets->Update(*cmd);
    RenderGraph graph(*m_device, *m_pool);
    const bool capture = !m_screenshot.Empty() && !m_screenshotBuffer && m_started && m_autotest && m_frame + 1 >= m_startFrame + m_autotest;
    const RGTexture backBuffer = graph.ImportTexture(m_swapChain->GetCurrentBackBuffer(), capture ? ResourceState::CopySource : ResourceState::Present);
    if (m_started) {
        ViewportFeatures& vf = m_viewport->features;
        vf.shadows = FeatureEnabled("shadows");
        vf.ambientOcclusion = FeatureEnabled("ambientOcclusion");
        vf.sky = FeatureEnabled("sky");
        vf.bloom = FeatureEnabled("bloom");
        vf.temporalAA = FeatureEnabled("temporalAA");
        vf.water = FeatureEnabled("water");
        vf.postProcessVolumes = FeatureEnabled("postProcessVolumes");
        vf.depthOfField = FeatureEnabled("depthOfField");
        vf.motionBlur = FeatureEnabled("motionBlur");
        vf.reflections = FeatureEnabled("reflections");
        vf.screenSpaceReflections = FeatureEnabled("screenSpaceReflections");
        vf.decals = FeatureEnabled("decals");
        vf.particles = FeatureEnabled("particles");
        vf.volumetricFog = FeatureEnabled("volumetricFog");
        vf.clouds = FeatureEnabled("clouds");
        vf.fluids = FeatureEnabled("fluids");
        ViewportCamera cam = m_sceneCamera;
        const ecs::Entity ge = FindGameCamera(m_world);
        const scene::WorldTransform* wt = m_world.Get<const scene::WorldTransform>(ge);
        const Camera* gc = m_world.Get<const Camera>(ge);
        if (wt && gc) {
            const Vec3 pos = wt->GetPosition();
            const Vec3 fwd = SafeNormalize(wt->matrix.TransformVector(Vec3::Forward()), Vec3::Forward());
            const Vec3 up = SafeNormalize(wt->matrix.TransformVector(Vec3::Up()), Vec3::Up());
            cam = ViewportCamera{Mat4::LookAt(pos, pos + fwd, up), pos, DegToRad(gc->fieldOfView), Max(gc->nearPlane, 0.001f)};
        }
        m_environment.playing = true;
        m_viewport->AddPasses(graph, m_world, cam, m_environment, backBuffer, size, m_timer.DeltaSeconds());
    } else {
        graph.AddPass(
            "Loading", PassType::Graphics, [&](PassBuilder& b) { b.RenderTarget(backBuffer, LoadOp::Clear, LinearColor(0, 0, 0, 1)); },
            [](PassContext&) {});
    }
    graph.Execute(*cmd);
    if (capture) {
        m_screenshotSize = size;
        m_screenshotBuffer = m_device->CreateBuffer(
            BufferDesc{AlignUp(u64(size.x) * 4, u64(256)) * size.y, BufferUsage::CopyDest, MemoryType::Readback, 0, "Screenshot"});
        m_screenshotPitch = u32(cmd->CopyTextureToBuffer(m_screenshotBuffer, 0, m_swapChain->GetCurrentBackBuffer()));
        cmd->Barrier(m_swapChain->GetCurrentBackBuffer(), ResourceState::Present);
    }
    m_device->Submit(cmd);
    m_swapChain->Present();
    m_device->EndFrame();
}

void Game::Shutdown()
{
    if (m_device)
        m_device->WaitIdle();
    if (m_screenshotBuffer) {
        const bool ok = png::WriteFile(m_screenshot, static_cast<const u8*>(m_screenshotBuffer->GetMappedData()), m_screenshotSize.x, m_screenshotSize.y,
                                       4, usize(m_screenshotPitch));
        ZX_LOG_INFO(LogGame, "Screenshot {} -> {}", ok ? "saved" : "FAILED", m_screenshot);
        m_device->Destroy(m_screenshotBuffer);
        m_screenshotBuffer = nullptr;
    }
    if (m_started) {
        GameSystems s{m_scripts.Get(), m_module.Get(), m_audio.Get(), &m_physics, &m_particles, &m_fire, m_renderAssets.Get(),
                      &m_inputActions, &m_inputActionState, {}};
        EndGame(s);
    }
    ClearScene(m_world);
    m_scripts.Reset();
    if (m_module)
        m_module->Unload();
    m_module.Reset();
    m_audio.Reset();
    if (m_audioEngine)
        m_audioEngine->Shutdown();
    m_audioEngine.Reset();
    m_particles.Clear();
    m_viewport.Reset();
    m_materials.Reset();
    m_prefabs.Reset();
    m_pool.Reset();
    m_renderAssets.Reset();
    m_animation.Reset(); // holds the asset manager
    m_assets.Reset();
    if (m_device)
        debugLayerErrors = d3d12::GetDebugLayerErrorCount(*m_device);
    m_swapChain.Reset();
    m_device.Reset();
    m_window.Reset();
}

} // namespace

int main()
{
    // Game.json decides the per-user save directory, so it is read before the core starts.
    const String exeDir(platform::GetExecutableDirectory());
    GameConfig config;
    const bool haveConfig = ReadConfig(exeDir, config);
    const String saved = platform::GetUserLocalAppDataDirectory() + "/" + SafeName(config.name) + "/Saved";
    if (!CoreInit(CoreInitParams{.appName = "Game", .savedDirectory = saved, .mountEngineDirectories = false}))
        return 1;
    int result = 0;
    if (!haveConfig) {
        platform::ShowErrorMessageBox("ZeqouX3D", "Game.json is missing next to the executable.");
        result = 1;
    } else if (!platform::InitializePlatform()) {
        result = 1;
    } else {
        rhi::ShaderCompiler::Initialize();
        {
            UniquePtr<Game> game(new Game());
            if (game->Initialize(exeDir, config)) {
                while (game->Frame()) {
                }
            } else {
                result = 1;
            }
            game->Shutdown();
            if (game->debugLayerErrors > 0)
                result = 3;
        }
        rhi::ShaderCompiler::Shutdown();
        platform::ShutdownPlatform();
    }
    const u64 leaks = CoreShutdown();
    return result != 0 ? result : (leaks == 0 ? 0 : 2);
}
