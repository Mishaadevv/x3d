#include "Internal.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Log/Log.h"
#include "Core/String/Utf.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogUI, Info);

namespace zx::ui {

namespace {

Context* g_context = nullptr;

constexpr i32 PopupLayer = 1 << 20;
constexpr i32 TooltipLayer = 1 << 21;
constexpr f32 DoubleClickTime = 0.32f;
constexpr f32 DoubleClickDistance = 6.0f;

} // namespace

Context::Impl& G()
{
    ZX_ASSERT(g_context, "No current ui::Context (call ui::SetCurrentContext)");
    return g_context->GetImpl();
}

void SetCurrentContext(Context* ctx) { g_context = ctx; }
Context* GetCurrentContext() { return g_context; }

StringView DisplayLabel(StringView label)
{
    const usize p = label.Find("##");
    return p == StringView::NPos ? label : label.Left(p);
}

Theme Theme::Light()
{
    Theme t;
    t.windowBg = Color(0xF4, 0xF5, 0xF7);
    t.childBg = Color(0xEC, 0xED, 0xF0);
    t.popupBg = Color(0xFF, 0xFF, 0xFF);
    t.titleBg = Color(0xE4, 0xE6, 0xEA);
    t.titleBgActive = Color(0xDA, 0xDD, 0xE3);
    t.menuBarBg = Color(0xE9, 0xEB, 0xEE);
    t.border = Color(0xCF, 0xD2, 0xD8);
    t.borderStrong = Color(0xB4, 0xB8, 0xC0);
    t.shadow = Color(0, 0, 0, 50);
    t.text = Color(0x1E, 0x20, 0x24);
    t.textDim = Color(0x5E, 0x63, 0x6C);
    t.textDisabled = Color(0xA0, 0xA4, 0xAC);
    t.frameBg = Color(0xFF, 0xFF, 0xFF);
    t.frameBgHovered = Color(0xF7, 0xF8, 0xFA);
    t.frameBgActive = Color(0xEE, 0xF0, 0xF4);
    t.button = Color(0xE6, 0xE8, 0xEC);
    t.buttonHovered = Color(0xDC, 0xDF, 0xE5);
    t.buttonActive = Color(0xD0, 0xD4, 0xDB);
    t.header = Color(0xE4, 0xE6, 0xEA);
    t.headerHovered = Color(0xDA, 0xDD, 0xE3);
    t.headerActive = Color(0xCF, 0xD3, 0xDA);
    t.selection = Color(0xC4, 0xD9, 0xFB);
    t.selectionInactive = Color(0xDD, 0xE0, 0xE6);
    t.separator = Color(0xD6, 0xD9, 0xDE);
    t.scrollbarGrab = Color(0xC2, 0xC6, 0xCD);
    t.scrollbarGrabHovered = Color(0xAD, 0xB2, 0xBA);
    t.scrollbarGrabActive = Color(0x98, 0x9D, 0xA7);
    t.tableHeaderBg = Color(0xEA, 0xEC, 0xEF);
    t.tableRowAlt = Color(0, 0, 0, 8);
    t.tableBorder = Color(0xD6, 0xD9, 0xDE);
    t.tabBg = Color(0xE4, 0xE6, 0xEA);
    t.tabHovered = Color(0xEC, 0xEE, 0xF1);
    t.tabActive = Color(0xF4, 0xF5, 0xF7);
    t.tooltipBg = Color(0xFF, 0xFF, 0xFF);
    t.modalDim = Color(0, 0, 0, 60);
    return t;
}

// --- Context -----------------------------------------------------------------------------------------------------------

Context::Context(FontSystem& fonts) : m_fonts(fonts) { m_impl.Reset(new Impl(*this, fonts, m_style)); }

Context::~Context()
{
    if (g_context == this)
        g_context = nullptr;
}

f32 Context::GetScale() const { return m_impl->scale; }

bool Context::LoadDefaultFonts(StringView fontsRoot)
{
    static constexpr StringView Subsets[] = {"latin", "latin-ext", "cyrillic", "cyrillic-ext", "greek"};
    struct Variant
    {
        FontStyle style;
        StringView weight;
        StringView name;
    };
    static constexpr Variant Variants[] = {{FontStyle::Regular, "400-normal", "Inter"},
                                           {FontStyle::Medium, "500-normal", "Inter Medium"},
                                           {FontStyle::SemiBold, "600-normal", "Inter SemiBold"},
                                           {FontStyle::Bold, "700-normal", "Inter Bold"},
                                           {FontStyle::Italic, "400-italic", "Inter Italic"}};
    bool ok = true;
    for (const Variant& v : Variants) {
        Array<String> paths;
        for (StringView subset : Subsets)
            paths.PushBack(String::Format("{}/Inter/inter-{}-{}.ttf", fontsRoot, subset, v.weight));
        Array<StringView> views;
        for (const String& p : paths)
            views.PushBack(p.View());
        FontHandle h = m_fonts.AddFont(v.name, views.View());
        if (h.IsValid())
            m_fonts.AddIconFace(h, String::Format("{}/Lucide/lucide.ttf", fontsRoot), 1.2f);
        else
            ok = false;
        m_impl->fontHandles[u32(v.style)] = h;
    }
    // Missing weights fall back to regular.
    for (FontHandle& h : m_impl->fontHandles)
        if (!h.IsValid())
            h = m_impl->fontHandles[u32(FontStyle::Regular)];
    return ok && m_impl->fontHandles[0].IsValid();
}

bool Context::WantsMouse() const
{
    const Impl& g = *m_impl;
    return g.hoveredRoot != nullptr || g.activeId != 0 || g.movingWindow != nullptr || !g.openPopups.Empty() ||
           g.dragDrop.active;
}

bool Context::WantsKeyboard() const
{
    const Impl& g = *m_impl;
    return g.wantsTextInput || g.activeId != 0;
}

bool Context::IsAnyItemActive() const
{
    const Impl& g = *m_impl;
    return g.activeId != 0 || g.wantsTextInput;
}

CursorShape Context::GetCursorShape() const { return m_impl->cursorShape; }
void Context::SetCursorShape(CursorShape shape) { m_impl->cursorShape = shape; }

Span<const DrawList* const> Context::GetDrawLists() const { return m_impl->outputLists.View(); }

void Context::BeginFrame(const FrameInput& in)
{
    Impl& g = *m_impl;
    ZX_ASSERT(g.fontHandles[0].IsValid(), "ui::Context: call LoadDefaultFonts() first");
    g.scale = Max(in.dpiScale, 0.5f) * m_userScale;
    ++g.frame;
    g.dt = Max(in.deltaTime, 1e-4f);
    g.time += g.dt;
    g.display = in.displaySize;
    g.hasFocus = in.hasFocus;
    m_fonts.BeginFrame(g.frame);

    // --- Input ---
    const InputState* input = in.input;
    const Vec2 mouse = input ? input->MousePosition() : Vec2(-F32Max, -F32Max);
    g.mouseDelta = g.frame > 1 ? mouse - g.mouse : Vec2::Zero();
    g.mouse = mouse;
    for (u32 b = 0; b < u32(MouseButton::Count); ++b) {
        const bool down = input && in.hasFocus && input->IsMouseDown(MouseButton(b));
        g.mousePressed[b] = down && !g.mouseDown[b];
        g.mouseReleased[b] = !down && g.mouseDown[b];
        g.mouseDown[b] = down;
        g.mouseDoubleClicked[b] = false;
        if (g.mousePressed[b]) {
            if (g.time - g.lastClickTime[b] < DoubleClickTime && Distance(mouse, g.mouseClickPos[b]) < DoubleClickDistance) {
                g.mouseDoubleClicked[b] = true;
                g.lastClickTime[b] = -100.0;
            } else {
                g.lastClickTime[b] = g.time;
            }
            g.mouseClickPos[b] = mouse;
        }
    }
    g.wheel = input ? input->MouseWheel() : 0.0f;
    g.wheelConsumed = false;
    g.mods = input ? input->Modifiers() : KeyModifiers::None;
    g.keysPressed.Clear();
    g.keysPressedFirst.Clear();
    g.typed.Clear();
    for (u32 k = 0; k < u32(Key::Count); ++k)
        g.keyDown[k] = input && input->IsKeyDown(Key(k));
    for (const WindowEvent& e : in.events) {
        if (e.type == WindowEventType::KeyDown) {
            g.keysPressed.PushBack(e.key);
            if (!e.repeat)
                g.keysPressedFirst.PushBack(e.key);
        }
        else if (e.type == WindowEventType::Char && e.codepoint >= 32 && e.codepoint != 127)
            Utf8Append(g.typed, e.codepoint);
    }

    // --- Per-frame state ---
    g.hotId = 0;
    g.lastItemId = 0;
    g.lastItemHovered = false;
    g.tooltipRequested = false;
    g.tooltipText.Clear();
    g.cursorShape = CursorShape::Arrow;
    g.deactivatedId = 0;
    g.editedId = 0;
    g.activeIdJustActivated = false;
    g.closePopupRequested = false;
    if (g.dragDrop.dropped) {
        g.dragDrop = DragDropState();
    }
    if (g.dragDrop.active && !g.mouseDown[0])
        g.dragDrop.dropped = true;

    // Drop the active id if its widget was not submitted last frame (window closed, item hidden).
    if (g.activeId != 0 && !g.activeIdAlive)
        g.ClearActive();
    g.activeIdAlive = false;
    g.droppedWindow = nullptr;
    if (g.movingWindow && !g.mouseDown[0]) {
        g.droppedWindow = g.movingWindow;
        g.movingWindow = nullptr;
    }

    g.UpdateHoveredRoot();

    // Click focus: bring the clicked window to front, close popups that were clicked outside of.
    if (g.mousePressed[0] || g.mousePressed[1]) {
        WindowState* clicked = g.hoveredRoot;
        if (clicked && !clicked->isPopup && !clicked->isTooltip) {
            g.focusedRoot = clicked;
            if (!HasAnyFlags(clicked->flags, WindowFlags::NoBringToFront))
                g.BringToFront(clicked);
        } else if (!clicked) {
            g.focusedRoot = nullptr;
        }
        g.ClosePopupsOverWindow(clicked);
    }
    if (!g.openPopups.Empty() && g.keysPressed.Find(Key::Escape) != NPos && g.activeId == 0)
        g.ClosePopupsFrom(u32(g.openPopups.Size() - 1));

    g.fontStack.Clear();
    g.fontStack.PushBack({g.fontHandles[u32(FontStyle::Regular)], g.S(m_style.fontSize)});
    g.textColorStack.Clear();
    g.disabledDepth = 0;
    g.windowStack.Clear();
    g.current = nullptr;
    g.foreground.Reset(&m_fonts, Rect(Vec2::Zero(), g.display));
}

void Context::EndFrame()
{
    Impl& g = *m_impl;
    ZX_ASSERT(g.windowStack.Empty(), "ui: missing End()/EndChild() ({} open)", g.windowStack.Size());

    // Tooltip window (after everything so it is on top).
    if (g.tooltipRequested && !g.tooltipText.Empty()) {
        if (BeginTooltip()) {
            const f32 wrap = g.S(360.0f);
            const Vec2 sz = g.CalcTextSize(g.tooltipText, wrap);
            const Rect r(g.current->cursor, g.current->cursor + sz);
            g.current->dl->AddText(g.Font(), g.FontSize(), r.min, g.style.colors.text, g.tooltipText, wrap);
            g.ItemSize(sz);
            EndTooltip();
        }
    }
    // Drag & drop preview.
    if (g.dragDrop.active && !g.dragDrop.preview.Empty()) {
        const Vec2 sz = g.CalcTextSize(g.dragDrop.preview);
        const Vec2 pad = g.S(Vec2(8.0f, 4.0f));
        const Rect r(g.mouse + g.S(Vec2(14.0f, 10.0f)), g.mouse + g.S(Vec2(14.0f, 10.0f)) + sz + pad * 2.0f);
        g.foreground.AddShadow(r, g.style.colors.shadow, g.S(4.0f), g.S(8.0f));
        g.foreground.AddRect(r, g.style.colors.popupBg, CornerRadii(g.S(4.0f)), 1.0f, g.style.colors.accent);
        g.foreground.AddText(g.Font(), g.FontSize(), r.min + pad, g.style.colors.text, g.dragDrop.preview);
    }
    if (g.dragDrop.dropped && !g.dragDrop.delivered)
        g.dragDrop = DragDropState(); // released over nothing

    // Popups not submitted this frame are closed.
    for (u32 i = 0; i < g.openPopups.Size(); ++i) {
        const PopupRef& p = g.openPopups[i];
        if (p.openFrame < g.frame && (!p.window || p.window->lastFrameActive < g.frame)) {
            g.ClosePopupsFrom(i);
            break;
        }
    }

    g.wantsTextInput = g.activeId != 0 && g.activeId == g.textEdit.id;

    // Output: root windows sorted by layer/order.
    StableSort(g.windowOrder, [](const WindowState* a, const WindowState* b) { return a->order < b->order; });
    g.outputLists.Clear();
    for (WindowState* w : g.windowOrder) {
        if (w->lastFrameActive != g.frame || w->hiddenFrames > 0 || w->dockHidden)
            continue;
        if (w->isModal) {
            g.modalDim.Reset(&m_fonts, Rect(Vec2::Zero(), g.display));
            g.modalDim.AddRect(Rect(Vec2::Zero(), g.display), g.style.colors.modalDim);
            g.outputLists.PushBack(&g.modalDim);
        }
        g.outputLists.PushBack(&w->ownDrawList);
    }
    g.outputLists.PushBack(&g.foreground);
    for (WindowState* w : g.windowOrder)
        if (w->hiddenFrames > 0 && w->lastFrameActive == g.frame)
            --w->hiddenFrames;
}

// --- Helpers -------------------------------------------------------------------------------------------------------------

f32 Context::Impl::LineHeight() const { return Round(fonts.GetLineMetrics(Font(), FontSize()).lineHeight); }

f32 Context::Impl::FrameHeight() const { return LineHeight() + Round(S(style.framePadding.y)) * 2.0f; }

Color Context::Impl::Col(Color c) const { return disabledDepth > 0 ? WithAlpha(c, 0.45f) : c; }

Color Context::Impl::TextColor() const
{
    if (disabledDepth > 0)
        return style.colors.textDisabled;
    return textColorStack.Empty() ? style.colors.text : textColorStack.Back();
}

ID Context::Impl::MakeID(StringView label) const
{
    const u32 seed = current && !current->idStack.Empty() ? current->idStack.Back() : 0;
    return HashWithSeed(label, seed);
}

ID Context::Impl::MakeID(i32 v) const
{
    const u32 seed = current && !current->idStack.Empty() ? current->idStack.Back() : 0;
    return HashWithSeed(StringView(reinterpret_cast<const char*>(&v), sizeof(v)), seed ^ 0x9E3779B9u);
}

Vec2 Context::Impl::CalcTextSize(StringView text, f32 wrap) const
{
    if (text.Empty())
        return Vec2(0.0f, LineHeight());
    const Vec2 s = fonts.MeasureText(Font(), FontSize(), text, wrap);
    return Vec2(Ceil(s.x), Round(s.y));
}

void Context::Impl::RenderText(Vec2 pos, StringView text, Color color)
{
    current->dl->AddText(Font(), FontSize(), pos, color, text);
}

void Context::Impl::RenderTextClipped(const Rect& rect, StringView text, Color color, TextAlign align)
{
    current->dl->AddTextInRect(Font(), FontSize(), rect, color, text, align, true);
}

void Context::Impl::RenderFrame(const Rect& r, Color fill, bool border, f32 rounding)
{
    const f32 rr = rounding < 0.0f ? S(style.frameRounding) : rounding;
    current->dl->AddRect(r, Col(fill), CornerRadii(rr), border ? 1.0f : 0.0f, Col(style.colors.border));
}

void Context::Impl::RenderArrow(Vec2 c, f32 size, bool down, Color color)
{
    const f32 h = size * 0.5f;
    if (down)
        current->dl->AddTriangle(c + Vec2(-h, -h * 0.5f), c + Vec2(h, -h * 0.5f), c + Vec2(0.0f, h * 0.6f), Col(color));
    else
        current->dl->AddTriangle(c + Vec2(-h * 0.5f, -h), c + Vec2(h * 0.6f, 0.0f), c + Vec2(-h * 0.5f, h), Col(color));
}

void Context::Impl::RenderCheckMark(const Rect& r, Color color)
{
    const f32 s = r.Width();
    const Vec2 o = r.min;
    const f32 t = Max(1.5f, s * 0.14f);
    const Vec2 a = o + Vec2(s * 0.22f, s * 0.52f), b = o + Vec2(s * 0.42f, s * 0.72f), c = o + Vec2(s * 0.78f, s * 0.3f);
    current->dl->AddLine(a, b, Col(color), t);
    current->dl->AddLine(b, c, Col(color), t);
}

// --- Layout ----------------------------------------------------------------------------------------------------------------

void Context::Impl::ItemSize(Vec2 size, f32 /*textBaselineOffset*/)
{
    WindowState* w = current;
    const f32 lineHeight = Max(w->currLineHeight, size.y);
    w->prevLineEnd = Vec2(w->cursor.x + size.x, w->cursor.y);
    w->cursorMax.x = Max(w->cursorMax.x, w->cursor.x + size.x);
    w->cursorMax.y = Max(w->cursorMax.y, w->cursor.y + lineHeight);
    w->cursor.x = Round(w->workMinX + w->indent);
    w->cursor.y = Round(w->cursor.y + lineHeight + S(style.itemSpacing.y));
    w->prevLineHeight = lineHeight;
    w->currLineHeight = 0.0f;
    w->lineTextAlign = false;
}

bool Context::Impl::IsHoveringRect(const Rect& r) const
{
    if (!current)
        return r.Contains(mouse);
    const Rect clipped = r.Intersect(current->clip);
    return clipped.Contains(mouse);
}

bool Context::Impl::ItemAdd(const Rect& rect, ID id)
{
    lastItemId = id;
    lastItemRect = rect;
    lastItemHovered = false;
    if (id != 0 && id == activeId)
        activeIdAlive = true;
    return rect.Intersects(current->clip);
}

bool Context::Impl::ItemHoverable(const Rect& rect, ID id)
{
    if (!current->contentHoverable || disabledDepth > 0)
        return false;
    // A keyboard-focused text field does not block hovering other widgets; mouse captures do.
    if (activeId != 0 && activeId != id && !dragDrop.active && activeId != textEdit.id)
        return false;
    if (movingWindow)
        return false;
    if (!IsHoveringRect(rect))
        return false;
    if (id != 0)
        hotId = id;
    lastItemHovered = true;
    return true;
}

void Context::Impl::SetActive(ID id, WindowState* window)
{
    if (activeId != id) {
        if (activeId != 0)
            deactivatedId = activeId;
        activeIdJustActivated = true;
    }
    activeId = id;
    activeWindow = window;
    activeIdAlive = true;
}

void Context::Impl::ClearActive()
{
    if (activeId != 0)
        deactivatedId = activeId;
    activeId = 0;
    activeWindow = nullptr;
}

bool Context::Impl::ButtonBehavior(const Rect& rect, ID id, bool* outHovered, bool* outHeld, bool pressOnClick)
{
    bool hovered = ItemHoverable(rect, id);
    bool pressed = false;
    if (hovered && mousePressed[0]) {
        SetActive(id, current);
        activeClickOffset = mouse - rect.min;
        if (pressOnClick) {
            pressed = true;
            ClearActive();
        }
    }
    bool held = false;
    if (activeId == id) {
        activeIdAlive = true;
        if (mouseDown[0]) {
            held = true;
        } else {
            pressed = hovered;
            ClearActive();
        }
    }
    if (outHovered)
        *outHovered = hovered;
    if (outHeld)
        *outHeld = held;
    return pressed;
}

f32 Context::Impl::CalcItemWidth()
{
    WindowState* w = current;
    f32 width;
    const f32 avail = Max(w->workMaxX - w->cursor.x, 1.0f);
    if (w->nextItemWidth != -F32Max) {
        width = w->nextItemWidth >= 0.0f ? S(w->nextItemWidth) : Max(avail + S(w->nextItemWidth), 1.0f);
        w->nextItemWidth = -F32Max;
    } else {
        width = Round(avail * style.defaultItemWidthRatio);
    }
    return Max(Round(width), 1.0f);
}

// --- Windows -------------------------------------------------------------------------------------------------------------

WindowState* Context::Impl::FindOrCreateWindow(StringView name, ID id)
{
    if (UniquePtr<WindowState>* existing = windows.Find(id))
        return existing->Get();
    UniquePtr<WindowState> w(new WindowState());
    w->id = id;
    w->name = name;
    WindowState* raw = w.Get();
    windows.Insert(ID(id), move(w));
    return raw;
}

void Context::Impl::BringToFront(WindowState* w)
{
    if (w->isPopup || w->isTooltip || w->isChild)
        return;
    w->order = nextOrder++;
}

void Context::Impl::UpdateHoveredRoot()
{
    hoveredRoot = nullptr;
    // Topmost window (by order) containing the mouse, among windows alive last frame.
    WindowState* best = nullptr;
    // Windows below an open modal can't be hovered. No modal: nothing is excluded (docked windows and the dock host
    // have negative orders - a -1 default made every docked panel unclickable).
    i32 modalOrder = I32Min;
    for (WindowState* w : windowOrder)
        if (w->isModal && w->lastFrameActive + 1 >= frame)
            modalOrder = Max(modalOrder, w->order);
    for (WindowState* w : windowOrder) {
        if (w->lastFrameActive + 1 < frame || w->isTooltip || w->hiddenFrames > 0 || w->dockHidden)
            continue;
        if (!w->outer.Contains(mouse))
            continue;
        if (w->order < modalOrder)
            continue;
        if (!best || w->order > best->order)
            best = w;
    }
    if (movingWindow)
        best = movingWindow;
    hoveredRoot = best;
}

void Context::Impl::ClosePopupsFrom(u32 index)
{
    if (index < openPopups.Size())
        openPopups.Resize(index);
    if (openPopups.Empty())
        menuBarActiveId = 0;
}

void Context::Impl::ClosePopupsOverWindow(WindowState* w)
{
    if (openPopups.Empty())
        return;
    // Keep popups up to (and including) the one that was clicked; a click on a regular window closes all.
    i32 keep = -1;
    if (w && w->isPopup)
        for (u32 i = 0; i < openPopups.Size(); ++i)
            if (openPopups[i].window == w)
                keep = i32(i);
    if (keep < 0 && IsAnyPopupModal()) {
        // Clicks outside a modal never close it (only popups above it).
        for (u32 i = 0; i < openPopups.Size(); ++i)
            if (openPopups[i].isModal)
                keep = Max(keep, i32(i));
    }
    ClosePopupsFrom(u32(keep + 1));
}

bool Context::Impl::IsAnyPopupModal() const
{
    for (const PopupRef& p : openPopups)
        if (p.isModal)
            return true;
    return false;
}

void Context::Impl::BeginWindowLayout(WindowState* w)
{
    const Vec2 pad = w->isChild && !w->childBorder ? Vec2::Zero() : Round(S(style.windowPadding.x)) * Vec2(1.0f, 0.0f) +
                                                                       Round(S(style.windowPadding.y)) * Vec2(0.0f, 1.0f);
    w->cursorStart = Vec2(Round(w->inner.min.x + pad.x), Round(w->inner.min.y + pad.y - w->scroll.y));
    w->cursor = w->cursorStart;
    w->cursorMax = w->cursorStart;
    w->prevLineEnd = w->cursor;
    w->currLineHeight = w->prevLineHeight = 0.0f;
    w->indent = 0.0f;
    w->workMinX = w->cursorStart.x;
    w->workMaxX = Round(w->inner.max.x - pad.x);
    w->nextItemWidth = -F32Max;
    w->idStack.Clear();
    w->idStack.PushBack(w->id);
    w->groups.Clear();
    w->lineTextAlign = false;
}

void Context::Impl::DrawScrollbar(WindowState* w)
{
    if (!w->scrollbarY)
        return;
    const f32 sbw = S(style.scrollbarSize);
    const Rect track(Vec2(w->inner.max.x, w->inner.min.y), Vec2(w->inner.max.x + sbw, w->inner.max.y));
    const f32 viewH = w->inner.Height();
    const f32 contentH = Max(w->contentSize.y, viewH);
    const f32 grabH = Max(viewH * viewH / contentH, S(style.grabMinSize));
    const f32 travel = Max(viewH - grabH, 1.0f);
    const f32 t = w->scrollMaxY > 0.0f ? w->scroll.y / w->scrollMaxY : 0.0f;
    const Rect grab(Vec2(track.min.x + S(2.0f), track.min.y + travel * t), Vec2(track.max.x - S(2.0f), track.min.y + travel * t + grabH));
    const ID id = HashWithSeed("#SCROLLY", w->id);
    bool hovered, held;
    current->dl->PushClipRect(track, 0.0f, false);
    const Rect savedClip = current->clip;
    current->clip = track;
    ButtonBehavior(track, id, &hovered, &held);
    if (held) {
        if (activeIdJustActivated) {
            // Clicking the track outside the grab jumps there (grab centered under the mouse).
            if (!grab.Contains(mouse))
                activeClickOffset.y = grabH * 0.5f;
            else
                activeClickOffset.y = mouse.y - grab.min.y;
        }
        const f32 newT = Clamp((mouse.y - activeClickOffset.y - track.min.y) / travel, 0.0f, 1.0f);
        w->scroll.y = Round(newT * w->scrollMaxY);
    }
    const Color c = held ? style.colors.scrollbarGrabActive : (hovered ? style.colors.scrollbarGrabHovered : style.colors.scrollbarGrab);
    current->dl->AddRect(grab, c, CornerRadii(grab.Width() * 0.5f));
    current->clip = savedClip;
    current->dl->PopClipRect();
}

void Context::Impl::EndWindowLayout(WindowState* w)
{
    const Vec2 pad = w->isChild && !w->childBorder ? Vec2::Zero() : Vec2(Round(S(style.windowPadding.x)), Round(S(style.windowPadding.y)));
    // Content size (unscrolled, padding included) for next frame's scrollbar / auto-resize.
    w->contentSize = Vec2(w->cursorMax.x - w->cursorStart.x + pad.x * 2.0f, w->cursorMax.y - w->cursorStart.y + pad.y * 2.0f);
    // Mouse wheel: innermost scrollable region under the mouse.
    if (!wheelConsumed && wheel != 0.0f && w->scrollMaxY > 0.0f && w->root == hoveredRoot && w->clip.Contains(mouse) &&
        !HasAnyFlags(w->flags, WindowFlags::NoScrollbar)) {
        w->scroll.y = Clamp(Round(w->scroll.y - wheel * LineHeight() * 3.0f), 0.0f, w->scrollMaxY);
        wheelConsumed = true;
    }
    w->dl->PopClipRect();
    DrawScrollbar(w);
}

bool Context::Impl::BeginPopupWindow(u32 index, WindowFlags flags, StringView title)
{
    PopupRef& p = openPopups[index];
    String name = String::Format("##Popup_{:08x}", p.id);
    if (p.isModal)
        name = String(title) + String::Format("##Modal_{:08x}", p.id);
    const ID windowId = HashWithSeed(name, 0);
    WindowState* w = FindOrCreateWindow(name, windowId);
    p.window = w;
    w->isPopup = true;
    w->isModal = p.isModal;
    w->isMenu = p.isMenu;
    w->order = PopupLayer + i32(index);
    if (w->lastFrameActive + 1 < frame)
        w->hiddenFrames = 1; // measure first
    popupBeginStack.PushBack(index);
    // Position.
    const Vec2 size = w->contentSize;
    Vec2 pos;
    if (p.isModal) {
        pos = Round((display - w->size) * 0.5f);
    } else if (p.isCombo) {
        pos = Vec2(p.anchor.min.x, p.anchor.max.y + S(2.0f));
        if (pos.y + size.y > display.y)
            pos.y = p.anchor.min.y - size.y - S(2.0f);
    } else if (p.isMenu) {
        const bool inMenuBar = p.parentWindow && !p.parentWindow->isPopup;
        pos = inMenuBar ? Vec2(p.anchor.min.x, p.anchor.max.y) : Vec2(p.anchor.max.x + S(2.0f), p.anchor.min.y - S(4.0f));
        if (!inMenuBar && pos.x + size.x > display.x)
            pos.x = p.anchor.min.x - size.x - S(2.0f);
    } else {
        pos = p.openPos + S(Vec2(2.0f, 2.0f));
    }
    pos.x = Clamp(pos.x, 0.0f, Max(display.x - size.x, 0.0f));
    pos.y = Clamp(pos.y, 0.0f, Max(display.y - size.y, 0.0f));
    nextPosSet = true;
    nextPosCond = Cond::Always;
    nextPos = pos / scale;
    WindowFlags wf = flags | WindowFlags::AlwaysAutoResize | WindowFlags::NoMove | WindowFlags::NoResize | WindowFlags::NoCollapse;
    if (!p.isModal)
        wf |= WindowFlags::NoTitleBar;
    const bool visible = Begin(name, nullptr, wf);
    if (!visible) {
        End();
        popupBeginStack.PopBack();
        return false;
    }
    return true;
}

// --- Public: windows -------------------------------------------------------------------------------------------------------

void SetNextWindowPos(Vec2 pos, Cond cond)
{
    Context::Impl& g = G();
    g.nextPosSet = true;
    g.nextPos = pos;
    g.nextPosCond = cond;
}

void SetNextWindowSize(Vec2 size, Cond cond)
{
    Context::Impl& g = G();
    g.nextSizeSet = true;
    g.nextSize = size;
    g.nextSizeCond = cond;
}

bool Begin(StringView title, bool* open, WindowFlags flags)
{
    Context::Impl& g = G();
    const Theme& col = g.style.colors;
    const ID id = HashWithSeed(title, 0);
    WindowState* w = g.FindOrCreateWindow(title, id);
    const bool firstUse = w->lastFrameActive == 0;
    const bool appearing = w->lastFrameActive + 1 < g.frame;
    w->appearing = appearing;
    if (w->name.Empty())
        w->name = title;
    w->hasCloseButton = open != nullptr;
    if (w->closeRequested) {
        w->closeRequested = false;
        if (open)
            *open = false;
    }
    DockNode* dockNode = w->dockNodeId ? g.FindDockNode(w->dockNodeId) : nullptr;
    if (w->dockNodeId && !dockNode)
        w->dockNodeId = 0;
    const bool docked = dockNode != nullptr;
    if (docked)
        flags = (flags & ~WindowFlags::AlwaysAutoResize) | WindowFlags::NoTitleBar | WindowFlags::NoResize | WindowFlags::NoMove |
                WindowFlags::NoCollapse | WindowFlags::NoBringToFront;
    if (HasAnyFlags(flags, WindowFlags::Fullscreen))
        flags |= WindowFlags::NoTitleBar | WindowFlags::NoResize | WindowFlags::NoMove | WindowFlags::NoCollapse |
                 WindowFlags::NoBringToFront;
    w->flags = flags;
    w->isChild = false;
    w->root = w;
    w->parent = nullptr;
    const bool isPopup = w->isPopup;
    if (firstUse) {
        w->pos = g.S(Vec2(60.0f, 60.0f) + Vec2(f32(g.windows.Size() % 8) * 24.0f));
        w->size = g.S(Vec2(420.0f, 320.0f));
        if (!isPopup && !HasAnyFlags(flags, WindowFlags::NoBringToFront))
            g.BringToFront(w);
        g.windowOrder.PushBack(w);
    } else if (appearing && !isPopup && !HasAnyFlags(flags, WindowFlags::NoBringToFront)) {
        g.BringToFront(w);
    }
    if (g.nextPosSet && (g.nextPosCond == Cond::Always || firstUse))
        w->pos = Round(g.S(g.nextPos));
    if (g.nextSizeSet && (g.nextSizeCond == Cond::Always || firstUse))
        w->size = Round(g.S(g.nextSize));
    g.nextPosSet = g.nextSizeSet = false;
    if (HasAnyFlags(flags, WindowFlags::Fullscreen)) {
        w->pos = Vec2::Zero();
        w->size = g.display;
    }
    w->dockHidden = false;
    if (docked) {
        w->pos = dockNode->content.min;
        w->size = Max(dockNode->content.Size(), Vec2(1.0f));
        w->order = -1000;
        w->dockHidden = dockNode->selected != w->id || !dockNode->visible;
        w->hiddenFrames = 0;
        w->collapsed = false;
    }
    w->lastFrameActive = g.frame;
    w->dl = &w->ownDrawList;
    w->dl->Reset(&g.fonts, Rect(Vec2::Zero(), g.display));
    g.windowStack.PushBack(w);
    g.current = w;

    const bool hasTitle = !HasAnyFlags(flags, WindowFlags::NoTitleBar);
    w->titleHeight = hasTitle ? Round(g.S(g.style.titleBarHeight)) : 0.0f;
    w->menuBarHeight = HasAnyFlags(flags, WindowFlags::MenuBar) ? Round(g.FrameHeight() + g.S(4.0f)) : 0.0f;
    const Vec2 pad(Round(g.S(g.style.windowPadding.x)), Round(g.S(g.style.windowPadding.y)));
    const f32 rounding = isPopup ? g.S(g.style.popupRounding)
                                 : (HasAnyFlags(flags, WindowFlags::Fullscreen) || docked ? 0.0f : g.S(g.style.windowRounding));

    if (HasAnyFlags(flags, WindowFlags::AlwaysAutoResize) && !w->collapsed) {
        w->size.x = Max(Max(w->contentSize.x, w->minAutoWidth), g.S(40.0f));
        w->size.y = w->contentSize.y + w->titleHeight + w->menuBarHeight;
    }
    w->contentHoverable = w == g.hoveredRoot && !w->dockHidden;

    // --- Move / resize / collapse (title bar interactions use last frame geometry) ---
    const Rect titleRect(w->pos, Vec2(w->pos.x + w->size.x, w->pos.y + w->titleHeight));
    if (hasTitle) {
        const ID moveId = HashWithSeed("#MOVE", id);
        const ID closeId = HashWithSeed("#CLOSE", id);
        const f32 btn = w->titleHeight - g.S(10.0f);
        const Rect closeRect(Vec2(titleRect.max.x - g.S(8.0f) - btn, titleRect.min.y + g.S(5.0f)),
                             Vec2(titleRect.max.x - g.S(8.0f), titleRect.min.y + g.S(5.0f) + btn));
        w->clip = Rect(Vec2::Zero(), g.display);
        bool closeHovered = false, closeHeld = false;
        if (open && g.ButtonBehavior(closeRect, closeId, &closeHovered, &closeHeld))
            *open = false;
        Rect dragRect = titleRect;
        if (open)
            dragRect.max.x = closeRect.min.x - g.S(2.0f);
        bool held = false;
        if (!HasAnyFlags(flags, WindowFlags::NoMove)) {
            g.ButtonBehavior(dragRect, moveId, nullptr, &held);
            if (held && g.activeIdJustActivated)
                g.movingWindow = w;
            if (held && g.movingWindow == w) {
                w->pos += g.mouseDelta;
                g.SetCursorShape(CursorShape::ResizeAll);
            }
        }
        if (!HasAnyFlags(flags, WindowFlags::NoCollapse) && g.mouseDoubleClicked[0] && g.IsHoveringRect(dragRect) &&
            w->contentHoverable)
            w->collapsed = !w->collapsed;
        // Remember close button state for drawing below.
        g.intStorage.InsertOrAssign(closeId, (closeHovered ? 1 : 0) | (closeHeld ? 2 : 0));
    }
    if (!HasAnyFlags(flags, WindowFlags::NoResize) && !w->collapsed) {
        const f32 grip = g.S(14.0f);
        const ID resizeId = HashWithSeed("#RESIZE", id);
        const Rect gripRect(w->pos + w->size - Vec2(grip), w->pos + w->size + Vec2(g.S(3.0f)));
        const Rect rightEdge(Vec2(w->pos.x + w->size.x - g.S(3.0f), w->pos.y + w->titleHeight), Vec2(w->pos.x + w->size.x + g.S(4.0f), w->pos.y + w->size.y - grip));
        const Rect bottomEdge(Vec2(w->pos.x, w->pos.y + w->size.y - g.S(3.0f)), Vec2(w->pos.x + w->size.x - grip, w->pos.y + w->size.y + g.S(4.0f)));
        w->clip = Rect(Vec2::Zero(), g.display);
        bool hovered = false, held = false;
        g.ButtonBehavior(gripRect, resizeId, &hovered, &held);
        if (hovered || held)
            g.SetCursorShape(CursorShape::ResizeNWSE);
        if (held)
            w->size += g.mouseDelta;
        const ID rId = HashWithSeed("#RESIZE_R", id), bId = HashWithSeed("#RESIZE_B", id);
        bool rh = false, rHeld = false, bh = false, bHeld = false;
        g.ButtonBehavior(rightEdge, rId, &rh, &rHeld);
        g.ButtonBehavior(bottomEdge, bId, &bh, &bHeld);
        if (rh || rHeld)
            g.SetCursorShape(CursorShape::ResizeEW);
        if (bh || bHeld)
            g.SetCursorShape(CursorShape::ResizeNS);
        if (rHeld)
            w->size.x += g.mouseDelta.x;
        if (bHeld)
            w->size.y += g.mouseDelta.y;
        w->size = Max(w->size, g.S(Vec2(120.0f, 60.0f)));
        g.intStorage.InsertOrAssign(resizeId, (hovered ? 1 : 0) | (held ? 2 : 0));
    }
    // Keep at least part of the title bar on screen.
    if (!HasAnyFlags(flags, WindowFlags::Fullscreen) && !isPopup && !docked) {
        w->pos.x = Clamp(w->pos.x, -w->size.x + g.S(60.0f), Max(g.display.x - g.S(60.0f), 0.0f));
        w->pos.y = Clamp(w->pos.y, 0.0f, Max(g.display.y - g.S(30.0f), 0.0f));
    }
    w->pos = Round(w->pos);
    w->size = Round(w->size);

    const f32 height = w->collapsed ? w->titleHeight : w->size.y;
    w->outer = Rect(w->pos, Vec2(w->pos.x + w->size.x, w->pos.y + height));
    DrawList& dl = *w->dl;

    // --- Background, shadow, border ---
    const bool focused = g.focusedRoot == w || isPopup;
    if (!HasAnyFlags(flags, WindowFlags::NoBackground)) {
        if (!HasAnyFlags(flags, WindowFlags::Fullscreen) && !docked)
            dl.AddShadow(w->outer, col.shadow, rounding, g.S(isPopup ? 10.0f : 18.0f), g.S(Vec2(0.0f, 4.0f)));
        const Color bg = isPopup ? col.popupBg : col.windowBg;
        dl.AddRect(w->outer, bg, CornerRadii(rounding), HasAnyFlags(flags, WindowFlags::Fullscreen) || docked ? 0.0f : 1.0f,
                   focused ? col.borderStrong : col.border);
    }
    if (hasTitle) {
        const Rect tr(w->outer.min, Vec2(w->outer.max.x, w->outer.min.y + w->titleHeight));
        dl.AddRect(Rect(tr.min + Vec2(1.0f), tr.max - Vec2(1.0f, 0.0f)), focused ? col.titleBgActive : col.titleBg,
                   w->collapsed ? CornerRadii(rounding - 1.0f) : CornerRadii::Top(rounding - 1.0f));
        if (!w->collapsed)
            dl.AddRect(Rect(Vec2(tr.min.x + 1.0f, tr.max.y - 1.0f), Vec2(tr.max.x - 1.0f, tr.max.y)), col.border);
        // Title text (semibold) and close button.
        g.fontStack.PushBack({g.fontHandles[u32(FontStyle::SemiBold)], g.S(g.style.fontSize)});
        const f32 btn = w->titleHeight - g.S(10.0f);
        const Rect textRect(Vec2(tr.min.x + pad.x, tr.min.y), Vec2(tr.max.x - pad.x - (open ? btn + g.S(6.0f) : 0.0f), tr.max.y));
        dl.AddTextInRect(g.Font(), g.FontSize(), textRect, focused ? col.text : col.textDim, DisplayLabel(title));
        g.fontStack.PopBack();
        if (open) {
            const ID closeId = HashWithSeed("#CLOSE", id);
            const i32 st = g.intStorage.FindOr(closeId, 0);
            const Rect closeRect(Vec2(tr.max.x - g.S(8.0f) - btn, tr.min.y + g.S(5.0f)), Vec2(tr.max.x - g.S(8.0f), tr.min.y + g.S(5.0f) + btn));
            if (st)
                dl.AddRect(closeRect, (st & 2) ? col.buttonActive : col.buttonHovered, CornerRadii(g.S(4.0f)));
            dl.AddTextInRect(g.Font(), g.FontSize(), closeRect, (st & 1) ? col.text : col.textDim, ZX_ICON_X, TextAlign::Center, false);
        }
    }
    if (!w->collapsed && !HasAnyFlags(flags, WindowFlags::NoResize)) {
        const ID resizeId = HashWithSeed("#RESIZE", id);
        const i32 st = g.intStorage.FindOr(resizeId, 0);
        const Vec2 br = w->outer.max - Vec2(g.S(4.0f));
        const f32 s = g.S(9.0f);
        dl.AddTriangle(br, br - Vec2(s, 0.0f), br - Vec2(0.0f, s), st ? col.accent : col.borderStrong);
    }

    // --- Content area ---
    w->inner = Rect(Vec2(w->outer.min.x, w->outer.min.y + w->titleHeight + w->menuBarHeight), w->outer.max);
    if (w->menuBarHeight > 0.0f) {
        const Rect mb(Vec2(w->outer.min.x + 1.0f, w->outer.min.y + w->titleHeight), Vec2(w->outer.max.x - 1.0f, w->inner.min.y));
        dl.AddRect(mb, col.menuBarBg, hasTitle ? CornerRadii() : CornerRadii::Top(rounding - 1.0f));
        dl.AddRect(Rect(Vec2(mb.min.x, mb.max.y - 1.0f), mb.max), col.border);
    }
    const f32 viewH = Max(w->inner.Height(), 0.0f);
    w->scrollbarY = !HasAnyFlags(flags, WindowFlags::NoScrollbar | WindowFlags::AlwaysAutoResize) && w->contentSize.y > viewH + 0.5f;
    if (w->scrollbarY)
        w->inner.max.x -= g.S(g.style.scrollbarSize);
    w->scrollMaxY = Max(0.0f, w->contentSize.y - viewH);
    w->scroll.y = Clamp(w->scroll.y, 0.0f, w->scrollMaxY);
    w->clip = w->collapsed ? Rect(w->inner.min, w->inner.min) : w->inner;
    w->clip.min.y = Max(w->clip.min.y, w->outer.min.y + w->titleHeight + w->menuBarHeight);
    dl.PushClipRect(w->clip, 0.0f, true);
    g.BeginWindowLayout(w);
    // Menu bar layout starts in the bar.
    w->menuBarCursorX = w->outer.min.x + pad.x * 0.5f;
    return !w->collapsed && !w->inner.IsEmpty() && !w->dockHidden;
}

void End()
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    ZX_ASSERT(w && !w->isChild, "ui::End() without Begin()");
    g.EndWindowLayout(w);
    g.windowStack.PopBack();
    g.current = g.windowStack.Empty() ? nullptr : g.windowStack.Back();
}

