#pragma once

// ZUI immediate-mode API (editor / tools). Own implementation in the spirit of Dear ImGui.
//
//   ui::Context ctx(fonts);            // once; ctx.LoadDefaultFonts();
//   ui::SetCurrentContext(&ctx);
//   per frame:
//     ctx.BeginFrame(frameInput);
//     if (ui::Begin("Inspector")) {
//         ui::Text("Hello, world!");
//         if (ui::Button(ZX_ICON_SAVE " Save")) Save();
//         ui::SliderFloat("Exposure", &exposure, -4.0f, 4.0f);
//     }
//     ui::End();
//     ctx.EndFrame();
//     uiRenderer.Prepare(*cmd, fonts, ctx.GetDrawLists());  ...  uiRenderer.Draw(...)
//
// Conventions: labels "Text##id" display "Text" and hash the whole string; "##id" hides the label. All sizes in the
// API are logical pixels (multiplied by the DPI/user scale internally); positions returned by queries are physical
// pixels of the render target.

#include "Core/Containers/Span.h"
#include "Core/Math/Color.h"
#include "Core/Math/Geometry.h"
#include "Core/Memory/Pointers.h"
#include "Platform/Input.h"
#include "Platform/Window.h"
#include "UI/DrawList.h"
#include "UI/Font.h"
#include "UI/Icons.h"

namespace zx::ui {

using ID = u32;
class Context;

// --- Style ---------------------------------------------------------------------------------------------------------

struct Theme
{
    Color windowBg = Color(0x26, 0x26, 0x26);
    Color childBg = Color(0x1F, 0x1F, 0x1F);
    Color popupBg = Color(0x2C, 0x2C, 0x2C);
    Color titleBg = Color(0x15, 0x15, 0x15);
    Color titleBgActive = Color(0x1A, 0x1A, 0x1A);
    Color menuBarBg = Color(0x1A, 0x1A, 0x1A);
    Color border = Color(0x3A, 0x3A, 0x3A);
    Color borderStrong = Color(0x50, 0x50, 0x50);
    Color shadow = Color(0, 0, 0, 110);
    Color text = Color(0xD6, 0xD6, 0xD6);
    Color textDim = Color(0x92, 0x92, 0x92);
    Color textDisabled = Color(0x5E, 0x5E, 0x5E);
    Color frameBg = Color(0x18, 0x18, 0x18);
    Color frameBgHovered = Color(0x1E, 0x1E, 0x1E);
    Color frameBgActive = Color(0x14, 0x14, 0x14);
    Color button = Color(0x3A, 0x3A, 0x3A);
    Color buttonHovered = Color(0x48, 0x48, 0x48);
    Color buttonActive = Color(0x2E, 0x2E, 0x2E);
    Color accent = Color(0x26, 0x7A, 0xE8);
    Color accentHovered = Color(0x3B, 0x8C, 0xF5);
    Color accentActive = Color(0x1D, 0x66, 0xC6);
    Color accentText = Color(0xFF, 0xFF, 0xFF);
    Color header = Color(0x30, 0x30, 0x30);
    Color headerHovered = Color(0x3A, 0x3A, 0x3A);
    Color headerActive = Color(0x44, 0x44, 0x44);
    Color selection = Color(0x1D, 0x4F, 0x91);
    Color selectionInactive = Color(0x3A, 0x3A, 0x3A);
    Color textSelection = Color(0x4C, 0x8D, 0xF6, 110);
    Color separator = Color(0x33, 0x33, 0x33);
    Color scrollbarGrab = Color(0x48, 0x48, 0x48);
    Color scrollbarGrabHovered = Color(0x5A, 0x5A, 0x5A);
    Color scrollbarGrabActive = Color(0x6A, 0x6A, 0x6A);
    Color tableHeaderBg = Color(0x2C, 0x2C, 0x2C);
    Color tableRowAlt = Color(0xFF, 0xFF, 0xFF, 7);
    Color tableBorder = Color(0x30, 0x30, 0x30);
    Color tabBg = Color(0x1A, 0x1A, 0x1A);
    Color tabHovered = Color(0x2C, 0x2C, 0x2C);
    Color tabActive = Color(0x26, 0x26, 0x26);
    Color modalDim = Color(0, 0, 0, 120);
    Color tooltipBg = Color(0x30, 0x30, 0x30);
    Color success = Color(0x4C, 0xC3, 0x7A);
    Color warning = Color(0xF0, 0xB4, 0x3C);
    Color error = Color(0xF0, 0x5A, 0x5A);
    Color focusRing = Color(0x4C, 0x8D, 0xF6, 160);

