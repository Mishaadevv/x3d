// Editor shell: custom title bar (logo, menus, caption buttons), main toolbar (tools, play controls), dock space with
// the panels, status bar.

#include "Editor.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Version.h"
#include "RHI/EngineProfile.h"
#include "Engine/SceneRendering.h"
#include "Platform/Platform.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogEditorShell, Info);

namespace zx::editor {

namespace {

constexpr f32 TitleBarHeight = 34.0f;
constexpr f32 ToolbarHeight = 40.0f;
constexpr f32 StatusBarHeight = 24.0f;

/// A borderless strip window (title bar, toolbar, status bar) that sits above the docked panels.
bool BeginStrip(StringView name, const Rect& r, Color bg)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    ui::SetNextWindowPos(r.min / s, ui::Cond::Always);
    ui::SetNextWindowSize(r.Size() / s, ui::Cond::Always);
    ui::Begin(name, nullptr,
              ui::WindowFlags::NoTitleBar | ui::WindowFlags::NoResize | ui::WindowFlags::NoMove | ui::WindowFlags::NoScrollbar |
                  ui::WindowFlags::NoCollapse | ui::WindowFlags::NoBringToFront | ui::WindowFlags::NoBackground);
    ui::GetWindowDrawList().PushClipRect(r, 0.0f, false);
    ui::GetWindowDrawList().AddRect(r, bg);
    return true;
}

void EndStrip()
{
    ui::GetWindowDrawList().PopClipRect();
    ui::End();
}

void DrawLogo(Editor& ed, DrawList& dl, const Rect& r, f32 scale)
{
    if (ed.appIcon) {
        dl.AddImage(ed.AppIconSrv(), r);
        return;
    }
    dl.AddRectGradient(r, Color(0x3B, 0x8C, 0xF5), Color(0x6A, 0x3D, 0xE8), ui::GradientDirection::Vertical, ui::CornerRadii(5.0f * scale));
    if (!ed.logo.shapes.Empty()) {
        dl.AddSvg(ed.logo, Rect(r.min + r.Size() * 0.12f, r.max - r.Size() * 0.12f), Color(255, 255, 255));
        return;
    }
    ui::PushFont(ui::FontStyle::Bold, 14.0f);
    ui::Context& ctx = *ui::GetCurrentContext();
    const ui::FontHandle bold = ctx.Fonts().FindFont("Inter Bold");
    dl.AddTextInRect(bold, 14.0f * scale, r, Color(255, 255, 255), "Z", ui::TextAlign::Center, false);
    ui::PopFont();
}

void CaptionButton(DrawList& dl, const Rect& r, StringView icon, bool hovered, bool danger)
{
    const ui::Theme& c = ui::GetCurrentContext()->GetStyle().colors;
    if (hovered)
        dl.AddRect(r, danger ? Color(0xC4, 0x2B, 0x1C) : Color(255, 255, 255, 22));
    ui::Context& ctx = *ui::GetCurrentContext();
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 13.0f * ctx.GetScale(), r, hovered ? Color(255, 255, 255) : c.textDim, icon,
                     ui::TextAlign::Center, false);
}