bool BeginChild(StringView idStr, Vec2 size, bool border)
{
    Context::Impl& g = G();
    WindowState* parent = g.current;
    const ID id = g.MakeID(idStr);
    WindowState* w = g.FindOrCreateWindow(idStr, id);
    w->isChild = true;
    w->childBorder = border;
    w->parent = parent;
    w->root = parent->root;
    w->dl = parent->dl;
    w->flags = WindowFlags::NoTitleBar;
    w->lastFrameActive = g.frame;
    w->hiddenFrames = 0;
    w->titleHeight = w->menuBarHeight = 0.0f;
    const Vec2 avail(parent->workMaxX - parent->cursor.x, parent->clip.max.y - parent->cursor.y);
    Vec2 sz(size.x > 0.0f ? g.S(size.x) : (size.x < 0.0f ? avail.x + g.S(size.x) : avail.x),
            size.y > 0.0f ? g.S(size.y) : (size.y < 0.0f ? avail.y + g.S(size.y) : Max(avail.y, g.FrameHeight() * 4.0f)));
    sz = Max(Round(sz), Vec2(4.0f));
    w->pos = parent->cursor;
    w->size = sz;
    w->outer = Rect(w->pos, w->pos + sz);
    w->contentHoverable = parent->contentHoverable;
    g.windowStack.PushBack(w);
    g.current = w;
    if (border)
        w->dl->AddRect(w->outer, g.style.colors.childBg, CornerRadii(g.S(g.style.frameRounding)), 1.0f, g.style.colors.border);
    w->inner = border ? Rect(w->outer.min + Vec2(1.0f), w->outer.max - Vec2(1.0f)) : w->outer;
    const f32 viewH = w->inner.Height();
    w->scrollbarY = w->contentSize.y > viewH + 0.5f;
    if (w->scrollbarY)
        w->inner.max.x -= g.S(g.style.scrollbarSize);
    w->scrollMaxY = Max(0.0f, w->contentSize.y - viewH);
    w->scroll.y = Clamp(w->scroll.y, 0.0f, w->scrollMaxY);
    w->clip = w->inner.Intersect(parent->clip);
    w->clip.max = Max(w->clip.max, w->clip.min);
    w->dl->PushClipRect(w->inner, 0.0f, true);
    g.BeginWindowLayout(w);
    return !w->clip.IsEmpty();
}

