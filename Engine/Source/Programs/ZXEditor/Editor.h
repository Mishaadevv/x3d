#pragma once

// ZXEditor: application state shared by the shell (title bar, toolbar, status bar, docking) and the panels.

#include "Asset/AssetManager.h"
#include "Core/Containers/RingBuffer.h"
#include "Core/Log/Log.h"
#include "Core/Threading/Mutex.h"
#include "Core/Time/Time.h"
#include "Engine/AudioSystem.h"
#include "Engine/BuiltinAssets.h"
#include "Engine/FlyCamera.h"
#include "Engine/GameModule.h"
#include "Engine/InputActions.h"
#include "Engine/Fire.h"
#include "Engine/Particles.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Materials.h"
#include "Engine/Prefab.h"
#include "Engine/Project.h"
#include "Engine/SceneUndo.h"
#include "Engine/Scripting.h"
#include "Engine/ViewportRenderer.h"
#include "EditorActions.h"
#include "EditorTools.h"
#include "MaterialEditor.h"
#include "ScriptEditor.h"
#include "Platform/Window.h"
#include "Render/DebugDraw.h"
#include "Render/RenderAssets.h"
#include "UI/Svg.h"
#include "UI/UI.h"
#include "UI/UIRenderer.h"

namespace zx::editor {

using ui::DrawList;
class ThumbnailCache;
using ui::WithAlpha;

/// Keeps log messages for the Output Log panel (thread-safe; any thread may log).
class EditorLogSink final : public LogSink
{
public:
    struct Entry
    {
        LogLevel level;
        String category;
        String text;
        f64 time;
    };
    void Write(const LogMessage& message) override;
    template <typename F>
    void ForEach(F&& f) const
    {
        ScopedLock lock(m_mutex);
        for (const Entry& e : m_entries)
            f(e);
    }
    usize Count() const;
    void Clear();
    u32 Version() const { return m_version; }
    u32 Counts(LogLevel level) const;
    String LastMessage() const;

private:
    mutable Mutex m_mutex;
    Array<Entry> m_entries;
    u32 m_version = 0;
    u32 m_counts[8] = {};
};

enum class TransformTool : u8
{
    Select,
    Move,
    Rotate,
    Scale,
};

enum class PlayState : u8
{
    Editing,
    Playing,
    Paused,
};

struct PendingImport
{
    asset::AssetId id;
    Transform transform;
    String name;
    bool undoable = true; // false for the startup sample scene
};

/// Actions that discard the current scene (confirmed with a "save changes?" dialog when it is dirty).
enum class SceneAction : u8
{
    None,
    New,
    Open,
    CloseProject, // back to the Project Browser
    Quit,
};

enum class EditorMode : u8
{
    ProjectBrowser, // full-window project picker (no project open)
    Editing,
};

enum class ProjectTemplate : u8
{
    Blank,
    Sample,      // Damaged Helmet + lights
    FirstPerson,
    ThirdPerson,
    Water,
    Count
};

struct NewProjectOptions
{
    ProjectTemplate templ = ProjectTemplate::Blank;
    ProjectLanguage language = ProjectLanguage::ZeqouScript;
    bool starterContent = true; // StarterContent/ materials + scripts (and a small showcase in a blank scene)
    // Ready-made prefabs in StarterContent/Prefabs (with Starter Content):
    bool starterEffects = true;    // fire, campfire, torch, smoke, sparks, steam, ground fog, magic, rain, fountain, pool
    bool starterCharacters = true; // first / third person characters, tank
    bool starterProps = true;      // door, elevator, jump pad, coin, crates, swinging lamp, rope bridge
    bool documentation = true;  // README.md, Docs/ for the team, AGENTS.md / CLAUDE.md for AI assistants
    bool gitRepository = false; // git init + .gitattributes + an initial commit (needs git on PATH)
};

/// State of the Project Browser page.
struct ProjectBrowserState
{
    bool newProjectPage = false; // -browserpage=new
    Array<RecentProject> recent;
    bool recentLoaded = false;
    i32 selected = -1;
    String search;
    String newName = "MyProject";
    String newLocation;
    NewProjectOptions options;
    Array<u32> recentColors; // tile color per recent project (read from the .zproj files)
    String error;
};

/// Per-panel UI state (owned by the editor so it is released before the engine shuts down).
struct PanelState
{
    String outlinerFilter;
    bool logInfo = true, logWarn = true, logError = true, logAutoScroll = true;
    String logSearch;
    struct LogLine
    {
        LogLevel level;
        String text;
    };
    Array<LogLine> logLines;
    u32 logVersion = U32Max;
    String logLastSearch;
    u32 logLastMask = 0;
    u32 logScrollFrames = 0; // pending scroll-to-bottom (repeated: the content height settles a frame later)
    // Console prompt (Output Log).
    String consoleInput;
    Array<String> consoleHistory;
    i32 consoleHistoryPos = -1; // -1 = editing a new line
    i32 consoleSuggestion = 0;
    // Content browser.
    String browserCurrent;
    String browserSelected;
    String browserFilter;
    struct FileItem
    {
        String name;
        String path;
        bool directory;
        u64 size;
    };
    Array<FileItem> browserItems;
    String browserListedDir;
    f64 browserListedAt = -100.0;
    f32 browserTileSize = 88.0f;
    f32 browserTreeWidth = 200.0f;
};

/// Command palette (Ctrl+P / Ctrl+Shift+P).
struct PaletteState
{
    bool open = false;
    bool focus = false;
    bool commandsOnly = false;
    String query;
    i32 selection = 0;
    Array<String> assets; // content files, scanned when opened
};

class Editor
{
public:
    Editor();
    ~Editor();
    ZX_NON_COPYABLE(Editor)