void DrawMenus(Editor& ed)
{
    if (ui::BeginMenu("File")) {
        for (StringView id : {StringView("file.newScene"), StringView("file.openScene"), StringView("file.saveScene"), StringView("file.saveSceneAs")})
            ActionMenuItem(ed, id);
        ui::Separator();
        ActionMenuItem(ed, "file.openProject");
        if (ui::MenuItem(ZX_ICON_FOLDER_SEARCH "  Show Project in Explorer", {}, false, !ed.project.descriptorPath.Empty()))
            platform::RevealInFileExplorer(ed.project.descriptorPath);
        ui::Separator();
        ActionMenuItem(ed, "file.import");
        ui::Separator();
        ActionMenuItem(ed, "build.bakeLighting");
        ActionMenuItem(ed, "build.navigation");
        ActionMenuItem(ed, "file.package");
        ui::Separator();
        ActionMenuItem(ed, "file.settings");
        ui::Separator();
        ActionMenuItem(ed, "file.exit");
        ui::EndMenu();
    }
    if (ui::BeginMenu("Edit")) {
        const String undoLabel = ed.undo.CanUndo() ? String::Format(ZX_ICON_UNDO_2 "  Undo {}", ed.undo.UndoLabel()) : String(ZX_ICON_UNDO_2 "  Undo");
        const String redoLabel = ed.undo.CanRedo() ? String::Format(ZX_ICON_REDO_2 "  Redo {}", ed.undo.RedoLabel()) : String(ZX_ICON_REDO_2 "  Redo");
        const EditorAction* undoAction = FindAction(ed, "edit.undo");
        const EditorAction* redoAction = FindAction(ed, "edit.redo");
        if (ui::MenuItem(undoLabel, undoAction ? undoAction->keys[0].ToString() : String(), false, ed.undo.CanUndo()))
            ed.Undo();
        if (ui::MenuItem(redoLabel, redoAction ? redoAction->keys[0].ToString() : String(), false, ed.undo.CanRedo()))
            ed.Redo();
        if (ui::BeginMenu(ZX_ICON_HISTORY "  Undo History")) {
            if (ed.undo.Count() == 0)
                ui::TextDisabled("(empty)");
            // Newest first; clicking a step undoes/redoes up to it.
            for (usize i = ed.undo.Count(); i > 0; --i) {
                if (ui::MenuItem(String::Format("{}##h{}", ed.undo.Label(i - 1), i), {}, i == ed.undo.CurrentIndex())) {
                    while (ed.undo.CurrentIndex() > i)
                        ed.Undo();
                    while (ed.undo.CurrentIndex() < i)
                        ed.Redo();
                }
            }
            ui::EndMenu();
        }
        ui::Separator();
        for (StringView id : {StringView("edit.duplicate"), StringView("edit.delete"), StringView("edit.focus"), StringView("edit.createPrefab")})
            ActionMenuItem(ed, id);
        ui::Separator();
        ActionMenuItem(ed, "editor.palette");
        ActionMenuItem(ed, "editor.commands");
        ActionMenuItem(ed, "editor.keys");
        ui::EndMenu();
    }
    if (ui::BeginMenu("Create")) {
        ActionMenuItem(ed, "create.empty");
        if (ui::BeginMenu(ZX_ICON_SHAPES "  Shapes")) {
            for (StringView id : {StringView("create.cube"), StringView("create.sphere"), StringView("create.cylinder"), StringView("create.plane")})
                ActionMenuItem(ed, id);
            ui::EndMenu();
        }
        ui::Separator();
        ActionMenuItem(ed, "create.pointLight");
        ActionMenuItem(ed, "create.spotLight");
        ui::Separator();
        ActionMenuItem(ed, "create.water");
        ActionMenuItem(ed, "create.terrain");
        ActionMenuItem(ed, "create.terrainFlat");
        ActionMenuItem(ed, "create.windZone");
        ActionMenuItem(ed, "create.navMeshBounds");
        ActionMenuItem(ed, "create.navAgent");
        if (ui::BeginMenu(ZX_ICON_MONITOR "  UI")) {
            for (StringView id : {StringView("create.ui.canvas"), StringView("create.ui.panel"), StringView("create.ui.text"), StringView("create.ui.button"),
                                  StringView("create.ui.image"), StringView("create.ui.progress"), StringView("create.ui.slider"), StringView("create.ui.toggle"), StringView("create.ui.input"),
                                  StringView("create.ui.scroll"), StringView("create.ui.nameplate")})
                ActionMenuItem(ed, id);
            ui::EndMenu();
        }
        ActionMenuItem(ed, "create.audioSource");
        ActionMenuItem(ed, "create.reverbZone");
        ActionMenuItem(ed, "create.postProcessVolume");
        ActionMenuItem(ed, "create.reflectionProbe");
        ActionMenuItem(ed, "create.decal");
        ActionMenuItem(ed, "create.sun");
        ActionMenuItem(ed, "create.skylight");
        ActionMenuItem(ed, "create.heightfog");
        ActionMenuItem(ed, "create.fogvolume");
        ActionMenuItem(ed, "create.clouds");
        ActionMenuItem(ed, "create.fluidfire");
        ActionMenuItem(ed, "create.fluidsmoke");
        ActionMenuItem(ed, "create.hdriSky");
        ui::Separator();
        ActionMenuItem(ed, "create.material");
        ui::EndMenu();
    }
    if (ui::BeginMenu("Tools")) {
        ActionMenuItem(ed, "tool.brush", ed.tools.active == EditorTool::Brush);
        ActionMenuItem(ed, "tool.measure", ed.tools.active == EditorTool::Measure);
        ActionMenuItem(ed, "tool.terrain", ed.tools.active == EditorTool::Terrain);
        ActionMenuItem(ed, "tool.uiDesigner", ed.tools.active == EditorTool::UIDesigner);
        ActionMenuItem(ed, "edit.dropToGround");
        ui::Separator();
        ActionMenuItem(ed, "game.compile");
        ActionMenuItem(ed, "game.createModule");
        ActionMenuItem(ed, "game.openSource");
        ActionMenuItem(ed, "window.inputActions", ed.showInputActions);
        ui::Separator();
        ActionMenuItem(ed, "window.features", ed.showFeatures);
        ui::EndMenu();
    }
    if (ui::BeginMenu("Window")) {
        ui::MenuItem(ViewportTitle, {}, &ed.showViewport);
        ui::MenuItem(OutlinerTitle, {}, &ed.showOutliner);
        ui::MenuItem(DetailsTitle, {}, &ed.showDetails);
        ui::MenuItem(ContentTitle, {}, &ed.showContent);
        ui::MenuItem(LogTitle, {}, &ed.showLog);
        ui::MenuItem(StatsTitle, {}, &ed.showStats);
        ui::MenuItem(AudioMixerTitle, {}, &ed.showMixer);
        ui::MenuItem(InputActionsTitle, {}, &ed.showInputActions);
        ui::MenuItem(LocalizationTitle, {}, &ed.showLocalization);
        ui::Separator();
        ActionMenuItem(ed, "window.resetLayout");
        ActionMenuItem(ed, "view.fullscreen");
        ui::EndMenu();
    }
    if (ui::BeginMenu("Help")) {
        ActionMenuItem(ed, "help.docs");
        ActionMenuItem(ed, "help.engineProfile");
        ActionMenuItem(ed, "help.about");
        ui::EndMenu();
    }
}