void EndChild()
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    ZX_ASSERT(w && w->isChild, "ui::EndChild() without BeginChild()");
    g.EndWindowLayout(w);
    g.windowStack.PopBack();
    g.current = g.windowStack.Back();
    g.ItemSize(w->size);
    g.ItemAdd(w->outer, w->id);
}

Rect GetWindowRect() { return G().current->outer; }

Vec2 GetContentRegionAvail()
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    return Vec2(w->workMaxX - w->cursor.x, w->clip.max.y - w->cursor.y);
}

DrawList& GetWindowDrawList() { return *G().current->dl; }
DrawList& GetForegroundDrawList() { return G().foreground; }

void SetScrollHereY(f32 ratio)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const f32 itemY = g.lastItemRect.min.y + g.lastItemRect.Height() * ratio;
    const f32 target = itemY - w->cursorStart.y - w->inner.Height() * ratio;
    w->scroll.y = Clamp(Round(target), 0.0f, Max(w->scrollMaxY, 0.0f));
}

// --- Layout API ----------------------------------------------------------------------------------------------------------

void SameLine(f32 offsetFromStart, f32 spacing)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    if (offsetFromStart != 0.0f)
        w->cursor.x = Round(w->workMinX + g.S(offsetFromStart));
    else
        w->cursor.x = Round(w->prevLineEnd.x + (spacing < 0.0f ? g.S(g.style.itemSpacing.x) : g.S(spacing)));
    w->cursor.y = w->prevLineEnd.y;
    w->currLineHeight = w->prevLineHeight;
}