    bool Initialize();
    /// One frame; returns false when the editor should exit.
    bool Frame();
    void Shutdown();

    // --- Shared state (panels) ---
    UniquePtr<Window> window;
    UniquePtr<rhi::Device> device;
    UniquePtr<rhi::SwapChain> swapChain;
    bool lite = true;
    String engineRoot;
    String contentRoot; // physical directory shown by the Content Browser

    ui::FontSystem fonts;
    ui::SvgImage logo; // Engine/Content/Icons/ZeqouLogo.svg (fallback when the brand images are missing)
    rhi::Texture* appIcon = nullptr; // Engine/Content/Branding/ZeqouX3D_Icon.png (title bar, Project Browser)
    rhi::Texture* appLogo = nullptr; // Engine/Content/Branding/ZeqouX3D_Logo.png (About)
    rhi::Texture* templateArt[u32(ProjectTemplate::Count)] = {}; // Engine/Content/Templates/<Name>.png (editor captures)
    u32 TemplateArtSrv(ProjectTemplate t) const
    {
        const rhi::Texture* tex = templateArt[u32(t)];
        return tex ? tex->GetSrvIndex() : rhi::InvalidDescriptor;
    }
    bool CleanViewportShot() const { return m_screenshotViewport; } // no overlays over the scene
    u32 AppIconSrv() const { return appIcon ? appIcon->GetSrvIndex() : rhi::InvalidDescriptor; }
    UniquePtr<ui::Context> ui;
    UniquePtr<ui::UIRenderer> uiRenderer;
    UniquePtr<asset::AssetManager> assets;
    UniquePtr<render::RenderAssets> renderAssets;
    UniquePtr<ViewportRenderer> viewport;
    UniquePtr<PrefabLibrary> prefabs; // prefab files of the open project
    UniquePtr<MaterialLibrary> materials; // material graphs of the open project
    UniquePtr<ScriptSystem> scripts;      // ZeqouScript programs of the open project; runs them while playing
    UniquePtr<audio::AudioEngine> audioEngine; // WASAPI output (-noaudio: null backend)
    UniquePtr<AudioSystem> audio;         // Audio Sources of the scene while playing, previews
    PhysicsSystem physics;                // rigid bodies and colliders while playing
    ParticleSystem particles;             // Particle Emitters (live in the editor too)
    FireSystem fire;                      // Flammable objects (Play only)
    UniquePtr<GameModuleSystem> game;     // C++ game module of the open project (Source/<Name>), hot-reloaded
    i64 gameDllTime = 0;                  // modification time of the loaded Binaries/<Name>.dll
    GameBuildState gameBuildState = GameBuildState::Idle;
    InputActionMap inputActions;          // Config/Input.json of the open project
    InputActionState inputActionState;    // evaluated once per game frame
    UniquePtr<ThumbnailCache> thumbnails;
    UniquePtr<render::DebugDraw> debugDraw;
    UniquePtr<render::TransientResourcePool> pool;
    EditorLogSink* log = nullptr; // owned by the log system