void DrawTitleBar(Editor& ed, const Rect& r, bool menus)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;
    BeginStrip("##TitleBar", r, c.titleBg);
    DrawList& dl = ui::GetWindowDrawList();
    ed.titleBarRect = r;
    ed.titleInteractive.Clear();

    const f32 logoSize = Round(20.0f * s);
    const Rect logo(Vec2(r.min.x + Round(10.0f * s), Round(r.Center().y - logoSize * 0.5f)),
                    Vec2(r.min.x + Round(10.0f * s) + logoSize, Round(r.Center().y - logoSize * 0.5f) + logoSize));
    if (ed.mode != EditorMode::ProjectBrowser) // the Project Browser shows the brand in its sidebar
        DrawLogo(ed, dl, logo, s);

    // Menus right of the logo.
    const Rect menuArea(Vec2(logo.max.x + Round(8.0f * s), r.min.y), Vec2(r.max.x * 0.5f, r.max.y));
    f32 menuEnd = logo.max.x;
    if (menus) {
        ui::BeginMenuBarInRect(menuArea);
        DrawMenus(ed);
        menuEnd = ui::GetCursorPos().x;
        ui::EndMenuBar();
        ed.titleInteractive.PushBack(Rect(menuArea.min, Vec2(menuEnd, menuArea.max.y)));
    }

    // Caption buttons (clicks are handled by the platform layer through hit testing).
    const f32 bw = Round(46.0f * s);
    ed.closeRect = Rect(Vec2(r.max.x - bw, r.min.y), r.max);
    ed.maximizeRect = Rect(Vec2(r.max.x - bw * 2.0f, r.min.y), Vec2(r.max.x - bw, r.max.y));
    ed.minimizeRect = Rect(Vec2(r.max.x - bw * 3.0f, r.min.y), Vec2(r.max.x - bw * 2.0f, r.max.y));
    const Vec2 m = ui::GetMousePos();
    CaptionButton(dl, ed.minimizeRect, ZX_ICON_MINUS, ed.minimizeRect.Contains(m), false);
    CaptionButton(dl, ed.maximizeRect, ed.window->IsMaximized() ? ZX_ICON_COPY : ZX_ICON_SQUARE, ed.maximizeRect.Contains(m), false);
    CaptionButton(dl, ed.closeRect, ZX_ICON_X, ed.closeRect.Contains(m), true);

    // Centered document title.
    const StringView playTag = ed.playState == PlayState::Playing ? "  [Playing]" : ed.playState == PlayState::Paused ? "  [Paused]" : "";
    const String title = !menus ? String("ZeqouX3D Project Browser")
                         : ed.project.descriptorPath.Empty()
                             ? String::Format("{}{}{}  -  ZeqouX3D Editor", ed.sceneName, ed.sceneDirty ? "*" : "", playTag)
                             : String::Format("{}{}{}  -  {}  -  ZeqouX3D Editor", ed.sceneName, ed.sceneDirty ? "*" : "", playTag,
                                              ed.project.descriptor.name);
    const ui::FontHandle font = ctx.Fonts().FindFont("Inter");
    const f32 tw = ctx.Fonts().MeasureWidth(font, 12.5f * s, title);
    Rect tr(Vec2(Round(r.Center().x - tw * 0.5f), r.min.y), Vec2(Round(r.Center().x + tw * 0.5f) + 2.0f, r.max.y));
    if (tr.min.x < menuEnd + 20.0f * s)
        tr = Rect(Vec2(menuEnd + 20.0f * s, r.min.y), Vec2(ed.minimizeRect.min.x - 10.0f * s, r.max.y));
    dl.AddTextInRect(font, 12.5f * s, tr, c.textDim, title, ui::TextAlign::Center);
    EndStrip();
}