void NewLine()
{
    Context::Impl& g = G();
    g.ItemSize(Vec2(0.0f, g.LineHeight()));
}

void Spacing()
{
    Context::Impl& g = G();
    g.ItemSize(Vec2(0.0f, g.S(g.style.itemSpacing.y)));
}

void Separator()
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const f32 y = Round(w->cursor.y + g.S(2.0f));
    const Rect r(Vec2(w->workMinX + w->indent, y), Vec2(w->workMaxX, y + 1.0f));
    w->dl->AddRect(r, g.Col(g.style.colors.separator));
    g.ItemSize(Vec2(0.0f, g.S(5.0f)));
}

void SeparatorText(StringView text)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    g.fontStack.PushBack({g.fontHandles[u32(FontStyle::SemiBold)], g.S(g.style.smallFontSize)});
    const Vec2 ts = g.CalcTextSize(text);
    const f32 h = Max(ts.y, g.S(14.0f));
    const f32 y = Round(w->cursor.y + h * 0.5f);
    const f32 x0 = w->workMinX + w->indent;
    const f32 gap = g.S(6.0f);
    w->dl->AddRect(Rect(Vec2(x0, y), Vec2(x0 + g.S(8.0f), y + 1.0f)), g.Col(g.style.colors.separator));
    w->dl->AddText(g.Font(), g.FontSize(), Vec2(x0 + g.S(8.0f) + gap, w->cursor.y + (h - ts.y) * 0.5f), g.Col(g.style.colors.textDim), text);
    w->dl->AddRect(Rect(Vec2(x0 + g.S(8.0f) + gap * 2.0f + ts.x, y), Vec2(w->workMaxX, y + 1.0f)), g.Col(g.style.colors.separator));
    g.fontStack.PopBack();
    g.ItemSize(Vec2(w->workMaxX - x0, h));
}

