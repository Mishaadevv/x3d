#include "Editor.h"
#include "Engine/PostProcessVolume.h"
#include "Thumbnails.h"
#include "Core/Console/Console.h"
#include "Core/Containers/Algorithm.h"
#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Image/Exr.h"
#include "Core/Image/Png.h"
#include "Core/Profiler/Profiler.h"
#include "Core/String/Parse.h"
#include "Core/String/Path.h"
#include "Core/Threading/Thread.h"
#include "Core/Version.h"
#include "Render/TextureLoader.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/Gameplay.h"
#include "Engine/Raycast.h"
#include "Engine/SceneSerialization.h"
#include "Platform/Platform.h"
#include "RHI/EngineProfile.h"
#include "RHI_D3D12/D3D12Backend.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogEditor, Info);
ZX_DEFINE_LOG_CATEGORY_STATIC(LogScript, Info);

namespace zx::editor {

using namespace rhi;
using namespace render;

// --- Log sink -------------------------------------------------------------------------------------------------------------

void EditorLogSink::Write(const LogMessage& message)
{
    ScopedLock lock(m_mutex);
    if (m_entries.Size() >= 20000)
        m_entries.RemoveAt(0, 5000);
    m_entries.PushBack(Entry{message.level, String(message.category ? message.category->name : "Log"), String(message.text),
                             message.timeSeconds});
    m_counts[u32(message.level) & 7]++;
    ++m_version;
}

usize EditorLogSink::Count() const
{
    ScopedLock lock(m_mutex);
    return m_entries.Size();
}

void EditorLogSink::Clear()
{
    ScopedLock lock(m_mutex);
    m_entries.Clear();
    for (u32& c : m_counts)
        c = 0;
    ++m_version;
}

u32 EditorLogSink::Counts(LogLevel level) const
{
    ScopedLock lock(m_mutex);
    return m_counts[u32(level) & 7];
}

String EditorLogSink::LastMessage() const
{
    ScopedLock lock(m_mutex);
    return m_entries.Empty() ? String() : m_entries.Back().text;
}

// --- Editor -------------------------------------------------------------------------------------------------------------

Editor::Editor() = default;
Editor::~Editor() = default;

bool Editor::Initialize()
{
    engineRoot = FindEngineRootDirectory();
    contentRoot = engineRoot + "/Samples/Content";
    m_layoutPath = "/Saved/Config/EditorLayout.ini";
    RegisterEngineComponents();
    scene::InstallHierarchyHooks(world);
    RegisterEditorActions(*this);
    {
        UniquePtr<EditorLogSink> sink(new EditorLogSink());
        log = sink.Get();
        Log::AddSink(move(sink));
    }

    WindowDesc wd;
    wd.title = "ZeqouX3D Editor";
    wd.width = 1600;
    wd.height = 900;
    wd.customTitleBar = true;
    window.Reset(Window::Create(wd));
    if (!window)
        return false;
    StringView resolution;
    if (CommandLine::GetValue("resolution", resolution)) {
        StringView w, h;
        if (resolution.Split('x', w, h))
            window->SetClientSize(u32(ParseInt(w).ValueOr(1600)), u32(ParseInt(h).ValueOr(900)));
    } else if (!CommandLine::HasFlag("autotest")) {
        window->Maximize();
    }
    window->SetHitTestCallback([this](IVec2 p) -> HitTestResult {
        const Vec2 pt(f32(p.x), f32(p.y));
        if (closeRect.Contains(pt))
            return HitTestResult::CloseButton;
        if (maximizeRect.Contains(pt))
            return HitTestResult::MaximizeButton;
        if (minimizeRect.Contains(pt))
            return HitTestResult::MinimizeButton;
        const f32 border = 5.0f * window->GetDpiScale();
        const UVec2 size = window->GetClientSize();
        if (!window->IsMaximized()) {
            const bool left = pt.x < border, right = pt.x >= f32(size.x) - border, top = pt.y < border,
                       bottom = pt.y >= f32(size.y) - border;
            if (top && left) return HitTestResult::ResizeTopLeft;
            if (top && right) return HitTestResult::ResizeTopRight;
            if (bottom && left) return HitTestResult::ResizeBottomLeft;
            if (bottom && right) return HitTestResult::ResizeBottomRight;
            if (left) return HitTestResult::ResizeLeft;
            if (right) return HitTestResult::ResizeRight;
            if (top) return HitTestResult::ResizeTop;
            if (bottom) return HitTestResult::ResizeBottom;
        }
        if (titleBarRect.Contains(pt)) {
            for (const Rect& r : titleInteractive)
                if (r.Contains(pt))
                    return HitTestResult::Client;
            return HitTestResult::Caption;
        }
        return HitTestResult::Client;
    });

    DeviceDesc dd;
    dd.enableDebugLayer = ZX_BUILD_DEBUG || CommandLine::HasFlag("debuglayer") || CommandLine::HasFlag("gpuvalidation");
    dd.enableGpuValidation = CommandLine::HasFlag("gpuvalidation"); // slow: resource states and descriptors checked on the GPU
    dd.useWarp = CommandLine::HasFlag("warp");
    device = d3d12::CreateD3D12Device(dd);
    if (!device) {
        platform::ShowErrorMessageBox("ZeqouX3D", "Failed to create a Direct3D 12 device.");
        return false;
    }
    lite = SelectEngineProfile(device->GetCaps()) == EngineProfile::Lite;
    const UVec2 size = window->GetClientSize();
    swapChain = device->CreateSwapChain(SwapChainDesc{window->GetNativeHandle(), Max(size.x, 1u), Max(size.y, 1u), 3,
                                                      Format::R8G8B8A8_UNORM, !CommandLine::HasFlag("novsync")});
    if (!swapChain)
        return false;

    ui.Reset(new ui::Context(fonts));
    if (!ui->LoadDefaultFonts()) {
        platform::ShowErrorMessageBox("ZeqouX3D", "Failed to load the editor fonts (Engine/Content/Fonts).");
        return false;
    }
    ui::SetCurrentContext(ui.Get());
    {
        render::TextureLoadOptions brand;
        brand.compression = render::TextureCompression::None; // crisp edges at small sizes
        if (Result<rhi::Texture*> t = render::LoadTexture(*device, "/Engine/Branding/ZeqouX3D_Icon.png", brand))
            appIcon = t.Value();
        if (Result<rhi::Texture*> t = render::LoadTexture(*device, "/Engine/Branding/ZeqouX3D_Logo.png", brand))
            appLogo = t.Value();
        // Template thumbnails: real captures of each template level (Tools/make_template_thumbs.py).
        static const char* const TemplateNames[] = {"Blank",   "Sample", "FirstPerson", "ThirdPerson", "Water", "Physics",
                                                    "Vehicle", "Empty",  "Classic2017", "Classic2011", "Classic2008"};
        static_assert(ZX_ARRAY_COUNT(TemplateNames) == u32(ProjectTemplate::Count));
        render::TextureLoadOptions art;
        art.compression = render::TextureCompression::None;
        art.srgb = false; // captured from the UNORM back buffer: the bytes are already display values
        for (u32 i = 0; i < u32(ProjectTemplate::Count); ++i)
            if (Result<rhi::Texture*> t = render::LoadTexture(*device, String::Format("/Engine/Templates/{}.png", TemplateNames[i]), art))
                templateArt[i] = t.Value();
    }
    if (Result<ui::SvgImage> svg = ui::LoadSvg("/Engine/Icons/ZeqouLogo.svg"))
        logo = move(svg.Value());
    else
        ZX_LOG_WARN(LogEditor, "{}", svg.GetError().message);
    ui::Style& style = ui->GetStyle();
    style.windowPadding = Vec2(8.0f, 8.0f);
    style.frameRounding = 3.0f;
    style.windowRounding = 6.0f;
    style.itemSpacing = Vec2(6.0f, 5.0f);
    style.fontSize = 13.0f;
    uiRenderer.Reset(new ui::UIRenderer(*device));
    if (!uiRenderer->Init(swapChain->GetFormat()))
        return false;

    asset::AssetManagerSettings as;
    as.ddcDirectory = engineRoot + "/Intermediate/DDC";
    as.textureMaxSize = lite ? 1024 : 0;
    assets.Reset(new asset::AssetManager(as));
    animation.Reset(new AnimationSystem(*assets));
    RegisterBuiltinAssets(*assets);
    renderAssets.Reset(new RenderAssets(*device, *assets));
    prefabs.Reset(new PrefabLibrary(assets.Get(), contentRoot));
    materials.Reset(new MaterialLibrary(*assets, *renderAssets, contentRoot));
    {
        audio::AudioSettings audioSettings;
        if (CommandLine::HasFlag("noaudio"))
            audioSettings.backend = audio::AudioSettings::Backend::Null;
        audioEngine.Reset(new audio::AudioEngine());
        audioEngine->Initialize(audioSettings);
        audio.Reset(new AudioSystem(*audioEngine));
        audio->SetContentRoot(contentRoot);
        // Occlusion: something between the listener and the source (meshes; the source's own entity is ignored).
        audio->occlusion = [this](Vec3 from, Vec3 to) {
            const Vec3 d = to - from;
            const f32 dist = Length(d);
            if (dist < 0.5f || !renderAssets)
                return 0.0f;
            RaycastHit hit;
            return RaycastMeshes(world, *renderAssets, Ray(from, d / dist), hit, dist - 0.5f) ? 1.0f : 0.0f;
        };
    }
    // Colliding particles: the physics world while playing, else the meshes.
    particles.raycast = [this](Vec3 from, Vec3 to, Vec3& point, Vec3& normal) {
        const Vec3 d = to - from;
        const f32 dist = Length(d);
        if (dist < 1e-3f)
            return false;
        if (physics.IsRunning()) {
            PhysicsHit hit;
            if (!physics.Raycast(from, d / dist, dist, hit))
                return false;
            point = hit.point;
            normal = hit.normal;
            return true;
        }
        RaycastHit hit;
        if (!renderAssets || !RaycastMeshes(world, *renderAssets, Ray(from, d / dist), hit, dist))
            return false;
        point = hit.point;
        normal = hit.normal;
        return true;
    };
    scripts.Reset(new ScriptSystem());
    scripts->SetContentRoot(contentRoot);
    animation->contentRoot = contentRoot; // Animator graphs (.zanimgraph)
    gameUI.contentRoot = contentRoot;     // UI Canvas styles (.zstyle)
    localization.Load(contentRoot);       // Localization/*.zloc (the editor previews; the player's choice isn't saved here)
    gameUI.localization = &localization;
    scripts->localization = &localization;
    animation->raycast = [this](Vec3 from, Vec3 to, ecs::Entity ignore, Vec3& point, Vec3& normal) {
        const Vec3 d = to - from;
        const f32 dist = Length(d);
        PhysicsHit hit;
        if (dist < 1e-3f || !physics.IsRunning() || !physics.Raycast(from, d / dist, dist, hit, ~0u, ignore))
            return false;
        point = hit.point;
        normal = hit.normal;
        return true;
    };
    scripts->audio = audio.Get();
    scripts->materialName = [this](asset::AssetId id) {
        const MaterialLibrary::Material* m = materials ? materials->Find(id) : nullptr;
        return m ? String(Path::Stem(m->path)) : String();
    };
    scripts->onLog = [](StringView message, bool isError) {
        if (isError)
            ZX_LOG_ERROR(LogScript, "{}", message);
        else
            ZX_LOG_INFO(LogScript, "{}", message);
    };
    game.Reset(new GameModuleSystem());
    game->onLog = [](StringView message, bool isError) {
        if (isError)
            ZX_LOG_ERROR(LogScript, "{}", message);
        else
            ZX_LOG_INFO(LogScript, "{}", message);
    };
    inputActions = InputActionMap::Defaults();
    viewport.Reset(new ViewportRenderer(*device, *renderAssets));
    // Decal images resolve through the open project's material library (same texture assets as material graphs).
    viewport->decalTextures = [this](StringView path, bool normalMap) {
        return materials ? materials->TextureAsset(path, normalMap) : asset::AssetId();
    };
    debugDraw.Reset(new DebugDraw(*device));
    pool.Reset(new TransientResourcePool(*device));
    viewport->SetParticleSystem(&particles);
    viewport->SetAnimationSystem(animation.Get());
    if (!renderAssets->Init() || !viewport->Init(lite, Format::R8G8B8A8_UNORM) ||
        !debugDraw->Init(viewport->HdrFormat(), viewport->DepthFormat()))
        return false;
    viewport->Settings().renderScale = 1.0f; // the viewport is smaller than the screen; keep it crisp
    RegisterEditorConsole(*this);
    LoadEditorSettings(*this);
    thumbnails.Reset(new ThumbnailCache(*device, *assets, *renderAssets));
    if (!thumbnails->Init())
        ZX_LOG_WARN(LogEditor, "Thumbnail renderer unavailable");
    thumbnails->SetMaterialLibrary(materials.Get());

    String layout;
    if (fs::ReadTextFile(m_layoutPath, layout) && !CommandLine::HasFlag("resetlayout"))
        ui::LoadDockLayout(layout);

    StringView screenshot;
    if (CommandLine::GetValue("screenshot", screenshot))
        m_screenshotPath = screenshot;
    m_screenshotEarly = CommandLine::HasFlag("screenshotnow");
    m_screenshotViewport = CommandLine::HasFlag("screenshotviewport");
    m_undoTest = CommandLine::HasFlag("undotest");
    m_pieTest = CommandLine::HasFlag("pietest");
    m_materialTest = CommandLine::HasFlag("materialtest");
    m_scriptTest = CommandLine::HasFlag("scripttest");
    m_cppTestStage = CommandLine::HasFlag("cpptest") ? 1u : 0u;
    m_postTestStage = CommandLine::HasFlag("posttest") ? 1u : 0u;
    m_reflTestStage = CommandLine::HasFlag("reflectiontest") ? 1u : 0u;
    m_decalTestStage = CommandLine::HasFlag("decaltest") ? 1u : 0u;
    m_hdriTestStage = CommandLine::HasFlag("hdritest") ? 1u : 0u;
    m_physicsTestStage = CommandLine::HasFlag("physicstest") ? 1u : 0u;
    m_toolTest = CommandLine::HasFlag("tooltest") ? 300u : 0u;
    m_audioTestStage = CommandLine::HasFlag("audiotest") ? 1u : 0u;
    StringView execArg;
    if (CommandLine::GetValue("exec", execArg))
        m_execCommands = execArg;
    StringView saveScene;
    if (CommandLine::GetValue("savescene", saveScene))
        m_saveScenePath = saveScene;
    StringView autotest;
    if (CommandLine::GetValue("autotest", autotest))
        m_autotestFrames = u64(Max<i64>(1, ParseInt(autotest).ValueOr(120)));
    else if (CommandLine::HasFlag("autotest"))
        m_autotestFrames = 120;

    ZX_LOG_INFO(LogEditor, "ZeqouX3D Editor {} ({}) on {} [{}]", EngineVersionString, BuildConfigurationName,
                device->GetCaps().adapter.name, lite ? "Lite" : "Full");
    // Startup: -project opens a project; -scene / autotests use the Samples project; otherwise the Project Browser.
    StringView projectArg, sceneArg, newProjectArg, templateArg, recentArg, pageArg;
    if (CommandLine::GetValue("recentfile", recentArg))
        recentProjectsFile = recentArg;
    browser.newProjectPage = CommandLine::GetValue("browserpage", pageArg) && pageArg == "new";
    if (CommandLine::GetValue("newproject", newProjectArg)) {
        CommandLine::GetValue("template", templateArg);
        NewProjectOptions options;
        options.templ = templateArg == "sample"        ? ProjectTemplate::Sample
                        : templateArg == "firstperson" ? ProjectTemplate::FirstPerson
                        : templateArg == "thirdperson" ? ProjectTemplate::ThirdPerson
                        : templateArg == "water"       ? ProjectTemplate::Water
                        : templateArg == "physics"     ? ProjectTemplate::Physics
                        : templateArg == "vehicle"     ? ProjectTemplate::Vehicle
                        : templateArg == "empty"       ? ProjectTemplate::Empty
                        : templateArg == "2017"        ? ProjectTemplate::Classic2017
                        : templateArg == "2011"        ? ProjectTemplate::Classic2011
                        : templateArg == "2008"        ? ProjectTemplate::Classic2008
                                                       : ProjectTemplate::Blank;
        options.starterContent = CommandLine::HasFlag("starter");
        options.documentation = !CommandLine::HasFlag("nodocs");
        options.gitRepository = CommandLine::HasFlag("git");
        StringView packsArg; // -packs=SurvivalPack,StylizedNature or -packs=all
        if (CommandLine::GetValue("packs", packsArg)) {
            for (const AssetPackInfo& pack : ListAssetPacks(engineRoot))
                if (packsArg == "all" || packsArg.Contains(pack.folder.View()))
                    options.assetPacks.PushBack(pack.folder);
        }
        StringView languageArg;
        if (CommandLine::GetValue("language", languageArg) && languageArg.EqualsIgnoreCase("cpp"))
            options.language = ProjectLanguage::Cpp;
        // -cpptest starts from a fresh test project each time (only under Saved/TestProjects).
        if (m_cppTestStage && Path::Normalize(newProjectArg).Contains("Saved/TestProjects/"))
            platform::DeleteDirectoryTree(newProjectArg);
        if (!CreateAndOpenProject(Path::Parent(newProjectArg), Path::FileName(newProjectArg), options)) {
            ZX_LOG_ERROR(LogEditor, "Can't create project {}: {}", newProjectArg, browser.error);
            ShowProjectBrowser();
        }
    } else if (CommandLine::GetValue("project", projectArg)) {
        if (!OpenProject(projectArg))
            ShowProjectBrowser();
    } else if (CommandLine::HasFlag("projectbrowser") || (!CommandLine::GetValue("scene", sceneArg) && !m_autotestFrames)) {
        ShowProjectBrowser();
    } else if (!OpenProject(engineRoot + "/Samples/Samples.zproj")) {
        BuildDefaultScene();
    }
    StringView browseArg;
    if (CommandLine::GetValue("browse", browseArg) && mode == EditorMode::Editing)
        panels.browserCurrent = Path::Join(contentRoot, browseArg); // -browse=<dir under Content> (screenshots/tests)
    return true;
}

// --- Projects -----------------------------------------------------------------------------------------------------------

void Editor::ShowProjectBrowser()
{
    NewScene();
    project = ProjectInfo();
    SetupGameModule(*this);
    mode = EditorMode::ProjectBrowser;
    browser.recentLoaded = false;
    browser.error.Clear();
    window->SetTitle("ZeqouX3D Project Browser");
}

bool Editor::OpenProject(StringView descriptorPath)
{
    Result<ProjectInfo> loaded = LoadProject(descriptorPath);
    if (!loaded) {
        browser.error = loaded.GetError().message;
        ZX_LOG_ERROR(LogEditor, "Can't open project: {}", browser.error);
        return false;
    }
    NewScene();
    project = move(loaded.Value());
    contentRoot = project.contentDir;
    platform::CreateDirectoryTree(contentRoot);
    prefabs.Reset(new PrefabLibrary(assets.Get(), contentRoot));
    materials.Reset(new MaterialLibrary(*assets, *renderAssets, contentRoot));
    if (thumbnails)
        thumbnails->SetMaterialLibrary(materials.Get());
    if (scripts)
        scripts->SetContentRoot(contentRoot);
    if (audio)
        audio->SetContentRoot(contentRoot);
    if (animation) {
        animation->contentRoot = contentRoot;
        gameUI.contentRoot = contentRoot;
        localization.Load(contentRoot);
        animation->Clear();
    }
    SetupGameModule(*this);
    scriptEditor.open = false;
    panels.browserCurrent = contentRoot;
    panels.browserListedDir.Clear();
    materials->ScanContent();
    mode = EditorMode::Editing;
    if (!m_autotestFrames || !recentProjectsFile.Empty())
        TouchRecentProject(project, recentProjectsFile);
    window->SetTitle(String::Format("{} - ZeqouX3D Editor", project.descriptor.name));
    ZX_LOG_INFO(LogEditor, "Opened project '{}' ({})", project.descriptor.name, project.root);

    StringView sceneArg;
    const String startup = project.descriptor.startupScene.Empty() ? String() : Path::Join(project.root, project.descriptor.startupScene);
    if (CommandLine::GetValue("scene", sceneArg) || Path::Normalize(project.root) == Path::Normalize(engineRoot + "/Samples"))
        BuildDefaultScene(); // sample content (Sponza) or the scene given on the command line
    else if (!startup.Empty() && platform::FileExists(startup))
        OpenScene(startup);
    return true;
}

bool Editor::CreateAndOpenProject(StringView parentDirectory, StringView name, const NewProjectOptions& options)
{
    ProjectDescriptor settings;
    settings.language = ProjectLanguageName(options.language);
    // The "Graphics 20xx" templates set the project's graphics generation (Project Settings can change it later).
    settings.graphicsEra = options.templ == ProjectTemplate::Classic2017   ? "2017"
                           : options.templ == ProjectTemplate::Classic2011 ? "2011"
                           : options.templ == ProjectTemplate::Classic2008 ? "2008"
                                                                            : "modern";
    Result<ProjectInfo> created = CreateProject(parentDirectory, name, settings);
    if (!created) {
        browser.error = created.GetError().message;
        return false;
    }
    // Template levels are built from the Starter Content materials.
    if (options.starterContent || (options.templ != ProjectTemplate::Blank && options.templ != ProjectTemplate::Empty))
        ZX_LOG_INFO(LogEditor, "Starter Content: {} files", WriteStarterContent(created->contentDir));
    const bool characterTemplate = options.templ == ProjectTemplate::FirstPerson || options.templ == ProjectTemplate::ThirdPerson ||
                                   options.templ == ProjectTemplate::Water || options.templ == ProjectTemplate::Physics;
    if (characterTemplate)
        WriteGameplayScripts(created->contentDir);
    for (const String& pack : options.assetPacks)
        ZX_LOG_INFO(LogEditor, "Asset pack {}: {} files", pack, CopyAssetPack(engineRoot, pack, created->contentDir));
    if (options.language == ProjectLanguage::Cpp) {
        // Source/<Name>: built in the background as soon as the project opens.
        const String module = GameModuleName(created.Value());
        if (WriteGameModuleSource(created->root, module, engineRoot))
            ZX_LOG_INFO(LogEditor, "C++ game module: Source/{}/{}.cpp", module, module);
    }
    if (options.documentation)
        ZX_LOG_INFO(LogEditor, "Documentation: {} files (README.md, Docs/, AGENTS.md)", WriteProjectDocs(created.Value()));
    if (options.gitRepository) {
        fs::WriteTextFile(created->root + "/.gitattributes",
                          "* text=auto\n*.zs text eol=lf\n*.zscene text\n*.zmat text\n*.zproj text\n"
                          "*.png binary\n*.jpg binary\n*.glb binary\n*.bin binary\n*.dds binary\n");
        i32 code = -1;
        const bool started = platform::RunProcess("git init -b main", created->root, &code);
        if (!started || code != 0) {
            ZX_LOG_WARN(LogEditor, "Git: 'git init' failed - is Git installed and on PATH?");
        } else {
            // The first commit needs a git identity; without one the repository simply starts empty.
            platform::RunProcess("git add -A", created->root, &code);
            platform::RunProcess("git commit -q -m \"Create project\"", created->root, &code);
            ZX_LOG_INFO(LogEditor, code == 0 ? "Git: repository created with an initial commit"
                                             : "Git: repository created (no commit: set user.name / user.email to commit)");
        }
    }
    String helmet;
    if (options.templ == ProjectTemplate::Sample) {
        Result<String> copy = CopySourceWithDependencies(engineRoot + "/Samples/Content/DamagedHelmet/DamagedHelmet.gltf",
                                                         created->contentDir + "/DamagedHelmet");
        if (copy)
            helmet = copy.Value();
        else
            ZX_LOG_WARN(LogEditor, "Sample template: {}", copy.GetError().message);
    }
    if (!OpenProject(created->descriptorPath))
        return false;
    // The Physics Playground places the joint props (door, bridge, lamp, elevator, jump pad) from the prefabs.
    NewProjectOptions prefabOptions = options;
    if (options.templ == ProjectTemplate::Physics)
        prefabOptions.starterProps = true;
    if (options.templ == ProjectTemplate::Vehicle) // the car and the crate stacks come from the prefabs
        prefabOptions.starterCharacters = prefabOptions.starterProps = true;
    const bool needsPrefabs = options.templ == ProjectTemplate::Physics || options.templ == ProjectTemplate::Vehicle;
    const u32 starterPrefabs = options.starterContent || needsPrefabs ? WriteStarterPrefabs(prefabOptions) : 0u;
    const String mainScene = ScenesDirectory() + "/Main.zscene";
    if (!helmet.Empty()) {
        ImportAsset(helmet, Vec3(0.0f, 1.2f, 0.0f), false);
        const Vec3 positions[2] = {Vec3(-2.0f, 2.5f, 1.5f), Vec3(2.0f, 1.5f, 2.0f)};
        for (u32 i = 0; i < 2; ++i) {
            const ecs::Entity e = CreateEntity(String::Format("Point Light {}", i + 1), Transform(positions[i]));
            PointLight l;
            l.lumens = 4000.0f;
            l.color = i == 0 ? Vec3(1.0f, 0.85f, 0.7f) : Vec3(0.6f, 0.75f, 1.0f);
            world.Add(e, l);
        }
        // A grid floor and a pedestal under the model.
        auto starterMaterial = [&](StringView name) {
            const String path = String::Format("{}/StarterContent/Materials/{}.zmat", contentRoot, name);
            return materials && platform::FileExists(path) ? materials->LoadFile(path) : Guid();
        };
        auto block = [&](StringView label, BuiltinShape shape, Vec3 center, Vec3 size, StringView mat) {
            Transform t(center);
            t.scale = size;
            const ecs::Entity e = CreateEntity(label, t);
            MeshRenderer mr;
            mr.mesh = builtin::Mesh(shape);
            if (const Guid g = starterMaterial(mat); g.IsValid())
                mr.materials.PushBack(g);
            world.Add(e, mr);
            EnsureEntityGuid(world, e);
        };
        block("Floor", BuiltinShape::Cube, Vec3(0, -0.25f, 0), Vec3(20, 0.5f, 20), "M_Proto_Grey");
        block("Pedestal", BuiltinShape::Cylinder, Vec3(0, 0.33f, 0), Vec3(1.2f, 0.66f, 1.2f), "M_Proto_Light");
        camera.position = Vec3(2.3f, 2.0f, 4.2f);
        camera.LookAt(Vec3(0.0f, 1.15f, 0.0f));
        m_saveAfterImports = mainScene;
    } else {
        if (options.templ == ProjectTemplate::Water)
            BuildWaterLevel();
        else if (options.templ == ProjectTemplate::Physics)
            BuildPhysicsLevel();
        else if (options.templ == ProjectTemplate::Vehicle)
            BuildVehicleLevel();
        else if (characterTemplate)
            BuildCharacterLevel(options.templ == ProjectTemplate::ThirdPerson);
        else if (options.templ == ProjectTemplate::Classic2017 || options.templ == ProjectTemplate::Classic2011 ||
                 options.templ == ProjectTemplate::Classic2008)
            BuildEraLevel();
        else if (options.templ == ProjectTemplate::Empty)
            ; // nothing: an empty scene
        else if (options.starterContent)
            BuildStarterShowcase(starterPrefabs > 0);
        SaveSceneTo(mainScene);
    }
    return true;
}

void Editor::BuildCharacterLevel(bool thirdPerson)
{
    // A prototype arena (outside the undo history), in the spirit of the UE template levels: grid-textured floor and
    // walls, ramps, stairs to a balcony, jump platforms, physics crates and balls to push around, plus the player set
    // up for the template.
    auto material = [&](StringView name) {
        const String path = String::Format("{}/StarterContent/Materials/{}.zmat", contentRoot, name);
        if (!platform::FileExists(path) || !materials)
            return Guid();
        const MaterialLibrary::Material* m = materials->FindByPath(path);
        return m ? m->guid : materials->LoadFile(path);
    };
    auto shape = [&](StringView label, BuiltinShape s, Vec3 center, Vec3 size, StringView mat, Quat rotation = {}, ecs::Entity parent = {}) {
        Transform t(center, rotation);
        t.scale = size;
        const ecs::Entity e = CreateEntity(label, t, parent);
        MeshRenderer mr;
        mr.mesh = builtin::Mesh(s);
        if (const Guid g = material(mat); g.IsValid())
            mr.materials.PushBack(g);
        world.Add(e, mr);
        EnsureEntityGuid(world, e);
        return e;
    };
    auto block = [&](StringView label, Vec3 min, Vec3 max, StringView mat) {
        return shape(label, BuiltinShape::Cube, (min + max) * 0.5f, max - min, mat);
    };
    auto impact = [&](ecs::Entity e, StringView sound) {
        ImpactSound s;
        s.sound = String::Format("StarterContent/Sounds/Impacts/{}", sound);
        world.Add(e, s);
    };
    auto physicsBox = [&](StringView label, Vec3 center, Vec3 size, StringView mat, f32 mass, StringView sound) {
        const ecs::Entity e = shape(label, BuiltinShape::Cube, center, size, mat);
        RigidBody rb;
        rb.mass = mass;
        world.Add(e, rb);
        world.Add(e, BoxCollider{});
        impact(e, sound);
        return e;
    };
    // Reflections of the arena.
    {
        const ecs::Entity probe = CreateEntity("Arena Reflections", Transform(Vec3(0.0f, 3.0f, 0.0f)));
        ReflectionProbe rp;
        rp.extent = Vec3(25.0f, 6.0f, 25.0f);
        rp.blendDistance = 3.0f;
        world.Add(probe, rp);
        EnsureEntityGuid(world, probe);
    }
    // Floor and walls.
    block("Floor", Vec3(-25, -1, -25), Vec3(25, 0, 25), "M_Proto_Grey");
    block("Wall North", Vec3(-25, 0, 24), Vec3(25, 4, 25), "M_Proto_Light");
    block("Wall South", Vec3(-25, 0, -25), Vec3(25, 4, -24), "M_Proto_Light");
    block("Wall East", Vec3(24, 0, -24), Vec3(25, 4, 24), "M_Proto_Light");
    block("Wall West", Vec3(-25, 0, -24), Vec3(-24, 4, 24), "M_Proto_Light");

    // Balcony along the north wall: stairs on one side, a ramp on the other.
    block("Balcony", Vec3(-12, 0, 16), Vec3(12, 2, 24), "M_Proto_Light");
    block("Balcony Trim", Vec3(-12, 2, 15.8f), Vec3(12, 2.15f, 16.2f), "M_Proto_Orange");
    for (u32 i = 0; i < 8; ++i)
        block(String::Format("Stair {}", i + 1), Vec3(12, 0, 16 + f32(i) * 1.0f), Vec3(16, 0.25f * f32(i + 1), 17 + f32(i) * 1.0f), "M_Proto_Light");
    {
        // A 20 degree ramp from the floor up to the balcony's west end.
        const f32 angle = DegToRad(20.0f), rise = 2.0f, run = rise / Tan(angle), len = Sqrt(run * run + rise * rise);
        const Quat tilt = Quat::FromAxisAngle(Vec3(0, 0, 1), angle);
        shape("Ramp", BuiltinShape::Cube, Vec3(-12.0f - run * 0.5f, rise * 0.5f - Cos(angle) * 0.25f, 20.0f), Vec3(len, 0.5f, 6.0f), "M_Proto_Teal", tilt);
    }

    // Jump course: platforms rising towards the east wall.
    const char* const jumpMats[] = {"M_Proto_Blue", "M_Proto_Orange", "M_Proto_Blue", "M_Proto_Orange"};
    for (u32 i = 0; i < 4; ++i) {
        const f32 x = 8.0f + f32(i) * 3.5f, h = 0.6f + f32(i) * 0.6f;
        block(String::Format("Jump Platform {}", i + 1), Vec3(x - 1.25f, 0, -2 - f32(i) * 2.5f), Vec3(x + 1.25f, h, 0.5f - f32(i) * 2.5f), jumpMats[i]);
    }
    // Low walls and pillars for cover on the west side.
    block("Cover 1", Vec3(-14, 0, -6), Vec3(-8, 1.2f, -5.5f), "M_Proto_Light");
    block("Cover 2", Vec3(-18, 0, 2), Vec3(-17.5f, 1.2f, 8), "M_Proto_Light");
    shape("Pillar 1", BuiltinShape::Cylinder, Vec3(-6, 2, 6), Vec3(1.2f, 4, 1.2f), "M_Proto_Teal");
    shape("Pillar 2", BuiltinShape::Cylinder, Vec3(6, 2, 6), Vec3(1.2f, 4, 1.2f), "M_Proto_Teal");

    // Physics props: a stack of crates, a pyramid of blue cubes and two balls.
    for (u32 i = 0; i < 3; ++i)
        physicsBox(String::Format("Crate {}", i + 1), Vec3(-3.0f + f32(i) * 1.05f, 0.5f, 10.0f), Vec3(1.0f), "M_Wood", 30.0f, "Impact_Wood_Medium");
    physicsBox("Crate 4", Vec3(-2.47f, 1.5f, 10.0f), Vec3(1.0f), "M_Wood", 30.0f, "Impact_Wood_Medium");
    physicsBox("Crate 5", Vec3(-1.42f, 1.5f, 10.0f), Vec3(1.0f), "M_Wood", 30.0f, "Impact_Wood_Medium");
    physicsBox("Crate 6", Vec3(-1.95f, 2.5f, 10.0f), Vec3(1.0f), "M_Wood", 30.0f, "Impact_Wood_Medium");
    for (u32 row = 0; row < 3; ++row)
        for (u32 i = 0; i < 3 - row; ++i)
            physicsBox(String::Format("Cube {}-{}", row + 1, i + 1), Vec3(-12.0f + f32(i) * 0.62f + f32(row) * 0.31f, 0.3f + f32(row) * 0.6f, 12.0f),
                       Vec3(0.6f), "M_Proto_Blue", 8.0f, "Impact_Plank");
    for (u32 i = 0; i < 2; ++i) {
        const ecs::Entity ball = shape(String::Format("Ball {}", i + 1), BuiltinShape::Sphere, Vec3(3.0f + f32(i) * 2.0f, 0.5f, 4.0f), Vec3(1.0f),
                                       i == 0 ? "M_Metal_Gold" : "M_Plastic_Red");
        RigidBody rb;
        rb.mass = 10.0f;
        rb.bounciness = 0.3f;
        world.Add(ball, rb);
        world.Add(ball, SphereCollider{});
        impact(ball, i == 0 ? "Impact_Metal_Heavy" : "Impact_Soft");
        world.Get<ImpactSound>(ball)->rollSound = "StarterContent/Sounds/Props/Rolling_Loop.ogg";
    }

    // Player: feet at the entity, a Character Controller and the template's controller script.
    const ecs::Entity player = CreateEntity("Player", Transform(Vec3(0, 0, -6)));
    world.Add(player, CharacterController{});
    EnsureEntityGuid(world, player);
    if (!thirdPerson) {
        world.Add(player, Script{String("Scripts/FirstPersonController.zs"), String()});
        const ecs::Entity cam = CreateEntity("Camera", Transform(Vec3(0, 1.65f, 0)), player);
        world.Add(cam, Camera{});
        EnsureEntityGuid(world, cam);
        camera.position = Vec3(-14.0f, 7.0f, -20.0f);
        camera.LookAt(Vec3(0.0f, 0.5f, 8.0f));
    } else {
        world.Add(player, Script{String("Scripts/ThirdPersonController.zs"), String()});
        // Bouncing slimes (morph targets) by the path.
        AddSlime(Transform(Vec3(-3.5f, 0, 1.5f), Quat::FromAxisAngle(Vec3(0, 1, 0), DegToRad(150.0f))), 1.0f);
        AddSlime(Transform(Vec3(-4.8f, 0, 3.2f), Quat::FromAxisAngle(Vec3(0, 1, 0), DegToRad(200.0f)), Vec3(0.7f)), 1.3f);
        AddSlime(Transform(Vec3(-2.4f, 0, 3.6f), Quat::FromAxisAngle(Vec3(0, 1, 0), DegToRad(170.0f)), Vec3(0.55f)), 1.6f);
        if (!AddAnimatedCharacter(player)) {
            // A simple mannequin (part of the player, so it doesn't block it): body, head, visor, arms.
            shape("Body", BuiltinShape::Cylinder, Vec3(0, 0.95f, 0), Vec3(0.55f, 1.1f, 0.4f), "M_Proto_Light", {}, player);
            shape("Head", BuiltinShape::Sphere, Vec3(0, 1.7f, 0), Vec3(0.36f), "M_Proto_Light", {}, player);
            shape("Visor", BuiltinShape::Cube, Vec3(0, 1.72f, 0.15f), Vec3(0.26f, 0.09f, 0.08f), "M_Emissive_Cyan", {}, player);
            shape("Arm L", BuiltinShape::Cylinder, Vec3(-0.36f, 1.05f, 0), Vec3(0.14f, 0.7f, 0.14f), "M_Proto_Grey", {}, player);
            shape("Arm R", BuiltinShape::Cylinder, Vec3(0.36f, 1.05f, 0), Vec3(0.14f, 0.7f, 0.14f), "M_Proto_Grey", {}, player);
            shape("Leg L", BuiltinShape::Cylinder, Vec3(-0.13f, 0.25f, 0), Vec3(0.17f, 0.5f, 0.17f), "M_Proto_Grey", {}, player);
            shape("Leg R", BuiltinShape::Cylinder, Vec3(0.13f, 0.25f, 0), Vec3(0.17f, 0.5f, 0.17f), "M_Proto_Grey", {}, player);
        }
        const ecs::Entity cam = CreateEntity("Camera", Transform(Vec3(0, 3, -11)));
        world.Add(cam, Camera{});
        world.Add(cam, Script{String("Scripts/ThirdPersonCamera.zs"), String()});
        EnsureEntityGuid(world, cam);
        camera.position = Vec3(3.0f, 2.6f, -11.0f);
        camera.LookAt(Vec3(0.0f, 1.2f, -2.0f));
    }
    environment.sunElevation = DegToRad(48.0f);
    environment.sunAzimuth = DegToRad(135.0f);
    AddAtmosphere(0.004f, 0.3f);
}

void Editor::AddSlime(const Transform& at, f32 speed)
{
    // The Starter Content slime (skin + morph targets): bounces with squash and stretch, squishes on landing.
    const String model = contentRoot + "/StarterContent/Characters/SK_Blob.glb";
    if (!assets || !platform::FileExists(model))
        return;
    const asset::AssetId id = assets->LoadPath(model);
    assets->WaitIdle();
    const asset::PrefabAsset* prefab = assets->GetPrefab(id);
    if (!prefab)
        return;
    const ecs::Entity root = zx::InstantiatePrefab(world, *prefab, at, {}, assets.Get());
    if (scene::EntityName* n = world.Get<scene::EntityName>(root))
        n->value = "Slime";
    Array<ecs::Entity> members;
    CollectHierarchy(world, root, members);
    for (ecs::Entity e : members) {
        EnsureEntityGuid(world, e);
        if (Animator* a = world.Get<Animator>(e)) {
            for (const asset::AssetId& clip : a->clips)
                if (assets->GetName(clip).View().EqualsIgnoreCase("Bounce"))
                    a->clip = clip;
            a->speed = speed;
        }
    }
    world.Add(root, Script{String("StarterContent/Scripts/Slime.zs"), String::Format(R"json({{"period":{}}})json", 1.0333f / speed)});
}

bool Editor::AddAnimatedCharacter(ecs::Entity player)
{
    // The Starter Content character (skinned glTF) under the player, driven by the locomotion graph: the Character
    // Controller's speed and ground contact pick Idle / Walk / Run / Jump.
    const String model = contentRoot + "/StarterContent/Characters/SK_Human.glb";
    if (!assets || !platform::FileExists(model))
        return false;
    const asset::AssetId id = assets->LoadPath(model);
    assets->WaitIdle(); // the template is built once, at project creation
    const asset::PrefabAsset* prefab = assets->GetPrefab(id);
    if (!prefab)
        return false;
    const ecs::Entity root = zx::InstantiatePrefab(world, *prefab, Transform(), player, assets.Get());
    Array<ecs::Entity> stack;
    stack.PushBack(root);
    while (!stack.Empty()) {
        const ecs::Entity e = stack.Back();
        stack.PopBack();
        EnsureEntityGuid(world, e);
        if (Animator* a = world.Get<Animator>(e))
            a->graph = "StarterContent/Characters/Locomotion.zanimgraph";
        stack.Append(scene::GetChildren(world, e));
    }
    return true;
}

void Editor::BuildPhysicsLevel()
{
    // Physics Playground (outside the undo history): stations around a grid floor in rolling hills - stacks, dominoes,
    // a ramp with balls / cylinders / rocks (convex hulls), joints (bridge, door, lamp, elevator, chain, seesaw), a
    // raycast scanner and a first-person player with a physics gun (left click pushes, hold right to carry).
    auto material = [&](StringView name) {
        const String path = String::Format("{}/StarterContent/Materials/{}.zmat", contentRoot, name);
        if (!platform::FileExists(path) || !materials)
            return Guid();
        const MaterialLibrary::Material* m = materials->FindByPath(path);
        return m ? m->guid : materials->LoadFile(path);
    };
    auto shape = [&](StringView label, BuiltinShape s, Vec3 center, Vec3 size, StringView mat, Quat rotation = {}, ecs::Entity parent = {}) {
        Transform t(center, rotation);
        t.scale = size;
        const ecs::Entity e = CreateEntity(label, t, parent);
        MeshRenderer mr;
        mr.mesh = builtin::Mesh(s);
        if (const Guid g = material(mat); g.IsValid())
            mr.materials.PushBack(g);
        world.Add(e, mr);
        EnsureEntityGuid(world, e);
        return e;
    };
    auto body = [&](ecs::Entity e, f32 mass, StringView sound) {
        RigidBody rb;
        rb.mass = mass;
        world.Add(e, rb);
        if (!sound.Empty()) {
            ImpactSound s;
            s.sound = String::Format("StarterContent/Sounds/Impacts/{}", sound);
            world.Add(e, s);
        }
        return e;
    };
    auto box = [&](StringView label, Vec3 center, Vec3 size, StringView mat, f32 mass, StringView sound, Quat rotation = {}) {
        const ecs::Entity e = shape(label, BuiltinShape::Cube, center, size, mat, rotation);
        world.Add(e, BoxCollider{});
        return body(e, mass, sound);
    };
    auto prefab = [&](StringView name, Vec3 position, f32 yaw) {
        const String rel = String::Format("StarterContent/Prefabs/{}.zprefab", name);
        if (prefabs && platform::FileExists(Path::Join(contentRoot, rel)))
            InstantiatePrefabFile(world, *prefabs, rel, Transform(position, Quat::FromAxisAngle(Vec3(0, 1, 0), DegToRad(yaw))));
    };

    // Ground: a grid floor in the flat middle of a terrain.
    shape("Floor", BuiltinShape::Cube, Vec3(0, -0.5f, 0), Vec3(64, 1, 64), "M_Proto_Grey");
    {
        Terrain t;
        t.size = 360.0f;
        t.height = 40.0f;
        t.flatCenter = 50.0f;
        t.featureSize = 120.0f;
        t.material = material("M_Terrain");
        Array<f32> heights;
        GenerateTerrainHeights(t, heights);
        const u32 n = TerrainSamples(t);
        const ecs::Entity terrain = CreateEntity("Terrain", Transform(Vec3(0.0f, -heights[usize(n / 2) * n + n / 2] - 0.2f, 0.0f)));
        world.Add(terrain, t);
        EnsureEntityGuid(world, terrain);
    }

    // 1. Stacks (north-west): a tower, a pyramid and a brick wall.
    for (u32 i = 0; i < 10; ++i)
        box(String::Format("Tower {}", i + 1), Vec3(-20.0f, 0.4f + f32(i) * 0.8f, 14.0f), Vec3(0.8f), i % 2 ? "M_Proto_Orange" : "M_Proto_Blue", 12.0f,
            "Impact_Plank", Quat::FromAxisAngle(Vec3(0, 1, 0), DegToRad(f32(i) * 9.0f)));
    for (u32 row = 0; row < 5; ++row)
        for (u32 i = 0; i < 5 - row; ++i)
            box(String::Format("Pyramid {}-{}", row + 1, i + 1), Vec3(-15.0f + f32(i) * 0.72f + f32(row) * 0.36f, 0.35f + f32(row) * 0.7f, 14.0f),
                Vec3(0.7f), "M_Wood", 15.0f, "Impact_Wood_Medium");
    for (u32 row = 0; row < 6; ++row)
        for (u32 i = 0; i < 8 - (row % 2); ++i)
            box(String::Format("Brick {}-{}", row + 1, i + 1),
                Vec3(-21.0f + f32(i) * 0.92f + (row % 2 ? 0.46f : 0.0f), 0.23f + f32(row) * 0.46f, 20.0f), Vec3(0.9f, 0.45f, 0.45f), "M_Bricks", 9.0f,
                "Impact_Wood_Light");

    // 2. Dominoes (north-east): an arc; the first one leans, so the chain falls when Play starts.
    for (u32 i = 0; i < 36; ++i) {
        const f32 a = DegToRad(-60.0f + f32(i) * 3.8f), r = 9.0f;
        const Vec3 p(12.0f + Sin(a) * r, 0.0f, 12.0f + Cos(a) * r - 6.0f);
        Quat rot = Quat::FromAxisAngle(Vec3(0, 1, 0), a + Pi * 0.5f);
        if (i == 0)
            rot = rot * Quat::FromAxisAngle(Vec3(1, 0, 0), DegToRad(24.0f));
        box(String::Format("Domino {}", i + 1), p + Vec3(0, 0.62f, 0), Vec3(0.5f, 1.2f, 0.12f), i % 2 ? "M_Proto_Light" : "M_Plastic_Red", 2.0f,
            "Impact_Plank", rot);
    }

    // 3. Ramp (west): balls with rolling friction and cylinders lying across roll down; convex rocks tumble nearby.
    {
        const f32 angle = DegToRad(20.0f), length = 14.0f;
        const Quat tilt = Quat::FromAxisAngle(Vec3(0, 0, 1), -angle);
        const Vec3 center(-18.0f, Sin(angle) * length * 0.5f, 2.0f);
        shape("Ramp Support", BuiltinShape::Cube, center + Vec3(-Cos(angle) * length * 0.5f + 0.4f, -Sin(angle) * length * 0.25f - 0.2f, 0.0f),
              Vec3(0.8f, Sin(angle) * length * 0.5f, 4.0f), "M_Proto_Light");
        shape("Ramp", BuiltinShape::Cube, center, Vec3(length, 0.4f, 5.0f), "M_Proto_Teal", tilt);
        shape("Ramp Rail L", BuiltinShape::Cube, center + Vec3(0, 0.4f, 2.6f), Vec3(length, 1.0f, 0.2f), "M_Proto_Light", tilt);
        shape("Ramp Rail R", BuiltinShape::Cube, center + Vec3(0, 0.4f, -2.6f), Vec3(length, 1.0f, 0.2f), "M_Proto_Light", tilt);
        const Vec3 down = tilt * Vec3(1, 0, 0), up = tilt * Vec3(0, 1, 0);
        auto onRamp = [&](f32 along, f32 across, f32 lift) { return center + down * along + up * (0.2f + lift) + Vec3(0, 0, across); };
        const char* const ballMats[] = {"M_Metal_Gold", "M_Plastic_Red", "M_Proto_Blue", "M_Rubber_Black"};
        for (u32 i = 0; i < 4; ++i) {
            const ecs::Entity ball = shape(String::Format("Ball {}", i + 1), BuiltinShape::Sphere, onRamp(-5.5f + f32(i) * 0.2f, -1.8f + f32(i) * 1.2f, 0.4f),
                                           Vec3(0.8f), ballMats[i]);
            world.Add(ball, SphereCollider{});
            body(ball, 6.0f, "Impact_Soft");
            world.Get<RigidBody>(ball)->rollingFriction = 0.04f;
            world.Get<ImpactSound>(ball)->rollSound = "StarterContent/Sounds/Props/Rolling_Loop.ogg";
            world.Get<RigidBody>(ball)->bounciness = i == 3 ? 0.6f : 0.2f;
        }
        for (u32 i = 0; i < 3; ++i) {
            // Lying across the ramp (axis along Z): they roll down.
            const ecs::Entity c = shape(String::Format("Barrel {}", i + 1), BuiltinShape::Cylinder, onRamp(-2.5f + f32(i) * 1.6f, 0.0f, 0.45f),
                                        Vec3(0.9f, 1.6f, 0.9f), "M_Paint_Olive", Quat::FromAxisAngle(Vec3(1, 0, 0), Pi * 0.5f));
            world.Add(c, CylinderCollider{});
            body(c, 20.0f, "Impact_Metal_Light");
            world.Get<ImpactSound>(c)->rollSound = "StarterContent/Sounds/Props/Rolling_Loop.ogg";
            world.Get<ImpactSound>(c)->rollVolume = 0.7f;
        }
        for (u32 i = 0; i < 5; ++i) {
            // Rocks: squashed spheres colliding as convex hulls of their mesh, dropped onto a heap.
            const ecs::Entity rock = shape(String::Format("Rock {}", i + 1), BuiltinShape::Sphere,
                                           Vec3(-8.0f + f32(i % 3) * 0.7f, 1.0f + f32(i) * 1.1f, 9.0f + f32(i % 2) * 0.6f), Vec3(1.1f, 0.6f, 0.8f),
                                           "M_Rock", Quat::FromAxisAngle(Normalize(Vec3(1, 2, 0.5f)), f32(i) * 0.9f));
            world.Add(rock, MeshCollider{.convex = true});
            body(rock, 40.0f, "Impact_Stone");
        }
    }

    // 4. Joints (east): the Starter Content props, a chain, a seesaw.
    prefab("Props/P_RopeBridge", Vec3(18.0f, 0.35f, -16.0f), 0.0f);
    prefab("Props/P_Door", Vec3(10.0f, 0.0f, -14.0f), 0.0f);
    prefab("Props/P_Elevator", Vec3(24.0f, 0.0f, 2.0f), 0.0f);
    prefab("Props/P_SwingingLamp", Vec3(4.0f, 4.5f, -18.0f), 0.0f);
    prefab("Props/P_JumpPad", Vec3(-4.0f, 0.0f, 4.0f), 0.0f);
    {
        shape("Chain Beam", BuiltinShape::Cube, Vec3(16.0f, 6.0f, -4.0f), Vec3(3.0f, 0.3f, 0.3f), "M_Metal_Steel");
        String previous;
        for (u32 i = 0; i < 6; ++i) {
            const String name = String::Format("Chain Link {}", i + 1);
            const ecs::Entity link = shape(name, BuiltinShape::Sphere, Vec3(16.0f + f32(i + 1) * 0.55f, 6.0f, -4.0f), Vec3(0.45f), "M_Metal_Steel");
            world.Add(link, SphereCollider{});
            body(link, 4.0f, "Impact_Metal_Light");
            BallJoint j;
            j.anchor = Vec3(-0.55f / 0.45f, 0, 0); // towards the previous link (local, scaled space)
            j.connectedBody = previous;
            world.Add(link, j);
            previous = name;
        }
        const ecs::Entity weight = shape("Chain Weight", BuiltinShape::Cube, Vec3(16.0f + 7.0f * 0.55f + 0.2f, 6.0f, -4.0f), Vec3(0.7f), "M_Metal_Gold");
        world.Add(weight, BoxCollider{});
        body(weight, 20.0f, "Impact_Metal_Heavy");
        BallJoint j;
        j.anchor = Vec3(-0.75f / 0.7f, 0, 0);
        j.connectedBody = previous;
        world.Add(weight, j);
    }
    {
        shape("Seesaw Base", BuiltinShape::Cube, Vec3(4.0f, 0.4f, 16.0f), Vec3(0.6f, 0.8f, 1.6f), "M_Proto_Light");
        const ecs::Entity plank = box("Seesaw", Vec3(4.0f, 0.9f, 16.0f), Vec3(6.0f, 0.2f, 1.2f), "M_Wood", 30.0f, "Impact_Wood_Heavy");
        HingeJoint h;
        h.axis = Vec3(0, 0, 1);
        world.Add(plank, h);
        box("Seesaw Weight", Vec3(6.6f, 6.0f, 16.0f), Vec3(0.8f), "M_Metal_Steel", 60.0f, "Impact_Metal_Heavy");
        box("Seesaw Crate", Vec3(1.6f, 1.4f, 16.0f), Vec3(0.6f), "M_Wood", 6.0f, "Impact_Wood_Medium");
    }

    // 5. Raycasts (south-west): a scanner sweeping a beam; the dot marks where its ray hits.
    {
        const ecs::Entity scanner = shape("Scanner", BuiltinShape::Cylinder, Vec3(-12.0f, 1.0f, -20.0f), Vec3(0.5f, 2.0f, 0.5f), "M_Proto_Light");
        world.Add(scanner, Script{String("StarterContent/Scripts/Scanner.zs"), String()});
        AudioSource hum;
        hum.clip = "StarterContent/Sounds/Props/Machine_Hum_Loop.ogg";
        hum.volume = 0.35f;
        hum.maxDistance = 12.0f;
        world.Add(scanner, hum);
        const ecs::Entity head = CreateEntity("Head", Transform(Vec3(0, 0.6f, 0)), scanner);
        EnsureEntityGuid(world, head);
        auto visual = [&](StringView label, BuiltinShape s, Vec3 size, StringView mat, ecs::Entity parent) {
            const ecs::Entity e = shape(label, s, Vec3(), size, mat, {}, parent);
            world.Get<MeshRenderer>(e)->collision = false; // the ray must not hit the beam
            return e;
        };
        visual("Beam", BuiltinShape::Cube, Vec3(0.04f, 0.04f, 1.0f), "M_Emissive_Cyan", head);
        const ecs::Entity dot = visual("Dot", BuiltinShape::Sphere, Vec3(0.25f), "M_Emissive_Cyan", head);
        PointLight glow;
        glow.color = Vec3(0.3f, 0.9f, 1.0f);
        glow.lumens = 300.0f;
        glow.range = 3.0f;
        world.Add(dot, glow);
        for (u32 i = 0; i < 5; ++i)
            box(String::Format("Target {}", i + 1), Vec3(-16.0f + f32(i) * 2.0f, 0.6f, -12.0f + f32(i % 2) * 1.5f), Vec3(1.2f), "M_Proto_Orange", 40.0f,
                "Impact_Plank");
    }

    // Player: first person, the camera carries the physics gun.
    const ecs::Entity player = CreateEntity("Player", Transform(Vec3(0, 0, -6)));
    world.Add(player, CharacterController{});
    world.Add(player, Script{String("Scripts/FirstPersonController.zs"), String()});
    EnsureEntityGuid(world, player);
    const ecs::Entity cam = CreateEntity("Camera", Transform(Vec3(0, 1.65f, 0)), player);
    world.Add(cam, Camera{});
    world.Add(cam, Script{String("StarterContent/Scripts/PhysicsGun.zs"), String()});
    EnsureEntityGuid(world, cam);

    camera.position = Vec3(-6.0f, 12.0f, -30.0f);
    camera.LookAt(Vec3(-2.0f, 0.5f, 4.0f));
    environment.sunElevation = DegToRad(42.0f);
    environment.sunAzimuth = DegToRad(140.0f);
    AddAtmosphere(0.003f, 0.35f);
}

void Editor::BuildVehicleLevel()
{
    // Vehicle Playground (outside the undo history): a soft-body car on a big grid floor in the hills - a jump, a
    // brick wall and concrete pillars to crash into, crate walls and barrels to plough through, a bumpy log road and a
    // steep hill. The car is the Starter Content prefab (Characters/P_SoftCar); a chase camera follows it.
    auto material = [&](StringView name) {
        const String path = String::Format("{}/StarterContent/Materials/{}.zmat", contentRoot, name);
        if (!platform::FileExists(path) || !materials)
            return Guid();
        const MaterialLibrary::Material* m = materials->FindByPath(path);
        return m ? m->guid : materials->LoadFile(path);
    };
    auto shape = [&](StringView label, BuiltinShape s, Vec3 center, Vec3 size, StringView mat, Quat rotation = {}) {
        Transform t(center, rotation);
        t.scale = size;
        const ecs::Entity e = CreateEntity(label, t);
        MeshRenderer mr;
        mr.mesh = builtin::Mesh(s);
        if (const Guid g = material(mat); g.IsValid())
            mr.materials.PushBack(g);
        world.Add(e, mr);
        EnsureEntityGuid(world, e);
        return e;
    };
    auto dynamic = [&](ecs::Entity e, f32 mass, StringView sound, bool box) {
        RigidBody rb;
        rb.mass = mass;
        world.Add(e, rb);
        if (box)
            world.Add(e, BoxCollider{});
        else
            world.Add(e, CylinderCollider{});
        ImpactSound s;
        s.sound = String::Format("StarterContent/Sounds/Impacts/{}", sound);
        world.Add(e, s);
        return e;
    };
    // A slab sloping up along `direction` (horizontal), its low edge at `start` on the ground.
    auto ramp = [&](StringView label, Vec3 start, Vec3 direction, f32 length, f32 width, f32 degrees, StringView mat) {
        const f32 a = DegToRad(degrees);
        const Vec3 flat = SafeNormalize(Vec3(direction.x, 0, direction.z), Vec3(0, 0, 1));
        const Vec3 along = flat * std::cos(a) + Vec3(0, std::sin(a), 0);
        const Vec3 up = SafeNormalize(Cross(along, Cross(Vec3(0, 1, 0), flat)), Vec3(0, 1, 0)) * (Dot(Cross(along, Cross(Vec3(0, 1, 0), flat)), Vec3(0, 1, 0)) < 0 ? -1.0f : 1.0f);
        const f32 thickness = 0.5f;
        return shape(label, BuiltinShape::Cube, start + along * (length * 0.5f) - up * (thickness * 0.5f), Vec3(width, thickness, length), mat,
                     Quat::LookRotation(along, up));
    };

    // Ground: a grid floor in the flat middle of rolling hills.
    shape("Floor", BuiltinShape::Cube, Vec3(0, -0.5f, 0), Vec3(240, 1, 240), "M_Proto_Grey");
    {
        Terrain t;
        t.size = 640.0f;
        t.height = 55.0f;
        t.flatCenter = 130.0f;
        t.featureSize = 160.0f;
        t.material = material("M_Terrain");
        Array<f32> heights;
        GenerateTerrainHeights(t, heights);
        const u32 n = TerrainSamples(t);
        const ecs::Entity terrain = CreateEntity("Terrain", Transform(Vec3(0.0f, -heights[usize(n / 2) * n + n / 2] - 0.2f, 0.0f)));
        world.Add(terrain, t);
        EnsureEntityGuid(world, terrain);
    }

    // 1. Straight ahead: a jump ramp, then open ground to land on.
    ramp("Jump Ramp", Vec3(0, 0, 45), Vec3(0, 0, 1), 12.0f, 7.0f, 14.0f, "M_Proto_Orange");
    shape("Jump Ramp Side L", BuiltinShape::Cube, Vec3(-3.8f, 0.7f, 50.8f), Vec3(0.6f, 1.4f, 11.6f), "M_Proto_Light");
    shape("Jump Ramp Side R", BuiltinShape::Cube, Vec3(3.8f, 0.7f, 50.8f), Vec3(0.6f, 1.4f, 11.6f), "M_Proto_Light");

    // 2. Crash zone (north-west): a brick wall and a row of concrete pillars.
    shape("Brick Wall", BuiltinShape::Cube, Vec3(-40, 2.0f, 50), Vec3(16, 4, 1.2f), "M_Bricks");
    for (u32 i = 0; i < 6; ++i)
        shape(String::Format("Pillar {}", i + 1), BuiltinShape::Cylinder, Vec3(-55.0f + f32(i) * 6.0f, 2.5f, 20.0f), Vec3(1.2f, 5.0f, 1.2f),
              "M_Proto_Light");

    // 3. Crate walls (west) and a triangle of barrels (east) to plough through.
    for (u32 wall = 0; wall < 2; ++wall)
        for (u32 row = 0; row < 4; ++row)
            for (u32 i = 0; i < 7; ++i) {
                const ecs::Entity crate = shape(String::Format("Crate {}-{}-{}", wall + 1, row + 1, i + 1), BuiltinShape::Cube,
                                                Vec3(-30.0f + f32(i) * 0.92f + f32(row % 2) * 0.46f, 0.45f + f32(row) * 0.9f, -15.0f - f32(wall) * 14.0f),
                                                Vec3(0.9f), "M_Wood");
                dynamic(crate, 25.0f, "Impact_Wood_Medium", true);
            }
    for (u32 row = 0; row < 5; ++row)
        for (u32 i = 0; i <= row; ++i) {
            const ecs::Entity barrel = shape(String::Format("Barrel {}-{}", row + 1, i + 1), BuiltinShape::Cylinder,
                                             Vec3(30.0f + (f32(i) - f32(row) * 0.5f) * 1.1f, 0.6f, -15.0f - f32(row) * 1.0f), Vec3(0.75f, 1.2f, 0.75f),
                                             i % 2 ? "M_Plastic_Red" : "M_Proto_Blue");
            dynamic(barrel, 40.0f, "Impact_Metal_Medium", false);
        }

    // 4. Bumpy road (east): half-buried logs across the lane - watch the suspension work.
    for (u32 i = 0; i < 14; ++i)
        shape(String::Format("Log {}", i + 1), BuiltinShape::Cylinder, Vec3(45.0f, 0.02f, 10.0f + f32(i) * 2.6f), Vec3(0.4f, 6.0f, 0.4f), "M_Wood",
              Quat::FromAxisAngle(Vec3(0, 0, 1), HalfPi));

    // 5. A steep hill (south) with a plateau, and a long gentle ramp back down.
    ramp("Hill Up", Vec3(-12, 0, -40), Vec3(0, 0, -1), 22.0f, 10.0f, 18.0f, "M_Proto_Grey");
    const f32 top = 22.0f * std::sin(DegToRad(18.0f));
    const f32 hillEnd = -40.0f - 22.0f * std::cos(DegToRad(18.0f));
    shape("Plateau", BuiltinShape::Cube, Vec3(-12, top * 0.5f, hillEnd - 6.0f), Vec3(10, top, 12), "M_Proto_Light");
    ramp("Hill Down", Vec3(-12, 0, hillEnd - 12.0f - top / std::tan(DegToRad(8.0f))), Vec3(0, 0, 1),
         top / std::sin(DegToRad(8.0f)), 10.0f, 8.0f, "M_Proto_Grey");

    // The car (Starter Content prefab) and a chase camera.
    const String carPrefab = "StarterContent/Prefabs/Characters/P_SoftCar.zprefab";
    if (prefabs && platform::FileExists(Path::Join(contentRoot, carPrefab)))
        InstantiatePrefabFile(world, *prefabs, carPrefab, Transform(Vec3(0, 0.15f, 0)));
    const ecs::Entity cam = CreateEntity("Camera", Transform(Vec3(0, 2.3f, -7.0f)));
    world.Add(cam, Camera{});
    world.Add(cam, Script{String("StarterContent/Scripts/CarCamera.zs"), String()});
    EnsureEntityGuid(world, cam);

    // HUD: speedometer and damage (bottom right), controls (bottom left), pause menu on Tab (CarHUD.zs).
    {
        const ecs::Entity hud = CreateEntity("HUD", Transform());
        UICanvas hudCanvas;
        hudCanvas.style = "StarterContent/UI/Game.zstyle"; // the look of every widget below
        world.Add(hud, hudCanvas);
        UIElement root;
        root.padding = Vec4(36.0f);
        world.Add(hud, root);
        world.Add(hud, Script{String("StarterContent/Scripts/CarHUD.zs"), String()});
        EnsureEntityGuid(world, hud);
        auto node = [&](StringView name, ecs::Entity parent, UIElement el) {
            const ecs::Entity e = CreateEntity(name, Transform(), parent);
            world.Add(e, el);
            EnsureEntityGuid(world, e);
            return e;
        };
        auto text = [&](StringView name, ecs::Entity parent, StringView value, f32 size, UIFontWeight weight, UITextAlign align,
                        LinearColor color = LinearColor(1, 1, 1, 1)) {
            const ecs::Entity e = node(name, parent, UIElement{});
            UIText t;
            t.text = String(value);
            t.fontSize = size;
            t.weight = weight;
            t.align = align;
            t.color = color;
            world.Add(e, t);
            return e;
        };
        auto corner = [&](Vec2 anchor) {
            UIElement el;
            el.absolute = true;
            el.anchor = el.pivot = anchor;
            el.padding = Vec4(22.0f, 16.0f, 22.0f, 18.0f);
            el.gap = 4.0f;
            return el;
        };
        UIElement speedBox = corner(Vec2(1.0f, 1.0f));
        speedBox.size = Vec2(260.0f, 0.0f);
        const ecs::Entity gauge = node("Speedometer", hud, speedBox);
        world.Add(gauge, UIPanel{});
        UIElement speedRow;
        speedRow.direction = UIDirection::Row;
        speedRow.align = UIAlign::End;
        speedRow.justify = UIJustify::End;
        speedRow.gap = 8.0f;
        const ecs::Entity speedLine = node("Speed Row", gauge, speedRow);
        text("Speed", speedLine, "0", 84.0f, UIFontWeight::Bold, UITextAlign::Right);
        UIElement unitBox;
        unitBox.margin = Vec4(0, 0, 0, 14.0f);
        unitBox.styleClass = "hint";
        const ecs::Entity unit = node("Unit", speedLine, unitBox);
        UIText u;
        u.text = "km/h";
        u.key = "hud.unit";
        u.fontSize = 24.0f;
        u.color = LinearColor(0.75f, 0.78f, 0.82f, 1.0f);
        world.Add(unit, u);
        const ecs::Entity damageLabel = text("Damage Label", gauge, "DAMAGE", 18.0f, UIFontWeight::SemiBold, UITextAlign::Left,
                                             LinearColor(0.75f, 0.78f, 0.82f, 1.0f));
        world.Get<UIElement>(damageLabel)->styleClass = "hint";
        world.Get<UIText>(damageLabel)->key = "hud.damage";
        UIElement barBox;
        barBox.size = Vec2(0.0f, 12.0f);
        const ecs::Entity damage = node("Damage", gauge, barBox);
        UIProgressBar bar;
        bar.value = 0.0f;
        bar.fillColor = LinearColor(1.0f, 0.25f, 0.05f, 1.0f);
        world.Add(damage, bar);

        const ecs::Entity hints = node("Controls", hud, corner(Vec2(0.0f, 1.0f)));
        world.Add(hints, UIPanel{});
        const ecs::Entity controls = text("Controls Text", hints, "W / S  throttle, brake      A / D  steer      Space  handbrake\nR  repair      Tab  menu",
                                          22.0f, UIFontWeight::Medium, UITextAlign::Left, LinearColor(0.85f, 0.87f, 0.9f, 1.0f));
        world.Get<UIElement>(controls)->styleClass = "hint";
        world.Get<UIText>(controls)->key = "hud.controls";

        UIElement menuBox;
        menuBox.absolute = true;
        menuBox.size = Vec2(460.0f, 0.0f);
        menuBox.padding = Vec4(36.0f);
        menuBox.gap = 16.0f;
        menuBox.visible = false; // shown by CarHUD.zs on Tab (tick Visible in Details to edit it)
        menuBox.styleClass = "menu";
        const ecs::Entity menu = node("Pause Menu", hud, menuBox);
        UIAnimation pop; // pops in every time it opens
        pop.preset = UIAnimationPreset::PopIn;
        pop.duration = 0.28f;
        pop.easing = UIEasing::Back;
        world.Add(menu, pop);
        UIPanel menuPanel;
        menuPanel.color = LinearColor(0.015f, 0.018f, 0.025f, 0.92f);
        menuPanel.cornerRadius = 16.0f;
        menuPanel.shadow = 40.0f;
        world.Add(menu, menuPanel);
        const ecs::Entity title = text("Title", menu, "Paused", 54.0f, UIFontWeight::Bold, UITextAlign::Center);
        world.Get<UIElement>(title)->styleClass = "title";
        world.Get<UIText>(title)->key = "menu.paused";
        for (StringView label : {StringView("Resume"), StringView("Repair"), StringView("Language")}) {
            UIElement b;
            b.size = Vec2(0.0f, 64.0f);
            b.justify = UIJustify::Center;
            b.align = UIAlign::Center;
            if (label == "Resume")
                b.styleClass = "primary";
            const ecs::Entity button = node(label, menu, b);
            world.Add(button, UIButton{});
            const ecs::Entity l = text(String::Format("{} Label", label), button, label, 28.0f, UIFontWeight::SemiBold, UITextAlign::Center);
            world.Get<UIText>(l)->shadow = false;
            if (label != "Language") // its text is set by CarHUD.zs (with the language's name)
                world.Get<UIText>(l)->key = label == "Resume" ? String("menu.resume") : String("menu.repair");
        }
        UIElement volumeRow;
        volumeRow.direction = UIDirection::Row;
        volumeRow.align = UIAlign::Center;
        volumeRow.gap = 16.0f;
        volumeRow.margin = Vec4(0, 8.0f, 0, 0);
        const ecs::Entity row = node("Volume Row", menu, volumeRow);
        world.Get<UIText>(text("Volume Label", row, "Volume", 24.0f, UIFontWeight::Medium, UITextAlign::Left))->key = "menu.volume";
        UIElement sliderBox;
        sliderBox.grow = 1.0f;
        sliderBox.size = Vec2(0.0f, 32.0f);
        const ecs::Entity volume = node("Volume", row, sliderBox);
        UISlider slider;
        slider.value = 1.0f;
        world.Add(volume, slider);
    }

    camera.position = Vec3(4.2f, 2.1f, -5.6f);
    camera.LookAt(Vec3(-0.3f, 0.7f, 3.0f));
    environment.sunElevation = DegToRad(38.0f);
    environment.sunAzimuth = DegToRad(150.0f);
    AddAtmosphere(0.002f, 0.35f);
}

void Editor::BuildEraLevel()
{
    // Sun, sky light, fog and clouds (the era decides which of them render) over a large grid floor.
    Transform t(Vec3(0, -0.5f, 0));
    t.scale = Vec3(200, 1, 200);
    const ecs::Entity floor = CreateEntity("Floor", t);
    MeshRenderer mr;
    mr.mesh = builtin::Mesh(BuiltinShape::Cube);
    const String path = String::Format("{}/StarterContent/Materials/M_Proto_Grey.zmat", contentRoot);
    if (materials && platform::FileExists(path))
        if (const Guid g = materials->LoadFile(path); g.IsValid())
            mr.materials.PushBack(g);
    world.Add(floor, mr);
    EnsureEntityGuid(world, floor);
    environment.sunElevation = DegToRad(35.0f);
    environment.sunAzimuth = DegToRad(150.0f);
    AddAtmosphere(0.003f, 0.35f);
    camera.position = Vec3(0.0f, 2.0f, -8.0f);
    camera.LookAt(Vec3(0.0f, 1.2f, 10.0f));
}

void Editor::AddAtmosphere(f32 fogDensity, f32 cloudCoverage)
{
    // Like a new UE level: a sun, sky light, height fog and clouds, each an entity to tweak in Details.
    const ecs::Entity sun = CreateEntity("Sun", Transform(Vec3(0.0f, 10.0f, 0.0f), SunRotation(environment.sunElevation, environment.sunAzimuth)));
    world.Add(sun, DirectionalLight{});
    EnsureEntityGuid(world, sun);
    const ecs::Entity skyLight = CreateEntity("Sky Light", Transform(Vec3(0.0f, 10.0f, 0.0f)));
    world.Add(skyLight, SkyLight{});
    EnsureEntityGuid(world, skyLight);
    const ecs::Entity fog = CreateEntity("Height Fog", Transform(Vec3(0.0f)));
    HeightFog f;
    f.density = fogDensity;
    world.Add(fog, f);
    EnsureEntityGuid(world, fog);
    const ecs::Entity clouds = CreateEntity("Volumetric Clouds", Transform(Vec3(0.0f)));
    VolumetricClouds c;
    ApplyCloudPreset(c, CloudPreset::FairWeather); // cumulus fields (tweak or pick another preset in Details)
    c.coverage = Max(c.coverage, cloudCoverage);
    world.Add(clouds, c);
    EnsureEntityGuid(world, clouds);
    // A gentle breeze: smoke, fire, fog and clouds drift with it (tune or add Wind Zones; Wind Response per object).
    const ecs::Entity windZone = CreateEntity("Wind", Transform(Vec3(0.0f, 4.0f, 0.0f), Quat::FromAxisAngle(Vec3(0, 1, 0), DegToRad(60.0f))));
    WindZone breeze;
    breeze.speed = 4.0f;
    world.Add(windZone, breeze);
    EnsureEntityGuid(world, windZone);
    // Outdoor ambience: birds and a light breeze everywhere (2D), like the sky they belong to.
    const ecs::Entity ambience = CreateEntity("Ambience", Transform(Vec3(0.0f, 2.0f, 0.0f)));
    AudioSource birds;
    birds.clip = "StarterContent/Sounds/Birds_Loop.ogg";
    birds.volume = 0.3f;
    birds.spatial = false;
    world.Add(ambience, birds);
    EnsureEntityGuid(world, ambience);
}

void Editor::BuildWaterLevel()
{
    // An island in the sea (outside the undo history): sea floor, beach and grass terraces, rocks, a pier, the water
    // surface and a first-person player to walk around with.
    auto material = [&](StringView name) {
        const String path = String::Format("{}/StarterContent/Materials/{}.zmat", contentRoot, name);
        if (!platform::FileExists(path) || !materials)
            return Guid();
        const MaterialLibrary::Material* m = materials->FindByPath(path);
        return m ? m->guid : materials->LoadFile(path);
    };
    auto block = [&](StringView label, BuiltinShape shape, Vec3 min, Vec3 max, StringView mat, ecs::Entity parent = {}) {
        Transform t((min + max) * 0.5f);
        t.scale = max - min;
        const ecs::Entity e = CreateEntity(label, t, parent);
        MeshRenderer mr;
        mr.mesh = builtin::Mesh(shape);
        if (const Guid g = material(mat); g.IsValid())
            mr.materials.PushBack(g);
        world.Add(e, mr);
        EnsureEntityGuid(world, e);
        return e;
    };
    auto shape = [&](StringView label, BuiltinShape sh, Vec3 center, Vec3 size, StringView mat, Quat rotation = {}, ecs::Entity parent = {}) {
        Transform t(center, rotation);
        t.scale = size;
        const ecs::Entity e = CreateEntity(label, t, parent);
        MeshRenderer mr;
        mr.mesh = builtin::Mesh(sh);
        if (const Guid g = material(mat); g.IsValid())
            mr.materials.PushBack(g);
        world.Add(e, mr);
        EnsureEntityGuid(world, e);
        return e;
    };
    // Sea floor, then the island as overlapping flattened domes: a wide sandy shelf, the beach, grassy hills.
    block("Sea Floor", BuiltinShape::Cube, Vec3(-200, -9, -200), Vec3(200, -8, 200), "M_Sand");
    struct Dome
    {
        const char* name;
        Vec3 center, size;
        const char* material;
    };
    const Dome domes[] = {
        {"Sand Shelf", Vec3(0, -6.5f, 2), Vec3(90, 12, 70), "M_Sand"}, {"Beach", Vec3(0, -3.4f, 0), Vec3(54, 8, 40), "M_Sand"},
        {"Meadow", Vec3(-3, -2.2f, -3), Vec3(38, 6, 26), "M_Grass"},   {"Hill", Vec3(-8, -1.0f, -6), Vec3(18, 7, 14), "M_Grass"},
        {"Dune", Vec3(13, -1.6f, 5), Vec3(12, 4, 8), "M_Sand"},
    };
    for (const Dome& d : domes)
        shape(d.name, BuiltinShape::Sphere, d.center, d.size, d.material);
    // Height of the island surface (the highest dome) at x, z.
    auto ground = [&](f32 x, f32 z) {
        f32 y = -8.0f;
        for (const Dome& d : domes) {
            const f32 u = (x - d.center.x) / (d.size.x * 0.5f), v = (z - d.center.z) / (d.size.z * 0.5f), r = u * u + v * v;
            if (r < 1.0f)
                y = Max(y, d.center.y + d.size.y * 0.5f * Sqrt(1.0f - r));
        }
        return y;
    };
    // Rocks: squashed, turned spheres along the shore.
    struct RockDesc
    {
        Vec3 position, size, euler;
    };
    const RockDesc rocks[] = {
        {Vec3(21, -0.2f, 6), Vec3(4.5f, 3.2f, 3.6f), Vec3(10, 30, 5)},  {Vec3(23.5f, -0.6f, 3), Vec3(2.4f, 1.8f, 2.2f), Vec3(-8, 70, 12)},
        {Vec3(-22, -0.5f, -10), Vec3(5, 3.6f, 4), Vec3(5, -20, -8)},    {Vec3(-19, -0.9f, -14), Vec3(2.8f, 2, 2.4f), Vec3(14, 40, 0)},
        {Vec3(-12, 3.2f, -10), Vec3(3, 2.2f, 2.6f), Vec3(-6, 10, 18)},  {Vec3(8, -1.2f, -17), Vec3(3.6f, 2.4f, 3), Vec3(0, 55, -10)},
        {Vec3(34, -3.0f, 26), Vec3(4, 5, 4), Vec3(6, 0, 10)},           {Vec3(-30, -2.6f, 18), Vec3(3.4f, 4, 3), Vec3(-10, 30, 4)},
    };
    for (u32 i = 0; i < ZX_ARRAY_COUNT(rocks); ++i)
        shape(String::Format("Rock {}", i + 1), BuiltinShape::Sphere, rocks[i].position, rocks[i].size, "M_Rock",
              Quat::FromEulerDegrees(rocks[i].euler));
    // Palm trees: a curved trunk of tilted segments and a crown of long leaves.
    auto palm = [&](StringView label, Vec3 base, f32 height, f32 lean, f32 heading) {
        base.y = ground(base.x, base.z) - 0.2f;
        const ecs::Entity root = CreateEntity(label, Transform(base, Quat::FromAxisAngle(Vec3(0, 1, 0), DegToRad(heading))));
        EnsureEntityGuid(world, root);
        Vec3 p(0.0f);
        const u32 segments = 6;
        const f32 segment = height / f32(segments);
        for (u32 i = 0; i < segments; ++i) {
            const f32 tilt = DegToRad(lean * (0.3f + 0.7f * f32(i) / f32(segments)));
            const Vec3 dir(0.0f, Cos(tilt), Sin(tilt));
            const f32 width = 0.42f - 0.035f * f32(i);
            shape(String::Format("Trunk {}", i + 1), BuiltinShape::Cylinder, p + dir * (segment * 0.5f), Vec3(width, segment * 1.04f, width),
                  "M_Wood", Quat::FromAxisAngle(Vec3(1, 0, 0), tilt), root);
            p += dir * segment;
        }
        // Fronds: an inner part rising slightly and an outer part drooping (rotation about the local X axis turns
        // +Z downwards for positive angles).
        for (u32 i = 0; i < 8; ++i) {
            const Quat yaw = Quat::FromAxisAngle(Vec3(0, 1, 0), DegToRad(f32(i) * 45.0f + 12.0f * f32(i % 2)));
            const Quat inner = yaw * Quat::FromAxisAngle(Vec3(1, 0, 0), DegToRad(-8.0f + 6.0f * f32(i % 3)));
            const Quat outer = yaw * Quat::FromAxisAngle(Vec3(1, 0, 0), DegToRad(38.0f + 5.0f * f32(i % 3)));
            const Vec3 mid = p + inner * Vec3(0.0f, 0.0f, 2.0f);
            shape(String::Format("Frond {}", i + 1), BuiltinShape::Sphere, p + inner * Vec3(0.0f, 0.0f, 1.0f), Vec3(0.75f, 0.1f, 2.3f), "M_Leaves", inner, root);
            shape(String::Format("Frond {} Tip", i + 1), BuiltinShape::Sphere, mid + outer * Vec3(0.0f, 0.0f, 0.9f), Vec3(0.55f, 0.08f, 2.0f), "M_Leaves", outer, root);
        }
        shape("Coconuts", BuiltinShape::Sphere, p - Vec3(0, 0.25f, 0), Vec3(0.55f), "M_Dirt", {}, root);
    };
    palm("Palm 1", Vec3(9, 1.2f, -4), 7.5f, 18.0f, 30.0f);
    palm("Palm 2", Vec3(12, 0.9f, 1), 6.0f, 26.0f, 110.0f);
    palm("Palm 3", Vec3(-4, 2.0f, 3), 8.0f, 12.0f, -40.0f);
    palm("Palm 4", Vec3(-15, 2.2f, -2), 6.5f, 22.0f, 200.0f);
    palm("Palm 5", Vec3(3, 1.4f, 9), 5.5f, 30.0f, 0.0f);
    // Pier towards the open sea (+Z).
    block("Pier", BuiltinShape::Cube, Vec3(-1.5f, 0.55f, 11), Vec3(1.5f, 0.75f, 34), "M_Wood");
    for (u32 i = 0; i < 6; ++i) {
        const f32 z = 15.0f + f32(i) * 3.8f;
        block(String::Format("Pier Post {}", i * 2 + 1), BuiltinShape::Cylinder, Vec3(-1.6f, -8, z), Vec3(-1.2f, 0.9f, z + 0.4f), "M_Wood");
        block(String::Format("Pier Post {}", i * 2 + 2), BuiltinShape::Cylinder, Vec3(1.2f, -8, z), Vec3(1.6f, 0.9f, z + 0.4f), "M_Wood");
    }
    // A small boat at the end of the pier, rocking on the waves (Buoyancy), and driftwood: crates and barrels.
    {
        const ecs::Entity boat = CreateEntity("Boat", Transform(Vec3(3.6f, 0.15f, 30.0f), Quat::FromAxisAngle(Vec3(0, 1, 0), DegToRad(8.0f))));
        EnsureEntityGuid(world, boat);
        for (const ecs::Entity part : {shape("Hull", BuiltinShape::Sphere, Vec3(0, 0, 0), Vec3(1.8f, 0.9f, 5.0f), "M_Plastic_White", {}, boat),
                                       shape("Deck", BuiltinShape::Cube, Vec3(0, 0.38f, 0), Vec3(1.4f, 0.06f, 3.6f), "M_Wood", {}, boat),
                                       shape("Bench", BuiltinShape::Cube, Vec3(0, 0.6f, -0.6f), Vec3(1.3f, 0.08f, 0.4f), "M_Wood", {}, boat)})
            world.Get<MeshRenderer>(part)->collision = false; // the boat's box collider stands for its parts
        RigidBody rb;
        rb.mass = 180.0f;
        world.Add(boat, rb);
        world.Add(boat, BoxCollider{Vec3(0, 0.05f, 0), Vec3(1.6f, 0.75f, 4.4f)});
        Buoyancy b;
        b.density = 250.0f; // hollow: floats high
        world.Add(boat, b);
        auto floater = [&](StringView label, BuiltinShape sh, Vec3 at, Vec3 size, StringView mat, f32 density, Vec3 euler) {
            const ecs::Entity e = shape(label, sh, at, size, mat, Quat::FromEulerDegrees(euler));
            world.Add(e, RigidBody{});
            if (sh == BuiltinShape::Cylinder)
                world.Add(e, CylinderCollider{});
            else
                world.Add(e, BoxCollider{});
            Buoyancy fb;
            fb.density = density;
            world.Add(e, fb);
        };
        floater("Crate 1", BuiltinShape::Cube, Vec3(-4.0f, 0.6f, 24.0f), Vec3(0.9f), "M_Wood", 450.0f, Vec3(0, 20, 0));
        floater("Crate 2", BuiltinShape::Cube, Vec3(-6.5f, 0.6f, 31.0f), Vec3(0.7f), "M_Wood", 550.0f, Vec3(10, 65, 5));
        floater("Barrel 1", BuiltinShape::Cylinder, Vec3(6.5f, 0.6f, 25.0f), Vec3(0.7f, 1.0f, 0.7f), "M_Wood", 400.0f, Vec3(90, 30, 0));
        floater("Barrel 2", BuiltinShape::Cylinder, Vec3(-3.0f, 0.6f, 36.0f), Vec3(0.7f, 1.0f, 0.7f), "M_Plastic_White", 300.0f, Vec3(0, 0, 0));
        floater("Log", BuiltinShape::Cylinder, Vec3(8.0f, 0.6f, 35.0f), Vec3(0.45f, 3.5f, 0.45f), "M_Wood", 650.0f, Vec3(90, 70, 0));
    }
    // Hut on the meadow: walls, a door and a two-slab roof.
    {
        const ecs::Entity hut = CreateEntity("Hut", Transform(Vec3(-9, ground(-9, -4.5f) - 0.15f, -4.5f)));
        EnsureEntityGuid(world, hut);
        shape("Walls", BuiltinShape::Cube, Vec3(0, 1.2f, 0), Vec3(4, 2.4f, 3), "M_Wood", {}, hut);
        shape("Door", BuiltinShape::Cube, Vec3(0.6f, 0.9f, 1.52f), Vec3(0.9f, 1.8f, 0.06f), "M_Dirt", {}, hut);
        const Quat left = Quat::FromAxisAngle(Vec3(0, 0, 1), DegToRad(32.0f)), right = Quat::FromAxisAngle(Vec3(0, 0, 1), DegToRad(-32.0f));
        shape("Roof L", BuiltinShape::Cube, Vec3(-1.1f, 3.05f, 0), Vec3(2.7f, 0.12f, 3.6f), "M_Grass", left, hut);
        shape("Roof R", BuiltinShape::Cube, Vec3(1.1f, 3.05f, 0), Vec3(2.7f, 0.12f, 3.6f), "M_Grass", right, hut);
    }

    // The sea: level at y = 0, with its sound all around the island.
    const ecs::Entity sea = CreateEntity("Sea", Transform(Vec3(0, 0, 0)));
    Water w;
    ApplyWaterPreset(w, WaterPreset::Tropical); // turquoise lagoon around the island
    w.size = Vec2(3000.0f, 3000.0f);            // to the horizon
    w.windSpeed = 7.0f;
    w.fetch = 60.0f;
    world.Add(sea, w);
    AudioSource surf;
    surf.clip = "StarterContent/Sounds/Waves_Loop.ogg";
    surf.volume = 0.6f;
    surf.spatial = false;
    surf.bus = audio::Bus::Music;
    world.Add(sea, surf);
    {
        AudioSource wind;
        wind.clip = "StarterContent/Sounds/Wind_Loop.ogg";
        wind.volume = 0.25f;
        wind.spatial = false;
        const ecs::Entity windEntity = CreateEntity("Wind", Transform(Vec3(0, 5, 0)));
        world.Add(windEntity, wind);
        EnsureEntityGuid(world, windEntity);
        // The hut sounds like a small room inside.
        const ecs::Entity room = CreateEntity("Hut Reverb", Transform(Vec3(-9, 3.4f, -4.5f)));
        ReverbZone zone;
        zone.radius = 3.0f;
        zone.fade = 1.5f;
        zone.roomSize = 0.4f;
        world.Add(room, zone);
        EnsureEntityGuid(world, room);
    }
    {
        // Reflections inside the hut (its floor and walls line up with the box).
        const ecs::Entity probe = CreateEntity("Hut Reflections", Transform(Vec3(-9, 3.4f, -4.5f)));
        ReflectionProbe rp;
        rp.extent = Vec3(2.6f, 1.8f, 2.6f);
        rp.blendDistance = 0.5f;
        world.Add(probe, rp);
        EnsureEntityGuid(world, probe);
    }
    EnsureEntityGuid(world, sea);

    // Player on the pier, looking at the open sea.
    const ecs::Entity player = CreateEntity("Player", Transform(Vec3(0, 0.8f, 16)));
    world.Add(player, CharacterController{});
    world.Add(player, Script{String("Scripts/FirstPersonController.zs"), String()});
    EnsureEntityGuid(world, player);
    const ecs::Entity cam = CreateEntity("Camera", Transform(Vec3(0, 1.65f, 0)), player);
    world.Add(cam, Camera{});
    EnsureEntityGuid(world, cam);

    // Afternoon sun over the sea (a glitter path towards the camera's view of the pier).
    environment.sunElevation = DegToRad(32.0f);
    environment.sunAzimuth = DegToRad(20.0f);
    AddAtmosphere(0.003f, 0.32f); // sea haze, fair-weather clouds
    camera.position = Vec3(17.0f, 4.5f, 40.0f);
    camera.LookAt(Vec3(-2.0f, 1.5f, 2.0f));
}

void Editor::BuildStarterShowcase(bool withPrefabs)
{
    if (withPrefabs) {
        // The Starter Content assets laid out on a grid floor, with a third-person character ready for Play.
        auto material = [&](StringView name) {
            const String path = String::Format("{}/StarterContent/Materials/{}.zmat", contentRoot, name);
            const MaterialLibrary::Material* m = materials ? materials->FindByPath(path) : nullptr;
            return m ? m->guid : (materials ? materials->LoadFile(path) : Guid());
        };
        {
            Transform t(Vec3(0, -0.5f, 0));
            t.scale = Vec3(60, 1, 60);
            const ecs::Entity floor = CreateEntity("Floor", t);
            MeshRenderer mr;
            mr.mesh = builtin::Mesh(BuiltinShape::Cube);
            if (const Guid g = material("M_Proto_Grey"); g.IsValid())
                mr.materials.PushBack(g);
            world.Add(floor, mr);
            EnsureEntityGuid(world, floor);
        }
        struct Placement
        {
            const char* prefab;
            Vec3 position;
            f32 yaw;
        };
        const Placement placements[] = {
            {"Effects/P_Campfire", Vec3(0, 0, 4), 0},          {"Effects/P_Torch", Vec3(-2.5f, 0, 8), 0},
            {"Effects/P_Torch", Vec3(2.5f, 0, 8), 0},           {"Effects/P_Fountain", Vec3(-9, 0, 6), 0},
            {"Effects/P_WaterPool", Vec3(10, 0.8f, 8), 0},      {"Effects/P_Magic", Vec3(-5, 1.6f, -1), 0},
            {"Effects/P_Steam", Vec3(5, 0, 1), 0},              {"Effects/P_Sparks", Vec3(8, 2.5f, -3), 0},
            {"Effects/P_SparkShower", Vec3(11, 2.2f, 3), 0},   {"Effects/P_Explosion", Vec3(16, 1.0f, 13), 0},
            {"Effects/P_Smoke", Vec3(-14, 0, 14), 0},           {"Effects/P_GroundFog", Vec3(-12, 0, 14), 0},
            {"Props/P_Door", Vec3(-3, 0, -6), 0},               {"Props/P_Elevator", Vec3(-12, 0, -6), 0},
            {"Props/P_JumpPad", Vec3(4, 0, -6), 0},             {"Props/P_CrateStack", Vec3(-6, 0, 12), 0},
            {"Props/P_SwingingLamp", Vec3(0, 4.5f, -10), 0},    {"Props/P_RopeBridge", Vec3(14, 0.35f, -8), 0},
            {"Props/P_Coin", Vec3(-1, 1, 1), 0},                {"Props/P_Coin", Vec3(1, 1, 1), 0},
            {"Props/P_Coin", Vec3(3, 1, 1), 0},                 {"Characters/P_Tank", Vec3(-16, 0, 2), 90},
            {"Characters/P_ThirdPersonCharacter", Vec3(0, 0, -2), 0},
            // Fire spreading: a burning hay bale lights the fence, which carries the fire to the wood pile and the hay.
            {"Props/P_HayBale", Vec3(-1.9f, 0, 16), 90},       {"Props/P_WoodenFence", Vec3(-1.1f, 0, 16), 0},
            {"Props/P_WoodPile", Vec3(6.0f, 0, 16.2f), 90},    {"Props/P_HayBale", Vec3(7.4f, 0, 17.3f), 0},
        };
        bool firstHay = true;
        for (const Placement& p : placements) {
            const String rel = String::Format("StarterContent/Prefabs/{}.zprefab", p.prefab);
            if (!platform::FileExists(Path::Join(contentRoot, rel)))
                continue;
            const ecs::Entity root =
                InstantiatePrefabFile(world, *prefabs, rel, Transform(p.position, Quat::FromAxisAngle(Vec3(0, 1, 0), DegToRad(p.yaw))));
            if (root && StringView(p.prefab) == "Props/P_HayBale" && firstHay) {
                firstHay = false;
                if (Flammable* f = world.Get<Flammable>(root))
                    f->startBurning = true;
            }
            if (root && StringView(p.prefab) == "Characters/P_Tank") {
                // The character is the player here: the tank waits (Controlled off, its camera inactive).
                if (Script* sc = world.Get<Script>(root))
                    sc->properties = R"json({"controlled":false})json";
                Array<ecs::Entity> members;
                CollectHierarchy(world, root, members);
                for (ecs::Entity m : members)
                    if (Camera* c = world.Get<Camera>(m))
                        c->active = false;
            }
        }
        // Relief materials (Parallax Occlusion): a brick wall over a cobblestone patch.
        auto block = [&](StringView label, Vec3 center, Vec3 size, StringView mat) {
            Transform t(center);
            t.scale = size;
            const ecs::Entity e = CreateEntity(label, t);
            MeshRenderer mr;
            mr.mesh = builtin::Mesh(BuiltinShape::Cube);
            if (const Guid g = material(mat); g.IsValid())
                mr.materials.PushBack(g);
            world.Add(e, mr);
            EnsureEntityGuid(world, e);
        };
        block("Brick Wall", Vec3(8.0f, 2.0f, -12.5f), Vec3(6.0f, 4.0f, 0.5f), "M_Bricks");
        block("Cobblestones", Vec3(8.0f, 0.01f, -10.0f), Vec3(6.0f, 0.02f, 4.5f), "M_Cobblestone");
        {
            // Color grading: a teal-and-orange look from one of the Starter Content LUTs over the whole level.
            const ecs::Entity grading = CreateEntity("Color Grading", Transform(Vec3(0.0f)));
            PostProcessVolume ppv;
            ppv.unbound = true;
            ppv.overrideColorLut = true;
            ppv.colorLut = "StarterContent/LUTs/LUT_Cinematic.png";
            ppv.colorLutIntensity = 0.7f;
            world.Add(grading, ppv);
            EnsureEntityGuid(world, grading);
        }
        environment.sunElevation = DegToRad(40.0f);
        environment.sunAzimuth = DegToRad(150.0f);
        AddAtmosphere(0.006f, 0.3f);
        camera.position = Vec3(7.0f, 5.0f, -13.0f);
        camera.LookAt(Vec3(-2.0f, 0.8f, 3.0f));
        return;
    }
    // A floor and a few shapes using the Starter Content materials and scripts (outside the undo history).
    auto material = [&](StringView name) {
        const String path = String::Format("{}/StarterContent/Materials/{}.zmat", contentRoot, name);
        const MaterialLibrary::Material* m = materials ? materials->FindByPath(path) : nullptr;
        return m ? m->guid : (materials ? materials->LoadFile(path) : Guid());
    };
    auto shape = [&](BuiltinShape s, StringView label, Vec3 pos, Vec3 scale, StringView mat, StringView script, StringView props) {
        Transform t(pos);
        t.scale = scale;
        const ecs::Entity e = CreateEntity(label, t);
        MeshRenderer mr;
        mr.mesh = builtin::Mesh(s);
        const Guid g = material(mat);
        if (g.IsValid())
            mr.materials.PushBack(g);
        world.Add(e, mr);
        if (!script.Empty())
            world.Add(e, Script{String::Format("StarterContent/Scripts/{}", script), String(props)});
        EnsureEntityGuid(world, e);
        return e;
    };
    shape(BuiltinShape::Plane, "Floor", Vec3(0, 0, 0), Vec3(4, 1, 4), "M_Proto_Grey", {}, {});
    const ecs::Entity cube = shape(BuiltinShape::Cube, "Spinning Cube", Vec3(-2.5f, 0.75f, 0), Vec3(1), "M_Plastic_Red", "Rotator.zs", {});
    {
        AudioSource hum; // a 3D sound: walk around the cube in Play to hear it move between the speakers
        hum.clip = "StarterContent/Sounds/Hum_Loop.wav";
        hum.volume = 0.5f;
        hum.maxDistance = 15.0f;
        world.Add(cube, hum);
    }
    shape(BuiltinShape::Sphere, "Floating Sphere", Vec3(0, 1.0f, 0), Vec3(1), "M_Metal_Gold", "Bobbing.zs", {});
    shape(BuiltinShape::Cylinder, "Player (WASD)", Vec3(2.5f, 0.5f, 0), Vec3(0.8f, 1, 0.8f), "M_Metal_Steel", "SimpleMover.zs", {});
    shape(BuiltinShape::Cube, "Watcher", Vec3(0, 0.5f, -3), Vec3(0.6f), "M_Emissive_Cyan", "LookAt.zs", R"json({"target":"Player (WASD)"})json");
    camera.position = Vec3(4.5f, 3.2f, 7.5f);
    camera.LookAt(Vec3(0.0f, 0.6f, -0.5f));
}

void Editor::UpdateStartupScene(StringView savedScene)
{
    if (project.descriptorPath.Empty())
        return;
    const String rel = Path::MakeRelative(savedScene, project.root);
    if (rel.Empty() || rel == project.descriptor.startupScene)
        return;
    project.descriptor.startupScene = rel;
    SaveProjectDescriptor(project.descriptorPath, project.descriptor);
}

String Editor::CopyIntoProject(StringView physicalPath)
{
    const String path = Path::Normalize(physicalPath);
    if (project.descriptorPath.Empty() || Path::IsUnder(path, contentRoot))
        return path;
    // Content/<Stem>/, or <Stem>_2, _3... if a different asset already uses that name.
    String dest = Path::Join(contentRoot, Path::Stem(path));
    for (u32 n = 2; platform::FileExists(Path::Join(dest, Path::FileName(path))); ++n)
        dest = Path::Join(contentRoot, String::Format("{}_{}", Path::Stem(path), n));
    Result<String> copy = CopySourceWithDependencies(path, dest);
    if (!copy) {
        ZX_LOG_ERROR(LogEditor, "Import: {}", copy.GetError().message);
        return {};
    }
    ZX_LOG_INFO(LogEditor, "Copied '{}' into {}", Path::FileName(path), Path::MakeRelative(dest, project.root));
    panels.browserListedDir.Clear(); // refresh the Content Browser
    return copy.Value();
}

void Editor::BuildDefaultScene()
{
    camera.position = Vec3(-9.0f, 2.2f, -0.6f);
    camera.LookAt(Vec3(5.0f, 2.8f, 0.0f));
    camera.moveSpeed = 4.0f;
    StringView sceneArg;
    const String path = CommandLine::GetValue("scene", sceneArg) ? String(sceneArg) : contentRoot + "/Sponza/Sponza.gltf";
    if (Path::HasExtension(path, ".zscene")) {
        if (!OpenScene(path))
            ZX_LOG_ERROR(LogEditor, "Failed to open scene {}", path);
        return;
    }
    if (platform::FileExists(path)) {
        ImportAsset(path, Vec3::Zero(), false);
        sceneName = Path::Stem(path);
    } else {
        ZX_LOG_WARN(LogEditor, "Sample scene not found ({}). Run Tools/fetch_samples.ps1 or drop a .gltf into the Content Browser.", path);
    }
    const ecs::Entity lights = CreateEntity("Lights", Transform::Identity());
    const Vec3 positions[4] = {Vec3(-6.0f, 1.6f, 2.4f), Vec3(-6.0f, 1.6f, -2.4f), Vec3(6.0f, 1.6f, 2.4f), Vec3(6.0f, 1.6f, -2.4f)};
    for (u32 i = 0; i < 4; ++i) {
        const ecs::Entity e = CreateEntity(String::Format("Point Light {}", i + 1), Transform(positions[i]), lights);
        PointLight l;
        l.color = Vec3(1.0f, 0.62f, 0.32f);
        l.lumens = 2500.0f;
        l.range = 8.0f;
        world.Add(e, l);
    }
    undo.Clear();
}

ecs::Entity Editor::CreateEntity(StringView name, const Transform& t, ecs::Entity parent)
{
    return scene::CreateNode(world, name, t, parent);
}

void Editor::CreateEmpty(Vec3 position, ecs::Entity parent)
{
    CommitCreated(CreateEntity("Entity", Transform(position), parent), "Create Entity");
}

void Editor::CreatePointLight(Vec3 position, ecs::Entity parent)
{
    const ecs::Entity e = CreateEntity("Point Light", Transform(position), parent);
    world.Add(e, PointLight{});
    CommitCreated(e, "Create Point Light");
}

void Editor::CreateSpotLight(Vec3 position, ecs::Entity parent)
{
    const ecs::Entity e = CreateEntity("Spot Light", Transform(position, Quat::FromEuler(DegToRad(60.0f), 0.0f, 0.0f)), parent);
    world.Add(e, SpotLight{});
    CommitCreated(e, "Create Spot Light");
}

void Editor::CreateWater(Vec3 position, ecs::Entity parent)
{
    const ecs::Entity e = CreateEntity("Water", Transform(Vec3(position.x, 0.0f, position.z)), parent);
    Water w;
    ApplyWaterPreset(w, WaterPreset::Lake);
    world.Add(e, w);
    CommitCreated(e, "Create Water");
}

void Editor::CreateAudioSource(Vec3 position, ecs::Entity parent)
{
    const ecs::Entity e = CreateEntity("Audio Source", Transform(position), parent);
    world.Add(e, AudioSource{});
    CommitCreated(e, "Create Audio Source");
}

void Editor::CreateDecal(Vec3 position, ecs::Entity parent)
{
    // Placed on the ground below the point when there is one (the decal projects down).
    if (renderAssets) {
        RaycastHit hit;
        if (RaycastMeshes(world, *renderAssets, Ray(position + Vec3(0, 0.5f, 0), Vec3(0, -1, 0)), hit, 100.0f))
            position = hit.point;
    }
    const ecs::Entity e = CreateEntity("Decal", Transform(position), parent);
    world.Add(e, Decal{});
    CommitCreated(e, "Create Decal");
}

void Editor::DrawPhysicsDebug()
{
    // Moving bodies: bounds colored per island (asleep: grey, kinematic: blue); contacts: point + normal; joints:
    // anchor to anchor.
    physics::PhysicsWorld& w = *physics.World();
    Array<physics::DebugBody> bodies;
    w.GetDebugBodies(bodies);
    const Color islandColors[6] = {Color(255, 120, 90), Color(120, 220, 120), Color(110, 170, 255),
                              Color(240, 210, 90), Color(220, 120, 240), Color(90, 220, 220)};
    for (const physics::DebugBody& b : bodies) {
        if (b.motion == physics::MotionType::Static)
            continue;
        const Color c = b.motion == physics::MotionType::Kinematic ? Color(80, 140, 255)
                        : b.sleeping || b.island == ~0u         ? Color(120, 120, 120)
                                                                : islandColors[b.island % 6];
        debugDraw->Box(b.bounds, Mat4::Identity(), c);
    }
    Array<physics::DebugContact> contacts;
    w.GetDebugContacts(contacts);
    for (const physics::DebugContact& c : contacts) {
        const Color pc = c.separation <= 0.0f ? Color(255, 60, 60) : Color(255, 170, 60); // touching / speculative
        for (Vec3 axis : {Vec3(0.04f, 0, 0), Vec3(0, 0.04f, 0), Vec3(0, 0, 0.04f)})
            debugDraw->Line(c.position - axis, c.position + axis, pc);
        debugDraw->Line(c.position, c.position + c.normal * 0.2f, Color(255, 235, 80));
    }
    Array<physics::JointId> joints;
    w.GetJoints(joints);
    for (physics::JointId j : joints) {
        if (!w.IsValid(j))
            continue;
        Vec3 a, b;
        w.GetJointAnchors(j, a, b);
        debugDraw->Line(a, b, Color(255, 160, 60));
        debugDraw->Sphere(a, 0.05f, Color(255, 160, 60), 8);
    }
}

void Editor::CreateWindZone()
{
    const Vec3 f = SafeNormalize(Vec3(camera.Forward().x, 0.0f, camera.Forward().z), Vec3(0, 0, 1));
    const ecs::Entity e = CreateEntity("Wind Zone", Transform(camera.position + camera.Forward() * 5.0f, Quat::LookRotation(f, Vec3(0, 1, 0))));
    world.Add(e, WindZone{});
    CommitCreated(e, "Create Wind Zone");
    Select(e);
}

ecs::Entity Editor::CreateUIWidget(StringView kind)
{
    ecs::Entity parent;
    if (world.IsAlive(selected) && world.Has<UIElement>(selected))
        parent = selected;
    if (!parent)
        world.Query<const UICanvas>().Each([&](ecs::Entity e, const UICanvas&) {
            if (!parent)
                parent = e;
        });
    ecs::Entity created; // the root of what this call made (one undo step)
    auto canvas = [&] {
        const ecs::Entity c = CreateEntity("UI Canvas", Transform());
        world.Add(c, UICanvas{});
        UIElement el;
        el.padding = Vec4(40.0f);
        world.Add(c, el);
        EnsureEntityGuid(world, c);
        return c;
    };
    if (kind.EqualsIgnoreCase("canvas") || !parent) {
        parent = canvas();
        created = parent;
        if (kind.EqualsIgnoreCase("canvas")) {
            CommitCreated(created, "Create UI Canvas");
            return created;
        }
    }
    auto widget = [&](StringView name, UIElement el) {
        const ecs::Entity e = CreateEntity(name, Transform(), parent);
        world.Add(e, el);
        EnsureEntityGuid(world, e);
        return e;
    };
    UIElement el;
    ecs::Entity e;
    if (kind.EqualsIgnoreCase("panel")) {
        el.size = Vec2(480.0f, 0.0f);
        el.padding = Vec4(24.0f);
        el.gap = 12.0f;
        e = widget("Panel", el);
        world.Add(e, UIPanel{});
    } else if (kind.EqualsIgnoreCase("text")) {
        e = widget("Text", el);
        world.Add(e, UIText{});
    } else if (kind.EqualsIgnoreCase("button")) {
        el.size = Vec2(280.0f, 64.0f);
        el.justify = UIJustify::Center;
        el.align = UIAlign::Center;
        e = widget("Button", el);
        world.Add(e, UIButton{});
        const ecs::Entity label = CreateEntity("Label", Transform(), e);
        world.Add(label, UIElement{});
        UIText t;
        t.text = "Button";
        t.align = UITextAlign::Center;
        t.weight = UIFontWeight::SemiBold;
        t.shadow = false;
        world.Add(label, t);
        EnsureEntityGuid(world, label);
    } else if (kind.EqualsIgnoreCase("image")) {
        el.size = Vec2(128.0f, 128.0f);
        e = widget("Image", el);
        world.Add(e, UIImage{});
    } else if (kind.EqualsIgnoreCase("progress") || kind.EqualsIgnoreCase("progressbar")) {
        el.size = Vec2(320.0f, 20.0f);
        e = widget("Progress Bar", el);
        world.Add(e, UIProgressBar{});
    } else if (kind.EqualsIgnoreCase("slider")) {
        el.size = Vec2(320.0f, 32.0f);
        e = widget("Slider", el);
        world.Add(e, UISlider{});
    } else if (kind.EqualsIgnoreCase("toggle")) {
        el.size = Vec2(64.0f, 34.0f);
        e = widget("Toggle", el);
        world.Add(e, UIToggle{});
    } else if (kind.EqualsIgnoreCase("input") || kind.EqualsIgnoreCase("inputfield")) {
        el.size = Vec2(420.0f, 52.0f);
        e = widget("Input Field", el);
        world.Add(e, UIInputField{});
    } else if (kind.EqualsIgnoreCase("scroll") || kind.EqualsIgnoreCase("scrollview")) {
        el.size = Vec2(480.0f, 360.0f);
        el.padding = Vec4(16.0f);
        el.gap = 8.0f;
        el.scroll = true;
        e = widget("Scroll View", el);
        world.Add(e, UIPanel{});
    } else if (kind.EqualsIgnoreCase("nameplate")) {
        // A world-space canvas floating over the selected object: a name and a health bar.
        const ecs::Entity owner = world.IsAlive(selected) && !world.Has<UIElement>(selected) ? selected : ecs::Entity();
        const ecs::Entity c = CreateEntity("Name Plate", Transform(), owner);
        UICanvas cv;
        cv.worldSpace = true;
        world.Add(c, cv);
        UIElement root;
        root.pivot = Vec2(0.5f, 1.0f);
        root.padding = Vec4(10.0f, 6.0f, 10.0f, 8.0f);
        root.gap = 4.0f;
        root.align = UIAlign::Center;
        world.Add(c, root);
        EnsureEntityGuid(world, c);
        const ecs::Entity name = CreateEntity("Name", Transform(), c);
        world.Add(name, UIElement{});
        UIText t;
        t.text = "Name";
        t.fontSize = 26.0f;
        t.weight = UIFontWeight::SemiBold;
        t.align = UITextAlign::Center;
        world.Add(name, t);
        EnsureEntityGuid(world, name);
        const ecs::Entity bar = CreateEntity("Health", Transform(), c);
        UIElement be;
        be.size = Vec2(160.0f, 10.0f);
        world.Add(bar, be);
        UIProgressBar pb;
        pb.value = 1.0f;
        world.Add(bar, pb);
        EnsureEntityGuid(world, bar);
        if (created)
            world.Destroy(created); // no screen canvas needed
        CommitCreated(c, "Create Name Plate");
        Select(c);
        return c;
    } else {
        ZX_LOG_WARN(LogEditor, "Unknown UI widget '{}' (canvas, panel, text, button, image, progress, slider, toggle, input, scroll, nameplate)", kind);
        if (created)
            CommitCreated(created, "Create UI Canvas");
        return created;
    }
    CommitCreated(created ? created : e, "Create UI Widget");
    Select(e);
    return e;
}

void Editor::CreateTerrain(bool flat)
{
    // Centered under the scene: the flat middle covers what is already there (meshes within 150 m) and sits just
    // below y = 0, so existing floors stay on top.
    Terrain t;
    f32 radius = 0.0f;
    scene::UpdateWorldTransforms(world);
    world.Query<const MeshRenderer, const scene::WorldTransform>().Each([&](const MeshRenderer& mr, const scene::WorldTransform& wt) {
        const render::GpuMesh* mesh = mr.visible ? renderAssets->GetMesh(mr.mesh) : nullptr;
        if (!mesh)
            return;
        for (u32 i = 0; i < 8; ++i) {
            const Vec3 c = wt.matrix.TransformPoint(Vec3((i & 1) ? mesh->bounds.max.x : mesh->bounds.min.x, (i & 2) ? mesh->bounds.max.y : mesh->bounds.min.y,
                                                         (i & 4) ? mesh->bounds.max.z : mesh->bounds.min.z));
            const f32 r = Sqrt(c.x * c.x + c.z * c.z);
            if (r < 150.0f)
                radius = Max(radius, r);
        }
    });
    t.flatCenter = Clamp(radius + 4.0f, t.flatCenter, 120.0f);
    t.size = Max(t.size, t.flatCenter * 5.0f);
    if (flat)
        t.hills = false; // a blank sheet for the Terrain tool
    // The terrain material (painting needs it): written into projects without Starter Content.
    if (WriteTerrainMaterial(contentRoot) && materials)
        materials->ScanContent();
    for (StringView name : {StringView("M_Terrain"), StringView("M_Grass")}) {
        const String path = String::Format("{}/StarterContent/Materials/{}.zmat", contentRoot, name);
        if (materials && platform::FileExists(path)) {
            const MaterialLibrary::Material* m = materials->FindByPath(path);
            t.material = m ? m->guid : materials->LoadFile(path);
            break;
        }
    }
    Array<f32> heights;
    GenerateTerrainHeights(t, heights);
    const u32 n = TerrainSamples(t);
    const f32 center = flat ? 0.0f : heights[usize(n / 2) * n + n / 2];
    const ecs::Entity e = CreateEntity(flat ? "Flat Terrain" : "Terrain", Transform(Vec3(0.0f, -center - (flat ? 0.02f : 0.15f), 0.0f)));
    world.Add(e, t);
    terrains.Update(world, *assets, renderAssets.Get());
    CommitCreated(e, flat ? "Create Flat Terrain" : "Create Terrain");
}

void Editor::CreateHeightFog(f32 density)
{
    ecs::Entity existing;
    world.Query<const HeightFog>().Each([&](ecs::Entity e, const HeightFog&) {
        if (existing.IsNull())
            existing = e;
    });
    if (existing) {
        if (density > 0.0f)
            world.Get<HeightFog>(existing)->density = density;
        selected = existing;
        return;
    }
    const ecs::Entity e = CreateEntity("Height Fog", Transform(Vec3(0.0f)));
    HeightFog fog;
    if (density > 0.0f)
        fog.density = density;
    world.Add(e, fog);
    CommitCreated(e, "Create Height Fog");
}

void Editor::CreateFluid(Vec3 position, bool isFire, ecs::Entity parent)
{
    Transform t(position + Vec3(0.0f, 2.0f, 0.0f)); // the box stands on the point
    t.scale = isFire ? Vec3(2.0f, 4.0f, 2.0f) : Vec3(3.0f, 6.0f, 3.0f);
    const ecs::Entity e = CreateEntity(isFire ? "Fluid Fire" : "Fluid Smoke", t, parent);
    FluidVolume f;
    if (!isFire) {
        f.heat = 0.15f;
        f.fireIntensity = 0.0f;
        f.smoke = 2.0f;
        f.smokeColor = Vec3(0.35f);
        f.buoyancy = 2.5f;
    }
    world.Add(e, f);
    CommitCreated(e, isFire ? "Create Fluid Fire" : "Create Fluid Smoke");
}

Quat Editor::SunRotation(f32 elevation, f32 azimuth)
{
    const Vec3 toSun = atmosphere::SunDirection(elevation, azimuth);
    return Quat::LookRotation(-toSun, Abs(toSun.y) > 0.99f ? Vec3(0, 0, 1) : Vec3(0, 1, 0));
}

ecs::Entity Editor::FindSunEntity()
{
    ecs::Entity found;
    world.Query<const DirectionalLight>().Each([&](ecs::Entity e, const DirectionalLight&) {
        if (found.IsNull())
            found = e;
    });
    return found;
}

void Editor::CreateSun()
{
    if (const ecs::Entity existing = FindSunEntity()) {
        selected = existing;
        return;
    }
    const ecs::Entity e = CreateEntity("Sun", Transform(Vec3(0.0f, 10.0f, 0.0f), SunRotation(environment.sunElevation, environment.sunAzimuth)));
    world.Add(e, DirectionalLight{});
    CommitCreated(e, "Create Directional Light");
}

void Editor::CreateSkyLight()
{
    ecs::Entity existing;
    world.Query<const SkyLight>().Each([&](ecs::Entity e, const SkyLight&) {
        if (existing.IsNull())
            existing = e;
    });
    if (existing) {
        selected = existing;
        return;
    }
    const ecs::Entity e = CreateEntity("Sky Light", Transform(Vec3(0.0f, 10.0f, 0.0f)));
    world.Add(e, SkyLight{});
    CommitCreated(e, "Create Sky Light");
}

void Editor::CreateClouds()
{
    ecs::Entity existing;
    world.Query<const VolumetricClouds>().Each([&](ecs::Entity e, const VolumetricClouds&) {
        if (existing.IsNull())
            existing = e;
    });
    if (existing) {
        selected = existing;
        return;
    }
    const ecs::Entity e = CreateEntity("Volumetric Clouds", Transform(Vec3(0.0f)));
    world.Add(e, VolumetricClouds{});
    CommitCreated(e, "Create Volumetric Clouds");
}

void Editor::CreateFogVolume(Vec3 position, ecs::Entity parent)
{
    Transform t(position);
    t.scale = Vec3(8.0f, 3.0f, 8.0f);
    const ecs::Entity e = CreateEntity("Fog Volume", t, parent);
    world.Add(e, FogVolume{});
    CommitCreated(e, "Create Fog Volume");
}

void Editor::UseHdriSky(StringView texture)
{
    ecs::Entity existing;
    world.Query<const HdriSky>().Each([&](ecs::Entity e, const HdriSky&) {
        if (existing.IsNull())
            existing = e;
    });
    if (existing) {
        undo.Begin("Set HDRI Sky");
        undo.Record(existing);
        world.Get<HdriSky>(existing)->texture = texture;
        undo.End();
        Select(existing);
        return;
    }
    const ecs::Entity e = CreateEntity("HDRI Sky", Transform(Vec3(0, 0, 0)));
    HdriSky sky;
    sky.texture = texture;
    world.Add(e, move(sky));
    CommitCreated(e, "Create HDRI Sky");
}

void Editor::CreateReflectionProbe(Vec3 position, ecs::Entity parent)
{
    const ecs::Entity e = CreateEntity("Reflection Probe", Transform(position), parent);
    world.Add(e, ReflectionProbe{});
    CommitCreated(e, "Create Reflection Probe");
}

void Editor::CreatePostProcessVolume(Vec3 position, ecs::Entity parent)
{
    const ecs::Entity e = CreateEntity("Post Process Volume", Transform(position), parent);
    world.Add(e, PostProcessVolume{});
    CommitCreated(e, "Create Post Process Volume");
}

void Editor::CreateReverbZone(Vec3 position, ecs::Entity parent)
{
    const ecs::Entity e = CreateEntity("Reverb Zone", Transform(position), parent);
    world.Add(e, ReverbZone{});
    CommitCreated(e, "Create Reverb Zone");
}

void Editor::CreateShape(BuiltinShape shape, Vec3 position, ecs::Entity parent)
{
    // Shapes rest on the spawn point (pivot at the center, so lift by half the height); the plane lies on it.
    const Vec3 lift = shape == BuiltinShape::Plane ? Vec3::Zero() : Vec3(0, 0.5f, 0);
    const ecs::Entity e = CreateEntity(builtin::ShapeName(shape), Transform(position + lift), parent);
    MeshRenderer mr;
    mr.mesh = builtin::Mesh(shape);
    world.Add(e, mr);
    // A matching collider, like Unity's primitives (static until a Rigid Body is added).
    switch (shape) {
    case BuiltinShape::Sphere: world.Add(e, SphereCollider{}); break;
    case BuiltinShape::Plane: world.Add(e, BoxCollider{Vec3(0.0f, -0.005f, 0.0f), Vec3(1.0f, 0.01f, 1.0f)}); break;
    case BuiltinShape::Cylinder: world.Add(e, CylinderCollider{}); break;
    default: world.Add(e, BoxCollider{}); break;
    }
    CommitCreated(e, String::Format("Create {}", builtin::ShapeName(shape)));
}

bool Editor::Reparent(ecs::Entity child, ecs::Entity parent)
{
    if (!world.IsAlive(child) || child == parent || (parent && scene::IsAncestorOf(world, child, parent)))
        return false;
    undo.Begin(parent ? "Attach" : "Detach");
    undo.Record(child);
    const bool ok = scene::SetParent(world, child, parent, true);
    undo.End();
    return ok;
}

void Editor::CommitCreated(ecs::Entity e, StringView label)
{
    undo.Begin(label);
    undo.RecordCreatedHierarchy(e);
    undo.End();
    Select(e);
}

void Editor::BeginContinuousEdit(StringView label, ecs::Entity e, const JsonValue* before)
{
    if (!m_continuousEdit) {
        undo.Begin(label);
        m_continuousEdit = true;
    }
    if (before)
        undo.RecordState(EnsureEntityGuid(world, e), *before);
    else
        undo.Record(e);
}

void Editor::ApplySelectionAfterUndo(const Array<ecs::Entity>& affected)
{
    if (!affected.Empty())
        selected = affected[0];
    else if (!world.IsAlive(selected))
        selected = ecs::NullEntity;
}

void Editor::Undo()
{
    if (m_continuousEdit)
        return;
    const String label(undo.UndoLabel());
    Array<ecs::Entity> affected;
    if (undo.Undo(&affected)) {
        ApplySelectionAfterUndo(affected);
        ZX_LOG_INFO(LogEditor, "Undo: {}", label);
    }
}

void Editor::Redo()
{
    if (m_continuousEdit)
        return;
    const String label(undo.RedoLabel());
    Array<ecs::Entity> affected;
    if (undo.Redo(&affected)) {
        ApplySelectionAfterUndo(affected);
        ZX_LOG_INFO(LogEditor, "Redo: {}", label);
    }
}

ecs::Entity Editor::FindByName(StringView name)
{
    ecs::Entity found;
    world.Query<const scene::EntityName>().Each([&](ecs::Entity e, const scene::EntityName& n) {
        if (n.value == name)
            found = e;
    });
    return found;
}

String Editor::CanonicalScene()
{
    Array<ecs::Entity> roots, all;
    CollectRoots(world, roots);
    for (ecs::Entity r : roots)
        CollectHierarchy(world, r, all);
    Array<String> lines;
    for (ecs::Entity e : all)
        lines.PushBack(ToJsonString(SerializeEntity(world, e), JsonWriteOptions{false}));
    Sort(lines, [](const String& a, const String& b) { return a.View().Compare(b.View()) < 0; });
    String out;
    for (const String& l : lines)
        out += l + "\n";
    return out;
}

bool Editor::RunUndoSelfTest()
{
    // Works on any scene: a "Lights" group is added (outside the history) if the scene has none.
    ecs::Entity lights = FindByName("Lights");
    if (!lights)
        lights = CreateEntity("Lights", Transform::Identity());
    undo.Clear();
    Array<String> states;
    states.PushBack(CanonicalScene());
    CreatePointLight(Vec3(1, 2, 3));
    states.PushBack(CanonicalScene());
    const ecs::Entity light = selected;
    BeginContinuousEdit("Move", light);
    world.Get<Transform>(light)->position = Vec3(4, 5, 6);
    world.Get<PointLight>(light)->lumens = 99.0f;
    undo.End();
    m_continuousEdit = false;
    states.PushBack(CanonicalScene());
    Reparent(light, lights);
    states.PushBack(CanonicalScene());
    Select(lights);
    DuplicateSelected();
    states.PushBack(CanonicalScene());
    ecs::Entity model = FindByName("Sponza");
    if (!model)
        world.Query<const MeshRenderer>().Each([&](ecs::Entity e, const MeshRenderer&) { model = e; });
    if (model) {
        Select(model);
        DeleteSelected();
        states.PushBack(CanonicalScene());
    }
    undo.Begin("Add Spot Light");
    undo.Record(light);
    world.Add(light, SpotLight{});
    world.Remove<PointLight>(light);
    undo.End();
    states.PushBack(CanonicalScene());

    const usize steps = states.Size() - 1;
    bool ok = undo.Count() == steps;
    for (usize i = steps; i > 0 && ok; --i) {
        Undo();
        ok = CanonicalScene() == states[i - 1];
        if (!ok)
            ZX_LOG_ERROR(LogEditor, "Undo self-test: undo of step {} did not restore the previous state", i);
    }
    for (usize i = 1; i <= steps && ok; ++i) {
        Redo();
        ok = CanonicalScene() == states[i];
        if (!ok)
            ZX_LOG_ERROR(LogEditor, "Undo self-test: redo of step {} did not reproduce its state", i);
    }
    while (ok && undo.CanUndo())
        Undo();
    ok = ok && CanonicalScene() == states[0];
    if (ok)
        ZX_LOG_INFO(LogEditor, "Undo self-test passed ({} steps)", steps);
    else
        ZX_LOG_ERROR(LogEditor, "Undo self-test FAILED");
    undo.Clear();
    return ok;
}

// --- Prefabs --------------------------------------------------------------------------------------------------------------

void Editor::CreatePrefabFrom(ecs::Entity e)
{
    if (!world.IsAlive(e) || !prefabs)
        return;
    if (world.Has<PrefabInstance>(e)) {
        ZX_LOG_WARN(LogEditor, "'{}' is already a prefab instance (prefab variants are not supported yet)", EntityLabel(e));
        return;
    }
    static const platform::FileDialogFilter Filters[] = {{"ZeqouX3D prefab", "*.zprefab"}};
    const String folder = contentRoot + "/Prefabs";
    platform::CreateDirectoryTree(folder);
    String path = platform::SaveFileDialog(window.Get(), "Create Prefab", Span<const platform::FileDialogFilter>(Filters, 1),
                                           Path::SanitizeFileName(EntityLabel(e)) + ".zprefab", folder);
    if (path.Empty())
        return;
    CreatePrefabAt(e, path);
}

bool Editor::CreatePrefabAt(ecs::Entity e, StringView physicalPath)
{
    if (!world.IsAlive(e) || !prefabs || world.Has<PrefabInstance>(e))
        return false;
    String path(physicalPath);
    if (!Path::HasExtension(path, ".zprefab"))
        path += ".zprefab";
    if (!Path::IsUnder(path, contentRoot)) {
        ZX_LOG_ERROR(LogEditor, "Prefabs must be saved inside the project's Content folder");
        return false;
    }
    const Guid guid = EnsureEntityGuid(world, e);
    undo.Begin("Create Prefab");
    undo.RecordHierarchy(e);
    const bool ok = CreatePrefabFromEntity(world, e, prefabs->RelativePath(path), *prefabs, assets.Get());
    const ecs::Entity root = FindEntityByGuid(world, guid);
    undo.RecordCreatedHierarchy(root);
    undo.End();
    Select(root);
    panels.browserListedDir.Clear();
    if (ok)
        ZX_LOG_INFO(LogEditor, "Created prefab {}", prefabs->RelativePath(path));
    return ok;
}

void Editor::InstantiatePrefab(StringView physicalPath, Vec3 position)
{
    if (!prefabs)
        return;
    const ecs::Entity e = InstantiatePrefabFile(world, *prefabs, prefabs->RelativePath(physicalPath), Transform(position));
    if (!e) {
        ZX_LOG_ERROR(LogEditor, "Can't instantiate prefab '{}'", physicalPath);
        return;
    }
    CommitCreated(e, String::Format("Place {}", Path::Stem(physicalPath)));
}

void Editor::ApplyPrefab(ecs::Entity e)
{
    const ecs::Entity root = FindPrefabInstanceRoot(world, e);
    if (!root || !prefabs)
        return;
    const String path = world.Get<const PrefabInstance>(root)->prefab;
    const Guid selectedGuid = GetEntityGuid(world, selected);
    // Every instance of the prefab changes: record them all.
    Array<Guid> roots;
    world.Query<const PrefabInstance>().Each([&](ecs::Entity r, const PrefabInstance& p) {
        if (p.prefab == path)
            roots.PushBack(EnsureEntityGuid(world, r));
    });
    undo.Begin(String::Format("Apply {}", Path::Stem(path)));
    for (const Guid& g : roots)
        undo.RecordHierarchy(FindEntityByGuid(world, g));
    const bool ok = ApplyPrefabInstance(world, root, *prefabs, assets.Get());
    for (const Guid& g : roots)
        undo.RecordCreatedHierarchy(FindEntityByGuid(world, g));
    undo.End();
    selected = FindEntityByGuid(world, selectedGuid);
    if (!ok)
        ZX_LOG_ERROR(LogEditor, "Applying to '{}' failed", path);
}

void Editor::RevertPrefab(ecs::Entity e)
{
    const ecs::Entity root = FindPrefabInstanceRoot(world, e);
    if (!root || !prefabs)
        return;
    const Guid guid = GetEntityGuid(world, root);
    const Guid selectedGuid = GetEntityGuid(world, selected);
    undo.Begin("Revert to Prefab");
    undo.RecordHierarchy(root);
    const ecs::Entity rebuilt = RefreshPrefabInstance(world, root, *prefabs, true);
    undo.RecordCreatedHierarchy(rebuilt);
    undo.End();
    selected = FindEntityByGuid(world, selectedGuid);
    if (!selected)
        selected = FindEntityByGuid(world, guid);
}

void Editor::UnpackPrefab(ecs::Entity e)
{
    const ecs::Entity root = FindPrefabInstanceRoot(world, e);
    if (!root)
        return;
    const Guid guid = GetEntityGuid(world, root);
    Array<ecs::Entity> all;
    CollectHierarchy(world, root, all);
    undo.Begin("Unpack Prefab");
    undo.RecordHierarchy(root);
    world.Remove<PrefabInstance>(root);
    for (ecs::Entity m : all)
        if (const PrefabLink* link = world.Get<const PrefabLink>(m); link && link->instance == guid)
            world.Remove<PrefabLink>(m);
    undo.End();
}

// --- Materials ------------------------------------------------------------------------------------------------------------

Guid Editor::CreateMaterial(StringView physicalPath)
{
    if (!materials)
        return {};
    String path(physicalPath);
    if (!Path::HasExtension(path, ".zmat"))
        path += ".zmat";
    const Guid guid = materials->Create(path);
    panels.browserListedDir.Clear();
    if (guid.IsValid())
        ZX_LOG_INFO(LogEditor, "Created material {}", Path::MakeRelative(path, contentRoot));
    return guid;
}

void Editor::AssignMaterial(ecs::Entity e, const Guid& material)
{
    MeshRenderer* mr = world.Get<MeshRenderer>(e);
    if (!mr)
        return;
    const render::GpuMesh* mesh = renderAssets->GetMesh(mr->mesh);
    const usize slots = Max<usize>(mesh ? mesh->materialSlots : 1, Max<usize>(mr->materials.Size(), 1));
    undo.Begin("Assign Material");
    undo.Record(e);
    mr = world.Get<MeshRenderer>(e);
    mr->materials.Resize(slots);
    for (asset::AssetId& m : mr->materials)
        m = material;
    undo.End();
}

bool Editor::RunMaterialSelfTest()
{
    // M8 acceptance: a material with a texture and parameters is built from nodes, and edits reach the viewport in
    // under a second (structural: shader compile; values: no compile at all).
    const String path = contentRoot + "/Materials/SelfTest.zmat";
    const Guid guid = CreateMaterial(path);
    MaterialLibrary::Material* m = materials ? materials->Find(guid) : nullptr;
    if (!m) {
        ZX_LOG_ERROR(LogEditor, "Material self-test: can't create a material");
        return false;
    }
    // Pick any image in the project for the texture.
    String texture;
    Array<String> dirs{contentRoot};
    while (!dirs.Empty() && texture.Empty()) {
        const String dir = dirs.PopBackValue();
        Array<platform::DirectoryEntry> entries;
        platform::ListDirectory(dir, entries);
        for (const platform::DirectoryEntry& e : entries) {
            if (e.isDirectory)
                dirs.PushBack(Path::Join(dir, e.name));
            else if (texture.Empty() && (Path::HasExtension(e.name, ".png") || Path::HasExtension(e.name, ".jpg")))
                texture = Path::MakeRelative(Path::Join(dir, e.name), contentRoot);
        }
    }
    // Shape + material on it.
    CreateShape(BuiltinShape::Sphere, camera.position + camera.Forward() * 4.0f);
    AssignMaterial(selected, guid);

    // Structural edit: Texture Sample * Tint (+ Roughness parameter).
    render::MaterialGraph& g = m->graph;
    const u32 out = g.OutputNode();
    const u32 tex = g.AddNode("TextureSample", Vec2(-300, 0))->id;
    g.FindNode(tex)->texture = texture;
    g.FindNode(tex)->name = "Albedo";
    const u32 tint = g.nodes[1].id; // the default "Base Color" parameter
    const u32 mul = g.AddNode("Multiply", Vec2(0, 0))->id;
    g.Connect(tex, 0, mul, 0);
    g.Connect(tint, 0, mul, 1);
    g.Connect(mul, 0, out, 0);
    MaterialGraphNode* rough = g.AddNode("Scalar", Vec2(0, 200));
    rough->name = "Roughness";
    rough->value.x = 0.3f;
    g.Connect(rough->id, 0, out, 2);
    Stopwatch sw;
    materials->Apply(guid);
    while (materials->ShaderState(guid) == render::MaterialShaderCache::State::Compiling && sw.ElapsedMilliseconds() < 5000.0)
        Thread::SleepMs(1);
    const f64 structuralMs = sw.ElapsedMilliseconds();
    const bool compiled = materials->ShaderState(guid) == render::MaterialShaderCache::State::Ready && m->compiled.Ok();

    // Value edit: same shader, new parameter block.
    const u32 shaderBefore = m->shader;
    g.FindNode(tint)->value = Vec4(1.0f, 0.3f, 0.1f, 1.0f);
    sw.Restart();
    materials->Apply(guid);
    const f64 valueMs = sw.ElapsedMilliseconds();
    const bool sameShader = m->shader == shaderBefore;
    materials->Save(guid);

    const bool ok = compiled && structuralMs < 1000.0 && sameShader && !texture.Empty();
    if (ok)
        ZX_LOG_INFO(LogEditor, "Material self-test passed: structural edit {:.0f} ms, value edit {:.2f} ms (no recompile), texture {}",
                    structuralMs, valueMs, texture);
    else
        ZX_LOG_ERROR(LogEditor, "Material self-test FAILED: compiled {}, structural {:.0f} ms, same shader {}, texture '{}'", compiled,
                     structuralMs, sameShader, texture);
    return ok;
}

// --- Play-in-Editor -----------------------------------------------------------------------------------------------------

void Editor::BeginPlay()
{
    if (IsPlaying())
        return;
    if (m_continuousEdit) {
        undo.End();
        m_continuousEdit = false;
    }
    m_pieSnapshot = zx::SaveScene(world, SceneIOOptions{});
    m_pieSelection = GetEntityGuid(world, selected);
    undo.SetSuspended(true);
    playState = PlayState::Playing;
    m_pieStep = false;
    GameSystems systems = GameSystemsForPlay();
    BeginGame(world, systems);
    PlayUISound("Toggle");
    gameCameraEjected = false;
    SetGameInputCaptured(FindGameCamera(world) != ecs::NullEntity && m_autotestFrames == 0); // tests keep the cursor free
    ZX_LOG_INFO(LogEditor, "Play-in-Editor started ({} entities)", m_pieSnapshot.Get("entities").Size());
}

void Editor::EndPlay()
{
    if (!IsPlaying())
        return;
    if (m_continuousEdit) {
        undo.End();
        m_continuousEdit = false;
    }
    GameSystems systems = GameSystemsForPlay();
    EndGame(systems);
    PlayUISound("Back");
    if (audioEngine)
        audioEngine->SetBusMuted(audio::Bus::Master, false);
    SetGameInputCaptured(false);
    gameCameraEjected = false;
    // Restore the edit-time world: same entities and guids, so the undo history stays valid.
    ClearScene(world);
    zx::LoadScene(m_pieSnapshot, world, SceneIOOptions{});
    m_pieSnapshot = JsonValue();
    selected = FindEntityByGuid(world, m_pieSelection);
    undo.SetSuspended(false);
    playState = PlayState::Editing;
    viewport->ResetHistory();
    ZX_LOG_INFO(LogEditor, "Play-in-Editor stopped, scene restored");
}

void Editor::SetFeatureEnabled(StringView id, bool enabled)
{
    Array<String>& off = project.descriptor.disabledFeatures;
    off.RemoveIf([&](const String& f) { return f == id; });
    if (!enabled)
        off.PushBack(String(id));
    if (!project.descriptorPath.Empty())
        SaveProjectDescriptor(project.descriptorPath, project.descriptor);
    if ((!FeatureEnabled("brush") && tools.active == EditorTool::Brush) || (!FeatureEnabled("measure") && tools.active == EditorTool::Measure))
        tools.active = EditorTool::None;
    ZX_LOG_INFO(LogEditor, "Feature '{}' {}", id, enabled ? "enabled" : "disabled");
}

void Editor::SetGraphicsEra(StringView id)
{
    project.descriptor.graphicsEra = String(GraphicsEraId(ParseGraphicsEra(id)));
    if (!project.descriptorPath.Empty())
        SaveProjectDescriptor(project.descriptorPath, project.descriptor);
    viewport->ResetHistory();
    ZX_LOG_INFO(LogEditor, "Graphics: {}", project.descriptor.graphicsEra);
}

bool Editor::UsingGameCamera()
{
    return IsPlaying() && !gameCameraEjected && FindGameCamera(world) != ecs::NullEntity;
}

void Editor::SetGameInputCaptured(bool captured)
{
    gameInputCaptured = captured && IsPlaying();
    if (gameInputCaptured)
        flying = false;
}

void Editor::ToggleEject()
{
    if (!IsPlaying())
        return;
    gameCameraEjected = !gameCameraEjected;
    SetGameInputCaptured(!gameCameraEjected && FindGameCamera(world) != ecs::NullEntity);
    ZX_LOG_INFO(LogEditor, gameCameraEjected ? "Ejected: editor camera (F8 to possess the game camera again)" : "Possessed the game camera");
}

void Editor::TogglePause()
{
    if (IsPlaying())
        playState = playState == PlayState::Paused ? PlayState::Playing : PlayState::Paused;
    if (audioEngine) // paused game = silent game
        audioEngine->SetBusMuted(audio::Bus::Master, playState == PlayState::Paused);
}

void Editor::PlayUISound(StringView name, f32 volume)
{
    if (uiSounds && audio)
        audio->PlayOneShot(String::Format("{}/Engine/Content/StarterContent/Sounds/UI/{}", engineRoot, name), volume, false, Vec3(0.0f),
                           audio::Bus::UI);
}

GameSystems Editor::GameSystemsForPlay()
{
    GameSystems s;
    s.scripts = scripts.Get();
    s.game = game.Get();
    s.audio = audio.Get();
    s.physics = &physics;
    s.particles = &particles;
    s.fire = &fire;
    s.animation = animation.Get();
    s.softBodies = &softBodies;
    s.gameUI = &gameUI;
    s.water = &waterSystem;
    s.renderAssets = renderAssets.Get();
    s.inputActions = &inputActions;
    s.inputActionState = &inputActionState;
    s.featureEnabled = [this](StringView feature) { return FeatureEnabled(feature); };
    return s;
}

ListenerPose Editor::CurrentListener()
{
    ListenerPose pose;
    pose.position = camera.position;
    pose.forward = camera.Forward();
    pose.up = Vec3::Up();
    if (UsingGameCamera())
        if (const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(FindGameCamera(world))) {
            pose.position = wt->GetPosition();
            pose.forward = SafeNormalize(wt->matrix.TransformVector(Vec3::Forward()), Vec3::Forward());
            pose.up = SafeNormalize(wt->matrix.TransformVector(Vec3::Up()), Vec3::Up());
        }
    return pose;
}

void Editor::StepFrame()
{
    if (!IsPlaying())
        BeginPlay();
    playState = PlayState::Paused;
    m_pieStep = true;
}

void Editor::RunPhysicsSelfTest()
{
    auto fail = [this](StringView why) {
        ZX_LOG_ERROR(LogEditor, "Physics self-test FAILED: {}", why);
        selfTestFailed = true;
        if (IsPlaying())
            EndPlay();
        m_physicsTestStage = 0;
    };
    switch (m_physicsTestStage) {
    case 1: {
        // Static collision for the level, then a column of cubes in front of the camera, above the floor.
        u32 meshes = 0;
        Array<ecs::Entity> targets;
        world.Query<const MeshRenderer>().Each([&](ecs::Entity e, const MeshRenderer&) { targets.PushBack(e); });
        for (ecs::Entity e : targets)
            if (!HasCollider(world, e)) {
                world.Add(e, MeshCollider{});
                ++meshes;
            }
        scene::UpdateWorldTransforms(world);
        const Vec3 ahead = camera.position + Vec3(camera.Forward().x, 0.0f, camera.Forward().z) * 3.0f;
        RaycastHit floorHit;
        if (!RaycastMeshes(world, *renderAssets, Ray(ahead + Vec3(0, 2, 0), Vec3(0, -1, 0)), floorHit, 50.0f)) {
            if (frameIndex > 600)
                return fail("no floor under the camera");
            return; // the level's meshes are still uploading
        }
        m_physicsTestFloor = floorHit.point.y;
        m_physicsTestCubes.Clear();
        m_physicsTestStart.Clear();
        for (u32 i = 0; i < 5; ++i) {
            CreateShape(BuiltinShape::Cube, Vec3(floorHit.point.x, m_physicsTestFloor + 1.0f + f32(i) * 1.3f, floorHit.point.z));
            if (!world.Get<BoxCollider>(selected))
                return fail("Create Cube did not add a Box Collider");
            world.Add(selected, RigidBody{});
            m_physicsTestCubes.PushBack(selected);
            m_physicsTestStart.PushBack(world.Get<Transform>(selected)->position);
        }
        // A cylinder on top colliding through a convex Mesh Collider (hull of its mesh).
        CreateShape(BuiltinShape::Cylinder, Vec3(floorHit.point.x, m_physicsTestFloor + 1.0f + 5.0f * 1.3f, floorHit.point.z));
        world.Remove<CapsuleCollider>(selected);
        world.Remove<CylinderCollider>(selected);
        world.Remove<BoxCollider>(selected);
        world.Remove<SphereCollider>(selected);
        world.Add(selected, MeshCollider{.convex = true});
        world.Add(selected, RigidBody{});
        m_physicsTestCubes.PushBack(selected);
        m_physicsTestStart.PushBack(world.Get<Transform>(selected)->position);
        Select(ecs::NullEntity);
        ZX_LOG_INFO(LogEditor, "Physics self-test: {} mesh colliders, 5 cubes, a convex cylinder", meshes);
        BeginPlay();
        m_physicsTestStage = 2;
        break;
    }
    case 2: { // the cubes fall, stack and come to rest
        if (frameIndex < m_physicsTestFrame + 30 || !physics.IsRunning())
            return;
        bool resting = true;
        for (ecs::Entity e : m_physicsTestCubes) {
            const physics::BodyId id = physics.BodyOf(e);
            if (!physics.World()->IsValid(id))
                return fail("a cube has no body");
            resting &= physics.World()->IsSleeping(id) || Length(physics.World()->GetLinearVelocity(id)) < 0.05f;
        }
        if (!resting) {
            if (frameIndex > m_physicsTestFrame + 1500)
                return fail("the cubes did not come to rest");
            return;
        }
        Array<f32> heights;
        for (ecs::Entity e : m_physicsTestCubes) {
            const Vec3 p = world.Get<Transform>(e)->position;
            if (!(p.y == p.y))
                return fail("NaN position");
            heights.PushBack(p.y - m_physicsTestFloor);
        }
        for (usize i = 1; i < heights.Size(); ++i)
            for (usize j = i; j > 0 && heights[j] < heights[j - 1]; --j)
                swap(heights[j], heights[j - 1]);
        if (heights[0] < 0.3f || heights[0] > 0.8f)
            return fail(String::Format("bottom cube at {:.2f} m above the floor", heights[0]));
        if (heights.Back() > 6.0f || heights.Back() < 0.3f)
            return fail(String::Format("top cube at {:.2f} m", heights.Back()));
        const physics::PhysicsStats& st = physics.World()->Stats();
        ZX_LOG_INFO(LogEditor, "Physics self-test: cubes at {:.2f}..{:.2f} m, {} bodies, {} contacts, step {:.2f} ms", heights[0], heights.Back(),
                    st.bodies, st.contacts, st.stepMs);
        EndPlay();
        m_physicsTestStage = 3;
        break;
    }
    case 3: { // Stop restored the scene (entities are recreated: find the cubes by their Rigid Body)
        u32 restored = 0;
        world.Query<const RigidBody, const Transform>().Each([&](const RigidBody&, const Transform& t) {
            for (Vec3 start : m_physicsTestStart)
                if (Distance(t.position, start) < 1e-3f) {
                    ++restored;
                    break;
                }
        });
        if (restored != m_physicsTestStart.Size())
            return fail(String::Format("Stop restored {} of {} cubes", restored, m_physicsTestStart.Size()));
        ZX_LOG_INFO(LogEditor, "Physics self-test passed");
        m_physicsTestStage = 0;
        return;
    }
    default: m_physicsTestStage = 0; return;
    }
    m_physicsTestFrame = frameIndex;
}

void Editor::RunHdriSelfTest()
{
    auto fail = [this](StringView why) {
        ZX_LOG_ERROR(LogEditor, "HDRI self-test FAILED: {}", why);
        selfTestFailed = true;
        m_hdriTestStage = 0;
    };
    // The panorama's sun: 30 degrees up, towards panorama +X (u = 0.75).
    const Vec3 sunPanorama = asset::LatLongDirection(0.75f, 1.0f / 3.0f);
    auto sunMatches = [this](Vec3 expected) {
        const render::SceneLighting& l = viewport->Lighting();
        return l.sunIlluminance > 0.0f && Dot(-l.sunDirection, Normalize(expected)) > Cos(DegToRad(3.0f));
    };
    HdriSky* sky = nullptr;
    world.Query<HdriSky>().Each([&](HdriSky& s) { sky = &s; });
    switch (m_hdriTestStage) {
    case 1: {
        // Sky gradient, darker ground, a small very bright sun disk.
        constexpr u32 W = 256, H = 128;
        Image img = Image::Create(W, H, ImageFormat::RGBA32F);
        for (u32 y = 0; y < H; ++y)
            for (u32 x = 0; x < W; ++x) {
                const Vec3 d = asset::LatLongDirection((f32(x) + 0.5f) / W, (f32(y) + 0.5f) / H);
                Vec3 c = d.y > 0.0f ? Lerp(Vec3(0.8f, 0.9f, 1.0f), Vec3(0.25f, 0.45f, 0.9f), d.y) : Vec3(0.12f, 0.1f, 0.08f);
                if (Dot(d, sunPanorama) > Cos(DegToRad(2.0f)))
                    c = Vec3(20000.0f, 19000.0f, 17000.0f);
                f32* p = reinterpret_cast<f32*>(img.Row(y) + x * 16);
                p[0] = c.x;
                p[1] = c.y;
                p[2] = c.z;
                p[3] = 1.0f;
            }
        m_hdriTestFile = contentRoot + "/HdriSelfTest.exr";
        const Array<u8> file = exr::Encode(img);
        if (!fs::WriteFile(m_hdriTestFile, file.View()))
            return fail("can't write the test panorama");
        UseHdriSky("HdriSelfTest.exr");
        m_hdriTestStage = 2;
        break;
    }
    case 2: // the panorama is resident: its sun is the directional light
        if (viewport->LastHdri().IsActive() && sunMatches(render::HdriToWorld(sunPanorama, 0.0f))) {
            if (!viewport->HdriSunFound())
                return fail("no sun found in the panorama");
            ZX_LOG_INFO(LogEditor, "HDRI self-test: sun found ({:.0f} lx)", viewport->Lighting().sunIlluminance);
            sky->rotation = 90.0f;
            m_hdriTestStage = 3;
            break;
        }
        if (frameIndex > m_hdriTestFrame + 600)
            return fail(String::Format("panorama not applied (active {}, sun {:.0f} lx)", viewport->LastHdri().IsActive(),
                                       viewport->Lighting().sunIlluminance));
        return;
    case 3: // rotating the sky turns its sun
        if (sunMatches(render::HdriToWorld(sunPanorama, DegToRad(90.0f)))) {
            sky->sun = HdriSunMode::None;
            m_hdriTestStage = 4;
            break;
        }
        if (frameIndex > m_hdriTestFrame + 120)
            return fail("the sun did not follow the rotation");
        return;
    case 4: // Sun = None: no directional light
        if (viewport->Lighting().sunIlluminance == 0.0f) {
            ZX_LOG_INFO(LogEditor, "HDRI self-test passed (the sun follows the rotation and the sun mode)");
            m_hdriTestStage = 0;
            return;
        }
        if (frameIndex > m_hdriTestFrame + 120)
            return fail("Sun = None still lights the scene");
        return;
    default: m_hdriTestStage = 0; return;
    }
    m_hdriTestFrame = frameIndex;
}

void Editor::RunDecalSelfTest()
{
    auto fail = [this](StringView why) {
        ZX_LOG_ERROR(LogEditor, "Decal self-test FAILED: {}", why);
        selfTestFailed = true;
        m_decalTestStage = 0;
    };
    switch (m_decalTestStage) {
    case 1: {
        // Any image of the project for the textured decal.
        String texture;
        Array<String> dirs{contentRoot};
        while (!dirs.Empty() && texture.Empty()) {
            const String dir = dirs.PopBackValue();
            Array<platform::DirectoryEntry> entries;
            platform::ListDirectory(dir, entries);
            for (const platform::DirectoryEntry& e : entries) {
                if (e.isDirectory)
                    dirs.PushBack(Path::Join(dir, e.name));
                else if (texture.Empty() && (Path::HasExtension(e.name, ".png") || Path::HasExtension(e.name, ".jpg")) &&
                         !e.name.View().Contains("normal") && !e.name.View().Contains("Normal"))
                    texture = Path::MakeRelative(Path::Join(dir, e.name), contentRoot);
            }
        }
        const Vec3 ahead = camera.position + camera.Forward() * 4.0f;
        CreateDecal(ahead + camera.Rotation().GetRight() * -1.2f);
        Decal* textured = world.Get<Decal>(selected);
        if (!textured)
            return fail("Create Decal did not create a decal");
        textured->texture = texture;
        textured->extent = Vec3(1.0f, 1.0f, 1.0f);
        CreateDecal(ahead + camera.Rotation().GetRight() * 1.2f);
        Decal* glow = world.Get<Decal>(selected);
        glow->color = Vec3(0.1f, 0.6f, 1.0f);
        glow->emissive = 4.0f;
        glow->extent = Vec3(0.8f, 1.0f, 0.8f);
        Select(ecs::NullEntity);
        m_decalTestStage = 2;
        break;
    }
    case 2: { // both reach the renderer once the texture streamed in
        const u32 drawn = viewport->Scene().GetStats().decals;
        if (drawn >= 2) {
            ZX_LOG_INFO(LogEditor, "Decal self-test passed ({} decals rendered)", drawn);
            m_decalTestStage = 0;
            return;
        }
        if (frameIndex > m_decalTestFrame + 300)
            return fail(String::Format("{} of 2 decals rendered", drawn));
        return;
    }
    default: m_decalTestStage = 0; return;
    }
    m_decalTestFrame = frameIndex;
}

void Editor::RunReflectionSelfTest()
{
    const Vec3 base(0.0f, 60.0f, 400.0f); // open air, away from the sample scene
    auto fail = [this](StringView why) {
        ZX_LOG_ERROR(LogEditor, "Reflection self-test FAILED: {}", why);
        selfTestFailed = true;
        m_reflTestStage = 0;
    };
    render::ReflectionRenderer& refl = viewport->Reflections();
    switch (m_reflTestStage) {
    case 1: {
        // Materials: a mirror (metal, roughness 0.05) and two emissive colors.
        auto makeMaterial = [this](StringView name, Vec3 color, f32 metallic, f32 roughness, bool emissive) {
            const String path = String::Format("{}/Materials/{}.zmat", contentRoot, name);
            platform::DeleteFileAt(path);
            const Guid guid = CreateMaterial(path);
            MaterialLibrary::Material* m = materials ? materials->Find(guid) : nullptr;
            if (!m)
                return Guid();
            m_reflTestFiles.PushBack(path);
            render::MaterialGraph& g = m->graph;
            const u32 out = g.OutputNode();
            g.nodes[1].value = Vec4(color.x, color.y, color.z, 1.0f);
            if (emissive)
                g.Connect(g.nodes[1].id, 0, out, 4);
            MaterialGraphNode* met = g.AddNode("Scalar", Vec2(0, 150));
            met->value.x = metallic;
            g.Connect(met->id, 0, out, 1);
            MaterialGraphNode* rough = g.AddNode("Scalar", Vec2(0, 250));
            rough->value.x = roughness;
            g.Connect(rough->id, 0, out, 2);
            materials->Apply(guid);
            return guid;
        };
        const Guid mirror = makeMaterial("ReflTestMirror", Vec3(0.95f, 0.95f, 0.95f), 1.0f, 0.05f, false);
        const Guid red = makeMaterial("ReflTestRed", Vec3(4.0f, 0.2f, 0.1f), 0.0f, 0.5f, true);
        const Guid green = makeMaterial("ReflTestGreen", Vec3(0.2f, 3.0f, 0.3f), 0.0f, 0.5f, true);
        if (!mirror.IsValid() || !red.IsValid() || !green.IsValid())
            return fail("can't create the test materials");
        CreateShape(BuiltinShape::Plane, base);
        AssignMaterial(selected, mirror);
        CreateShape(BuiltinShape::Cube, base + Vec3(-1.5f, 0.6f, 2.5f));
        AssignMaterial(selected, red);
        m_reflTestMover = selected;
        CreateShape(BuiltinShape::Sphere, base + Vec3(1.5f, 0.6f, 2.0f));
        AssignMaterial(selected, green);
        CreateReflectionProbe(base + Vec3(0.0f, 1.0f, 0.0f));
        world.Get<ReflectionProbe>(selected)->extent = Vec3(5.0f, 3.0f, 5.0f);
        Select(ecs::NullEntity);
        camera.position = base + Vec3(0.0f, 1.6f, -4.5f);
        camera.LookAt(base + Vec3(0.0f, 0.3f, 2.0f));
        viewport->Settings().reflections.ssr = true;
        m_reflTestStage = 2;
        break;
    }
    case 2: // the probe is captured within its 6 faces (+ a few frames for materials and transforms)
        if (refl.CapturedProbes() >= 1 && !refl.IsCapturing()) {
            // Moving a shape changes the scene content: the probe captures again.
            world.Get<Transform>(m_reflTestMover)->position.x += 0.5f;
            m_reflTestStage = 3;
        } else if (frameIndex > m_reflTestFrame + 120) {
            return fail(String::Format("the probe was not captured ({} probes, {} captured)", refl.ProbeCount(), refl.CapturedProbes()));
        }
        return;
    case 3:
        if (!refl.IsCapturing()) {
            if (frameIndex > m_reflTestFrame + 30)
                return fail("moving a shape did not recapture the probe");
            return;
        }
        m_reflTestStage = 4;
        return;
    case 4:
        if (refl.IsCapturing()) {
            if (frameIndex > m_reflTestFrame + 120)
                return fail("the recapture did not finish");
            return;
        }
        ZX_LOG_INFO(LogEditor, "Reflection self-test passed (probe captured and recaptured, SSR on)");
        m_reflTestStage = 0;
        return;
    default: m_reflTestStage = 0; return;
    }
    m_reflTestFrame = frameIndex;
}

void Editor::RunPostSelfTest()
{
    // Stage 1 places a global volume (saturation 0.2) and a local one 30 m away (exposure +2, saturation 1.6, depth of
    // field, 1 m half size, 4 m blend radius, higher priority); each later stage moves the editor camera and checks the
    // settings the viewport rendered with two frames later.
    const Vec3 center(0.0f, 2.0f, 30.0f);
    auto fail = [this](StringView why) {
        ZX_LOG_ERROR(LogEditor, "Post self-test FAILED: {}", why);
        selfTestFailed = true;
        m_postTestStage = 0;
    };
    auto near = [](f32 a, f32 b) { return Abs(a - b) < 0.02f; };
    if (m_postTestStage > 1 && frameIndex < m_postTestFrame + 3)
        return;
    const render::PostSettings& s = viewport->LastPostSettings();
    switch (m_postTestStage) {
    case 1: {
        PostProcessVolume global;
        global.unbound = true;
        global.overrideSaturation = true;
        global.saturation = 0.2f;
        const ecs::Entity g = CreateEntity("Global Post Volume", Transform(Vec3(0.0f)));
        world.Add(g, global);
        PostProcessVolume local;
        local.extent = Vec3(1.0f);
        local.blendRadius = 4.0f;
        local.priority = 1;
        local.overrideExposureCompensation = true;
        local.exposureCompensation = 2.0f;
        local.overrideSaturation = true;
        local.saturation = 1.6f;
        local.overrideDepthOfField = true;
        local.overrideFocusDistance = true;
        local.focusDistance = 2.0f;
        const ecs::Entity l = CreateEntity("Local Post Volume", Transform(center));
        world.Add(l, local);
        camera.position = Vec3(0.0f, 2.0f, -20.0f); // 49 m away from the local volume
        m_postTestStage = 2;
        break;
    }
    case 2: // global only
        if (!near(s.saturation, 0.2f) || s.depthOfField)
            return fail(String::Format("outside: saturation {} dof {}", s.saturation, s.depthOfField));
        camera.position = center; // inside
        m_postTestStage = 3;
        break;
    case 3:
        if (!near(s.saturation, 1.6f) || !near(s.exposureCompensation, 2.0f) || !s.depthOfField ||
            !near(s.focusDistance, 2.0f))
            return fail(String::Format("inside: saturation {} exposure {} dof {}", s.saturation, s.exposureCompensation, s.depthOfField));
        camera.position = center + Vec3(2.9f, 0.0f, 0.0f); // 1.9 m out of the box: weight 1 - 1.9 / 4
        m_postTestStage = 4;
        break;
    case 4: {
        const f32 w = 1.0f - 1.9f / 4.0f;
        const f32 base = viewport->Settings().post.exposureCompensation; // r.ExposureCompensation
        if (!near(s.saturation, Lerp(0.2f, 1.6f, w)) || !near(s.exposureCompensation, Lerp(base, 2.0f, w)) || !s.depthOfField)
            return fail(String::Format("blend: saturation {} exposure {} dof {}", s.saturation, s.exposureCompensation, s.depthOfField));
        // Motion blur: playing with the camera moving (rendered under the debug layer).
        showMotionBlur = true;
        viewport->Settings().post.motionBlur = true;
        BeginPlay();
        m_postTestStage = 5;
        break;
    }
    case 5:
        camera.position += Vec3(0.3f, 0.0f, 0.0f);
        if (frameIndex < m_postTestFrame + 20)
            return;
        if (!s.motionBlur)
            return fail("motion blur is not enabled while playing");
        EndPlay();
        viewport->Settings().post.motionBlur = false;
        ZX_LOG_INFO(LogEditor, "Post self-test passed (volumes blend, depth of field, motion blur)");
        m_postTestStage = 0;
        return;
    default: m_postTestStage = 0; return;
    }
    m_postTestFrame = frameIndex;
}

bool Editor::RunPieSelfTest()
{
    // Give the scene something to animate (outside the undo history), remember it, play, mutate, stop, compare.
    ecs::Entity target = FindByName("Lights");
    if (!target)
        target = CreateEntity("Lights", Transform::Identity());
    world.Add(target, Rotator{});
    world.Add(target, Oscillator{});
    const String before = CanonicalScene();
    const usize undoSteps = undo.Count();
    const Quat startRotation = world.Get<Transform>(target)->rotation;

    BeginPlay();
    for (u32 i = 0; i < 30; ++i)
        TickGameplay(world, 1.0f / 30.0f);
    const ecs::Entity playTarget = FindByName("Lights");
    const bool animated = playTarget && Abs(Dot(world.Get<Transform>(playTarget)->rotation, startRotation)) < 0.999f;
    // Edits while playing must not survive: delete something, create something, try to record an undo step.
    if (const ecs::Entity any = FindByName("Point Light 1"))
        scene::DestroyHierarchy(world, any);
    CreatePointLight(Vec3(1, 1, 1));
    EndPlay();

    const bool restored = CanonicalScene() == before;
    const bool historyKept = undo.Count() == undoSteps;
    const bool ok = animated && restored && historyKept;
    if (ok)
        ZX_LOG_INFO(LogEditor, "PIE self-test passed");
    else
        ZX_LOG_ERROR(LogEditor, "PIE self-test FAILED (animated {}, restored {}, history kept {})", animated, restored, historyKept);
    world.Remove<Rotator>(FindByName("Lights"));
    world.Remove<Oscillator>(FindByName("Lights"));
    return ok;
}

bool Editor::RunToolSelfTest(bool& retry)
{
    // Picking, Drop to Ground, Brush paint / undo / redo / erase and feature toggles on built-in shapes.
    static ecs::Entity s_floor, s_box;
    retry = false;
    if (!s_box) {
        Transform ft(Vec3(0, 0, 50));
        ft.scale = Vec3(20.0f, 1.0f, 20.0f);
        s_floor = CreateEntity("Tool Test Floor", ft);
        MeshRenderer fm;
        fm.mesh = builtin::Mesh(BuiltinShape::Plane);
        world.Add(s_floor, fm);
        s_box = CreateEntity("Tool Test Box", Transform(Vec3(0, 3, 50)));
        MeshRenderer bm;
        bm.mesh = builtin::Mesh(BuiltinShape::Cube);
        world.Add(s_box, bm);
        scene::UpdateWorldTransforms(world);
    }
    if (!renderAssets->GetMesh(builtin::Mesh(BuiltinShape::Plane)) || !renderAssets->GetMesh(builtin::Mesh(BuiltinShape::Cube))) {
        retry = true; // meshes still uploading
        return true;
    }
    scene::UpdateWorldTransforms(world);
    RaycastHit hit;
    const bool picked = RaycastMeshes(world, *renderAssets, Ray(Vec3(0.2f, 10, 50.1f), Vec3(0, -1, 0)), hit) && hit.entity == s_box &&
                        Abs(hit.point.y - 3.5f) < 1e-3f && hit.normal.y > 0.99f;
    Select(s_box);
    const usize undoBefore = undo.Count();
    const bool dropped = DropSelectedToGround(*this) && Abs(world.Get<Transform>(s_box)->position.y - 0.5f) < 1e-3f;
    // Brush: the box as the model, paint around (0, 0, 55), undo, redo, erase.
    tools.items.Clear();
    tools.brush = BrushSettings{};
    tools.brush.density = 6.0f;
    tools.brush.spacing = 0.8f;
    AddSelectedBrushItem(*this);
    const u32 painted = BrushStrokeAt(*this, Vec3(0, 0, 55), Vec3::Up(), false);
    bool onFloor = painted > 0;
    world.Query<const PaintedInstance, const Transform>().Each([&](const PaintedInstance&, const Transform& t) {
        onFloor &= Abs(t.position.y) < 0.05f && Abs(t.position.z - 55.0f) <= tools.brush.radius + 0.01f;
    });
    Undo();
    u32 afterUndo = 0;
    world.Query<const PaintedInstance>().Each([&](const PaintedInstance&) { ++afterUndo; });
    Redo();
    u32 afterRedo = 0;
    world.Query<const PaintedInstance>().Each([&](const PaintedInstance&) { ++afterRedo; });
    const u32 afterErase = BrushStrokeAt(*this, Vec3(0, 0, 55), Vec3::Up(), true);
    // Features: off -> viewport mask off, back on; stored in the descriptor.
    SetFeatureEnabled("water", false);
    const bool featureOff = !FeatureEnabled("water") && project.descriptor.disabledFeatures.Size() == 1;
    SetFeatureEnabled("water", true);
    const bool featureOn = FeatureEnabled("water") && project.descriptor.disabledFeatures.Empty();
    const bool history = undo.Count() > undoBefore;
    const bool ok = picked && dropped && onFloor && afterUndo == 0 && afterRedo == painted && afterErase == 0 && featureOff && featureOn && history;
    if (ok)
        ZX_LOG_INFO(LogEditor, "Tool self-test passed ({} instances painted)", painted);
    else
        ZX_LOG_ERROR(LogEditor,
                     "Tool self-test FAILED (picked {}, dropped {}, painted {} on floor {}, undo {}, redo {}, erase {}, features {}/{})",
                     picked, dropped, painted, onFloor, afterUndo, afterRedo, afterErase, featureOff, featureOn);
    tools.items.Clear();
    return ok;
}

bool Editor::RunAudioSelfTest()
{
    // 64 looping 3D sources around the camera play through the real device for ~2 s: every source becomes a voice,
    // at most 64 are mixed, the device never runs dry; Stop silences everything.
    Array<ecs::Entity>& s_sources = m_audioTestSources; // a member: freed even when the editor exits mid-test
    const String file = contentRoot + "/AudioSelfTest.wav";
    if (m_audioTestStage == 1) {
        audio::PcmData tone;
        tone.sampleRate = 44100;
        tone.channels = 1;
        tone.samples.Resize(44100);
        for (u32 i = 0; i < 44100; ++i)
            tone.samples[i] = 0.2f * Sin(TwoPi * 220.0f * f32(i) / 44100.0f) * (0.6f + 0.4f * Sin(TwoPi * 2.0f * f32(i) / 44100.0f));
        fs::WriteFile(file, audio::EncodeWav(tone).View());
        audio->ClearCache();
        s_sources.Clear();
        for (u32 i = 0; i < 64; ++i) {
            const f32 a = f32(i) * TwoPi / 64.0f;
            const f32 r = 3.0f + f32(i % 8) * 2.0f;
            const ecs::Entity e = CreateEntity(String::Format("Audio Test {}", i), Transform(camera.position + Vec3(Cos(a) * r, 0.0f, Sin(a) * r)));
            AudioSource src;
            src.clip = "AudioSelfTest.wav";
            src.volume = 0.15f;
            src.maxDistance = 30.0f;
            src.occlusion = (i % 4) == 0;
            world.Add(e, src);
            s_sources.PushBack(e);
        }
        BeginPlay();
        m_audioTestUnderruns = audioEngine->GetStats().underruns;
        m_audioTestFrame = frameIndex;
        m_audioTestStage = 2;
        return false;
    }
    if (m_audioTestStage == 2 && frameIndex >= m_audioTestFrame + 120) {
        const audio::AudioStats st = audioEngine->GetStats();
        const bool voices = st.voices == 64 && audio->PlayingSources() == 64 && st.audible > 0 && st.audible <= 64;
        const u64 underruns = st.underruns - m_audioTestUnderruns;
        // Underruns only count with a real device (none on a machine without audio output).
        const bool smooth = !st.deviceOpen || underruns == 0;
        ZX_LOG_INFO(LogEditor, "Audio self-test: {} voices ({} audible), {} underruns, mix {:.3f} ms/block, device '{}'", st.voices, st.audible,
                    underruns, st.mixCpuMs, st.deviceOpen ? st.deviceName : String("none"));
        EndPlay();
        m_audioTestFrame = frameIndex;
        m_audioTestStage = voices && smooth ? 3 : 4;
        if (m_audioTestStage == 4) {
            ZX_LOG_ERROR(LogEditor, "Audio self-test FAILED (voices ok {}, smooth {})", voices, smooth);
            selfTestFailed = true;
            m_audioTestStage = 0;
            s_sources.Reset();
        }
        return false;
    }
    if (m_audioTestStage == 3 && frameIndex >= m_audioTestFrame + 30) {
        const bool silent = audioEngine->GetStats().voices == 0;
        if (silent)
            ZX_LOG_INFO(LogEditor, "Audio self-test passed");
        else
            ZX_LOG_ERROR(LogEditor, "Audio self-test FAILED: {} voices still playing after Stop", audioEngine->GetStats().voices);
        selfTestFailed |= !silent;
        // Stop restored the scene (with new entity ids): remove the test sources by name.
        Array<ecs::Entity> leftovers;
        world.Query<const scene::EntityName>().Each([&](ecs::Entity e, const scene::EntityName& n) {
            if (n.value.StartsWith("Audio Test "))
                leftovers.PushBack(e);
        });
        for (ecs::Entity e : leftovers)
            if (world.IsAlive(e))
                scene::DestroyHierarchy(world, e);
        s_sources.Reset(); // frees the memory (a static would otherwise show up as a leak at exit)
        platform::DeleteFileAt(file);
        m_audioTestStage = 0;
        return true;
    }
    return false;
}

bool Editor::RunScriptSelfTest()
{
    // A script on an entity: props from the component, start/update while playing, hot reload keeps the state,
    // Stop restores the scene. The file lives in Content until the editor exits (the Details panel shows it).
    m_scriptTestFile = contentRoot + "/ScriptSelfTest.zs";
    const StringView v1 = "script SelfTest {\n"
                          "    @range(0, 360) @tooltip(\"Degrees per second\") prop speed = 30.0\n"
                          "    @color prop tint = vec3(1, 0.5, 0.2)\n"
                          "    prop label = \"spinner\"\n"
                          "    var ticks = 0\n"
                          "    fn start() { log(\"SelfTest started on \" + entity.name + \" (\" + label + \")\") }\n"
                          "    fn update(dt) {\n"
                          "        ticks += 1\n"
                          "        entity.rotate(vec3(0, speed * dt, 0))\n"
                          "        entity.position.y = ticks\n"
                          "    }\n"
                          "}\n";
    if (!fs::WriteTextFile(m_scriptTestFile, v1))
        return false;
    const u32 errorsBefore = log ? log->Counts(LogLevel::Error) : 0;
    const ecs::Entity e = CreateEntity("Script Test", Transform::Identity());
    undo.Begin("Add Script");
    undo.Record(e);
    world.Add(e, Script{String("ScriptSelfTest.zs"), String(R"({"speed":90})")});
    undo.End();
    Select(e);
    OpenScriptEditor(*this, m_scriptTestFile);

    BeginPlay();
    for (u32 i = 0; i < 10; ++i)
        scripts->Tick(world, 1.0f / 30.0f, nullptr);
    const ecs::Entity playing = FindByName("Script Test");
    const Transform* t = playing ? world.Get<const Transform>(playing) : nullptr;
    const bool ran = t && Abs(t->position.y - 10.0f) < 1e-4f && Abs(t->rotation.ToEulerDegrees().y - 30.0f) < 0.05f;

    // Hot reload: the new code continues from the running `ticks`.
    Thread::SleepMs(20);
    String v2(v1);
    v2.ReplaceAll("entity.position.y = ticks", "entity.position.y = ticks + 100");
    fs::WriteTextFile(m_scriptTestFile, v2);
    scripts->CheckForChanges();
    scripts->Tick(world, 1.0f / 30.0f, nullptr);
    const bool reloaded = playing && Abs(world.Get<const Transform>(playing)->position.y - 111.0f) < 1e-4f;
    EndPlay();

    const ecs::Entity restored = FindByName("Script Test");
    const bool reset = restored && world.Get<const Transform>(restored)->position.y == 0.0f && world.Has<Script>(restored);
    const bool clean = (log ? log->Counts(LogLevel::Error) : 0) == errorsBefore;
    const bool ok = ran && reloaded && reset && clean;
    if (ok)
        ZX_LOG_INFO(LogEditor, "Script self-test passed");
    else
        ZX_LOG_ERROR(LogEditor, "Script self-test FAILED (ran {}, reloaded {}, restored {}, no errors {})", ran, reloaded, reset, clean);
    fs::WriteTextFile(m_scriptTestFile, v1);
    return ok;
}

// --- Scene files -------------------------------------------------------------------------------------------------------

namespace {
/// Degrees rounded to 1e-4 so saving a loaded scene again writes identical text.
f32 SettingDegrees(f32 radians) { return Round(RadToDeg(radians) * 1e4f) / 1e4f; }
} // namespace

JsonValue Editor::SceneSettings() const
{
    JsonValue env = JsonValue::MakeObject();
    env.Set("night", environment.night);
    env.Set("sunElevation", SettingDegrees(environment.sunElevation));
    env.Set("sunAzimuth", SettingDegrees(environment.sunAzimuth));
    env.Set("autoExposure", environment.autoExposure);
    JsonValue cam = JsonValue::MakeObject();
    JsonValue pos = JsonValue::MakeArray();
    pos.PushBack(camera.position.x);
    pos.PushBack(camera.position.y);
    pos.PushBack(camera.position.z);
    cam.Set("position", move(pos));
    cam.Set("yaw", SettingDegrees(camera.yaw));
    cam.Set("pitch", SettingDegrees(camera.pitch));
    cam.Set("speed", camera.moveSpeed);
    JsonValue s = JsonValue::MakeObject();
    s.Set("environment", move(env));
    s.Set("editorCamera", move(cam));
    return s;
}

void Editor::ApplySceneSettings(const JsonValue& settings)
{
    const JsonValue& env = settings.Get("environment");
    if (env.IsObject()) {
        environment.night = env.Get("night").AsBool(environment.night);
        environment.sunElevation = DegToRad(env.Get("sunElevation").AsFloat(RadToDeg(environment.sunElevation)));
        environment.sunAzimuth = DegToRad(env.Get("sunAzimuth").AsFloat(RadToDeg(environment.sunAzimuth)));
        environment.autoExposure = env.Get("autoExposure").AsBool(environment.autoExposure);
    }
    const JsonValue& cam = settings.Get("editorCamera");
    if (cam.IsObject()) {
        const JsonValue& p = cam.Get("position");
        if (p.Size() == 3)
            camera.position = Vec3(p[0].AsFloat(), p[1].AsFloat(), p[2].AsFloat());
        camera.yaw = DegToRad(cam.Get("yaw").AsFloat(RadToDeg(camera.yaw)));
        camera.pitch = DegToRad(cam.Get("pitch").AsFloat(RadToDeg(camera.pitch)));
        camera.moveSpeed = cam.Get("speed").AsFloat(camera.moveSpeed);
    }
    viewport->ResetHistory();
}

String Editor::BrowseForScene(bool save)
{
    static const platform::FileDialogFilter Filters[] = {{"ZeqouX3D scene", "*.zscene"}, {"All files", "*.*"}};
    const Span<const platform::FileDialogFilter> filters(Filters, 2);
    platform::CreateDirectoryTree(ScenesDirectory());
    const String initial = scenePath.Empty() ? ScenesDirectory() : String(Path::Parent(scenePath));
    if (save) {
        String path = platform::SaveFileDialog(window.Get(), "Save Scene As", filters, sceneName + ".zscene", initial);
        if (!path.Empty() && !Path::HasExtension(path, ".zscene"))
            path += ".zscene";
        return path;
    }
    const Array<String> paths = platform::OpenFileDialog(window.Get(), "Open Scene", filters, false, initial);
    return paths.Empty() ? String() : paths[0];
}

void Editor::RequestSceneAction(SceneAction action, StringView path)
{
    EndPlay(); // scene actions always act on the edit-time scene
    if (action == SceneAction::Open) {
        pendingOpenPath = path.Empty() ? BrowseForScene(false) : String(path);
        if (pendingOpenPath.Empty())
            return;
    }
    if (undo.IsDirty())
        pendingAction = action; // the shell shows the save-changes dialog
    else
        RunSceneAction(action);
}

void Editor::RequestRestart()
{
    restartCommandLine = String::Format("\"{}\"", platform::GetExecutablePath());
    if (!project.descriptorPath.Empty())
        restartCommandLine += String::Format(" \"-project={}\"", project.descriptorPath);
    RequestSceneAction(SceneAction::Quit);
}

void Editor::RunSceneAction(SceneAction action)
{
    pendingAction = SceneAction::None;
    switch (action) {
    case SceneAction::New: NewScene(); break;
    case SceneAction::Open: OpenScene(pendingOpenPath); break;
    case SceneAction::CloseProject: ShowProjectBrowser(); break;
    case SceneAction::Quit: quit = true; break;
    case SceneAction::None: break;
    }
}

void Editor::NewScene()
{
    if (m_continuousEdit) {
        undo.Discard();
        m_continuousEdit = false;
    }
    ClearScene(world);
    pendingImports.Clear();
    undo.Clear();
    selected = ecs::NullEntity;
    sceneName = "Untitled";
    scenePath.Clear();
    environment = ViewportEnvironment();
    camera.position = Vec3(0.0f, 2.0f, -6.0f);
    camera.LookAt(Vec3(0.0f, 0.5f, 0.0f));
    viewport->ResetHistory();
    ZX_LOG_INFO(LogEditor, "New scene");
}

bool Editor::OpenScene(StringView physicalPath)
{
    NewScene();
    JsonValue settings;
    if (!LoadSceneFile(physicalPath, world, SceneIO(), &settings))
        return false;
    ApplySceneSettings(settings);
    scenePath = physicalPath;
    sceneName = Path::Stem(physicalPath);
    undo.Clear();
    UpdateStartupScene(physicalPath);
    waitForAssets = true; // the progress window waits for its assets
    ZX_LOG_INFO(LogEditor, "Opened scene '{}' ({} entities)", physicalPath, world.EntityCount());
    return true;
}

bool Editor::SaveScene(bool saveAs)
{
    EndPlay(); // never save the play state
    String path = scenePath;
    if (saveAs || path.Empty())
        path = BrowseForScene(true);
    return !path.Empty() && SaveSceneTo(path);
}

bool Editor::SaveSceneTo(StringView physicalPath)
{
    platform::CreateDirectoryTree(Path::Parent(physicalPath));
    if (!SaveSceneFile(physicalPath, world, SceneIO(), SceneSettings()))
        return false;
    scenePath = physicalPath;
    sceneName = Path::Stem(physicalPath);
    undo.MarkSaved();
    UpdateStartupScene(physicalPath);
    ZX_LOG_INFO(LogEditor, "Saved scene '{}'", physicalPath);
    return true;
}

void Editor::Select(ecs::Entity e) { selected = world.IsAlive(e) ? e : ecs::NullEntity; }

String Editor::EntityLabel(ecs::Entity e) const
{
    if (const scene::EntityName* n = world.Get<scene::EntityName>(e); n && !n->value.Empty())
        return n->value;
    return String::Format("Entity {}", e.index);
}

void Editor::DeleteSelected()
{
    if (!world.IsAlive(selected))
        return;
    ZX_LOG_INFO(LogEditor, "Deleted '{}'", EntityLabel(selected));
    undo.Begin("Delete");
    undo.RecordHierarchy(selected);
    scene::DestroyHierarchy(world, selected);
    undo.End();
    selected = ecs::NullEntity;
}

void Editor::DuplicateSelected()
{
    if (!world.IsAlive(selected))
        return;
    // Snapshot the hierarchy, give every entity a new guid (parents remapped) and re-create it next to the original.
    Array<ecs::Entity> all;
    CollectHierarchy(world, selected, all);
    HashMap<Guid, Guid> remap;
    for (ecs::Entity e : all)
        remap.InsertOrAssign(EnsureEntityGuid(world, e), Guid::New());
    Array<EntitySnapshot> snapshots;
    for (ecs::Entity e : all) {
        JsonValue s = SerializeEntity(world, e);
        const Guid guid = *remap.Find(GetEntityGuid(world, e));
        s.Set("guid", String::Format("{}", guid));
        if (const JsonValue* p = s.Find("parent"))
            if (const Optional<Guid> pg = Guid::Parse(p->AsString()); pg && remap.Find(*pg))
                s.Set("parent", String::Format("{}", *remap.Find(*pg)));
        if (e == selected) {
            s.Set("index", s.Get("index").AsInt() + 1);
            if (JsonValue* name = s["components"].Find("EntityName"))
                name->Set("value", String::Format("{} (Copy)", name->Get("value").AsString()));
        }
        snapshots.PushBack(EntitySnapshot{guid, move(s)});
    }
    undo.Begin("Duplicate");
    const Array<ecs::Entity> created = ApplyEntitySnapshots(world, snapshots.View());
    for (ecs::Entity e : created)
        undo.RecordCreated(e);
    undo.End();
    if (!created.Empty())
        selected = created[0];
}

void Editor::FocusSelected()
{
    if (!world.IsAlive(selected))
        return;
    const Vec3 target = scene::ComputeWorldMatrix(world, selected).GetTranslation();
    camera.position = target - camera.Forward() * 6.0f;
}

void Editor::ImportAsset(StringView physicalPathIn, Vec3 position, bool undoable)
{
    if (Path::HasExtension(physicalPathIn, ".zprefab")) {
        InstantiatePrefab(physicalPathIn, position);
        return;
    }
    const String physicalPath = CopyIntoProject(physicalPathIn);
    if (physicalPath.Empty())
        return;
    const asset::AssetId id = assets->LoadPath(physicalPath);
    if (!id.IsValid()) {
        ZX_LOG_ERROR(LogEditor, "Cannot import '{}'", physicalPath);
        return;
    }
    pendingImports.PushBack(PendingImport{id, Transform(position), String(Path::Stem(physicalPath)), undoable});
    ZX_LOG_INFO(LogEditor, "Importing '{}'...", Path::FileName(physicalPath));
}

void Editor::UpdateImports()
{
    for (usize i = 0; i < pendingImports.Size();) {
        PendingImport& p = pendingImports[i];
        if (const asset::PrefabAsset* prefab = assets->GetPrefab(p.id)) {
            const ecs::Entity root = zx::InstantiatePrefab(world, *prefab, p.transform, {}, assets.Get());
            if (scene::EntityName* n = world.Get<scene::EntityName>(root))
                n->value = p.name;
            if (p.undoable)
                CommitCreated(root, String::Format("Import {}", p.name));
            selected = root;
            ZX_LOG_INFO(LogEditor, "Placed '{}' ({} nodes)", p.name, prefab->nodes.Size());
            pendingImports.RemoveAt(i);
        } else if (assets->GetState(p.id) == asset::AssetState::Failed) {
            ZX_LOG_ERROR(LogEditor, "Import of '{}' failed: {}", p.name, assets->GetErrorMessage(p.id));
            pendingImports.RemoveAt(i);
        } else {
            ++i;
        }
    }
}

void Editor::ProcessShortcuts()
{
    if (ui->WantsKeyboard() || flying || mode != EditorMode::Editing || materialEditor.capturesKeys || scriptEditor.capturesKeys || palette.open)
        return;
    const KeyModifiers mods = input.Modifiers() & (KeyModifiers::Control | KeyModifiers::Shift | KeyModifiers::Alt);
    for (const WindowEvent& e : m_events) {
        if (e.type != WindowEventType::KeyDown)
            continue;
        // While the game has the input only the play controls (F keys) work; everything else is game input.
        if (gameInputCaptured && !(e.key >= Key::F1 && e.key <= Key::F12) && e.key != Key::Escape)
            continue;
        // Escape depends on the state: stop playing, otherwise clear the selection.
        if (e.key == Key::Escape && mods == KeyModifiers::None) {
            if (!e.repeat) {
                if (tools.active != EditorTool::None && !IsPlaying())
                    tools.active = EditorTool::None;
                else if (IsPlaying())
                    EndPlay();
                else
                    selected = ecs::NullEntity;
            }
            continue;
        }
        DispatchShortcut(*this, e.key, mods, e.repeat);
    }
}

void Editor::EnsureViewportTexture(UVec2 size)
{
    size = UVec2(Max(size.x, 16u), Max(size.y, 16u));
    if (viewportTexture && viewportSize == size)
        return;
    device->Destroy(viewportTexture);
    TextureDesc td;
    td.width = size.x;
    td.height = size.y;
    td.format = Format::R8G8B8A8_UNORM;
    td.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    td.debugName = "EditorViewport";
    viewportTexture = device->CreateTexture(td);
    viewportSize = size;
    viewport->ResetHistory();
}

bool Editor::Frame()
{
    if (!platform::PumpMessages() || device->IsDeviceLost() || quit)
        return false;
    if (window->ShouldClose() && (m_autotestFrames || !undo.IsDirty()))
        return false;
    profiler::BeginFrame();
    dt = timer.Tick();
    m_events.Clear();
    platform::UpdateInput(*window, input, m_events);
    // Automated runs ignore the person at the machine (their mouse over the test window would fly the camera);
    // -autotestinput keeps the real input (tests that post input messages).
    static const bool s_autotestInput = CommandLine::HasFlag("autotestinput");
    if (m_autotestFrames > 0 && !s_autotestInput) {
        input.ReleaseAll();
        m_events.RemoveIf([](const WindowEvent& e) {
            return e.type == WindowEventType::MouseDown || e.type == WindowEventType::MouseUp || e.type == WindowEventType::MouseWheel ||
                   e.type == WindowEventType::KeyDown || e.type == WindowEventType::KeyUp || e.type == WindowEventType::Char;
        });
    }
    // Console input.press: a key held for a few frames (works in automated runs too).
    if (simulatedKey != Key::Unknown) {
        input.SetKey(simulatedKey, simulatedFrames > 0);
        if (simulatedFrames > 0)
            --simulatedFrames;
        else
            simulatedKey = Key::Unknown;
    }
    const UVec2 client = window->GetClientSize();
    if (client.x == 0 || client.y == 0 || window->IsMinimized()) {
        Thread::SleepMs(16);
        return true;
    }
    if (client != swapChain->GetSize())
        swapChain->Resize(client.x, client.y);

    // Fly mode ends with the right mouse button (also when the viewport is hidden: it blocks all UI input).
    if (flying && !input.IsMouseDown(MouseButton::Right))
        flying = false;

    // --- UI ---
    ui::FrameInput fi;
    fi.displaySize = Vec2(f32(client.x), f32(client.y));
    fi.deltaTime = dt;
    // -uiscale=<x> multiplies the system DPI scale (screenshots of tall panels).
    static const f32 s_uiScale = [] {
        StringView v;
        return CommandLine::GetValue("uiscale", v) ? Clamp(f32(ParseDouble(v).ValueOr(1.0)), 0.5f, 3.0f) : 1.0f;
    }();
    fi.dpiScale = window->GetDpiScale() * s_uiScale * Clamp(prefs.uiScale, 0.5f, 3.0f);
    fi.input = flying ? nullptr : &input;
    fi.events = m_events.View();
    fi.hasFocus = window->IsFocused() || m_autotestFrames > 0;
    Stopwatch sw;
    {
        ZX_PROFILE_SCOPE("Editor.UI");
        ui->BeginFrame(fi);
        DrawShell(*this);
        ui->EndFrame();
    }
    m_timeUi += sw.ElapsedMilliseconds();
    // A drag/edit step ends once nothing is held any more.
    if (m_continuousEdit && !input.IsMouseDown(MouseButton::Left) && !ui->IsAnyItemActive()) {
        undo.End();
        m_continuousEdit = false;
    }
    if (window->ShouldClose() && !quit) {
        window->CancelClose();
        RequestSceneAction(SceneAction::Quit);
    }
    if (!flying)
        window->SetCursorShape(ui->GetCursorShape());
    window->SetCursorLocked(flying || (gameInputCaptured && !gameUI.cursorRequested)); // menus free the mouse
    ProcessShortcuts();

    // --- Scene ---
    UpdateImports();
    UpdateGameModule(*this);
    UpdatePackage();
    if (playState == PlayState::Playing || m_pieStep) {
        const f32 gameDt = m_pieStep ? 1.0f / 60.0f : dt;
        GameSystems systems = GameSystemsForPlay();
        // While a UI Input Field has the keyboard the game gets no input (typing doesn't move the player).
        TickGame(world, systems, gameDt, gameInputCaptured && !gameUI.WantsKeyboard() ? &input : nullptr, CurrentListener(), !m_pieStep);
        m_pieStep = false;
    } else if (audio && !IsPlaying()) {
        audio->TickIdle(dt); // previews
    }
    // Terrains: (re)built when one appears or its shape changes. Soft bodies: their mesh, wheels at rest.
    {
        ZX_PROFILE_SCOPE("Terrains");
        terrains.Update(world, *assets, renderAssets.Get());
        softBodies.Update(world, *assets, renderAssets.Get());
        UpdateWaterPresets(world);
        UpdateCloudPresets(world);
        TickWind(world, playState == PlayState::Paused ? 0.0f : dt * (IsPlaying() ? GameTimeScale(GameSystemsForPlay()) : 1.0f));
    }
    // Particles: emitters with Simulate In Editor run live while editing, all of them while playing (frozen when paused).
    if (FeatureEnabled("particles")) {
        scene::UpdateWorldTransforms(world);
        particles.Tick(world, playState == PlayState::Paused ? 0.0f : dt * (IsPlaying() ? GameTimeScale(GameSystemsForPlay()) : 1.0f), IsPlaying());
    }
    // Animators pose their skinned meshes (frozen when paused).
    scene::UpdateWorldTransforms(world);
    animation->Tick(world, playState == PlayState::Paused ? 0.0f : dt * (IsPlaying() ? GameTimeScale(GameSystemsForPlay()) : 1.0f), IsPlaying());
    if (!IsPlaying() && scripts && (m_scriptCheckTimer += dt) > 0.5) {
        m_scriptCheckTimer = 0.0; // edit mode: keep the Details panel in sync with files edited elsewhere
        scripts->CheckForChanges();
    }
    if (!m_saveAfterImports.Empty() && pendingImports.Empty()) {
        SaveSceneTo(m_saveAfterImports);
        m_saveAfterImports.Clear();
    }
    if (m_audioTestStage && frameIndex > 5 && pendingImports.Empty() && assets->IsIdle())
        RunAudioSelfTest();
    if (m_toolTest && frameIndex > 5 && pendingImports.Empty() && assets->IsIdle()) {
        bool retry = false;
        const bool ok = RunToolSelfTest(retry);
        if (!retry || --m_toolTest == 0) {
            selfTestFailed |= !ok || retry;
            m_toolTest = 0;
        }
    }
    if (m_cppTestStage && frameIndex > 5 && pendingImports.Empty() && assets->IsIdle())
        RunCppSelfTest();
    if (m_postTestStage && frameIndex > 5 && pendingImports.Empty() && assets->IsIdle())
        RunPostSelfTest();
    if (m_reflTestStage && frameIndex > 5 && pendingImports.Empty() && assets->IsIdle())
        RunReflectionSelfTest();
    if (m_decalTestStage && frameIndex > 5 && pendingImports.Empty() && assets->IsIdle())
        RunDecalSelfTest();
    if (m_hdriTestStage && frameIndex > 5 && pendingImports.Empty())
        RunHdriSelfTest();
    if (m_physicsTestStage && frameIndex > 5 && pendingImports.Empty() && assets->IsIdle())
        RunPhysicsSelfTest();
    if (m_scriptTest && frameIndex > 5 && pendingImports.Empty() && assets->IsIdle()) {
        m_scriptTest = false;
        selfTestFailed |= !RunScriptSelfTest();
    }
    if (m_materialTest && frameIndex > 5 && pendingImports.Empty() && assets->IsIdle()) {
        m_materialTest = false;
        selfTestFailed |= !RunMaterialSelfTest();
    }
    if (m_pieTest && frameIndex > 5 && pendingImports.Empty() && assets->IsIdle()) {
        m_pieTest = false;
        selfTestFailed |= !RunPieSelfTest();
    }
    if (m_undoTest && frameIndex > 5 && pendingImports.Empty() && assets->IsIdle()) {
        m_undoTest = false;
        selfTestFailed |= !RunUndoSelfTest();
    }
    if (!m_execCommands.Empty() && frameIndex >= m_execAtFrame && pendingImports.Empty()) {
        // Up to the first "wait N": the rest runs N frames later (scripted captures of Play, timings after warm-up).
        String now = m_execCommands, later;
        for (usize start = 0; start < m_execCommands.Size();) {
            usize end = m_execCommands.View().Find(';', start);
            if (end == StringView::NPos)
                end = m_execCommands.Size();
            const StringView statement = m_execCommands.View().SubStr(start, end - start).Trim();
            if (statement.StartsWith("wait ")) {
                now = String(m_execCommands.View().SubStr(0, start));
                later = end < m_execCommands.Size() ? String(m_execCommands.View().SubStr(end + 1)) : String();
                m_execAtFrame = frameIndex + u64(Max<i64>(1, ParseInt(statement.SubStr(5).Trim()).ValueOr(1)));
                break;
            }
            start = end + 1;
        }
        if (!now.View().Trim().Empty()) {
            ZX_LOG_INFO(LogEditor, "> {}", now);
            Console::Execute(now);
        }
        m_execCommands = later;
        ui::FocusDockedWindow(LogTitle);
    }
    sceneDirty = undo.IsDirty();
    if (viewport->LastHdri().key != m_lastHdriKey) {
        m_lastHdriKey = viewport->LastHdri().key;
        if (viewport->LastHdri().IsActive())
            Console::Execute("hdri.info");
    }
    scene::UpdateWorldTransforms(world);
    sw.Restart();
    RenderFrame();
    m_timeRender += sw.ElapsedMilliseconds();
    ++m_timedFrames;

    ++frameIndex;
    if (m_autotestFrames && frameIndex >= m_autotestFrames && assets->IsIdle() && pendingImports.Empty() && !IsPackaging() && !m_cppTestStage && !m_postTestStage && !m_reflTestStage && !m_decalTestStage && !m_hdriTestStage && !m_physicsTestStage &&
        (m_screenshotPath.Empty() || m_screenshotBuffer)) {
        if (!m_saveScenePath.Empty() && SaveSceneFile(m_saveScenePath, world, SceneIO(), SceneSettings()))
            ZX_LOG_INFO(LogEditor, "Saved scene '{}'", m_saveScenePath);
        return false;
    }
    if (m_autotestFrames && frameIndex > m_autotestFrames + 3000 && !m_cppTestStage) // -cpptest has its own timeout
        return false;
    return true;
}

void Editor::RenderFrame()
{
    ZX_PROFILE_SCOPE("Editor.RenderFrame");
    const UVec2 size = swapChain->GetSize();
    device->BeginFrame();
    CommandList* cmd = device->BeginCommandList();
    cmd->BeginTimer("Frame");
    renderAssets->Update(*cmd);
    RenderGraph graph(*device, *pool);
    Array<RGTexture> sampled;
    if (viewportTexture && showViewport && !viewportRect.IsEmpty() && mode == EditorMode::Editing) {
        const RGTexture vp = graph.ImportTexture(viewportTexture, ResourceState::ShaderResource);
        sampled.PushBack(vp);
        ViewportFeatures& vf = viewport->features;
        viewport->graphicsEra = ParseGraphicsEra(project.descriptor.graphicsEra.View());
        vf.shadows = FeatureEnabled("shadows");
        vf.ambientOcclusion = FeatureEnabled("ambientOcclusion");
        vf.sky = FeatureEnabled("sky");
        vf.bloom = FeatureEnabled("bloom");
        vf.temporalAA = FeatureEnabled("temporalAA");
        vf.water = FeatureEnabled("water");
        vf.postProcessVolumes = FeatureEnabled("postProcessVolumes") && showPostVolumes;
        vf.depthOfField = FeatureEnabled("depthOfField") && showDepthOfField;
        vf.motionBlur = FeatureEnabled("motionBlur") && showMotionBlur && IsPlaying(); // not while editing
        vf.reflections = FeatureEnabled("reflections");
        vf.screenSpaceReflections = FeatureEnabled("screenSpaceReflections");
        vf.decals = FeatureEnabled("decals");
        vf.particles = FeatureEnabled("particles");
        vf.volumetricFog = FeatureEnabled("volumetricFog");
        vf.clouds = FeatureEnabled("clouds");
        vf.fluids = FeatureEnabled("fluids");
        ApplyEditorSettings(*this); // File > Settings: quality of this viewport, audio, autosave
        ViewportCamera cam{camera.ViewMatrix(), camera.position, camera.fovY, camera.nearPlane};
        if (UsingGameCamera()) {
            // Game view: the Camera entity's world position and orientation (+Z forward), scale ignored.
            const ecs::Entity ge = FindGameCamera(world);
            const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(ge);
            const Camera* gc = world.Get<const Camera>(ge);
            if (wt && gc) {
                const Vec3 pos = wt->GetPosition();
                const Vec3 fwd = SafeNormalize(wt->matrix.TransformVector(Vec3::Forward()), Vec3::Forward());
                const Vec3 up = SafeNormalize(wt->matrix.TransformVector(Vec3::Up()), Vec3::Up());
                cam = ViewportCamera{Mat4::LookAt(pos, pos + fwd, up), pos, DegToRad(gc->fieldOfView), Max(gc->nearPlane, 0.001f)};
            }
        }
        cam.cut = UsingGameCamera() != lastViewWasGame;
        lastViewWasGame = UsingGameCamera();
        Array<ecs::Entity> outlined;
        if (!m_screenshotViewport)
            if (tools.active != EditorTool::Terrain) // the terrain being sculpted isn't outlined
                CollectHierarchy(world, selected, outlined);
        viewport->SetOutlinedEntities(outlined.View());
        viewport->Outline().thickness = 2.0f * window->GetDpiScale();
        environment.playing = IsPlaying() && playState != PlayState::Paused; // every Fluid Volume simulates
        viewport->AddPasses(graph, world, cam, environment, vp, viewportSize, dt,
                            [this](CommandList& c, const RenderView& view, f32) {
                                bool hasTerrain = false; // the grid would cut through the ground
                                world.Query<const Terrain>().Each([&](ecs::Entity, const Terrain&) { hasTerrain = true; });
                                if (showGrid && !m_screenshotViewport && !hasTerrain) {
                                    debugDraw->Grid(Vec3::Zero(), 40.0f, 40, Color(58, 58, 58));
                                    debugDraw->Line(Vec3(-20, 0.002f, 0), Vec3(20, 0.002f, 0), Color(150, 50, 50));
                                    debugDraw->Line(Vec3(0, 0.002f, -20), Vec3(0, 0.002f, 20), Color(50, 80, 160));
                                }
                                DrawToolOverlay(*this, *debugDraw);
                                if (showPhysicsDebug && physics.IsRunning() && !m_screenshotViewport)
                                    DrawPhysicsDebug();
                                if (world.IsAlive(selected) && !m_screenshotViewport) {
                                    const Mat4 m = scene::ComputeWorldMatrix(world, selected);
                                    // Colliders of the selection (and its children): green wireframes.
                                    Array<ecs::Entity> colliderOwners;
                                    CollectHierarchy(world, selected, colliderOwners);
                                    Array<ColliderOutline> outlines;
                                    for (ecs::Entity ce : colliderOwners)
                                        GatherColliderOutlines(world, ce, outlines, renderAssets.Get());
                                    const Color green(120, 230, 120);
                                    for (const ColliderOutline& o : outlines) {
                                        const Vec3 oc = o.transform.GetTranslation();
                                        if (o.type == physics::ShapeType::Box) {
                                            debugDraw->Box(AABB(-o.halfExtents, o.halfExtents), o.transform, green);
                                        } else if (o.type == physics::ShapeType::Sphere) {
                                            debugDraw->Sphere(oc, o.radius, green, 24);
                                        } else if (o.type == physics::ShapeType::Capsule) {
                                            const Vec3 up = (o.transform * Vec4(0, 1, 0, 0)).XYZ();
                                            const Vec3 side = (o.transform * Vec4(1, 0, 0, 0)).XYZ(), fwd = (o.transform * Vec4(0, 0, 1, 0)).XYZ();
                                            const Vec3 top = oc + up * o.halfHeight, bottom = oc - up * o.halfHeight;
                                            debugDraw->Sphere(top, o.radius, green, 20);
                                            debugDraw->Sphere(bottom, o.radius, green, 20);
                                            for (Vec3 d : {side, -side, fwd, -fwd})
                                                debugDraw->Line(top + d * o.radius, bottom + d * o.radius, green);
                                        } else if (o.type == physics::ShapeType::ConvexHull) {
                                            for (usize i = 0; i + 1 < o.lines.Size(); i += 2)
                                                debugDraw->Line(o.lines[i], o.lines[i + 1], green);
                                        }
                                    }
                                    // Joints: anchor, axis and a line to what they connect to (orange).
                                    Array<JointGizmo> joints;
                                    GatherJointGizmos(world, selected, joints);
                                    const Color orange(255, 160, 60);
                                    for (const JointGizmo& g : joints) {
                                        debugDraw->Sphere(g.anchor, 0.06f, orange, 12);
                                        if (LengthSq(g.axis) > 0.0f) {
                                            if (g.linear)
                                                debugDraw->Arrow(g.anchor - g.axis * 0.5f, g.anchor + g.axis * 0.5f, orange);
                                            else
                                                debugDraw->Arrow(g.anchor, g.anchor + g.axis * 0.5f, orange);
                                        }
                                        if (g.hasConnected)
                                            debugDraw->Line(g.anchor, g.connected, Color(200, 120, 50));
                                    }
                                    if (const PointLight* pl = world.Get<PointLight>(selected))
                                        debugDraw->Sphere(m.GetTranslation(), pl->range, Color(255, 200, 80), 32);
                                    if (const AudioSource* as = world.Get<AudioSource>(selected); as && as->spatial) {
                                        debugDraw->Sphere(m.GetTranslation(), as->minDistance, Color(90, 200, 255), 24);
                                        debugDraw->Sphere(m.GetTranslation(), as->maxDistance, Color(60, 120, 170), 32);
                                    }
                                    if (const Decal* dc = world.Get<Decal>(selected)) {
                                        const Mat4 box = m * Mat4::Scale(Vec3(Max(Abs(dc->extent.x), 0.001f), Max(Abs(dc->extent.y), 0.001f),
                                                                              Max(Abs(dc->extent.z), 0.001f)));
                                        debugDraw->Box(AABB(Vec3(-1.0f), Vec3(1.0f)), box, Color(255, 120, 200));
                                        const Vec3 top = box.TransformPoint(Vec3(0, 1, 0)), bottom = box.TransformPoint(Vec3(0, -1, 0));
                                        debugDraw->Arrow(top, bottom, Color(255, 120, 200)); // projection direction
                                    }
                                    if (const ReflectionProbe* rp = world.Get<ReflectionProbe>(selected)) {
                                        // Influence box (axis-aligned, scaled) and its fade-in margin.
                                        const Vec3 pc = m.GetTranslation(), sc = m.GetScale();
                                        const Vec3 half(Abs(rp->extent.x * sc.x), Abs(rp->extent.y * sc.y), Abs(rp->extent.z * sc.z));
                                        debugDraw->Box(AABB(pc - half, pc + half), Color(110, 220, 255));
                                        const Vec3 inner = Max(half - Vec3(rp->blendDistance), Vec3(0.0f));
                                        debugDraw->Box(AABB(pc - inner, pc + inner), Color(60, 130, 160));
                                        debugDraw->Sphere(pc, 0.25f, Color(110, 220, 255), 16);
                                    }
                                    if (const PostProcessVolume* pv = world.Get<PostProcessVolume>(selected); pv && !pv->unbound) {
                                        const AABB box(-pv->extent, pv->extent);
                                        debugDraw->Box(box, m, Color(255, 170, 60));
                                        if (pv->blendRadius > 0.0f) {
                                            // Blend region, approximated by the box grown by the radius in world units.
                                            const Vec3 sc = m.GetScale();
                                            const Vec3 grow(pv->blendRadius / Max(sc.x, 1e-3f), pv->blendRadius / Max(sc.y, 1e-3f),
                                                            pv->blendRadius / Max(sc.z, 1e-3f));
                                            debugDraw->Box(AABB(-pv->extent - grow, pv->extent + grow), m, Color(140, 95, 40));
                                        }
                                    }
                                    if (const ReverbZone* rz = world.Get<ReverbZone>(selected)) {
                                        debugDraw->Sphere(m.GetTranslation(), rz->radius, Color(180, 130, 255), 32);
                                        debugDraw->Sphere(m.GetTranslation(), Max(rz->radius - rz->fade, 0.0f), Color(120, 90, 180), 24);
                                    }
                                }
                                debugDraw->Render(c, view.viewProj);
                            });
    }
    const bool capture = !m_screenshotPath.Empty() && !m_screenshotBuffer && m_autotestFrames && frameIndex + 1 >= m_autotestFrames &&
                         (m_screenshotEarly || (assets->IsIdle() && pendingImports.Empty()));
    const RGTexture backBuffer =
        graph.ImportTexture(swapChain->GetCurrentBackBuffer(), capture ? ResourceState::CopySource : ResourceState::Present);
    graph.AddPass(
        "Clear", PassType::Graphics,
        [&](PassBuilder& b) { b.RenderTarget(backBuffer, LoadOp::Clear, LinearColor(0.008f, 0.008f, 0.008f, 1)); },
        [](PassContext&) {});
    if (mode == EditorMode::Editing && thumbnails)
        thumbnails->Update(graph, sampled);
    if (mode == EditorMode::Editing)
        RenderMaterialPreview(*this, graph, sampled);
        RenderAnimationPreview(*this, graph, sampled);
    uiRenderer->Prepare(*cmd, fonts, ui->GetDrawLists());
    uiRenderer->AddPass(graph, backBuffer, size, sampled.View());
    graph.Execute(*cmd);
    if (capture) {
        m_screenshotSize = size;
        m_screenshotCrop = viewportRect;
        m_screenshotBuffer = device->CreateBuffer(BufferDesc{AlignUp(u64(size.x) * 4, u64(256)) * size.y, BufferUsage::CopyDest,
                                                             MemoryType::Readback, 0, "Screenshot"});
        m_screenshotPitch = cmd->CopyTextureToBuffer(m_screenshotBuffer, 0, swapChain->GetCurrentBackBuffer());
        cmd->Barrier(swapChain->GetCurrentBackBuffer(), ResourceState::Present);
    }
    cmd->EndTimer();
    device->Submit(cmd);
    Stopwatch present;
    swapChain->Present();
    device->EndFrame();
    m_timePresent += present.ElapsedMilliseconds();
    for (const GpuTiming& t : device->GetGpuTimings())
        if (StringView(t.name) == "Frame")
            gpuFrameMs = t.milliseconds;
    if (viewport)
        viewport->UpdateDynamicResolution(f32(gpuFrameMs), dt);
}

void Editor::Shutdown()
{
    if (packageJob && packageJob->thread.IsJoinable())
        packageJob->thread.Join();
    if (!device)
        return;
    device->WaitIdle();
    if (m_screenshotBuffer) {
        const u8* pixels = static_cast<const u8*>(m_screenshotBuffer->GetMappedData());
        u32 x0 = 0, y0 = 0, w = m_screenshotSize.x, h = m_screenshotSize.y;
        if (m_screenshotViewport && m_screenshotCrop.Width() > 8.0f && m_screenshotCrop.Height() > 8.0f) {
            x0 = Min(u32(Max(m_screenshotCrop.min.x, 0.0f)), w - 1);
            y0 = Min(u32(Max(m_screenshotCrop.min.y, 0.0f)), h - 1);
            w = Min(u32(m_screenshotCrop.Width()), w - x0);
            h = Min(u32(m_screenshotCrop.Height()), h - y0);
        }
        const bool ok = png::WriteFile(m_screenshotPath, pixels + usize(y0) * m_screenshotPitch + usize(x0) * 4, w, h, 4, usize(m_screenshotPitch));
        ZX_LOG_INFO(LogEditor, "Screenshot {} -> {}", ok ? "saved" : "FAILED", m_screenshotPath);
        device->Destroy(m_screenshotBuffer);
        m_screenshotBuffer = nullptr;
    }
    for (const PendingImport& p : pendingImports)
        ZX_LOG_WARN(LogEditor, "Import of '{}' did not finish (asset state {}, {})", p.name, u32(assets->GetState(p.id)),
                    assets->GetSourcePath(p.id));
    if (ui) {
        fs::WriteTextFile(m_layoutPath, ui::SaveDockLayout());
        ui::SetCurrentContext(nullptr);
    }
    for (const GpuTiming& t : device->GetGpuTimings())
        ZX_LOG_INFO(LogEditor, "  GPU {:<20} {:.3f} ms", t.name, t.milliseconds);
    ZX_LOG_INFO(LogEditor, "Average frame {:.2f} ms; CPU: UI {:.2f} ms, render {:.2f} ms (present {:.2f} ms)",
                timer.SmoothedDeltaSeconds() * 1000.0f, m_timeUi / f64(Max<u64>(m_timedFrames, 1)),
                m_timeRender / f64(Max<u64>(m_timedFrames, 1)), m_timePresent / f64(Max<u64>(m_timedFrames, 1)));
    debugLayerErrors = d3d12::GetDebugLayerErrorCount(*device);
    if (viewport && viewport->Reflections().ProbeCount())
        ZX_LOG_INFO(LogEditor, "Reflection probes: {} ({} captured{})", viewport->Reflections().ProbeCount(),
                    viewport->Reflections().CapturedProbes(), viewport->Reflections().IsCapturing() ? ", still capturing" : "");
    ZX_LOG_INFO(LogEditor, "Exited after {} frames, debug layer errors: {}", frameIndex, debugLayerErrors);
    device->Destroy(viewportTexture);
    viewportTexture = nullptr;
    device->Destroy(appIcon);
    device->Destroy(appLogo);
    for (rhi::Texture*& t : templateArt) {
        device->Destroy(t);
        t = nullptr;
    }
    appIcon = appLogo = nullptr;
    pool->Clear();
    UnregisterEditorConsole();
    materialEditor.preview.Reset();
    animViewer.preview.Reset();
    debugDraw.Reset();
    thumbnails.Reset();
    materials.Reset();
    game.Reset(); // stops running components while the world and audio still exist
    scripts.Reset();
    audio.Reset();
    audioEngine.Reset();
    if (!m_scriptTestFile.Empty())
        platform::DeleteFileAt(m_scriptTestFile);
    for (const String& f : m_reflTestFiles)
        platform::DeleteFileAt(f);
    m_reflTestFiles.Reset();
    if (!m_hdriTestFile.Empty()) {
        platform::DeleteFileAt(m_hdriTestFile);
        platform::DeleteFileAt(m_hdriTestFile + ".zmeta");
    }
    prefabs.Reset();
    viewport.Reset();
    renderAssets.Reset();
    animation.Reset(); // holds the asset manager
    assets.Reset();
    uiRenderer.Reset();
    ui.Reset();
    swapChain.Reset();
    device.Reset();
    window.Reset();
    if (log) {
        Log::RemoveSink(log);
        log = nullptr;
    }
}

} // namespace zx::editor