bool ToolButton(StringView icon, StringView tooltip, bool selected)
{
    return ui::IconButton(icon, tooltip, selected);
}

void VerticalDivider()
{
    ui::SameLine(0.0f, 6.0f);
    const Vec2 p = ui::GetCursorPos();
    const f32 h = ui::GetFrameHeight();
    ui::GetWindowDrawList().AddRect(Rect(Vec2(p.x, p.y + h * 0.2f), Vec2(p.x + 1.0f, p.y + h * 0.8f)),
                                     ui::GetCurrentContext()->GetStyle().colors.border);
    ui::Dummy(Vec2(1.0f, h / ui::GetCurrentContext()->GetScale()));
    ui::SameLine(0.0f, 6.0f);
}

void DrawToolbar(Editor& ed, const Rect& r)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;
    BeginStrip("##Toolbar", r, c.menuBarBg);
    DrawList& dl = ui::GetWindowDrawList();
    dl.AddRect(Rect(Vec2(r.min.x, r.max.y - 1.0f), r.max), c.border);
    const f32 fh = ui::GetFrameHeight();
    ui::SetCursorPos(Vec2(r.min.x + Round(8.0f * s), Round(r.Center().y - fh * 0.5f)));

    if (ToolButton(ZX_ICON_MOUSE_POINTER_2, "Select (Q)", ed.tool == TransformTool::Select))
        ed.tool = TransformTool::Select;
    ui::SameLine(0.0f, 2.0f);
    if (ToolButton(ZX_ICON_MOVE, "Move (W)", ed.tool == TransformTool::Move))
        ed.tool = TransformTool::Move;
    ui::SameLine(0.0f, 2.0f);
    if (ToolButton(ZX_ICON_ROTATE_3D, "Rotate (E)", ed.tool == TransformTool::Rotate))
        ed.tool = TransformTool::Rotate;
    ui::SameLine(0.0f, 2.0f);
    if (ToolButton(ZX_ICON_SCALE_3D, "Scale (R)", ed.tool == TransformTool::Scale))
        ed.tool = TransformTool::Scale;
    VerticalDivider();
    if (ToolButton(ed.localSpace ? ZX_ICON_BOX : ZX_ICON_GLOBE, ed.localSpace ? "Local space" : "World space", false))
        ed.localSpace = !ed.localSpace;
    ui::SameLine(0.0f, 2.0f);
    if (ToolButton(ZX_ICON_MAGNET, "Grid snapping", ed.snapEnabled))
        ed.snapEnabled = !ed.snapEnabled;
    ui::SameLine(0.0f, 2.0f);
    ui::SetNextItemWidth(56.0f);
    ui::DragFloat("##snap", &ed.snapStep, 0.01f, 0.01f, 100.0f, "{:.2f}");
    ui::ItemTooltip("Snap step (m)");
    VerticalDivider();
    if (ToolButton(ZX_ICON_GRID_3X3, "Show grid", ed.showGrid))
        ed.showGrid = !ed.showGrid;
    if (ed.FeatureEnabled("brush") || ed.FeatureEnabled("measure"))
        VerticalDivider();
    if (ed.FeatureEnabled("brush")) {
        if (ToolButton(ZX_ICON_PAINTBRUSH, "Brush: paint models onto surfaces (B)", ed.tools.active == EditorTool::Brush))
            SetActiveTool(ed, EditorTool::Brush);
        ui::SameLine(0.0f, 2.0f);
    }
    if (ed.FeatureEnabled("measure")) {
        if (ToolButton(ZX_ICON_RULER, "Measure distances (M)", ed.tools.active == EditorTool::Measure))
            SetActiveTool(ed, EditorTool::Measure);
        ui::SameLine(0.0f, 2.0f);
    }
    if (ed.FeatureEnabled("terrainTool")) {
        if (ToolButton(ZX_ICON_MOUNTAIN, "Terrain: sculpt and paint terrains (T)", ed.tools.active == EditorTool::Terrain))
            SetActiveTool(ed, EditorTool::Terrain);
        ui::SameLine(0.0f, 2.0f);
    }
    if (ToolButton(ZX_ICON_LAYOUT_DASHBOARD, "UI Designer: build HUDs and menus with the mouse (U)", ed.tools.active == EditorTool::UIDesigner))
        SetActiveTool(ed, EditorTool::UIDesigner);
    ui::SameLine(0.0f, 2.0f);
    if (ed.tool == TransformTool::Scale) {
        if (ToolButton(ZX_ICON_ARROW_RIGHT_FROM_LINE, "Scale from one side: the opposite side stays put (Alt while dragging: the other way)",
                       ed.prefs.scaleFromOneSide))
            ed.prefs.scaleFromOneSide = !ed.prefs.scaleFromOneSide;
        ui::SameLine(0.0f, 2.0f);
    }
    if (ToolButton(ZX_ICON_ARROW_DOWN_TO_LINE, "Drop selection to the ground (End)", false))
        DropSelectedToGround(ed);

    // Play controls, centered (with Compile in front of them when the project has C++ game code).
    const f32 bw = fh;
    const f32 groupW = bw * 3.0f + 4.0f * s;
    const bool hasGameCode = !ed.project.descriptorPath.Empty() && ed.game && !ed.game->ProjectRoot().Empty() &&
                             HasGameModuleSource(ed.project.root, GameModuleName(ed.project));
    if (hasGameCode) {
        ui::SetCursorPos(Vec2(Round(r.Center().x - groupW * 0.5f - bw - 10.0f * s), Round(r.Center().y - fh * 0.5f)));
        const bool building = ed.gameBuildState == GameBuildState::Building;
        const String tip = building ? String("Compiling game code...")
                           : ed.gameBuildState == GameBuildState::Failed
                               ? String("Compile game code (F7) - the last build FAILED, see the Output Log")
                               : String::Format("Compile game code (F7) - hot-reloads while playing{}",
                                                ed.game->IsLoaded() ? String::Format(" (loaded: generation {})", ed.game->Generation()) : String());
        ui::BeginDisabled(building);
        if (ToolButton(building ? ZX_ICON_LOADER_CIRCLE : ZX_ICON_HAMMER, tip, false))
            CompileGameCode(ed);
        ui::EndDisabled();
        if (ed.gameBuildState == GameBuildState::Failed || ed.gameBuildState == GameBuildState::Succeeded) {
            // Small status dot on the button.
            const Rect br = ui::GetItemRect();
            const Color dot = ed.gameBuildState == GameBuildState::Failed ? Color(0xE5, 0x48, 0x4D) : Color(0x4C, 0xC3, 0x7A);
            dl.AddCircle(Vec2(br.max.x - 4.0f * s, br.min.y + 4.0f * s), 3.0f * s, dot);
        }
    }
    ui::SetCursorPos(Vec2(Round(r.Center().x - groupW * 0.5f), Round(r.Center().y - fh * 0.5f)));
    const bool playing = ed.IsPlaying();
    if (ToolButton(playing ? ZX_ICON_SQUARE : ZX_ICON_PLAY, playing ? "Stop (F5 / Esc)" : "Play (F5)", playing)) {
        if (playing)
            ed.EndPlay();
        else
            ed.BeginPlay();
    }
    ui::SameLine(0.0f, 2.0f);
    ui::BeginDisabled(!playing);
    if (ToolButton(ZX_ICON_PAUSE, "Pause (F6)", ed.playState == PlayState::Paused))
        ed.TogglePause();
    ui::EndDisabled();
    ui::SameLine(0.0f, 2.0f);
    if (ToolButton(ZX_ICON_STEP_FORWARD, "Step one frame (F10)", false))
        ed.StepFrame();

    // Right side: profile badge.
    const String badge = ed.lite ? "LITE" : "FULL";
    const ui::FontHandle bold = ctx.Fonts().FindFont("Inter SemiBold");
    const f32 bwText = ctx.Fonts().MeasureWidth(bold, 11.0f * s, badge) + 14.0f * s;
    const Rect br(Vec2(r.max.x - bwText - 10.0f * s, r.Center().y - 10.0f * s), Vec2(r.max.x - 10.0f * s, r.Center().y + 10.0f * s));
    ui::SetCursorPos(br.min);
    if (ui::InvisibleButton("##engine_profile", br.Size() / s))
        ed.showProfileDialog = true;
    const bool badgeHovered = ui::IsItemHovered();
    ui::ItemTooltip(String::Format("Engine profile: {} - click to change", ed.lite ? "Lite (for laptops and integrated graphics)" : "Full"));
    dl.AddRect(br, WithAlpha(c.accent, badgeHovered ? 0.32f : 0.18f), ui::CornerRadii(4.0f * s), 1.0f, WithAlpha(c.accent, 0.6f));
    dl.AddTextInRect(bold, 11.0f * s, br, c.accentHovered, badge, ui::TextAlign::Center, false);
    EndStrip();
}