void Dummy(Vec2 size)
{
    Context::Impl& g = G();
    const Vec2 s = g.S(size);
    g.ItemAdd(Rect(g.current->cursor, g.current->cursor + s), 0);
    g.ItemSize(s);
}

void Indent(f32 amount)
{
    Context::Impl& g = G();
    g.current->indent += g.S(amount > 0.0f ? amount : g.style.indentSpacing);
    g.current->cursor.x = Round(g.current->workMinX + g.current->indent);
}

void Unindent(f32 amount)
{
    Context::Impl& g = G();
    g.current->indent -= g.S(amount > 0.0f ? amount : g.style.indentSpacing);
    g.current->cursor.x = Round(g.current->workMinX + g.current->indent);
}

void SetNextItemWidth(f32 width) { G().current->nextItemWidth = width; }

void BeginGroup()
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    w->groups.PushBack(GroupData{w->cursor, w->cursorMax, w->workMinX, w->prevLineHeight});
    w->workMinX = w->cursor.x - w->indent;
    w->cursorMax = w->cursor;
}

void EndGroup()
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    ZX_ASSERT(!w->groups.Empty(), "EndGroup without BeginGroup");
    const GroupData gd = w->groups.PopBackValue();
    const Rect r(gd.cursorStart, Max(w->cursorMax, gd.cursorStart));
    w->workMinX = gd.workMinX;
    w->cursor = gd.cursorStart;
    w->cursorMax = Max(gd.cursorMax, r.max);
    w->currLineHeight = 0.0f;
    g.ItemSize(r.Size());
    g.ItemAdd(r, 0);
}

