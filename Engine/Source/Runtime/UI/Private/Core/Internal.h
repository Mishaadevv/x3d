#pragma once

// ZUI internals shared by the widget implementation files.

#include "Core/Containers/HashMap.h"
#include "Core/Hash/Hash.h"
#include "UI/UI.h"

namespace zx::ui {

/// FNV-1a of a string combined with a seed (never 0).
inline u32 HashWithSeed(StringView s, u32 seed)
{
    u32 h = seed ^ 0x811c9dc5u;
    for (char c : s) {
        h ^= u8(c);
        h *= 0x01000193u;
    }
    return h ? h : 1;
}

struct DockNode
{
    ID id = 0;
    DockNode* parent = nullptr;
    UniquePtr<DockNode> child[2];
    bool horizontal = true; // children side by side (left | right); false = stacked (top / bottom)
    f32 ratio = 0.5f;       // size of child[0]
    Array<ID> windows;      // tabs (window ids), in order
    ID selected = 0;
    Rect rect;              // whole node
    Rect content;           // leaf: below the tab bar
    bool visible = true;    // has at least one live window (leaf) or visible child
    bool IsLeaf() const { return !child[0]; }
};

struct DockSpaceState
{
    ID id = 0;
    String name;
    UniquePtr<DockNode> root;
    Rect rect;
    u32 nextNode = 1;
    u64 lastFrame = 0;
    ID previewNode = 0; // dock target under the mouse while dragging a window
    i32 previewDir = -1; // 0 center, 1 left, 2 right, 3 up, 4 down
};

struct GroupData
{
    Vec2 cursorStart;
    Vec2 cursorMax;
    f32 workMinX;
    f32 prevLineHeight;
};

struct WindowState
{
    ID id = 0;
    String name;
    WindowFlags flags = WindowFlags::None;
    Vec2 pos;          // physical px
    Vec2 size;
    Vec2 scroll;
    Vec2 contentSize;  // measured last frame (physical px)
    bool collapsed = false;
    bool appearing = true;
    u32 hiddenFrames = 1; // auto-sized windows are measured invisibly on their first frame
    u64 lastFrameActive = 0;
    i32 order = 0;
    bool open = true;
    ID dockNodeId = 0;      // docked into this node (0 = floating)
    bool dockHidden = false; // docked but not the selected tab this frame
    bool hasCloseButton = false;
    bool closeRequested = false;

    DrawList ownDrawList;
    DrawList* dl = nullptr;
    WindowState* root = nullptr;
    WindowState* parent = nullptr;
    bool isChild = false;
    bool isPopup = false;
    bool isTooltip = false;
    bool isModal = false;
    bool isMenu = false;
    bool childBorder = false;

    // Per frame.
    Rect outer;     // full window rect
    Rect inner;     // content area (below title/menu bar, left of the scrollbar)
    Rect clip;      // content clip
    Rect savedClip; // while inside the menu bar
    f32 minAutoWidth = 0.0f; // auto-resized popups (combo: at least the combo width)
    f32 titleHeight = 0.0f;
    f32 menuBarHeight = 0.0f;
    bool scrollbarY = false;
    f32 scrollMaxY = 0.0f;
    bool contentHoverable = false; // root is hovered and nothing above blocks it

    // Layout.
    Vec2 cursorStart;   // content origin (scroll applied)
    Vec2 cursor;
    Vec2 cursorMax;
    Vec2 prevLineEnd;   // where SameLine continues
    f32 currLineHeight = 0.0f;
    f32 prevLineHeight = 0.0f;
    f32 indent = 0.0f;
    f32 workMinX = 0.0f; // left edge for new lines (table cells / groups shift it)
    f32 workMaxX = 0.0f; // right edge of the available region
    f32 nextItemWidth = -F32Max;
    bool lineTextAlign = false; // the current line is frame-height (center text)
    Array<ID> idStack;
    Array<GroupData> groups;

    // Menu bar layout.
    f32 menuBarCursorX = 0.0f;
    bool inMenuBar = false;
    Vec2 menuBarSavedCursor;
    f32 menuBarSavedLineHeight = 0.0f;
};

struct PopupRef
{
    ID id = 0;
    ID openerId = 0;          // item that opened it
    WindowState* window = nullptr;
    WindowState* parentWindow = nullptr;
    Vec2 openPos;             // mouse / anchor at open time (physical px)
    Rect anchor;              // menus: rect of the menu item
    u64 openFrame = 0;
    bool isMenu = false;
    bool isModal = false;
    bool isContext = false;
    bool isCombo = false;     // positioned below the anchor
};

struct TextEditState
{
    ID id = 0;
    usize cursor = 0;
    usize anchor = 0;     // selection = [min(anchor,cursor), max)
    f32 scrollX = 0.0f;
    f32 scrollY = 0.0f;
    f32 blink = 0.0f;
    String initial;       // for Escape
    String temp;          // numeric editing buffer
    bool numeric = false;
    bool selectAll = false;
    bool dragging = false;
    struct Snapshot
    {
        String text;
        usize cursor;
        usize anchor;
    };
    Array<Snapshot> undo;
    Array<Snapshot> redo;
    f64 lastEditTime = 0.0;