void DrawStatusBar(Editor& ed, const Rect& r)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;
    BeginStrip("##StatusBar", r, c.titleBg);
    DrawList& dl = ui::GetWindowDrawList();
    const ui::FontHandle font = ctx.Fonts().FindFont("Inter");
    const f32 size = 12.0f * s;
    const u32 errors = ed.log->Counts(LogLevel::Error) + ed.log->Counts(LogLevel::Fatal);
    const u32 warnings = ed.log->Counts(LogLevel::Warning);
    String left = ed.log->LastMessage();
    if (left.Empty())
        left = "Ready";
    Color leftColor = c.textDim;
    dl.AddTextInRect(font, size, Rect(r.min + Vec2(10.0f * s, 0.0f), Vec2(r.Center().x, r.max.y)), leftColor, left);
    const String right = String::Format(ZX_ICON_CIRCLE_X " {}   " ZX_ICON_TRIANGLE_ALERT " {}   |   {} entities   |   {:.0f} FPS   |   GPU {:.1f} ms",
                                        errors, warnings, ed.world.EntityCount(), 1.0f / Max(ed.timer.SmoothedDeltaSeconds(), 1e-4f),
                                        ed.gpuFrameMs);
    dl.AddTextInRect(font, size, Rect(Vec2(r.Center().x, r.min.y), r.max - Vec2(10.0f * s, 0.0f)), c.textDim, right, ui::TextAlign::Right);
    EndStrip();
}