    EditorMode mode = EditorMode::Editing;
    ProjectInfo project;          // descriptorPath is empty while no project is open
    ProjectBrowserState browser;
    String recentProjectsFile; // -recentfile=<path> (tests); empty = per-user default

    ecs::World world;
    SceneUndoStack undo{world};
    ecs::Entity selected;
    String sceneName = "Untitled";
    String scenePath;          // physical .zscene path; empty until saved
    bool sceneDirty = false;   // = undo.IsDirty(), refreshed every frame
    bool waitForAssets = false; // a scene is opening: the progress window blocks until its assets are loaded
    SceneAction pendingAction = SceneAction::None; // waiting for the save-changes dialog
    String pendingOpenPath;
    Array<PendingImport> pendingImports;
    PanelState panels;
    MaterialEditorState materialEditor;
    ToolState tools;          // Brush / Measure (EditorTools.cpp)
    bool showFeatures = false; // Window > Features
    ScriptEditorState scriptEditor;
    Array<EditorAction> actions; // menus, shortcuts, palette (EditorActions.cpp)
    PaletteState palette;

    FlyCamera camera;
    ViewportEnvironment environment;
    TransformTool tool = TransformTool::Move;
    bool localSpace = false;
    bool snapEnabled = false;
    f32 snapStep = 0.5f;
    PlayState playState = PlayState::Editing;
    // While playing: the game sees the keyboard/mouse (cursor locked) and the view comes from the game Camera.
    bool gameInputCaptured = false; // Shift+F1 releases, a click in the viewport captures again
    bool gameCameraEjected = false; // F8: back to the editor camera while the game keeps running
    /// The game camera is shown (playing, not ejected, a Camera exists).
    bool UsingGameCamera();
    void SetGameInputCaptured(bool captured);
    void ToggleEject();
    bool showGrid = true;
    // Viewport show flags for post-processing (motion blur only while playing).
    bool showPostVolumes = true, showDepthOfField = true, showMotionBlur = true;
    bool lastViewWasGame = false; // camera cut detection (editor <-> game camera)

    // Viewport target (recreated when the panel is resized).
    rhi::Texture* viewportTexture = nullptr;
    UVec2 viewportSize;
    Rect viewportRect;        // physical px, last frame
    bool viewportHovered = false;
    bool flying = false;

    InputState input;
    u32 debugLayerErrors = 0;
    bool selfTestFailed = false;

    // Frame stats.
    FrameTimer timer;
    f32 dt = 0.0f;
    f64 gpuFrameMs = 0.0;
    u64 frameIndex = 0;

    // Panel visibility (Window menu).
    bool showOutliner = true, showDetails = true, showContent = true, showLog = true, showViewport = true, showStats = false;
    bool showAbout = false;
    bool showMixer = false; // Window > Audio Mixer
    bool showInputActions = false; // Window > Input Actions
    bool requestResetLayout = false;
    bool quit = false;

    // Title bar hit testing (client px, last frame).
    Rect titleBarRect;
    Array<Rect> titleInteractive; // menus/buttons inside the caption area
    Rect minimizeRect, maximizeRect, closeRect;

    // --- Helpers ---
    void Select(ecs::Entity e);
    String EntityLabel(ecs::Entity e) const;
    ecs::Entity CreateEntity(StringView name, const Transform& t, ecs::Entity parent = {});
    void DeleteSelected();
    void DuplicateSelected();
    void FocusSelected();
    void ImportAsset(StringView physicalPath, Vec3 position, bool undoable = true);
    /// Scene file options for the open project (assets, content root, prefabs).
    SceneIOOptions SceneIO() const { return SceneIOOptions{assets.Get(), contentRoot, prefabs.Get()}; }