Vec2 GetCursorPos() { return G().current->cursor; }

void SetCursorPos(Vec2 pos)
{
    WindowState* w = G().current;
    w->cursor = pos;
    w->cursorMax = Max(w->cursorMax, pos);
}

f32 GetFrameHeight() { return G().FrameHeight(); }
f32 GetTextLineHeight() { return G().LineHeight(); }

// --- IDs, fonts, colors ------------------------------------------------------------------------------------------------------

void PushID(StringView id)
{
    Context::Impl& g = G();
    g.current->idStack.PushBack(g.MakeID(id));
}

void PushID(i32 id)
{
    Context::Impl& g = G();
    g.current->idStack.PushBack(g.MakeID(id));
}

void PushID(const void* ptr)
{
    Context::Impl& g = G();
    const uptr v = reinterpret_cast<uptr>(ptr);
    g.current->idStack.PushBack(g.MakeID(StringView(reinterpret_cast<const char*>(&v), sizeof(v))));
}

void PopID()
{
    Context::Impl& g = G();
    ZX_ASSERT(g.current->idStack.Size() > 1, "PopID without PushID");
    g.current->idStack.PopBack();
}

ID GetID(StringView label) { return G().MakeID(label); }

void PushFont(FontStyle style, f32 sizeLogical)
{
    Context::Impl& g = G();
    const f32 size = sizeLogical > 0.0f ? g.S(sizeLogical) : g.FontSize();
    g.fontStack.PushBack({g.fontHandles[u32(style)], size});
}