    bool HasSelection() const { return cursor != anchor; }
    usize SelMin() const { return cursor < anchor ? cursor : anchor; }
    usize SelMax() const { return cursor < anchor ? anchor : cursor; }
};

struct TableColumn
{
    String label;
    f32 initWidth = 0.0f; // logical; 0 = stretch
    f32 width = 0.0f;     // physical, resolved
    f32 x = 0.0f;         // left edge (physical)
    f32 contentMaxY = 0.0f;
};

struct TableState
{
    ID id = 0;
    TableFlags flags = TableFlags::None;
    u32 columnCount = 0;
    Array<TableColumn> columns;
    Array<f32> savedWidths; // persistent user widths (physical px at scale `savedScale`), 0 = stretch
    f32 savedScale = 1.0f;
    TableSortSpec sort;
    // Per frame.
    Rect rect;
    f32 rowY = 0.0f;
    f32 rowHeight = 0.0f;
    f32 rowMinHeight = 0.0f;
    i32 rowIndex = -1;
    bool rowOpen = false;
    u32 setupIndex = 0;
    i32 column = -1;
    u32 rowBgInsertIndex = 0;
    bool headerRow = false;
    bool setupDone = false;
    f32 savedWorkMinX = 0.0f;
    f32 savedWorkMaxX = 0.0f;
    f32 savedIndent = 0.0f;
    Vec2 savedCursorMax;
    f32 cellPadX = 0.0f;
    f32 cellPadY = 0.0f;
    bool hasScroll = false;
};

struct TabBarState
{
    ID id = 0;
    ID selected = 0;
    ID nextSelected = 0;
    Rect barRect;
    f32 nextTabX = 0.0f;
    f32 contentStartY = 0.0f;
    bool anyContent = false;
    bool sawSelected = false;
    ID firstTab = 0;
};

struct DragDropState
{
    bool active = false;
    bool delivered = false;
    bool dropped = false; // mouse released this frame
    ID sourceId = 0;
    String type;
    Array<u8> data;
    String preview;
};

struct Context::Impl
{
    explicit Impl(Context& owner, FontSystem& fonts, Style& style) : owner(owner), fonts(fonts), style(style) {}

    Context& owner;
    FontSystem& fonts;
    Style& style;
    FontHandle fontHandles[u32(FontStyle::Count)];
    f32 scale = 1.0f;

    // Frame.
    u64 frame = 0;
    f64 time = 0.0;
    f32 dt = 1.0f / 60.0f;
    Vec2 display;
    bool hasFocus = true;

    // Input.
    Vec2 mouse;
    Vec2 mouseDelta;
    bool mouseDown[u32(MouseButton::Count)] = {};
    bool mousePressed[u32(MouseButton::Count)] = {};
    bool mouseReleased[u32(MouseButton::Count)] = {};
    bool mouseDoubleClicked[u32(MouseButton::Count)] = {};
    f64 lastClickTime[u32(MouseButton::Count)] = {};
    Vec2 mouseClickPos[u32(MouseButton::Count)];
    f32 wheel = 0.0f;
    bool wheelConsumed = false;
    KeyModifiers mods = KeyModifiers::None;
    Array<Key> keysPressed;      // includes auto-repeat
    Array<Key> keysPressedFirst; // initial presses only
    bool keyDown[u32(Key::Count)] = {};
    String typed;

    // Interaction.
    ID hotId = 0;
    ID activeId = 0;
    bool focusNextTextField = false; // SetKeyboardFocusHere
    bool activeIdAlive = false;
    bool activeIdJustActivated = false;
    WindowState* activeWindow = nullptr;
    Vec2 activeClickOffset;
    f32 activeDragValue = 0.0f;   // accumulated drag for Drag* widgets
    ID focusId = 0;               // keyboard focus (text fields)
    ID deactivatedId = 0;         // id that lost active state this frame
    ID editedId = 0;

    // Windows.
    HashMap<ID, UniquePtr<WindowState>> windows;
    Array<WindowState*> windowOrder; // root windows, back to front
    Array<WindowState*> windowStack; // Begin/End nesting
    WindowState* current = nullptr;
    WindowState* hoveredRoot = nullptr;
    WindowState* focusedRoot = nullptr;
    WindowState* movingWindow = nullptr;
    WindowState* droppedWindow = nullptr; // was being moved, mouse released this frame

    // Docking.
    HashMap<ID, UniquePtr<DockSpaceState>> dockSpaces;
    HashMap<ID, DockNode*> dockNodes;
    DockNode* FindDockNode(ID id) const
    {
        DockNode* const* n = dockNodes.Find(id);
        return n ? *n : nullptr;
    }
    DockNode* NewDockNode(DockSpaceState& ds, DockNode* parent);
    void UndockWindow(WindowState* w);
    void DockWindowInto(WindowState* w, DockNode* node, i32 dir);
    i32 nextOrder = 1;