    // Prefabs (all undoable; Apply also rewrites the .zprefab file, which is not part of the undo history).
    void CreatePrefabFrom(ecs::Entity e);  // asks for a file name under Content
    bool CreatePrefabAt(ecs::Entity e, StringView physicalPath);
    void InstantiatePrefab(StringView physicalPath, Vec3 position);
    void ApplyPrefab(ecs::Entity e);       // e: any entity of the instance
    void RevertPrefab(ecs::Entity e);
    void UnpackPrefab(ecs::Entity e);

    // Materials.
    Guid CreateMaterial(StringView physicalPath);
    /// Sets every material slot of the entity's mesh (undoable).
    void AssignMaterial(ecs::Entity e, const Guid& material);
    /// Creates an empty entity / light at `position` (child of `parent`), as one undo step, and selects it.
    void CreateEmpty(Vec3 position, ecs::Entity parent = {});
    void CreatePointLight(Vec3 position, ecs::Entity parent = {});
    void CreateSpotLight(Vec3 position, ecs::Entity parent = {});
    void CreateWater(Vec3 position, ecs::Entity parent = {}); // 50 x 50 m at sea level (y = 0)
    void CreateAudioSource(Vec3 position, ecs::Entity parent = {});
    void CreateReverbZone(Vec3 position, ecs::Entity parent = {});
    void CreatePostProcessVolume(Vec3 position, ecs::Entity parent = {});
    void CreateReflectionProbe(Vec3 position, ecs::Entity parent = {});
    void CreateDecal(Vec3 position, ecs::Entity parent = {}); // on the surface under / in front of the point
    void CreateHeightFog(f32 density = 0.0f);                  // replaces the scene's one; density 0 = default
    void CreateFogVolume(Vec3 position, ecs::Entity parent = {});
    /// Sets the scene's HDRI Sky to the Content-relative panorama (creates the entity when there is none; undoable).
    void UseHdriSky(StringView texture);
    /// The listener while playing: the game camera when it is shown, else the editor camera.
    ListenerPose CurrentListener();
    void CreateShape(BuiltinShape shape, Vec3 position, ecs::Entity parent = {});
    /// Undoable re-parenting (keeps the world transform). Returns false if it would create a cycle.
    bool Reparent(ecs::Entity child, ecs::Entity parent);
    /// Wraps up an entity (hierarchy) created by the caller as one undo step and selects it.
    void CommitCreated(ecs::Entity e, StringView label);
    /// Opens an undo step that stays open while the user keeps interacting (gizmo drag, slider drag, text edit) and
    /// records `e` (or the explicit `before` state). Closed automatically once the mouse and UI are released.
    void BeginContinuousEdit(StringView label, ecs::Entity e, const JsonValue* before = nullptr);
    void Undo();
    void Redo();

    /// Features window: engine subsystems / tools switched off for the project (stored in the .zproj).
    bool FeatureEnabled(StringView id) const { return project.descriptor.IsFeatureEnabled(id); }
    void SetFeatureEnabled(StringView id, bool enabled);

    // Play-in-Editor: the scene is snapshotted on Play and restored on Stop (edits made while playing are discarded).
    bool IsPlaying() const { return playState != PlayState::Editing; }
    void BeginPlay();
    void EndPlay();
    void TogglePause();
    void StepFrame(); // one game frame while paused

    // Scene files.
    void RequestSceneAction(SceneAction action, StringView path = {}); // asks to save first if dirty
    void RunSceneAction(SceneAction action);
    void NewScene();
    bool OpenScene(StringView physicalPath);
    bool SaveScene(bool saveAs);
    bool SaveSceneTo(StringView physicalPath);