void PushFont(FontHandle font, f32 sizeLogical)
{
    Context::Impl& g = G();
    const f32 size = sizeLogical > 0.0f ? g.S(sizeLogical) : g.FontSize();
    g.fontStack.PushBack({font.IsValid() ? font : g.Font(), size});
}

void PopFont()
{
    Context::Impl& g = G();
    ZX_ASSERT(g.fontStack.Size() > 1, "PopFont without PushFont");
    g.fontStack.PopBack();
}

void PushTextColor(Color color) { G().textColorStack.PushBack(color); }
void PopTextColor() { G().textColorStack.PopBack(); }

void BeginDisabled(bool disabled)
{
    Context::Impl& g = G();
    g.disabledStack.PushBack(disabled);
    if (disabled)
        ++g.disabledDepth;
}

void EndDisabled()
{
    Context::Impl& g = G();
    ZX_ASSERT(!g.disabledStack.Empty(), "EndDisabled without BeginDisabled");
    if (g.disabledStack.PopBackValue())
        --g.disabledDepth;
}

// --- Queries ---------------------------------------------------------------------------------------------------------------

bool IsItemHovered()
{
    Context::Impl& g = G();
    if (g.lastItemHovered)
        return true;
    if (!g.current || !g.current->contentHoverable || g.disabledDepth > 0)
        return false;
    if (g.activeId != 0 && g.activeId != g.lastItemId)
        return false;
    return g.IsHoveringRect(g.lastItemRect);
}

bool IsItemActive()
{
    Context::Impl& g = G();
    return g.lastItemId != 0 && g.activeId == g.lastItemId;
}

bool IsItemClicked(MouseButton button)
{
    Context::Impl& g = G();
    return g.mousePressed[u32(button)] && IsItemHovered();
}

bool IsItemDoubleClicked()
{
    Context::Impl& g = G();
    return g.mouseDoubleClicked[0] && IsItemHovered();
}

bool IsItemEdited()
{
    Context::Impl& g = G();
    return g.lastItemId != 0 && g.editedId == g.lastItemId;
}