    // Next-window data.
    bool nextPosSet = false;
    Cond nextPosCond = Cond::Always;
    Vec2 nextPos;
    bool nextSizeSet = false;
    Cond nextSizeCond = Cond::Always;
    Vec2 nextSize;
    bool nextOpenSet = false;
    bool nextOpen = false;
    Cond nextOpenCond = Cond::Always;

    // Last item.
    ID lastItemId = 0;
    Rect lastItemRect;
    bool lastItemHovered = false;

    // Storage for widget state (tree open flags, scroll...).
    HashMap<ID, i32> intStorage;
    HashMap<ID, f32> floatStorage;

    // Style stacks.
    struct FontEntry
    {
        FontHandle font;
        f32 size; // physical px
    };
    Array<FontEntry> fontStack;
    Array<Color> textColorStack;
    i32 disabledDepth = 0;
    Array<bool> disabledStack;

    // Popups.
    Array<PopupRef> openPopups;
    Array<u32> popupBeginStack; // indices into openPopups currently between Begin/EndPopup
    bool closePopupRequested = false;
    ID menuBarActiveId = 0;     // window whose menu bar has an open menu

    // Text editing.
    TextEditState textEdit;

    // Tables / tabs.
    HashMap<ID, UniquePtr<TableState>> tables;
    Array<TableState*> tableStack;
    HashMap<ID, UniquePtr<TabBarState>> tabBars;
    Array<TabBarState*> tabBarStack;

    // Tooltip.
    String tooltipText;
    bool tooltipRequested = false;
    bool tooltipBegun = false;
    ID hoverTimerId = 0;
    f32 hoverTime = 0.0f;

    DragDropState dragDrop;
    CursorShape cursorShape = CursorShape::Arrow;
    bool wantsTextInput = false;

    DrawList foreground;
    DrawList modalDim;
    Array<const DrawList*> outputLists;

    // --- Helpers (Context.cpp) ---
    f32 S(f32 logical) const { return logical * scale; }
    Vec2 S(Vec2 logical) const { return logical * scale; }
    FontHandle Font() const { return fontStack.Back().font; }
    f32 FontSize() const { return fontStack.Back().size; }
    f32 LineHeight() const;
    f32 FrameHeight() const;
    Color TextColor() const;
    Color Col(Color c) const; // applies the disabled alpha

    ID MakeID(StringView label) const;
    ID MakeID(i32 v) const;
    void ItemSize(Vec2 size, f32 textBaselineOffset = 0.0f);
    bool ItemAdd(const Rect& rect, ID id);
    bool ItemHoverable(const Rect& rect, ID id);
    bool IsHoveringRect(const Rect& r) const;
    bool ButtonBehavior(const Rect& rect, ID id, bool* hovered, bool* held, bool pressOnClick = false);
    void SetActive(ID id, WindowState* window);
    void ClearActive();
    f32 CalcItemWidth();
    Vec2 CalcTextSize(StringView text, f32 wrap = 0.0f) const;
    void RenderText(Vec2 pos, StringView text, Color color);
    void RenderTextClipped(const Rect& rect, StringView text, Color color, TextAlign align = TextAlign::Left);
    void RenderFrame(const Rect& r, Color fill, bool border = true, f32 rounding = -1.0f);
    void RenderArrow(Vec2 center, f32 size, bool down, Color color);
    void RenderCheckMark(const Rect& r, Color color);

    WindowState* FindOrCreateWindow(StringView name, ID id);
    void BringToFront(WindowState* w);
    void BeginWindowLayout(WindowState* w);
    void EndWindowLayout(WindowState* w);
    void DrawScrollbar(WindowState* w);
    void UpdateHoveredRoot();
    void ClosePopupsOverWindow(WindowState* w);
    void ClosePopupsFrom(u32 index);
    bool IsAnyPopupModal() const;
    bool BeginPopupWindow(u32 popupIndex, WindowFlags flags, StringView title);
    void SetCursorShape(CursorShape c) { cursorShape = c; }
    void PushFontSmall() { fontStack.PushBack({fontHandles[u32(FontStyle::Regular)], S(style.smallFontSize)}); }
    void RenderColorSwatch(const Rect& r, Color color);

    // Text editing (WidgetsText.cpp).
    bool TextEditBehavior(ID id, const Rect& frame, String& text, InputTextFlags flags, StringView hint, bool justFocused,
                          const CodeEditorOptions* code = nullptr);
    HashMap<ID, Vec2> codeScroll; // scroll of code editors while they are not active
};

Context::Impl& G();

/// Text field for a number (InputFloat/InputInt and the typing mode of Drag/Slider widgets). Returns true when the
/// edited value was committed and differs.
bool NumericTextEdit(ID id, const Rect& frame, f64& value, bool isInt, StringView format, bool justFocused);

/// "Label##id" -> "Label".
StringView DisplayLabel(StringView label);

} // namespace zx::ui