    static Theme Dark() { return Theme(); }
    static Theme Light();
};

struct Style
{
    f32 fontSize = 13.5f;
    f32 smallFontSize = 12.0f;
    f32 headingFontSize = 17.0f;
    Vec2 windowPadding = Vec2(10.0f, 10.0f);
    Vec2 framePadding = Vec2(8.0f, 4.0f);
    Vec2 itemSpacing = Vec2(8.0f, 6.0f);
    f32 itemInnerSpacing = 6.0f;
    f32 indentSpacing = 16.0f;
    f32 frameRounding = 4.0f;
    f32 windowRounding = 8.0f;
    f32 popupRounding = 6.0f;
    f32 scrollbarSize = 10.0f;
    f32 grabMinSize = 18.0f;
    f32 titleBarHeight = 30.0f;
    f32 defaultItemWidthRatio = 0.62f; // of the available width (rest for the label)
    f32 tooltipDelay = 0.45f;
    Theme colors;
};

// --- Flags ---------------------------------------------------------------------------------------------------------

enum class WindowFlags : u32
{
    None = 0,
    NoTitleBar = 1 << 0,
    NoResize = 1 << 1,
    NoMove = 1 << 2,
    NoScrollbar = 1 << 3,
    NoBackground = 1 << 4,
    MenuBar = 1 << 5,
    NoCollapse = 1 << 6,
    AlwaysAutoResize = 1 << 7,
    NoBringToFront = 1 << 8,
    /// Fills the whole viewport (host panel); implies NoTitleBar | NoResize | NoMove.
    Fullscreen = 1 << 9,
};
ZX_ENUM_FLAGS(WindowFlags)

enum class Cond : u8
{
    Always,
    FirstUse, // only when the window/item has no saved state yet
};

enum class TreeNodeFlags : u32
{
    None = 0,
    DefaultOpen = 1 << 0,
    Leaf = 1 << 1,
    Selected = 1 << 2,
    OpenOnArrow = 1 << 3, // clicking the label selects, only the arrow toggles
    Framed = 1 << 4,      // collapsing-header look
    NoTreePushOnOpen = 1 << 5,
};
ZX_ENUM_FLAGS(TreeNodeFlags)

enum class InputTextFlags : u32
{
    None = 0,
    Multiline = 1 << 0,
    ReadOnly = 1 << 1,
    Password = 1 << 2,
    CharsDecimal = 1 << 3,
    EnterReturnsTrue = 1 << 4, // return true only on Enter (otherwise on every edit)
    AutoSelectAll = 1 << 5,
    KeepFocusOnEnter = 1 << 6, // with EnterReturnsTrue: stay active after Enter (console prompts)
};
ZX_ENUM_FLAGS(InputTextFlags)

enum class TableFlags : u32
{
    None = 0,
    Borders = 1 << 0,
    RowBg = 1 << 1,
    Resizable = 1 << 2,
    Sortable = 1 << 3,
    ScrollY = 1 << 4, // fixed outer height with a scrolling body (header stays visible)
};
ZX_ENUM_FLAGS(TableFlags)

enum class ButtonVariant : u8
{
    Default,
    Primary, // accent color
    Ghost,   // no background until hovered
    Danger,
};

struct TableSortSpec
{
    i32 column = -1;
    bool ascending = true;
    bool dirty = false; // true for one frame after the user changed the sort
};

// --- Frame input -----------------------------------------------------------------------------------------------------

struct FrameInput
{
    Vec2 displaySize;                    // render target size in pixels
    f32 deltaTime = 1.0f / 60.0f;
    f32 dpiScale = 1.0f;                 // window DPI scale (96 DPI = 1)
    const InputState* input = nullptr;   // mouse/keys (positions in pixels)
    Span<const WindowEvent> events;      // key repeats and typed characters
    bool hasFocus = true;
};

// --- Context -----------------------------------------------------------------------------------------------------------

class Context
{
public:
    explicit Context(FontSystem& fonts);
    ~Context();
    ZX_NON_COPYABLE(Context)

