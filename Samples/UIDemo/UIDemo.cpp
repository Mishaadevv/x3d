// UIDemo (M6 sample): every ZUI widget in a few windows - menus, buttons, toggles, sliders/drags, text fields, combo,
// color picker, trees, tabs, a sortable 10k-row virtualized table, popups/modals, tooltips, drag & drop.
// Keys: 1/2/3 - UI scale 100/125/150 %, Ctrl +/- zoom, F11 - fullscreen, Esc (no popup) - quit.
// Command line: -scale=<f> (UI zoom), -light (light theme), -warp, -debuglayer, -novsync, -autotest[=frames],
// -screenshot=<path>, -scenario=menu|combo|color|modal|tooltip (scripted clicks for screenshots).

#include "Core/Console/Console.h"
#include "Core/CoreInit.h"
#include "Core/Containers/Algorithm.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Image/Png.h"
#include "Core/Log/Log.h"
#include "Core/Math/Random.h"
#include "Core/Profiler/Profiler.h"
#include "Core/String/Parse.h"
#include "Core/Threading/Thread.h"
#include "Core/Time/Time.h"
#include "Platform/Platform.h"
#include "Platform/Window.h"
#include "RHI/EngineProfile.h"
#include "RHI/ShaderCompiler.h"
#include "RHI_D3D12/D3D12Backend.h"
#include "UI/UI.h"
#include "UI/UIRenderer.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogUIDemo, Info);

using namespace zx;
using namespace zx::rhi;