void DrawAbout(Editor& ed)
{
    if (ed.showAbout) {
        ui::OpenPopup("About ZeqouX3D");
        ed.showAbout = false;
    }
    if (ui::BeginPopupModal("About ZeqouX3D")) {
        if (ed.appLogo) {
            ui::Image(ed.appLogo->GetSrvIndex(), Vec2(160.0f, 160.0f));
            ui::SameLine(0.0f, 16.0f);
            ui::BeginGroup();
        }
        ui::Heading("ZeqouX3D Editor");
        ui::TextDisabled(String::Format("Version {} ({})", EngineVersionString, BuildConfigurationName));
        ui::Spacing();
        ui::Text("A from-scratch C++20 / Direct3D 12 engine. Part of the Zeqou ecosystem.");
        ui::LabelText("GPU", ed.device->GetCaps().adapter.name);
        ui::LabelText("Profile", ed.lite ? "Lite" : "Full");
        if (ed.appLogo)
            ui::EndGroup();
        ui::Spacing();
        if (ui::Button("Close", Vec2(100.0f, 0.0f)))
            ui::CloseCurrentPopup();
        ui::EndPopup();
    }
}

void DrawProfileDialog(Editor& ed)
{
    static constexpr StringView Title = "Engine Profile";
    static i32 choice = 0; // 0 = Lite, 1 = Full
    if (ed.showProfileDialog) {
        ui::OpenPopup(Title);
        ed.showProfileDialog = false;
        choice = ed.lite ? 0 : 1;
    }
    if (!ui::BeginPopupModal(Title))
        return;
    const ui::Theme& c = ui::GetCurrentContext()->GetStyle().colors;
    const bool recommendedLite = RecommendedEngineProfile(ed.device->GetCaps()) == EngineProfile::Lite;
    ui::Text(String::Format("Current profile: {}", ed.lite ? "Lite" : "Full"));
    ui::TextDisabled(String::Format("Recommended for this computer ({}): {}", ed.device->GetCaps().adapter.name, recommendedLite ? "Lite" : "Full"));
    ui::Spacing();
    ui::RadioButton("Lite", &choice, 0);
    ui::Indent(24.0f);
    ui::TextDisabled("For laptops and integrated graphics: forward renderer, lighter shadows, effects,\n"
                     "water and clouds tuned for 60 FPS on Intel UHD-class GPUs.");
    ui::Unindent(24.0f);
    ui::RadioButton("Full", &choice, 1);
    ui::Indent(24.0f);
    ui::TextDisabled("Everything at high quality: deferred renderer, bigger shadow maps, full-resolution\n"
                     "effects, water and clouds. Made for dedicated graphics cards.");
    ui::Unindent(24.0f);
    ui::Spacing();
    if (choice == 1 && recommendedLite) {
        ui::PushTextColor(c.warning);
        ui::TextWrapped(ZX_ICON_TRIANGLE_ALERT "  This computer has integrated graphics (or little memory). With Full the editor and "
                        "your game will run at a very low frame rate (FPS) and the laptop may get hot. You can switch "
                        "back to Lite here at any time.");
        ui::PopTextColor();
        ui::Spacing();
    }
    if (IsEngineProfileForced()) {
        ui::PushTextColor(c.warning);
        ui::TextWrapped("The editor was started with -lite / -full (or engine.Profile is set): that setting wins until it is removed.");
        ui::PopTextColor();
        ui::Spacing();
    }
    ui::TextDisabled("The editor restarts to apply the profile (you will be asked to save changes first).");
    ui::Spacing();
    const bool changed = (choice == 0) != ed.lite;
    ui::BeginDisabled(!changed);
    if (ui::Button(ZX_ICON_ROTATE_CCW "  Restart Now", Vec2(150.0f, 0.0f), ui::ButtonVariant::Primary)) {
        ui::CloseCurrentPopup();
        SaveEngineProfilePreference(choice == 0 ? "lite" : "full");
        ed.RequestRestart();
    }
    ui::EndDisabled();
    ui::SameLine();
    if (ui::Button("Cancel", Vec2(100.0f, 0.0f)) || ui::IsKeyPressed(Key::Escape, false))
        ui::CloseCurrentPopup();
    ui::EndPopup();
}

