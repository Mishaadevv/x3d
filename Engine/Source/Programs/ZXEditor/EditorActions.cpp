#include "EditorActions.h"
#include "Editor.h"
#include "Core/Containers/Algorithm.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Engine/SceneRendering.h"
#include "Platform/Platform.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogActions, Info);

namespace zx::editor {

namespace {

constexpr StringView KeysFile = "/Saved/Config/Keys.ini";

Key KeyFromName(StringView name)
{
    for (u32 k = 1; k < u32(Key::Count); ++k)
        if (StringView(KeyName(Key(k))).EqualsIgnoreCase(name))
            return Key(k);
    if (name.EqualsIgnoreCase("Del"))
        return Key::Delete;
    if (name.EqualsIgnoreCase("Esc"))
        return Key::Escape;
    return Key::Unknown;
}

void Add(Editor& ed, StringView id, StringView label, StringView category, StringView icon, StringView keys,
         Function<void(Editor&), 16> run, Function<bool(Editor&), 16> enabled = nullptr, bool repeat = false)
{
    EditorAction a;
    a.id = id;
    a.label = label;
    a.category = category;
    a.icon = icon;
    // "Ctrl+Y, Ctrl+Shift+Z"
    StringView rest = keys;
    for (u32 i = 0; i < 2 && !rest.Empty(); ++i) {
        StringView chord;
        rest.Split(',', chord, rest);
        a.defaultKeys[i] = a.keys[i] = KeyChord::Parse(chord.Trim());
    }
    a.repeat = repeat;
    a.run = move(run);
    a.enabled = move(enabled);
    ed.actions.PushBack(move(a));
}

bool HasSelection(Editor& ed) { return ed.world.IsAlive(ed.selected); }

/// Subsequence match: every query character in order; consecutive runs and word starts score higher. -1 = no match.
i32 FuzzyScore(StringView text, StringView query)
{
    if (query.Empty())
        return 0;
    i32 score = 0, run = 0;
    usize qi = 0;
    for (usize i = 0; i < text.Size() && qi < query.Size(); ++i) {
        char a = text[i], b = query[qi];
        if (a >= 'A' && a <= 'Z')
            a = char(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = char(b + 32);
        if (a == b) {
            const bool wordStart = i == 0 || text[i - 1] == ' ' || text[i - 1] == '/' || text[i - 1] == '_' || text[i - 1] == '.' ||
                                   (text[i] >= 'A' && text[i] <= 'Z' && text[i - 1] >= 'a' && text[i - 1] <= 'z');
            score += 1 + run * 4 + (wordStart ? 6 : 0);
            ++run;
            ++qi;
        } else {
            run = 0;
        }
    }
    if (qi < query.Size())
        return -1;
    return score * 100 / i32(text.Size() + 8); // prefer shorter matches
}

void ScanContent(const String& dir, const String& root, Array<String>& out)
{
    Array<platform::DirectoryEntry> entries;
    platform::ListDirectory(dir, entries);
    for (const platform::DirectoryEntry& e : entries) {
        if (e.name.View().StartsWith(".") || Path::HasExtension(e.name, ".zmeta"))
            continue;
        const String path = Path::Join(dir, e.name);
        if (e.isDirectory)
            ScanContent(path, root, out);
        else if (out.Size() < 20000)
            out.PushBack(path);
    }
}

} // namespace

// --- Chords -------------------------------------------------------------------------------------------------------------

String KeyChord::ToString() const
{
    if (!IsValid())
        return {};
    String s;
    if (HasAnyFlags(mods, KeyModifiers::Control))
        s += "Ctrl+";
    if (HasAnyFlags(mods, KeyModifiers::Shift))
        s += "Shift+";
    if (HasAnyFlags(mods, KeyModifiers::Alt))
        s += "Alt+";
    s += KeyName(key);
    return s;
}

KeyChord KeyChord::Parse(StringView text)
{
    KeyChord c;
    StringView rest = text.Trim();
    while (!rest.Empty()) {
        StringView part;
        rest.Split('+', part, rest);
        part = part.Trim();
        if (part.EqualsIgnoreCase("Ctrl") || part.EqualsIgnoreCase("Control"))
            c.mods |= KeyModifiers::Control;
        else if (part.EqualsIgnoreCase("Shift"))
            c.mods |= KeyModifiers::Shift;
        else if (part.EqualsIgnoreCase("Alt"))
            c.mods |= KeyModifiers::Alt;
        else if (!part.Empty())
            c.key = KeyFromName(part);
    }
    return c;
}

String EditorAction::ShortcutText() const
{
    String s = keys[0].ToString();
    if (keys[1].IsValid())
        s += String::Format(", {}", keys[1].ToString());
    return s;
}

// --- Registry -----------------------------------------------------------------------------------------------------------

void RegisterEditorActions(Editor& ed)
{
    ed.actions.Clear();
    // File
    Add(ed, "file.newScene", "New Scene", "File", ZX_ICON_FILE_PLUS, "Ctrl+N", [](Editor& e) { e.RequestSceneAction(SceneAction::New); });
    Add(ed, "file.openScene", "Open Scene...", "File", ZX_ICON_FOLDER_OPEN, "Ctrl+O", [](Editor& e) { e.RequestSceneAction(SceneAction::Open); });
    Add(ed, "file.saveScene", "Save Scene", "File", ZX_ICON_SAVE, "Ctrl+S", [](Editor& e) { e.SaveScene(false); });
    Add(ed, "file.saveSceneAs", "Save Scene As...", "File", ZX_ICON_SAVE_ALL, "Ctrl+Shift+S", [](Editor& e) { e.SaveScene(true); });
    Add(ed, "file.import", "Import Asset...", "File", ZX_ICON_IMPORT, "Ctrl+I", [](Editor& e) {
        static const platform::FileDialogFilter Filters[] = {{"3D models", "*.gltf;*.glb;*.obj"}, {"All files", "*.*"}};
        for (const String& path : platform::OpenFileDialog(e.window.Get(), "Import Asset", Span<const platform::FileDialogFilter>(Filters, 2), true))
            e.ImportAsset(path, e.camera.position + e.camera.Forward() * 5.0f);
    });
    Add(ed, "file.openProject", "Open Project...", "File", ZX_ICON_FOLDER_KANBAN, "", [](Editor& e) { e.RequestSceneAction(SceneAction::CloseProject); });
    Add(ed, "file.exit", "Exit", "File", ZX_ICON_LOG_OUT, "Alt+F4", [](Editor& e) { e.RequestSceneAction(SceneAction::Quit); });
    // Edit
    Add(ed, "edit.undo", "Undo", "Edit", ZX_ICON_UNDO_2, "Ctrl+Z", [](Editor& e) { e.Undo(); }, [](Editor& e) { return e.undo.CanUndo(); }, true);
    Add(ed, "edit.redo", "Redo", "Edit", ZX_ICON_REDO_2, "Ctrl+Y, Ctrl+Shift+Z", [](Editor& e) { e.Redo(); },
        [](Editor& e) { return e.undo.CanRedo(); }, true);
    Add(ed, "edit.duplicate", "Duplicate", "Edit", ZX_ICON_COPY, "Ctrl+D", [](Editor& e) { e.DuplicateSelected(); }, HasSelection);
    Add(ed, "edit.delete", "Delete", "Edit", ZX_ICON_TRASH_2, "Delete", [](Editor& e) { e.DeleteSelected(); }, HasSelection);
    Add(ed, "edit.focus", "Focus Selection", "Edit", ZX_ICON_SCAN, "F", [](Editor& e) { e.FocusSelected(); }, HasSelection);
    Add(ed, "edit.createPrefab", "Create Prefab from Selection...", "Edit", ZX_ICON_PACKAGE, "", [](Editor& e) { e.CreatePrefabFrom(e.selected); },
        HasSelection);
    // Tools
    Add(ed, "tool.select", "Select Tool", "Tools", ZX_ICON_MOUSE_POINTER_2, "Q", [](Editor& e) { e.tool = TransformTool::Select; });
    Add(ed, "tool.move", "Move Tool", "Tools", ZX_ICON_MOVE, "W", [](Editor& e) { e.tool = TransformTool::Move; });
    Add(ed, "tool.rotate", "Rotate Tool", "Tools", ZX_ICON_ROTATE_3D, "E", [](Editor& e) { e.tool = TransformTool::Rotate; });
    Add(ed, "tool.scale", "Scale Tool", "Tools", ZX_ICON_SCALE_3D, "R", [](Editor& e) { e.tool = TransformTool::Scale; });
    Add(ed, "tool.space", "Toggle Local / World Space", "Tools", ZX_ICON_GLOBE, "", [](Editor& e) { e.localSpace = !e.localSpace; });
    Add(ed, "tool.snap", "Toggle Grid Snapping", "Tools", ZX_ICON_MAGNET, "", [](Editor& e) { e.snapEnabled = !e.snapEnabled; });
    Add(ed, "view.grid", "Toggle Grid", "View", ZX_ICON_GRID_3X3, "", [](Editor& e) { e.showGrid = !e.showGrid; });
    // Play
    Add(ed, "play.toggle", "Play / Stop", "Play", ZX_ICON_PLAY, "F5", [](Editor& e) { e.IsPlaying() ? e.EndPlay() : e.BeginPlay(); });
    Add(ed, "play.stop", "Stop", "Play", ZX_ICON_SQUARE, "Shift+F5", [](Editor& e) { e.EndPlay(); }, [](Editor& e) { return e.IsPlaying(); });
    Add(ed, "play.pause", "Pause / Resume", "Play", ZX_ICON_PAUSE, "F6", [](Editor& e) { e.TogglePause(); }, [](Editor& e) { return e.IsPlaying(); });
    Add(ed, "play.step", "Step One Frame", "Play", ZX_ICON_STEP_FORWARD, "F10", [](Editor& e) { e.StepFrame(); });
    Add(ed, "play.eject", "Eject / Possess Game Camera", "Play", ZX_ICON_VIDEO, "F8", [](Editor& e) { e.ToggleEject(); },
        [](Editor& e) { return e.IsPlaying(); });
    Add(ed, "play.releaseMouse", "Release Mouse", "Play", ZX_ICON_MOUSE_POINTER, "Shift+F1",
        [](Editor& e) { e.SetGameInputCaptured(false); }, [](Editor& e) { return e.gameInputCaptured; });
    // Create
    auto spawn = [](Editor& e) { return e.camera.position + e.camera.Forward() * 5.0f; };
    Add(ed, "create.empty", "Create Empty Entity", "Create", ZX_ICON_CIRCLE_DASHED, "", [spawn](Editor& e) { e.CreateEmpty(spawn(e)); });
    Add(ed, "create.cube", "Create Cube", "Create", ZX_ICON_BOX, "", [spawn](Editor& e) { e.CreateShape(BuiltinShape::Cube, spawn(e)); });
    Add(ed, "create.sphere", "Create Sphere", "Create", ZX_ICON_CIRCLE, "", [spawn](Editor& e) { e.CreateShape(BuiltinShape::Sphere, spawn(e)); });
    Add(ed, "create.cylinder", "Create Cylinder", "Create", ZX_ICON_CYLINDER, "", [spawn](Editor& e) { e.CreateShape(BuiltinShape::Cylinder, spawn(e)); });
    Add(ed, "create.plane", "Create Plane", "Create", ZX_ICON_SQUARE, "", [spawn](Editor& e) { e.CreateShape(BuiltinShape::Plane, spawn(e)); });
    Add(ed, "create.pointLight", "Create Point Light", "Create", ZX_ICON_LIGHTBULB, "", [spawn](Editor& e) { e.CreatePointLight(spawn(e)); });
    Add(ed, "create.spotLight", "Create Spot Light", "Create", ZX_ICON_SPOTLIGHT, "", [spawn](Editor& e) { e.CreateSpotLight(spawn(e)); });
    Add(ed, "create.water", "Create Water", "Create", ZX_ICON_DROPLETS, "", [spawn](Editor& e) { e.CreateWater(spawn(e)); });
    Add(ed, "create.audioSource", "Create Audio Source", "Create", ZX_ICON_VOLUME_2, "", [spawn](Editor& e) { e.CreateAudioSource(spawn(e)); });
    Add(ed, "create.reverbZone", "Create Reverb Zone", "Create", ZX_ICON_AUDIO_LINES, "", [spawn](Editor& e) { e.CreateReverbZone(spawn(e)); });
    Add(ed, "create.decal", "Create Decal", "Create", ZX_ICON_STAMP, "", [spawn](Editor& e) { e.CreateDecal(spawn(e)); });
    Add(ed, "create.heightfog", "Create Height Fog", "Create", ZX_ICON_CLOUD_FOG, "", [](Editor& e) { e.CreateHeightFog(); });
    Add(ed, "create.clouds", "Create Volumetric Clouds", "Create", ZX_ICON_CLOUDY, "", [](Editor& e) { e.CreateClouds(); });
    Add(ed, "create.fogvolume", "Create Fog Volume", "Create", ZX_ICON_HAZE, "", [spawn](Editor& e) { e.CreateFogVolume(spawn(e)); });
    Add(ed, "create.reflectionProbe", "Create Reflection Probe", "Create", ZX_ICON_CIRCLE_DOT, "",
        [spawn](Editor& e) { e.CreateReflectionProbe(spawn(e)); });
    Add(ed, "create.hdriSky", "Create HDRI Sky", "Create", ZX_ICON_CLOUD_SUN, "", [](Editor& e) { e.UseHdriSky(""); });
    Add(ed, "view.recaptureReflections", "Recapture Reflections", "View", ZX_ICON_REFRESH_CW, "",
        [](Editor& e) { e.viewport->Reflections().RecaptureAll(); });
    Add(ed, "create.postProcessVolume", "Create Post Process Volume", "Create", ZX_ICON_APERTURE, "",
        [spawn](Editor& e) { e.CreatePostProcessVolume(spawn(e)); });
    Add(ed, "window.audioMixer", "Audio Mixer", "Window", ZX_ICON_SLIDERS_VERTICAL, "", [](Editor& e) { e.showMixer = !e.showMixer; });
    Add(ed, "window.inputActions", "Input Actions", "Window", ZX_ICON_GAMEPAD_2, "", [](Editor& e) { e.showInputActions = !e.showInputActions; });
    // Game code
    auto hasProject = [](Editor& e) { return !e.project.descriptorPath.Empty(); };
    Add(ed, "game.compile", "Compile Game Code", "Game", ZX_ICON_HAMMER, "F7", [](Editor& e) { CompileGameCode(e); },
        [](Editor& e) { return !e.project.descriptorPath.Empty() && e.gameBuildState != GameBuildState::Building; });
    Add(ed, "game.createModule", "Create C++ Game Module", "Game", ZX_ICON_FOLDER_CODE, "", [](Editor& e) { CreateGameModule(e); }, hasProject);
    Add(ed, "game.openSource", "Open Game Source Folder", "Game", ZX_ICON_FOLDER_OPEN, "", [](Editor& e) {
        platform::OpenUrl(Path::Join(e.project.root, String::Format("Source/{}", GameModuleName(e.project))));
    }, [](Editor& e) { return !e.project.descriptorPath.Empty() && HasGameModuleSource(e.project.root, GameModuleName(e.project)); });
    Add(ed, "create.material", "Create Material", "Create", ZX_ICON_PALETTE, "", [](Editor& e) {
        String dir = e.panels.browserCurrent.Empty() ? e.contentRoot : e.panels.browserCurrent;
        String path = Path::Join(dir, "NewMaterial.zmat");
        for (u32 n = 2; platform::FileExists(path); ++n)
            path = Path::Join(dir, String::Format("NewMaterial{}.zmat", n));
        const Guid g = e.CreateMaterial(path);
        if (g.IsValid())
            OpenMaterialEditor(e, g);
    });
    // Window
    Add(ed, "window.viewport", "Viewport", "Window", ZX_ICON_CLAPPERBOARD, "", [](Editor& e) { e.showViewport = !e.showViewport; });
    Add(ed, "window.outliner", "Outliner", "Window", ZX_ICON_LIST_TREE, "", [](Editor& e) { e.showOutliner = !e.showOutliner; });
    Add(ed, "window.details", "Details", "Window", ZX_ICON_SLIDERS_HORIZONTAL, "", [](Editor& e) { e.showDetails = !e.showDetails; });
    Add(ed, "window.content", "Content Browser", "Window", ZX_ICON_FOLDER_OPEN, "", [](Editor& e) { e.showContent = !e.showContent; });
    Add(ed, "window.log", "Output Log", "Window", ZX_ICON_SQUARE_TERMINAL, "", [](Editor& e) { e.showLog = !e.showLog; });
    Add(ed, "window.stats", "Statistics", "Window", ZX_ICON_GAUGE, "", [](Editor& e) { e.showStats = !e.showStats; });
    Add(ed, "window.features", "Features", "Window", ZX_ICON_TOGGLE_RIGHT, "", [](Editor& e) { e.showFeatures = !e.showFeatures; });
    Add(ed, "tool.brush", "Brush Tool", "Tools", ZX_ICON_PAINTBRUSH, "B", [](Editor& e) { SetActiveTool(e, EditorTool::Brush); },
        [](Editor& e) { return e.FeatureEnabled("brush") && !e.IsPlaying(); });
    Add(ed, "tool.measure", "Measure Tool", "Tools", ZX_ICON_RULER, "M", [](Editor& e) { SetActiveTool(e, EditorTool::Measure); },
        [](Editor& e) { return e.FeatureEnabled("measure") && !e.IsPlaying(); });
    Add(ed, "edit.dropToGround", "Drop to Ground", "Edit", ZX_ICON_ARROW_DOWN_TO_LINE, "End", [](Editor& e) { DropSelectedToGround(e); },
        [](Editor& e) { return e.world.IsAlive(e.selected) && !e.IsPlaying(); });
    Add(ed, "brush.smaller", "Brush Smaller", "Tools", ZX_ICON_MINUS, "[", [](Editor& e) { e.tools.brush.radius = Max(0.25f, e.tools.brush.radius / 1.15f); },
        [](Editor& e) { return e.tools.active == EditorTool::Brush; }, true);
    Add(ed, "brush.bigger", "Brush Bigger", "Tools", ZX_ICON_PLUS, "]", [](Editor& e) { e.tools.brush.radius = Min(50.0f, e.tools.brush.radius * 1.15f); },
        [](Editor& e) { return e.tools.active == EditorTool::Brush; }, true);
    Add(ed, "window.resetLayout", "Reset Layout", "Window", ZX_ICON_LAYOUT_DASHBOARD, "", [](Editor& e) { e.requestResetLayout = true; });
    Add(ed, "view.fullscreen", "Toggle Fullscreen", "View", ZX_ICON_MAXIMIZE, "F11", [](Editor& e) {
        e.window->SetMode(e.window->GetMode() == WindowMode::Windowed ? WindowMode::BorderlessFullscreen : WindowMode::Windowed);
    });
    Add(ed, "editor.palette", "Go to Anything...", "Editor", ZX_ICON_SEARCH, "Ctrl+P", [](Editor& e) { OpenCommandPalette(e, false); });
    Add(ed, "editor.commands", "Command Palette...", "Editor", ZX_ICON_SQUARE_CHEVRON_RIGHT, "Ctrl+Shift+P", [](Editor& e) { OpenCommandPalette(e, true); });
    Add(ed, "editor.keys", "Edit Keyboard Shortcuts", "Editor", ZX_ICON_KEYBOARD, "", [](Editor&) { platform::OpenUrl(fs::ToPhysicalPath(KeysFile)); });
    // Help
    Add(ed, "help.docs", "Documentation", "Help", ZX_ICON_BOOK_OPEN, "F1", [](Editor& e) { platform::OpenUrl("file:///" + e.engineRoot + "/docs/TZ.md"); });
    Add(ed, "help.about", "About ZeqouX3D", "Help", ZX_ICON_INFO, "", [](Editor& e) { e.showAbout = true; });

    // Keys.ini: "id = Ctrl+S" or "id = Ctrl+Y, Ctrl+Shift+Z" ("id =" unbinds). Written with the defaults if missing.
    String text;
    if (fs::ReadTextFile(KeysFile, text)) {
        StringView rest = text.View();
        u32 lineNo = 0;
        while (!rest.Empty()) {
            StringView line;
            rest.Split('\n', line, rest);
            ++lineNo;
            line = line.Trim();
            if (line.Empty() || line.StartsWith("#") || line.StartsWith(";"))
                continue;
            StringView id, value;
            if (!line.Split('=', id, value)) {
                ZX_LOG_WARN(LogActions, "Keys.ini:{}: expected 'action = keys'", lineNo);
                continue;
            }
            id = id.Trim();
            EditorAction* action = nullptr;
            for (EditorAction& a : ed.actions)
                if (a.id == id)
                    action = &a;
            if (!action) {
                ZX_LOG_WARN(LogActions, "Keys.ini:{}: unknown action '{}'", lineNo, id);
                continue;
            }
            action->keys[0] = action->keys[1] = KeyChord();
            StringView chords = value.Trim();
            for (u32 i = 0; i < 2 && !chords.Empty(); ++i) {
                StringView chord;
                chords.Split(',', chord, chords);
                action->keys[i] = KeyChord::Parse(chord);
                if (!chord.Trim().Empty() && !action->keys[i].IsValid())
                    ZX_LOG_WARN(LogActions, "Keys.ini:{}: can't parse '{}'", lineNo, chord.Trim());
            }
        }
    } else {
        String out = "# ZeqouX3D Editor keyboard shortcuts: action = Key, Ctrl+Key, Ctrl+Shift+Key ... (two chords: 'A, B').\n"
                     "# Empty value = no shortcut. Restart the editor after editing.\n\n";
        StringView category;
        for (const EditorAction& a : ed.actions) {
            if (a.category != category) {
                category = a.category;
                out += String::Format("\n# {}\n", category);
            }
            out += String::Format("{} = {}\n", a.id, a.ShortcutText());
        }
        fs::WriteTextFile(KeysFile, out);
    }
}

const EditorAction* FindAction(Editor& ed, StringView id)
{
    for (const EditorAction& a : ed.actions)
        if (a.id == id)
            return &a;
    return nullptr;
}

bool RunAction(Editor& ed, StringView id)
{
    const EditorAction* a = FindAction(ed, id);
    if (!a || (a->enabled && !a->enabled(ed)))
        return false;
    a->run(ed);
    return true;
}

bool DispatchShortcut(Editor& ed, Key key, KeyModifiers mods, bool repeat)
{
    mods &= KeyModifiers::Control | KeyModifiers::Shift | KeyModifiers::Alt;
    for (const EditorAction& a : ed.actions)
        for (const KeyChord& k : a.keys)
            if (k.IsValid() && k.key == key && k.mods == mods && (!repeat || a.repeat)) {
                if (!a.enabled || a.enabled(ed))
                    a.run(ed);
                return true;
            }
    return false;
}

void ActionMenuItem(Editor& ed, StringView id, bool selected)
{
    const EditorAction* a = FindAction(ed, id);
    if (!a)
        return;
    const bool enabled = !a->enabled || a->enabled(ed);
    if (ui::MenuItem(String::Format("{}  {}", a->icon, a->label), a->keys[0].ToString(), selected, enabled))
        a->run(ed);
}

// --- Command palette ---------------------------------------------------------------------------------------------------------

void OpenCommandPalette(Editor& ed, bool commandsOnly)
{
    PaletteState& p = ed.palette;
    p.open = true;
    p.focus = true;
    p.commandsOnly = commandsOnly;
    p.query.Clear();
    p.selection = 0;
    p.assets.Clear();
    if (!commandsOnly && !ed.contentRoot.Empty())
        ScanContent(ed.contentRoot, ed.contentRoot, p.assets);
}

void DrawCommandPalette(Editor& ed)
{
    PaletteState& p = ed.palette;
    static constexpr StringView Title = "##command_palette";
    if (p.open && !ui::IsPopupOpen(Title))
        ui::OpenPopup(Title);
    if (!ui::BeginPopupModal(Title)) {
        p.open = false;
        return;
    }
    ui::Context& ctx = *ui::GetCurrentContext();
    const ui::Theme& th = ctx.GetStyle().colors;
    ui::SetNextItemWidth(560.0f);
    if (p.focus) {
        ui::SetKeyboardFocusHere();
        p.focus = false;
    }
    const bool enter = ui::InputText("##palette_query", p.query, ui::InputTextFlags::EnterReturnsTrue | ui::InputTextFlags::KeepFocusOnEnter,
                                     p.commandsOnly ? ZX_ICON_SQUARE_CHEVRON_RIGHT "  Run a command..." : ZX_ICON_SEARCH "  Commands, assets, entities...");

    // Gather and rank.
    struct Item
    {
        i32 score;
        u8 kind; // 0 action, 1 asset, 2 entity
        usize index;
        ecs::Entity entity;
    };
    Array<Item> items;
    const StringView q = p.query.View().Trim();
    for (usize i = 0; i < ed.actions.Size(); ++i) {
        const EditorAction& a = ed.actions[i];
        const String text = String::Format("{}: {}", a.category, a.label);
        const i32 s = FuzzyScore(text, q);
        if (s >= 0 && (!a.enabled || a.enabled(ed)))
            items.PushBack(Item{s + 5, 0, i, {}});
    }
    if (!p.commandsOnly && !q.Empty()) {
        for (usize i = 0; i < p.assets.Size(); ++i) {
            const i32 s = FuzzyScore(Path::FileName(p.assets[i]), q);
            if (s >= 0)
                items.PushBack(Item{s, 1, i, {}});
        }
        ed.world.Query<const scene::EntityName>().Each([&](ecs::Entity e, const scene::EntityName& n) {
            const i32 s = FuzzyScore(n.value, q);
            if (s >= 0)
                items.PushBack(Item{s, 2, 0, e});
        });
    }
    StableSort(items, [](const Item& a, const Item& b) { return a.score > b.score; });
    const usize shown = Min<usize>(items.Size(), 14);
    p.selection = items.Empty() ? 0 : Clamp(p.selection, 0, i32(shown) - 1);
    if (ui::IsKeyPressed(Key::Down) && shown > 0)
        p.selection = (p.selection + 1) % i32(shown);
    if (ui::IsKeyPressed(Key::Up) && shown > 0)
        p.selection = (p.selection + i32(shown) - 1) % i32(shown);

    i32 run = enter && shown > 0 ? p.selection : -1;
    ui::Spacing();
    for (usize i = 0; i < shown; ++i) {
        const Item& it = items[i];
        String label, detail;
        StringView icon;
        if (it.kind == 0) {
            const EditorAction& a = ed.actions[it.index];
            // "Create: Sphere" rather than "Create: Create Sphere".
            StringView shortLabel = a.label;
            if (shortLabel.StartsWith(a.category) && shortLabel.Size() > a.category.Size() + 1 && shortLabel[a.category.Size()] == ' ')
                shortLabel = shortLabel.SubStr(a.category.Size() + 1);
            label = String::Format("{}: {}", a.category, shortLabel);
            detail = a.ShortcutText();
            icon = a.icon;
        } else if (it.kind == 1) {
            label = Path::FileName(p.assets[it.index]);
            detail = Path::MakeRelative(Path::Parent(p.assets[it.index]), ed.contentRoot);
            icon = ZX_ICON_FILE;
        } else {
            label = ed.EntityLabel(it.entity);
            detail = "Entity";
            icon = ZX_ICON_BOX;
        }
        ui::PushID(i32(i));
        if (ui::Selectable(String::Format("{}  {}", icon, label), i32(i) == p.selection, Vec2(560.0f, 24.0f)))
            run = i32(i);
        if (!detail.Empty()) {
            const Rect r = ui::GetItemRect();
            ui::GetWindowDrawList().AddTextInRect(ctx.Fonts().FindFont("Inter"), 11.5f * ctx.GetScale(),
                                                  Rect(Vec2(r.Center().x, r.min.y), r.max - Vec2(8.0f * ctx.GetScale(), 0.0f)), th.textDisabled,
                                                  detail, ui::TextAlign::Right);
        }
        ui::PopID();
    }
    if (shown == 0)
        ui::TextDisabled("No matches");

    const bool escape = ui::IsKeyPressed(Key::Escape, false);
    if (run >= 0 || escape) {
        ui::CloseCurrentPopup();
        p.open = false;
    }
    ui::EndPopup();
    if (run < 0)
        return;
    const Item it = items[usize(run)];
    if (it.kind == 0) {
        RunAction(ed, ed.actions[it.index].id);
    } else if (it.kind == 2) {
        ed.Select(it.entity);
        ed.FocusSelected();
    } else {
        const String path = p.assets[it.index];
        if (Path::HasExtension(path, ".zscene")) {
            ed.RequestSceneAction(SceneAction::Open, path);
        } else if (Path::HasExtension(path, ".zmat") || Path::HasExtension(path, ".zmati")) {
            MaterialLibrary::Material* m = ed.materials ? ed.materials->FindByPath(path) : nullptr;
            const Guid g = m ? m->guid : (ed.materials ? ed.materials->LoadFile(path) : Guid());
            if (g.IsValid())
                OpenMaterialEditor(ed, g);
        } else if (Path::HasExtension(path, ".gltf") || Path::HasExtension(path, ".glb") || Path::HasExtension(path, ".obj") ||
                   Path::HasExtension(path, ".zprefab")) {
            ed.ImportAsset(path, ed.camera.position + ed.camera.Forward() * 5.0f);
        } else {
            ed.panels.browserCurrent = String(Path::Parent(path));
            ed.panels.browserSelected = path;
            ui::FocusDockedWindow(ContentTitle);
        }
    }
}

} // namespace zx::editor