    /// Inter (regular/medium/semibold/bold/italic, Latin+Cyrillic+Greek fallback chain) + Lucide icons.
    /// `fontsRoot` holds Inter/ and Lucide/ (virtual or physical path).
    bool LoadDefaultFonts(StringView fontsRoot = "/Engine/Fonts");

    void BeginFrame(const FrameInput& input);
    void EndFrame();
    /// Draw lists of this frame in back-to-front order (valid until the next BeginFrame).
    Span<const DrawList* const> GetDrawLists() const;

    Style& GetStyle() { return m_style; }
    /// Extra zoom on top of the DPI scale (Ctrl+= / Ctrl+- in the editor).
    void SetUserScale(f32 scale) { m_userScale = Clamp(scale, 0.5f, 3.0f); }
    f32 GetUserScale() const { return m_userScale; }
    f32 GetScale() const;

    bool WantsMouse() const;    // the UI is hovered or captures the mouse: the app should ignore mouse input
    bool WantsKeyboard() const; // a text field or widget has keyboard focus
    bool IsAnyItemActive() const; // a widget is being dragged/held or a text field is being edited
    CursorShape GetCursorShape() const;

    FontSystem& Fonts() { return m_fonts; }
    struct Impl;
    Impl& GetImpl() { return *m_impl; }

private:
    FontSystem& m_fonts;
    Style m_style;
    f32 m_userScale = 1.0f;
    UniquePtr<Impl> m_impl;
};

void SetCurrentContext(Context* ctx);
Context* GetCurrentContext();

enum class FontStyle : u8
{
    Regular,
    Medium,
    SemiBold,
    Bold,
    Italic,
    Count
};

// --- Windows -----------------------------------------------------------------------------------------------------------

void SetNextWindowPos(Vec2 logicalPos, Cond cond = Cond::FirstUse);
void SetNextWindowSize(Vec2 logicalSize, Cond cond = Cond::FirstUse);
/// Returns false when the window is collapsed or clipped (End() must be called anyway).
bool Begin(StringView title, bool* open = nullptr, WindowFlags flags = WindowFlags::None);
void End();
/// Scrollable sub-region. size: 0 = fill the available width/height.
bool BeginChild(StringView id, Vec2 size = Vec2::Zero(), bool border = false);
void EndChild();

// --- Docking -------------------------------------------------------------------------------------------------------
//
//   const ui::ID root = ui::DockSpace("Main", dockRect);        // every frame, before the docked windows
//   if (!ui::DockSpaceHasLayout("Main")) {                         // first run: build a default layout
//       ui::ID left;  const ui::ID right = ui::DockSplitNode(root, ui::DockDir::Right, 0.25f, &left);
//       ui::DockWindow("Viewport", left);  ui::DockWindow("Details", right);
//   }
// Tabs can be dragged out (floating window) and floating windows dropped onto a node (center = tab, sides = split).

enum class DockDir : u8
{
    Left,
    Right,
    Up,
    Down,
};

/// Dock host covering `rect` (physical px). Returns its root node id.
ID DockSpace(StringView id, const Rect& rect);
bool DockSpaceHasLayout(StringView id);
/// Splits leaf `node`: returns the new node on `dir` taking `ratio` of the size; `remaining` receives the other part
/// (which keeps the node's tabs).
ID DockSplitNode(ID node, DockDir dir, f32 ratio, ID* remaining = nullptr);
/// Docks a window (by title) as a tab of `node`.
void DockWindow(StringView windowTitle, ID node);
/// Selects a docked window's tab.
void FocusDockedWindow(StringView windowTitle);
/// Docks `windowTitle` as a tab next to `otherTitle` unless it is docked already (no-op if the other isn't docked).
bool DockWindowWith(StringView windowTitle, StringView otherTitle);
/// Serialized docking layout (text) for all dock spaces; LoadDockLayout replaces the current one.
String SaveDockLayout();
bool LoadDockLayout(StringView text);

Rect GetWindowRect();
/// Content area of the current window (physical px, scroll not applied).
Rect GetWindowContentRect();
Vec2 GetContentRegionAvail();
DrawList& GetWindowDrawList();
/// Draw list above all windows (debug overlays, drag previews).
DrawList& GetForegroundDrawList();
void SetScrollHereY(f32 ratio = 0.5f);

// --- Layout ------------------------------------------------------------------------------------------------------------

void SameLine(f32 offsetFromStart = 0.0f, f32 spacing = -1.0f);
void NewLine();
void Spacing();
void Separator();
/// Separator with a caption ("—— Lighting ——").
void SeparatorText(StringView text);
void Dummy(Vec2 size);
void Indent(f32 amount = 0.0f);
void Unindent(f32 amount = 0.0f);
void SetNextItemWidth(f32 width); // logical px; negative = available width minus |width|
/// Makes the current line frame-height tall so following Text() is centered against buttons/fields.
void AlignTextToFramePadding();
void BeginGroup();
void EndGroup();
Vec2 GetCursorPos();              // physical pixels
void SetCursorPos(Vec2 pos);
f32 GetFrameHeight();             // physical pixels
f32 GetTextLineHeight();

// --- IDs ---------------------------------------------------------------------------------------------------------------

void PushID(StringView id);
void PushID(i32 id);
void PushID(const void* ptr);
void PopID();
ID GetID(StringView label);

// --- Fonts & colors ------------------------------------------------------------------------------------------------------

void PushFont(FontStyle style, f32 sizeLogical = 0.0f);
/// A font of the FontSystem by handle (e.g. a monospace face for code); size 0 = current size.
void PushFont(FontHandle font, f32 sizeLogical = 0.0f);
void PopFont();
void PushTextColor(Color color);
void PopTextColor();
void BeginDisabled(bool disabled = true);
void EndDisabled();

// --- Items: queries -----------------------------------------------------------------------------------------------------

bool IsItemHovered();
bool IsItemActive();
bool IsItemClicked(MouseButton button = MouseButton::Left);
bool IsItemDoubleClicked();
bool IsItemEdited();      // value changed this frame
bool IsItemDeactivated(); // was active, released this frame (end of a drag/edit)
Rect GetItemRect();
bool IsWindowHovered();
bool IsMouseDoubleClicked(MouseButton button = MouseButton::Left);
Vec2 GetMousePos();
bool IsKeyPressed(Key key, bool repeat = true);
/// Shortcut helper: Ctrl/Shift/Alt modifiers must match exactly.
bool IsShortcutPressed(KeyModifiers mods, Key key);

// --- Widgets: text -------------------------------------------------------------------------------------------------------

void Text(StringView text);
template <typename... Args>
void Textf(StringView fmt, const Args&... args)
{
    Text(String::Format(fmt, args...));
}
void TextColored(Color color, StringView text);
void TextDisabled(StringView text);
void TextWrapped(StringView text);
void Heading(StringView text);
void BulletText(StringView text);
/// "label    value" row (read-only property).
void LabelText(StringView label, StringView value);
bool TextLink(StringView text);

// --- Widgets: buttons & toggles ------------------------------------------------------------------------------------------

bool Button(StringView label, Vec2 size = Vec2::Zero(), ButtonVariant variant = ButtonVariant::Default);
/// Square icon button (an icon codepoint string from Icons.h); tooltip shown on hover.
bool IconButton(StringView icon, StringView tooltip = {}, bool selected = false);
bool InvisibleButton(StringView id, Vec2 size);
bool Checkbox(StringView label, bool* value);
bool Toggle(StringView label, bool* value);
bool RadioButton(StringView label, bool active);
bool RadioButton(StringView label, i32* value, i32 buttonValue);
bool Selectable(StringView label, bool selected = false, Vec2 size = Vec2::Zero());
void ProgressBar(f32 fraction, Vec2 size = Vec2(-1.0f, 0.0f), StringView overlay = {});
void Spinner(f32 radius = 8.0f);

// --- Widgets: values -----------------------------------------------------------------------------------------------------

bool SliderFloat(StringView label, f32* value, f32 min, f32 max, StringView format = "{:.3f}");
bool SliderInt(StringView label, i32* value, i32 min, i32 max);
/// Drag horizontally to change; double-click or Ctrl+click to type a value.
bool DragFloat(StringView label, f32* value, f32 speed = 0.01f, f32 min = 0.0f, f32 max = 0.0f, StringView format = "{:.3f}");
bool DragFloat2(StringView label, f32 value[2], f32 speed = 0.01f, f32 min = 0.0f, f32 max = 0.0f);
bool DragFloat3(StringView label, f32 value[3], f32 speed = 0.01f, f32 min = 0.0f, f32 max = 0.0f);
bool DragInt(StringView label, i32* value, f32 speed = 0.1f, i32 min = 0, i32 max = 0);
bool InputText(StringView label, String& text, InputTextFlags flags = InputTextFlags::None, StringView hint = {});
/// Moves the caret of the active text field to the end of its text (after the app replaced the text, e.g. history).
void SetActiveTextCursorToEnd();
/// The next InputText takes keyboard focus (e.g. the search field of a palette that just opened).
void SetKeyboardFocusHere();
bool InputTextMultiline(StringView label, String& text, Vec2 size = Vec2::Zero(), InputTextFlags flags = InputTextFlags::None);

/// Syntax coloring for InputTextCode: writes one color per byte of `line`. `state` carries multi-line constructs
/// (block comments) from one line to the next; it starts at 0.
using CodeColorizer = void (*)(StringView line, Color* colors, u32& state);

struct CodeEditorOptions
{
    CodeColorizer colorizer = nullptr;
    bool lineNumbers = true;
    u32 errorLine = 0;     // 1-based line marked as an error (0 = none)
    u32 scrollToLine = 0;  // 1-based: scroll so this line is visible (one-shot; reset it after the call)
};

/// Multi-line code editor: line numbers, syntax colors, auto-indent, Tab / Shift+Tab on selections, error line marker.
/// Uses the current font (push a monospace one). Size 0 = fill the available space.
bool InputTextCode(StringView label, String& text, Vec2 size, const CodeEditorOptions& options);
bool InputFloat(StringView label, f32* value, StringView format = "{:.3f}");
bool InputInt(StringView label, i32* value);
bool Combo(StringView label, i32* current, Span<const StringView> items);
/// Color swatch + popup picker (HSV square, hue bar, alpha, hex). Edits linear color, shown in sRGB.
bool ColorEdit(StringView label, LinearColor* color, bool alpha = true);
bool ColorPicker(StringView label, LinearColor* color, bool alpha = true);
void ColorSwatch(Color color, Vec2 size);

// --- Trees, headers, tabs -------------------------------------------------------------------------------------------------

bool TreeNode(StringView label, TreeNodeFlags flags = TreeNodeFlags::None);
void TreePop();
bool CollapsingHeader(StringView label, TreeNodeFlags flags = TreeNodeFlags::None);
void SetNextItemOpen(bool open, Cond cond = Cond::Always);
bool BeginTabBar(StringView id);
void EndTabBar();
bool BeginTabItem(StringView label, bool* open = nullptr);
void EndTabItem();

// --- Tables ---------------------------------------------------------------------------------------------------------------

bool BeginTable(StringView id, u32 columns, TableFlags flags = TableFlags::Borders | TableFlags::RowBg | TableFlags::Resizable,
                Vec2 outerSize = Vec2::Zero());
/// width 0 = stretch (weight 1).
void TableSetupColumn(StringView label, f32 width = 0.0f);
void TableHeadersRow();
void TableNextRow(f32 minHeight = 0.0f);
bool TableNextColumn();
bool TableSetColumnIndex(u32 column);
TableSortSpec TableGetSortSpec();
void EndTable();

/// Virtualized list: only the visible items are submitted.
///   ui::ListClipper clip(count, itemHeight);  for (u32 i = clip.begin; i < clip.end; ++i) ...;  clip.End();
struct ListClipper
{
    u32 begin = 0;
    u32 end = 0;
    ListClipper(u32 itemCount, f32 itemHeightPixels);
    void End();

private:
    u32 m_count;
    f32 m_itemHeight;
    f32 m_step = 0.0f;
    f32 m_startY = 0.0f;
};

// --- Popups, menus, tooltips ----------------------------------------------------------------------------------------------

void OpenPopup(StringView id);
bool BeginPopup(StringView id);
bool BeginPopupModal(StringView title, bool* open = nullptr);
/// Opens on right-click over the last item (id empty = derived from the item).
bool BeginPopupContextItem(StringView id = {});
bool BeginPopupContextWindow(StringView id = "##WindowContext");
void EndPopup();
void CloseCurrentPopup();
bool IsPopupOpen(StringView id);

bool BeginMainMenuBar();
void EndMainMenuBar();
bool BeginMenuBar();
/// Menu bar laid out inside `bar` (physical px) of the current window, e.g. inside a custom title bar.
bool BeginMenuBarInRect(const Rect& bar);
void EndMenuBar();
bool BeginMenu(StringView label, bool enabled = true);
void EndMenu();
bool MenuItem(StringView label, StringView shortcut = {}, bool selected = false, bool enabled = true);
bool MenuItem(StringView label, StringView shortcut, bool* selected, bool enabled = true);

void SetTooltip(StringView text);
/// Shows `text` as a tooltip after the hover delay when the last item is hovered.
void ItemTooltip(StringView text);
bool BeginTooltip();
void EndTooltip();

// --- Drag & drop ------------------------------------------------------------------------------------------------------------

/// Call after an item: starts dragging it with a typed payload. Returns true while dragging (draw a preview inside).
bool BeginDragDropSource(StringView type, const void* data, usize size, StringView preview = {});
/// Call after an item: returns the payload when something of `type` is dropped on it.
const void* AcceptDragDropPayload(StringView type, usize* size = nullptr);
bool IsDragDropActive(StringView type = {});

/// Vertical/horizontal splitter between two regions; sizes in logical px.
bool Splitter(StringView id, bool vertical, f32* size1, f32* size2, f32 min1 = 40.0f, f32 min2 = 40.0f, f32 thickness = 6.0f);

/// Bindless texture (SRV index) of `size` logical px.
void Image(u32 textureSrv, Vec2 size, Vec2 uv0 = Vec2(0, 0), Vec2 uv1 = Vec2(1, 1), Color tint = Color(255, 255, 255));
/// An SVG icon item (logical size), tinted with `color` (alpha 0 = the current text color).
void SvgIcon(const struct SvgImage& image, Vec2 size, Color color = Color(0, 0, 0, 0));

} // namespace zx::ui