bool IsItemDeactivated()
{
    Context::Impl& g = G();
    return g.lastItemId != 0 && g.deactivatedId == g.lastItemId;
}

Rect GetItemRect() { return G().lastItemRect; }

bool IsWindowHovered()
{
    Context::Impl& g = G();
    return g.current && g.current->contentHoverable && g.current->clip.Contains(g.mouse);
}

bool IsMouseDoubleClicked(MouseButton button) { return G().mouseDoubleClicked[u32(button)]; }
Vec2 GetMousePos() { return G().mouse; }

bool IsKeyPressed(Key key, bool repeat)
{
    Context::Impl& g = G();
    const Array<Key>& keys = repeat ? g.keysPressed : g.keysPressedFirst;
    return keys.Find(key) != NPos;
}

bool IsShortcutPressed(KeyModifiers mods, Key key)
{
    Context::Impl& g = G();
    const KeyModifiers relevant = g.mods & (KeyModifiers::Control | KeyModifiers::Shift | KeyModifiers::Alt);
    return relevant == mods && g.keysPressed.Find(key) != NPos;
}

// --- Popups -------------------------------------------------------------------------------------------------------------------

void OpenPopup(StringView idStr)
{
    Context::Impl& g = G();
    const ID id = g.MakeID(idStr);
    // Popups opened from inside a popup stack above it; from a regular window they replace the stack.
    u32 level = 0;
    if (!g.popupBeginStack.Empty())
        level = g.popupBeginStack.Back() + 1;
    if (level < g.openPopups.Size() && g.openPopups[level].id == id) {
        g.openPopups[level].openFrame = g.frame;
        return;
    }
    g.ClosePopupsFrom(level);
    PopupRef p;
    p.id = id;
    p.openerId = g.lastItemId;
    p.parentWindow = g.current;
    p.openPos = g.mouse;
    p.anchor = g.lastItemRect;
    p.openFrame = g.frame;
    g.openPopups.PushBack(p);
}

bool IsPopupOpen(StringView idStr)
{
    Context::Impl& g = G();
    const ID id = g.MakeID(idStr);
    for (const PopupRef& p : g.openPopups)
        if (p.id == id)
            return true;
    return false;
}

namespace {

i32 FindPopupLevel(Context::Impl& g, ID id)
{
    const u32 level = g.popupBeginStack.Empty() ? 0 : g.popupBeginStack.Back() + 1;
    if (level < g.openPopups.Size() && g.openPopups[level].id == id)
        return i32(level);
    return -1;
}

} // namespace

bool BeginPopup(StringView idStr)
{
    Context::Impl& g = G();
    const ID id = g.MakeID(idStr);
    const i32 level = FindPopupLevel(g, id);
    if (level < 0)
        return false;
    return g.BeginPopupWindow(u32(level), WindowFlags::None, {});
}

bool BeginPopupModal(StringView title, bool* open)
{
    Context::Impl& g = G();
    const ID id = g.MakeID(title);
    const i32 level = FindPopupLevel(g, id);
    if (level < 0)
        return false;
    g.openPopups[u32(level)].isModal = true;
    if (open && !*open) {
        g.ClosePopupsFrom(u32(level));
        return false;
    }
    PopupRef& p = g.openPopups[u32(level)];
    p.isModal = true;
    // Modal windows have a title bar with a close button, unless the title is hidden ("##id").
    const bool untitled = DisplayLabel(title).Empty();
    String name = String(DisplayLabel(title)) + String::Format("##Modal_{:08x}", p.id);
    const ID windowId = HashWithSeed(name, 0);
    WindowState* w = g.FindOrCreateWindow(name, windowId);
    p.window = w;
    w->isPopup = true;
    w->isModal = true;
    w->order = PopupLayer + level;
    if (w->lastFrameActive + 1 < g.frame)
        w->hiddenFrames = 1;
    g.popupBeginStack.PushBack(u32(level));
    SetNextWindowPos(Round((g.display - w->size) * 0.5f) / g.scale, Cond::Always);
    const bool visible = Begin(name, open, WindowFlags::AlwaysAutoResize | WindowFlags::NoMove | WindowFlags::NoResize | WindowFlags::NoCollapse |
                                               (untitled ? WindowFlags::NoTitleBar : WindowFlags::None));
    if (open && !*open) {
        End();
        g.popupBeginStack.PopBack();
        g.ClosePopupsFrom(u32(level));
        return false;
    }
    if (!visible) {
        End();
        g.popupBeginStack.PopBack();
        return false;
    }
    return true;
}

bool BeginPopupContextItem(StringView idStr)
{
    Context::Impl& g = G();
    String autoId;
    if (idStr.Empty()) {
        autoId = String::Format("##ctx_{:08x}", g.lastItemId);
        idStr = autoId;
    }
    if (g.mouseReleased[1] && IsItemHovered())
        OpenPopup(idStr);
    const ID id = g.MakeID(idStr);
    const i32 level = FindPopupLevel(g, id);
    if (level >= 0)
        g.openPopups[u32(level)].isContext = true;
    return BeginPopup(idStr);
}

bool BeginPopupContextWindow(StringView idStr)
{
    Context::Impl& g = G();
    if (g.mouseReleased[1] && IsWindowHovered() && g.hotId == 0)
        OpenPopup(idStr);
    return BeginPopup(idStr);
}

void EndPopup()
{
    Context::Impl& g = G();
    ZX_ASSERT(!g.popupBeginStack.Empty(), "EndPopup without BeginPopup");
    End();
    const u32 level = g.popupBeginStack.PopBackValue();
    if (g.closePopupRequested) {
        g.closePopupRequested = false;
        g.ClosePopupsFrom(level);
    }
}

void CloseCurrentPopup()
{
    Context::Impl& g = G();
    if (!g.popupBeginStack.Empty())
        g.closePopupRequested = true;
}

// --- Tooltips -----------------------------------------------------------------------------------------------------------------

void SetTooltip(StringView text)
{
    Context::Impl& g = G();
    g.tooltipRequested = true;
    g.tooltipText = text;
}

void ItemTooltip(StringView text)
{
    Context::Impl& g = G();
    if (!IsItemHovered() || g.activeId != 0) {
        if (g.hoverTimerId == g.lastItemId)
            g.hoverTimerId = 0;
        return;
    }
    if (g.hoverTimerId != g.lastItemId) {
        g.hoverTimerId = g.lastItemId;
        g.hoverTime = 0.0f;
    }
    g.hoverTime += g.dt;
    if (g.hoverTime >= g.style.tooltipDelay)
        SetTooltip(text);
}

bool BeginTooltip()
{
    Context::Impl& g = G();
    const String name = "##Tooltip";
    const ID id = HashWithSeed(name, 0);
    WindowState* w = g.FindOrCreateWindow(name, id);
    w->isTooltip = true;
    w->isPopup = true;
    w->order = TooltipLayer;
    if (w->lastFrameActive + 1 < g.frame)
        w->hiddenFrames = 1;
    Vec2 pos = g.mouse + g.S(Vec2(14.0f, 18.0f));
    if (pos.x + w->size.x > g.display.x)
        pos.x = g.display.x - w->size.x - g.S(4.0f);
    if (pos.y + w->size.y > g.display.y)
        pos.y = g.mouse.y - w->size.y - g.S(6.0f);
    SetNextWindowPos(Max(pos, Vec2::Zero()) / g.scale, Cond::Always);
    // Tooltips may be requested from inside other windows; the caller's layout state is preserved by the stack.
    Begin(name, nullptr, WindowFlags::NoTitleBar | WindowFlags::AlwaysAutoResize | WindowFlags::NoMove | WindowFlags::NoResize);
    g.current->contentHoverable = false;
    g.tooltipBegun = true;
    return true;
}

void EndTooltip()
{
    Context::Impl& g = G();
    g.tooltipBegun = false;
    End();
}

} // namespace zx::ui