void DrawSaveChangesDialog(Editor& ed)
{
    static constexpr StringView Title = "Save Changes?";
    if (ed.pendingAction != SceneAction::None && !ui::IsPopupOpen(Title))
        ui::OpenPopup(Title);
    if (ui::BeginPopupModal(Title)) {
        ui::Text(String::Format("Save changes to '{}' before {}?", ed.sceneName,
                                ed.pendingAction == SceneAction::Quit ? "closing" : "continuing"));
        ui::TextDisabled("Your changes will be lost if you don't save them.");
        ui::Spacing();
        const SceneAction action = ed.pendingAction;
        if (ui::Button(ZX_ICON_SAVE "  Save", Vec2(100.0f, 0.0f), ui::ButtonVariant::Primary)) {
            ui::CloseCurrentPopup();
            if (ed.SaveScene(false))
                ed.RunSceneAction(action);
            else
                ed.pendingAction = SceneAction::None;
        }
        ui::SameLine();
        if (ui::Button("Don't Save", Vec2(100.0f, 0.0f))) {
            ui::CloseCurrentPopup();
            ed.RunSceneAction(action);
        }
        ui::SameLine();
        if (ui::Button("Cancel", Vec2(100.0f, 0.0f)) || ui::IsKeyPressed(Key::Escape, false)) {
            ui::CloseCurrentPopup();
            ed.pendingAction = SceneAction::None;
            ed.restartCommandLine.Clear(); // a profile switch waits for the next restart
        }
        ui::EndPopup();
    }
}