    // Projects.
    bool OpenProject(StringView descriptorPath);
    bool CreateAndOpenProject(StringView parentDirectory, StringView name, const NewProjectOptions& options);
    void ShowProjectBrowser();
    String ScenesDirectory() const { return contentRoot + "/Scenes"; }
    String BrowseForScene(bool save);
    void EnsureViewportTexture(UVec2 size);

private:
    void ProcessShortcuts();
    void UpdateImports();
    void RenderFrame();
    void BuildDefaultScene();
    void BuildStarterShowcase(bool withPrefabs);
    /// Starter Content prefabs (StarterAssets.cpp) for the categories chosen in the New Project page; returns the count.
    u32 WriteStarterPrefabs(const NewProjectOptions& options);
    void BuildCharacterLevel(bool thirdPerson);
    void BuildWaterLevel();
    JsonValue SceneSettings() const;
    void ApplySceneSettings(const JsonValue& settings);
    void ApplySelectionAfterUndo(const Array<ecs::Entity>& affected);
    /// -undotest: runs every kind of undoable edit through the editor, then checks undo/redo restore each state.
    bool RunUndoSelfTest();
    ecs::Entity FindByName(StringView name);
    String CanonicalScene();
    bool m_undoTest = false;
    String m_execCommands; // -exec="cmd; cmd": console commands run once the UI is up

    bool m_continuousEdit = false;
    JsonValue m_pieSnapshot;
    Guid m_pieSelection;
    bool m_pieStep = false;
    bool m_pieTest = false;
    bool m_materialTest = false;
    bool RunMaterialSelfTest();
    bool m_scriptTest = false;
    bool RunScriptSelfTest();
    u32 m_toolTest = 0; // -tooltest: frames left to wait for the test meshes (0 = off)
    u32 m_audioTestStage = 0; // -audiotest: 1 = start, 2 = playing, 3 = stopped, 0 = off
    u64 m_audioTestFrame = 0, m_audioTestUnderruns = 0;
    Array<ecs::Entity> m_audioTestSources;
    bool RunAudioSelfTest(); // returns false while still running; sets selfTestFailed on failure
    bool RunToolSelfTest(bool& retry);
    f64 m_scriptCheckTimer = 0.0;
    String m_scriptTestFile; // -scripttest: deleted on shutdown
    bool RunPieSelfTest();
    // -cpptest: build a C++ game module, run it in PIE, change the code, hot-reload while playing (EditorGameCode.cpp).
    u32 m_cppTestStage = 0;
    u64 m_cppTestFrame = 0;
    f64 m_cppTestStart = 0.0;
    u32 m_cppTestGeneration = 0;
    f32 m_cppTestY = 0.0f;
    Quat m_cppTestRotation;
    void RunCppSelfTest();
    // -posttest: Post Process Volumes blend as the camera moves through them; DOF + motion blur render.
    u32 m_postTestStage = 0;
    u64 m_postTestFrame = 0;
    void RunPostSelfTest();
    // -reflectiontest: mirror floor + emissive shapes + a Reflection Probe in open air; capture, recapture, SSR.
    u32 m_reflTestStage = 0;
    u64 m_reflTestFrame = 0;
    ecs::Entity m_reflTestMover;
    Array<String> m_reflTestFiles; // test materials, deleted on exit
    void RunReflectionSelfTest();
    // -decaltest: a textured and a glowing decal on the floor reach the renderer; editing the texture keeps working.
    u32 m_decalTestStage = 0;
    u64 m_decalTestFrame = 0;
    void RunDecalSelfTest();
    // -hdritest: a generated .exr panorama with a sun becomes the HDRI Sky; its sun drives the directional light and
    // follows the rotation; Sun = None turns it off.
    // -physicstest: Mesh Colliders on the scene, a column of Rigid Body cubes falls onto the floor and stacks, Stop
    // puts them back.
    u32 m_physicsTestStage = 0;
    u64 m_physicsTestFrame = 0;
    Array<ecs::Entity> m_physicsTestCubes;
    Array<Vec3> m_physicsTestStart;
    f32 m_physicsTestFloor = 0.0f;
    void RunPhysicsSelfTest();
    u32 m_hdriTestStage = 0;
    u64 m_hdriTestFrame = 0;
    u64 m_lastHdriKey = 0; // logs the HDRI Sky (hdri.info) when it changes
    String m_hdriTestFile;
    void RunHdriSelfTest();

public:
    bool CppSelfTestRunning() const { return m_cppTestStage != 0; }

private:
    String m_saveAfterImports; // new-project template: save the startup scene once its imports are placed
    void UpdateStartupScene(StringView savedScene);
    /// Copies a source from outside the project into Content/<Name>/ (with its dependencies); returns the copy.
    String CopyIntoProject(StringView physicalPath);