namespace {

struct Asset
{
    String name;
    String type;
    f32 sizeMb;
    i32 refs;
};

struct SceneNode
{
    String name;
    const char* icon;
    i32 parent;
};

struct DemoState
{
    // Widgets.
    bool checkA = true, checkB = false, toggleA = true, toggleB = false;
    i32 radio = 1;
    f32 exposure = 0.0f, roughness = 0.45f, fov = 70.0f, speed = 4.0f;
    i32 samples = 16, count = 3, quality = 2;
    f32 position[3] = {1.25f, 0.0f, -3.5f};
    f32 scale2[2] = {1.0f, 1.0f};
    LinearColor tint = LinearColor::FromHex("#4C8DF6");
    LinearColor fog = LinearColor(0.6f, 0.65f, 0.8f, 0.5f);
    String name = "Sponza";
    String search;
    String password = "secret";
    String notes = "Multi-line editor:\n- select with the mouse or Shift+arrows\n- Ctrl+C / Ctrl+V / Ctrl+Z\n- Ctrl+arrows jump by words";
    f32 progress = 0.0f;
    // Tree.
    Array<SceneNode> nodes;
    i32 selectedNode = 2;
    // Table.
    Array<Asset> assets;
    i32 selectedAsset = -1;
    // Tabs.
    bool tabOpen[4] = {true, true, true, true};
    bool showModal = false;
    bool showDemoWindow = true, showTable = true, showTree = true, showTabs = true, showStats = true;
    String log;
    // Rects of a few items for scripted screenshots (-scenario=...).
    Rect viewMenuRect, scaleMenuRect, comboRect, colorRect, deleteRect, treeItemRect;
};

void InitDemo(DemoState& s)
{
    s.nodes = {{"Sponza", ZX_ICON_BOX, -1},           {"Atrium", ZX_ICON_FOLDER, 0},     {"Columns", ZX_ICON_LAYERS, 1},
               {"Arches", ZX_ICON_LAYERS, 1},           {"Curtains", ZX_ICON_SHIRT, 1},       {"Light", ZX_ICON_FOLDER, 0},
               {"Sun", ZX_ICON_SUN, 5},            {"Lantern 01", ZX_ICON_LAMP, 5},    {"Lantern 02", ZX_ICON_LAMP, 5},
               {"Camera", ZX_ICON_CAMERA, 0},         {"Audio: Wind", ZX_ICON_MUSIC, 0}, {"Post Process", ZX_ICON_SPARKLES, 0}};
    static const char* Types[] = {"Mesh", "Texture", "Material", "Audio", "Scene", "Prefab"};
    static const char* Names[] = {"Rock", "Door", "Lamp", "Wall", "Floor", "Barrel", "Crate", "Tree", "Jar", "Stone", "Pillar"};
    Random rng(42);
    for (u32 i = 0; i < 10000; ++i) {
        Asset a;
        a.name = String::Format("{}_{:04}", Names[rng.NextU32() % 11], i);
        a.type = Types[rng.NextU32() % 6];
        a.sizeMb = rng.NextFloat() * 64.0f;
        a.refs = i32(rng.NextU32() % 40);
        s.assets.PushBack(move(a));
    }
}

void TreeRecursive(DemoState& s, i32 parent)
{
    for (i32 i = 0; i < i32(s.nodes.Size()); ++i) {
        if (s.nodes[usize(i)].parent != parent)
            continue;
        bool hasChildren = false;
        for (const SceneNode& n : s.nodes)
            hasChildren |= n.parent == i;
        ui::TreeNodeFlags flags = ui::TreeNodeFlags::OpenOnArrow | ui::TreeNodeFlags::DefaultOpen;
        if (!hasChildren)
            flags |= ui::TreeNodeFlags::Leaf;
        if (s.selectedNode == i)
            flags |= ui::TreeNodeFlags::Selected;
        ui::PushID(i);
        const bool open = ui::TreeNode(String::Format("{}  {}##node", s.nodes[usize(i)].icon, s.nodes[usize(i)].name), flags);
        if (ui::IsItemClicked())
            s.selectedNode = i;
        if (i == 7)
            s.treeItemRect = ui::GetItemRect();
        ui::ItemTooltip(String::Format("Entity #{} - drag onto another one to reparent", i));
        if (ui::BeginDragDropSource("node", &i, sizeof(i), s.nodes[usize(i)].name)) {
        }
        if (const void* payload = ui::AcceptDragDropPayload("node")) {
            const i32 dragged = *static_cast<const i32*>(payload);
            // Refuse cycles.
            bool cycle = false;
            for (i32 p = i; p >= 0; p = s.nodes[usize(p)].parent)
                cycle |= p == dragged;
            if (!cycle) {
                s.nodes[usize(dragged)].parent = i;
                s.log += String::Format("Reparented: {} -> {}\n", s.nodes[usize(dragged)].name, s.nodes[usize(i)].name);
            }
        }
        if (ui::BeginPopupContextItem()) {
            ui::TextDisabled(s.nodes[usize(i)].name);
            ui::Separator();
            if (ui::MenuItem(ZX_ICON_COPY "  Duplicate", "Ctrl+D"))
                s.nodes.PushBack({s.nodes[usize(i)].name + " (copy)", s.nodes[usize(i)].icon, s.nodes[usize(i)].parent});
            if (ui::MenuItem(ZX_ICON_PENCIL "  Rename", "F2"))
                s.log += "Rename (demo)\n";
            ui::Separator();
            if (ui::MenuItem(ZX_ICON_TRASH_2 "  Delete", "Del", false, i != 0))
                s.nodes[usize(i)].parent = -2; // hidden
            ui::EndPopup();
        }
        if (open) {
            if (hasChildren)
                TreeRecursive(s, i);
            ui::TreePop();
        }
        ui::PopID();
    }
}

void DrawDemo(DemoState& s, ui::Context& ctx, bool& quit, f32 frameMs, f32 gpuMs, const ui::UIRenderer::Stats& uiStats, Vec2 display)
{
    ui::Style& style = ctx.GetStyle();
    if (ui::BeginMainMenuBar()) {
        if (ui::BeginMenu("File")) {
            ui::MenuItem(ZX_ICON_FILE_PLUS "  New Scene", "Ctrl+N");
            ui::MenuItem(ZX_ICON_FOLDER_OPEN "  Open…", "Ctrl+O");
            ui::MenuItem(ZX_ICON_SAVE "  Save", "Ctrl+S");
            if (ui::BeginMenu("Recent")) {
                ui::MenuItem("Sponza.zscene");
                ui::MenuItem("UIShowcase.zscene");
                ui::EndMenu();
            }
            ui::Separator();
            if (ui::MenuItem("Exit", "Alt+F4"))
                quit = true;
            ui::EndMenu();
        }
        const bool viewOpen = ui::BeginMenu("View");
        s.viewMenuRect = ui::GetItemRect();
        if (viewOpen) {
            ui::MenuItem("Widgets", {}, &s.showDemoWindow);
            ui::MenuItem("Asset Table", {}, &s.showTable);
            ui::MenuItem("Hierarchy", {}, &s.showTree);
            ui::MenuItem("Tabs", {}, &s.showTabs);
            ui::MenuItem("Statistics", {}, &s.showStats);
            ui::Separator();
            const bool scaleOpen = ui::BeginMenu("UI Scale");
            s.scaleMenuRect = ui::GetItemRect();
            if (scaleOpen) {
                for (f32 sc : {1.0f, 1.25f, 1.5f, 2.0f})
                    if (ui::MenuItem(String::Format("{}%", i32(sc * 100.0f)), {}, Abs(ctx.GetUserScale() - sc) < 0.01f))
                        ctx.SetUserScale(sc);
                ui::EndMenu();
            }
            ui::EndMenu();
        }
        if (ui::BeginMenu("Help")) {
            ui::MenuItem("About ZeqouX3D");
            ui::EndMenu();
        }
        ui::EndMainMenuBar();
    }

    if (s.showDemoWindow) {
        ui::SetNextWindowPos(Vec2(20, 50));
        ui::SetNextWindowSize(Vec2(470, 760));
        if (ui::Begin(ZX_ICON_LAYOUT_DASHBOARD "  ZUI Widgets", &s.showDemoWindow)) {
            ui::Heading("Hello, ZeqouX3D!");
            ui::TextWrapped("ZeqouX3D's own UI: SDF primitives in a single draw call, a custom font rasterizer (Inter + Lucide), "
                            "crisp text and icons at any DPI scale.");
            ui::Spacing();
            if (ui::CollapsingHeader("Buttons", ui::TreeNodeFlags::DefaultOpen)) {
                if (ui::Button("Default"))
                    s.log += "Default button pressed\n";
                ui::SameLine();
                ui::Button(ZX_ICON_PLAY "  Play", Vec2::Zero(), ui::ButtonVariant::Primary);
                ui::SameLine();
                ui::Button("Ghost", Vec2::Zero(), ui::ButtonVariant::Ghost);
                ui::SameLine();
                if (ui::Button(ZX_ICON_TRASH_2 "  Delete", Vec2::Zero(), ui::ButtonVariant::Danger))
                    ui::OpenPopup("Confirm");
                s.deleteRect = ui::GetItemRect();
                ui::IconButton(ZX_ICON_MOVE, "Move (W)", true);
                ui::SameLine(0.0f, 2.0f);
                ui::IconButton(ZX_ICON_ROTATE_CW, "Rotate (E)");
                ui::SameLine(0.0f, 2.0f);
                ui::IconButton(ZX_ICON_SCALE_3D, "Scale (R)");
                ui::SameLine(0.0f, 2.0f);
                ui::IconButton(ZX_ICON_MAGNET, "Grid snapping");
                ui::SameLine(0.0f, 2.0f);
                ui::IconButton(ZX_ICON_GLOBE, "World space");
                ui::SameLine();
                ui::Button("Full width", Vec2(-1.0f, 0.0f));
                if (ui::BeginPopupModal("Confirm")) {
                    ui::Text("Delete the selected objects? This can be undone.");
                    ui::Spacing();
                    if (ui::Button("Delete", Vec2(120.0f, 0.0f), ui::ButtonVariant::Danger)) {
                        s.log += "Deleted (demo)\n";
                        ui::CloseCurrentPopup();
                    }
                    ui::SameLine();
                    if (ui::Button("Cancel", Vec2(120.0f, 0.0f)))
                        ui::CloseCurrentPopup();
                    ui::EndPopup();
                }
            }
            if (ui::CollapsingHeader("Toggles", ui::TreeNodeFlags::DefaultOpen)) {
                ui::Checkbox("Shadows", &s.checkA);
                ui::SameLine(200.0f);
                ui::Toggle("Fog", &s.toggleA);
                ui::Checkbox("Reflections", &s.checkB);
                ui::SameLine(200.0f);
                ui::Toggle("Bloom", &s.toggleB);
                ui::RadioButton("Lite", &s.radio, 0);
                ui::SameLine();
                ui::RadioButton("Medium", &s.radio, 1);
                ui::SameLine();
                ui::RadioButton("Full", &s.radio, 2);
            }
            if (ui::CollapsingHeader("Values", ui::TreeNodeFlags::DefaultOpen)) {
                ui::SliderFloat("Exposure", &s.exposure, -4.0f, 4.0f, "{:.2f} EV");
                ui::SliderFloat("Roughness", &s.roughness, 0.0f, 1.0f);
                ui::SliderInt("Samples", &s.samples, 1, 64);
                ui::DragFloat("Field of view", &s.fov, 0.2f, 20.0f, 120.0f, "{:.1f}°");
                ui::DragFloat3("Position", s.position, 0.01f);
                ui::DragFloat2("UV scale", s.scale2, 0.01f);
                ui::InputInt("Count", &s.count);
                ui::InputFloat("Speed", &s.speed, "{:.2f}");
                static const StringView Qualities[] = {"Low", "Medium", "High", "Epic"};
                ui::Combo("Quality", &s.quality, Span<const StringView>(Qualities, 4));
                s.comboRect = ui::GetItemRect();
                ui::ColorEdit("Tint", &s.tint, false);
                ui::ColorEdit("Fog", &s.fog, true);
                s.colorRect = ui::GetItemRect();
                ui::ItemTooltip("Click to open the color picker");
            }
            if (ui::CollapsingHeader("Text Fields", ui::TreeNodeFlags::DefaultOpen)) {
                ui::InputText("Name", s.name);
                ui::InputText("##search", s.search, ui::InputTextFlags::None, ZX_ICON_SEARCH "  Search assets…");
                ui::InputText("Password", s.password, ui::InputTextFlags::Password);
                ui::InputTextMultiline("##notes", s.notes, Vec2(0.0f, 90.0f));
            }
            if (ui::CollapsingHeader("Progress & Text")) {
                s.progress = Frac(s.progress + 0.002f);
                ui::ProgressBar(s.progress, Vec2(-1.0f, 0.0f));
                ui::Spinner();
                ui::SameLine();
                ui::AlignTextToFramePadding();
                ui::TextDisabled("Compiling shaders…");
                ui::BulletText("Bullet text");
                ui::TextColored(style.colors.success, ZX_ICON_CIRCLE_CHECK "  Build succeeded");
                ui::TextColored(style.colors.warning, ZX_ICON_TRIANGLE_ALERT "  3 warnings");
                ui::TextColored(style.colors.error, ZX_ICON_CIRCLE_X "  Error: shader not found");
                ui::LabelText("Vertices", "262,267");
                ui::LabelText("Triangles", "1,179,344");
                if (ui::TextLink("ZUI documentation"))
                    s.log += "Link clicked\n";
                ui::SeparatorText("Unicode coverage");
                ui::Text("αβγδ ΩΣΦ — Ærøskøbing, Łódź, Ἀθῆναι");
                ui::PushFont(ui::FontStyle::Italic);
                ui::Text("Italic: The five boxing wizards jump quickly");
                ui::PopFont();
                ui::PushFont(ui::FontStyle::Bold);
                ui::Text("Bold: The quick brown fox jumps over the lazy dog");
                ui::PopFont();
            }
            if (ui::BeginPopupContextWindow()) {
                ui::MenuItem("Reset values");
                ui::MenuItem("Copy as JSON");
                ui::EndPopup();
            }
        }
        ui::End();
    }

    if (s.showTree) {
        ui::SetNextWindowPos(Vec2(510, 50));
        ui::SetNextWindowSize(Vec2(320, 380));
        if (ui::Begin(ZX_ICON_LIST_TREE "  Hierarchy", &s.showTree)) {
            ui::SetNextItemWidth(-1.0f);
            ui::InputText("##filter", s.search, ui::InputTextFlags::None, ZX_ICON_SEARCH "  Filter");
            if (ui::BeginChild("tree", Vec2(0.0f, -110.0f), true)) {
                TreeRecursive(s, -1);
            }
            ui::EndChild();
            ui::TextDisabled("Log:");
            if (ui::BeginChild("log", Vec2::Zero(), true))
                ui::TextWrapped(s.log.Empty() ? StringView("(empty)") : s.log.View());
            ui::EndChild();
        }
        ui::End();
    }

    if (s.showTable) {
        ui::SetNextWindowPos(Vec2(510, 450));
        ui::SetNextWindowSize(Vec2(620, 360));
        if (ui::Begin(ZX_ICON_TABLE "  Assets (10,000 rows, virtualized)", &s.showTable)) {
            if (ui::BeginTable("assets", 4, ui::TableFlags::Borders | ui::TableFlags::RowBg | ui::TableFlags::Resizable |
                                                 ui::TableFlags::Sortable | ui::TableFlags::ScrollY,
                               Vec2(0.0f, -1.0f))) {
                ui::TableSetupColumn("Name");
                ui::TableSetupColumn("Type", 100.0f);
                ui::TableSetupColumn("Size, MB", 100.0f);
                ui::TableSetupColumn("Refs", 80.0f);
                ui::TableHeadersRow();
                const ui::TableSortSpec sort = ui::TableGetSortSpec();
                if (sort.dirty) {
                    const bool asc = sort.ascending;
                    switch (sort.column) {
                    case 0: StableSort(s.assets, [asc](const Asset& a, const Asset& b) { return asc ? a.name < b.name : b.name < a.name; }); break;
                    case 1: StableSort(s.assets, [asc](const Asset& a, const Asset& b) { return asc ? a.type < b.type : b.type < a.type; }); break;
                    case 2: StableSort(s.assets, [asc](const Asset& a, const Asset& b) { return asc ? a.sizeMb < b.sizeMb : b.sizeMb < a.sizeMb; }); break;
                    default: StableSort(s.assets, [asc](const Asset& a, const Asset& b) { return asc ? a.refs < b.refs : b.refs < a.refs; }); break;
                    }
                }
                ui::ListClipper clipper(u32(s.assets.Size()), 0.0f); // 0 in a table = default row height
                for (u32 i = clipper.begin; i < clipper.end; ++i) {
                    const Asset& a = s.assets[i];
                    ui::TableNextRow();
                    ui::TableNextColumn();
                    ui::PushID(i32(i));
                    if (ui::Selectable(a.name, s.selectedAsset == i32(i)))
                        s.selectedAsset = i32(i);
                    ui::PopID();
                    ui::TableNextColumn();
                    ui::TextDisabled(a.type);
                    ui::TableNextColumn();
                    ui::Textf("{:.2f}", a.sizeMb);
                    ui::TableNextColumn();
                    ui::Textf("{}", a.refs);
                }
                clipper.End();
                ui::EndTable();
            }
        }
        ui::End();
    }

    if (s.showTabs) {
        ui::SetNextWindowPos(Vec2(850, 50));
        ui::SetNextWindowSize(Vec2(380, 380));
        if (ui::Begin(ZX_ICON_PANELS_TOP_LEFT "  Tabs & Color", &s.showTabs)) {
            if (ui::BeginTabBar("tabs")) {
                if (ui::BeginTabItem("Palette", &s.tabOpen[0])) {
                    ui::ColorPicker("##pick", &s.tint, false);
                    ui::EndTabItem();
                }
                if (ui::BeginTabItem("Light", &s.tabOpen[1])) {
                    ui::SliderFloat("Intensity", &s.exposure, -4.0f, 4.0f);
                    ui::Checkbox("Cast shadows", &s.checkA);
                    ui::EndTabItem();
                }
                if (ui::BeginTabItem("Camera", &s.tabOpen[2])) {
                    ui::DragFloat("FOV", &s.fov, 0.2f, 20.0f, 120.0f);
                    ui::EndTabItem();
                }
                if (ui::BeginTabItem("Pinned")) {
                    ui::TextWrapped("Tabs can be closed with the cross; this one is pinned.");
                    if (ui::Button("Reopen closed tabs"))
                        for (bool& b : s.tabOpen)
                            b = true;
                    ui::EndTabItem();
                }
                ui::EndTabBar();
            }
        }
        ui::End();
    }

    if (s.showStats) {
        ui::SetNextWindowPos(Vec2(1150, 450));
        ui::SetNextWindowSize(Vec2(250, 200));
        if (ui::Begin(ZX_ICON_GAUGE "  Statistics", &s.showStats)) {
            ui::LabelText("Frame", String::Format("{:.2f} ms", frameMs));
            ui::LabelText("GPU (UI)", String::Format("{:.3f} ms", gpuMs));
            ui::LabelText("Primitives", String::Format("{}", uiStats.primitives));
            ui::LabelText("Clips", String::Format("{}", uiStats.clips));
            ui::LabelText("Overdraw", String::Format("{:.2f}x", uiStats.coveredPixels / Max(display.x * display.y, 1.0f)));
            ui::LabelText("Cached glyphs", String::Format("{}", ctx.Fonts().GlyphCacheSize()));
            ui::LabelText("Scale", String::Format("{}%", i32(ctx.GetScale() * 100.0f + 0.5f)));
        }
        ui::End();
    }
}

int Run()
{
    WindowDesc wd;
    wd.title = "ZeqouX3D - UI Demo";
    wd.width = 1440;
    wd.height = 860;
    StringView resolution;
    if (CommandLine::GetValue("resolution", resolution)) {
        const usize x = resolution.Find('x');
        if (x != StringView::NPos) {
            wd.width = i32(ParseInt(resolution.Left(x)).ValueOr(wd.width));
            wd.height = i32(ParseInt(resolution.DropLeft(x + 1)).ValueOr(wd.height));
        }
    }
    UniquePtr<Window> window(Window::Create(wd));
    if (!window)
        return 1;
    DeviceDesc dd;
    dd.enableDebugLayer = ZX_BUILD_DEBUG || CommandLine::HasFlag("debuglayer");
    dd.useWarp = CommandLine::HasFlag("warp");
    UniquePtr<Device> device = d3d12::CreateD3D12Device(dd);
    if (!device) {
        platform::ShowErrorMessageBox("ZeqouX3D", "Failed to create a Direct3D 12 device.");
        return 1;
    }
    SelectEngineProfile(device->GetCaps());
    UVec2 size = window->GetClientSize();
    UniquePtr<SwapChain> swapChain = device->CreateSwapChain(
        SwapChainDesc{window->GetNativeHandle(), size.x, size.y, 3, Format::R8G8B8A8_UNORM, !CommandLine::HasFlag("novsync")});
    if (!swapChain)
        return 1;

    ui::FontSystem fonts;
    ui::Context ctx(fonts);
    if (!ctx.LoadDefaultFonts()) {
        platform::ShowErrorMessageBox("ZeqouX3D", "Failed to load UI fonts (Engine/Content/Fonts).");
        return 1;
    }
    ui::SetCurrentContext(&ctx);
    if (CommandLine::HasFlag("light"))
        ctx.GetStyle().colors = ui::Theme::Light();
    StringView scaleValue;
    if (CommandLine::GetValue("scale", scaleValue))
        ctx.SetUserScale(ParseFloat(scaleValue).ValueOr(1.0f));
    ui::UIRenderer uiRenderer(*device);
    if (!uiRenderer.Init(swapChain->GetFormat()))
        return 1;

    DemoState demo;
    InitDemo(demo);

    StringView autotestValue, screenshotPath;
    const bool autotest = CommandLine::HasFlag("autotest") || CommandLine::GetValue("autotest", autotestValue);
    const u64 autotestFrames = autotestValue.Empty() ? 120 : u64(Max<i64>(2, ParseInt(autotestValue).ValueOr(120)));
    const bool takeScreenshot = CommandLine::GetValue("screenshot", screenshotPath);
    StringView scenario;
    CommandLine::GetValue("scenario", scenario);
    const u64 screenshotFrame = autotest ? autotestFrames - 1 : 60;
    Buffer* screenshotBuffer = nullptr;
    u64 screenshotPitch = 0;
    UVec2 screenshotSize;

    InputState input;
    Array<WindowEvent> events;
    FrameTimer timer;
    u64 frames = 0;
    f64 titleTimer = 0.0;
    f32 gpuUiMs = 0.0f;
    bool quit = false;
    f64 cpuUiMsSum = 0.0, gpuUiMsSum = 0.0;
    u64 gpuUiSamples = 0;
    u64 cpuUiSamples = 0;

    while (!window->ShouldClose() && !device->IsDeviceLost() && !quit) {
        if (!platform::PumpMessages())
            break;
        profiler::BeginFrame();
        const f32 dt = timer.Tick();
        events.Clear();
        platform::UpdateInput(*window, input, events);
        bool resized = false;
        for (const WindowEvent& e : events) {
            if (e.type == WindowEventType::Resize)
                resized = true;
            if (e.type == WindowEventType::KeyDown && !e.repeat && !ctx.WantsKeyboard()) {
                if (e.key == Key::F11)
                    window->SetMode(window->GetMode() == WindowMode::Windowed ? WindowMode::BorderlessFullscreen : WindowMode::Windowed);
                if (e.key == Key::Num1)
                    ctx.SetUserScale(1.0f);
                if (e.key == Key::Num2)
                    ctx.SetUserScale(1.25f);
                if (e.key == Key::Num3)
                    ctx.SetUserScale(1.5f);
                if ((e.key == Key::Equals || e.key == Key::KeypadAdd) && HasAnyFlags(e.modifiers, KeyModifiers::Control))
                    ctx.SetUserScale(ctx.GetUserScale() + 0.125f);
                if ((e.key == Key::Minus || e.key == Key::KeypadSubtract) && HasAnyFlags(e.modifiers, KeyModifiers::Control))
                    ctx.SetUserScale(ctx.GetUserScale() - 0.125f);
                if (e.key == Key::Escape && !ctx.WantsMouse())
                    quit = true;
            }
        }
        const UVec2 clientSize = window->GetClientSize();
        if (clientSize.x == 0 || clientSize.y == 0 || window->IsMinimized()) {
            Thread::SleepMs(16);
            continue;
        }
        if (resized || clientSize != swapChain->GetSize())
            swapChain->Resize(clientSize.x, clientSize.y);
        const UVec2 sz = swapChain->GetSize();

        // Scripted input for visual checks: click an item at frame 20, optionally hover another one afterwards.
        if (!scenario.Empty() && frames >= 18) {
            Rect click, hover;
            if (scenario == "menu") {
                click = demo.viewMenuRect;
                hover = demo.scaleMenuRect;
            } else if (scenario == "combo") {
                click = demo.comboRect;
            } else if (scenario == "color") {
                click = Rect(demo.colorRect.min, demo.colorRect.min + Vec2(20.0f, demo.colorRect.Height()));
            } else if (scenario == "modal") {
                click = demo.deleteRect;
            } else if (scenario == "tooltip") {
                hover = demo.treeItemRect;
            }
            if (!click.IsEmpty() && frames < 24) {
                input.SetMousePosition(click.Center());
                input.SetMouseButton(MouseButton::Left, frames == 20 || frames == 21);
            } else if (!hover.IsEmpty()) {
                input.SetMousePosition(hover.Center());
            }
        }
        // --- UI frame ---
        const u64 uiStart = Clock::NowTicks();
        ui::FrameInput fi;
        fi.displaySize = Vec2(f32(sz.x), f32(sz.y));
        fi.deltaTime = dt;
        fi.dpiScale = window->GetDpiScale();
        fi.input = &input;
        fi.events = events.View();
        fi.hasFocus = window->IsFocused() || autotest;
        ctx.BeginFrame(fi);
        DrawDemo(demo, ctx, quit, timer.SmoothedDeltaSeconds() * 1000.0f, gpuUiMs, uiRenderer.GetStats(), fi.displaySize);
        ctx.EndFrame();
        window->SetCursorShape(ctx.GetCursorShape());
        if (frames > 10) {
            cpuUiMsSum += Clock::TicksToMilliseconds(Clock::NowTicks() - uiStart);
            ++cpuUiSamples;
        }

        device->BeginFrame();
        CommandList* cmd = device->BeginCommandList();
        cmd->BeginTimer("Frame");
        Texture* backBuffer = swapChain->GetCurrentBackBuffer();
        uiRenderer.Prepare(*cmd, fonts, ctx.GetDrawLists());
        // Background: a soft vertical gradient stands in for the 3D viewport.
        const ColorAttachment color{backBuffer, LoadOp::Clear, StoreOp::Store, LinearColor(0.012f, 0.014f, 0.02f, 1)};
        cmd->BeginRenderPass(Span<const ColorAttachment>(&color, 1));
        cmd->BeginTimer("UI");
        uiRenderer.Draw(*cmd, sz);
        cmd->EndTimer();
        cmd->EndRenderPass();
        if (takeScreenshot && frames == screenshotFrame && !screenshotBuffer) {
            screenshotSize = sz;
            screenshotBuffer = device->CreateBuffer(
                BufferDesc{AlignUp(u64(sz.x) * 4, u64(256)) * sz.y, BufferUsage::CopyDest, MemoryType::Readback, 0, "Screenshot"});
            screenshotPitch = cmd->CopyTextureToBuffer(screenshotBuffer, 0, backBuffer);
        }
        cmd->Barrier(backBuffer, ResourceState::Present);
        cmd->EndTimer();
        device->Submit(cmd);
        swapChain->Present();
        device->EndFrame();
        for (const GpuTiming& t : device->GetGpuTimings())
            if (StringView(t.name) == "UI") {
                gpuUiMs = f32(t.milliseconds);
                if (frames > 10) {
                    gpuUiMsSum += t.milliseconds;
                    ++gpuUiSamples;
                }
            }

        titleTimer += dt;
        if (titleTimer > 0.5) {
            titleTimer = 0.0;
            window->SetTitle(String::Format("ZeqouX3D - UI Demo | {}x{} @ {}% | CPU {:.2f} ms | UI GPU {:.3f} ms | {} prims", sz.x, sz.y,
                                            i32(ctx.GetScale() * 100.0f + 0.5f), timer.SmoothedDeltaSeconds() * 1000.0f, gpuUiMs,
                                            uiRenderer.GetStats().primitives));
        }
        ++frames;
        if (autotest && frames >= autotestFrames)
            window->RequestClose();
    }

    device->WaitIdle();
    if (screenshotBuffer) {
        const bool ok = png::WriteFile(screenshotPath, static_cast<const u8*>(screenshotBuffer->GetMappedData()), screenshotSize.x,
                                       screenshotSize.y, 4, usize(screenshotPitch));
        ZX_LOG_INFO(LogUIDemo, "Screenshot {} -> {}", ok ? "saved" : "FAILED", screenshotPath);
        device->Destroy(screenshotBuffer);
    }
    if (cpuUiSamples)
        ZX_LOG_INFO(LogUIDemo, "UI build (CPU) {:.3f} ms avg, GPU {:.3f} ms avg, {} primitives, overdraw {:.2f}x, {} glyphs cached, {} atlas evictions",
                    cpuUiMsSum / f64(cpuUiSamples), gpuUiMsSum / f64(Max<u64>(gpuUiSamples, 1)), uiRenderer.GetStats().primitives,
                    uiRenderer.GetStats().coveredPixels / f32(Max(1u, swapChain->GetSize().x * swapChain->GetSize().y)), fonts.GlyphCacheSize(), fonts.Evictions());
    const u32 debugErrors = d3d12::GetDebugLayerErrorCount(*device);
    ui::SetCurrentContext(nullptr);
    ZX_LOG_INFO(LogUIDemo, "Exited after {} frames, debug layer errors: {}", frames, debugErrors);
    return debugErrors == 0 ? 0 : 3;
}

} // namespace

int main()
{
    if (!CoreInit(CoreInitParams{.appName = "UIDemo"}))
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