void DrawStatsPanel(Editor& ed)
{
    if (!ed.showStats)
        return;
    if (ui::Begin(StatsTitle, &ed.showStats)) {
        const render::SceneRenderer::Stats& st = ed.viewport->Scene().GetStats();
        ui::LabelText("Viewport", String::Format("{}x{}", ed.viewportSize.x, ed.viewportSize.y));
        ui::LabelText("Draw calls", String::Format("{} (+{} shadow)", st.draws, st.shadowDraws));
        ui::LabelText("Triangles", String::Format("{:.0f}k", f64(st.triangles) / 1000.0));
        ui::LabelText("UI primitives", String::Format("{}", ed.uiRenderer->GetStats().primitives));
        ui::SeparatorText("GPU passes");
        for (const rhi::GpuTiming& t : ed.device->GetGpuTimings())
            ui::LabelText(t.name, String::Format("{:.2f} ms", t.milliseconds));
    }
    ui::End();
}

} // namespace

void BuildDefaultLayout(ui::ID root)
{
    ui::ID mainArea;
    const ui::ID right = ui::DockSplitNode(root, ui::DockDir::Right, 0.22f, &mainArea);
    ui::ID viewportNode;
    const ui::ID bottom = ui::DockSplitNode(mainArea, ui::DockDir::Down, 0.30f, &viewportNode);
    ui::ID outliner;
    const ui::ID details = ui::DockSplitNode(right, ui::DockDir::Down, 0.55f, &outliner);
    ui::DockWindow(ViewportTitle, viewportNode);
    ui::DockWindow(OutlinerTitle, outliner);
    ui::DockWindow(DetailsTitle, details);
    ui::DockWindow(ContentTitle, bottom);
    ui::DockWindow(LogTitle, bottom);
    ui::DockWindow(StatsTitle, outliner);
    ui::FocusDockedWindow(ContentTitle);
    ui::FocusDockedWindow(OutlinerTitle);
}

void DrawShell(Editor& ed)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const Vec2 display = Vec2(f32(ed.swapChain->GetSize().x), f32(ed.swapChain->GetSize().y));
    const f32 th = Round(TitleBarHeight * s), tbh = Round(ToolbarHeight * s), sh = Round(StatusBarHeight * s);
    if (ed.mode == EditorMode::ProjectBrowser) {
        DrawTitleBar(ed, Rect(Vec2::Zero(), Vec2(display.x, th)), false);
        DrawProjectBrowser(ed, Rect(Vec2(0.0f, th), display));
        DrawAbout(ed);
        DrawProfileDialog(ed);
        return;
    }
    DrawTitleBar(ed, Rect(Vec2::Zero(), Vec2(display.x, th)), true);
    DrawToolbar(ed, Rect(Vec2(0.0f, th), Vec2(display.x, th + tbh)));
    DrawStatusBar(ed, Rect(Vec2(0.0f, display.y - sh), display));

    if (ed.requestResetLayout) {
        ui::LoadDockLayout("");
        ed.requestResetLayout = false;
        ed.showViewport = ed.showOutliner = ed.showDetails = ed.showContent = ed.showLog = true;
    }
    const f32 gap = Round(2.0f * s);
    const ui::ID root = ui::DockSpace("EditorDock", Rect(Vec2(gap, th + tbh + gap), Vec2(display.x - gap, display.y - sh - gap)));
    if (!ui::DockSpaceHasLayout("EditorDock"))
        BuildDefaultLayout(root);

    if (ed.showViewport)
        DrawViewportPanel(ed);
    if (ed.showOutliner)
        DrawOutlinerPanel(ed);
    if (ed.showDetails)
        DrawDetailsPanel(ed);
    if (ed.showContent)
        DrawContentBrowserPanel(ed);
    if (ed.showLog)
        DrawOutputLogPanel(ed);
    DrawStatsPanel(ed);
    DrawMaterialEditorPanel(ed);
    DrawAnimationViewer(ed);
    DrawAnimGraphEditor(ed);
    DrawToolPanel(ed);
    DrawFeaturesPanel(ed);
    DrawSettingsWindow(ed);
    DrawAudioMixerPanel(ed);
    DrawInputActionsPanel(ed);
    DrawLocalizationPanel(ed);
    DrawPackagePanel(ed);
    DrawScriptEditorPanel(ed);
    DrawZGraphEditorPanel(ed);
    DrawBTEditorPanel(ed);
    DrawAbout(ed);
    DrawProfileDialog(ed);
    DrawSaveChangesDialog(ed);
    DrawProgress(ed);
    DrawCommandPalette(ed);
}

} // namespace zx::editor