    Array<WindowEvent> m_events;
    u64 m_autotestFrames = 0;
    String m_layoutPath;
    String m_screenshotPath;
    bool m_screenshotEarly = false; // -screenshotnow: capture at the autotest frame even while loading
    bool m_screenshotViewport = false; // -screenshotviewport: only the viewport, no grid / selection (template thumbnails)
    Rect m_screenshotCrop;
    String m_saveScenePath; // -savescene=<path>: save the scene when an autotest run ends
    f64 m_timeUi = 0.0, m_timeRender = 0.0, m_timePresent = 0.0;
    u64 m_timedFrames = 0;
    rhi::Buffer* m_screenshotBuffer = nullptr;
    u64 m_screenshotPitch = 0;
    UVec2 m_screenshotSize;
};

// Panels / shell (one file each).
void DrawShell(Editor& ed);           // title bar, toolbar, dock space, status bar
void DrawProjectBrowser(Editor& ed, const Rect& area);
void DrawViewportPanel(Editor& ed);
/// Progress of loading work: a modal "Hold On" window while the user has to wait (imports, opening scenes), a small
/// panel above the status bar for background work (cooking, shader and C++ compiles).
void DrawProgress(Editor& ed);
void DrawOutlinerPanel(Editor& ed);
void DrawDetailsPanel(Editor& ed);
void DrawContentBrowserPanel(Editor& ed);
void DrawOutputLogPanel(Editor& ed);
void BuildDefaultLayout(ui::ID root);

/// Inspector widgets for reflected data. Returns true if a value changed.
bool PropertyEditor(const TypeInfo& type, void* object, StringView idPrefix = {});
/// Audio (EditorAudio.cpp).
void DrawAudioMixerPanel(Editor& ed);
/// C++ game code (EditorGameCode.cpp).
String GameModuleName(const ProjectInfo& project); // the project name as a C++ identifier
void SetupGameModule(Editor& ed);  // after a project opened: load the DLL, rebuild in the background
void UpdateGameModule(Editor& ed); // every frame: build results, hot reload
bool CompileGameCode(Editor& ed);  // F7: background build, hot reload when it succeeds
bool CreateGameModule(Editor& ed); // writes Source/<Name> (sample components) and builds it
void DrawInputActionsPanel(Editor& ed);
bool EditAudioSourceClip(Editor& ed, AudioSource& source);

/// Console variables/commands of the editor (EditorConsole.cpp).
void RegisterEditorConsole(Editor& ed);
void UnregisterEditorConsole();

// Panel titles (window ids for docking).
inline constexpr StringView ViewportTitle = ZX_ICON_CLAPPERBOARD "  Viewport";
inline constexpr StringView OutlinerTitle = ZX_ICON_LIST_TREE "  Outliner";
inline constexpr StringView DetailsTitle = ZX_ICON_SLIDERS_HORIZONTAL "  Details";
inline constexpr StringView ContentTitle = ZX_ICON_FOLDER_OPEN "  Content Browser";
inline constexpr StringView LogTitle = ZX_ICON_SQUARE_TERMINAL "  Output Log";
inline constexpr StringView StatsTitle = ZX_ICON_GAUGE "  Statistics";
inline constexpr StringView AudioMixerTitle = ZX_ICON_SLIDERS_VERTICAL "  Audio Mixer";
inline constexpr StringView InputActionsTitle = ZX_ICON_GAMEPAD_2 "  Input Actions";

} // namespace zx::editor
