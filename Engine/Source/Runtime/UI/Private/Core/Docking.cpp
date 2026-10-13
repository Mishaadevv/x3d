// Docking: dock spaces are trees of split nodes; leaves hold tabbed windows. Docked windows are positioned by their
// node; tabs, splitters and drop targets are drawn by the dock space's host window.

#include "Internal.h"
#include "Core/String/Parse.h"

namespace zx::ui {

namespace {

constexpr i32 DockHostLayer = -2000;
constexpr i32 DockedLayer = -1000;

DockSpaceState* FindDockSpaceOf(Context::Impl& g, DockNode* node)
{
    while (node && node->parent)
        node = node->parent;
    for (auto& kv : g.dockSpaces)
        if (kv.second->root.Get() == node)
            return kv.second.Get();
    return nullptr;
}

bool IsWindowAlive(Context::Impl& g, ID id)
{
    UniquePtr<WindowState>* w = g.windows.Find(id);
    return w && (*w)->lastFrameActive + 1 >= g.frame && (*w)->lastFrameActive != 0;
}

WindowState* FindWindow(Context::Impl& g, ID id)
{
    UniquePtr<WindowState>* w = g.windows.Find(id);
    return w ? w->Get() : nullptr;
}

bool UpdateVisibility(Context::Impl& g, DockNode* n)
{
    if (n->IsLeaf()) {
        n->visible = false;
        for (ID id : n->windows)
            n->visible |= IsWindowAlive(g, id);
        if (n->visible && !IsWindowAlive(g, n->selected)) {
            for (ID id : n->windows)
                if (IsWindowAlive(g, id)) {
                    n->selected = id;
                    break;
                }
        }
        return n->visible;
    }
    const bool a = UpdateVisibility(g, n->child[0].Get());
    const bool b = UpdateVisibility(g, n->child[1].Get());
    n->visible = a || b;
    return n->visible;
}

void Layout(Context::Impl& g, DockNode* n, const Rect& r)
{
    n->rect = r;
    if (n->IsLeaf()) {
        const f32 tabH = Round(g.FrameHeight() + g.S(2.0f));
        n->content = Rect(Vec2(r.min.x, Min(r.min.y + tabH, r.max.y)), r.max);
        return;
    }
    DockNode* a = n->child[0].Get();
    DockNode* b = n->child[1].Get();
    if (!a->visible || !b->visible) {
        Layout(g, a, a->visible ? r : Rect(r.min, r.min));
        Layout(g, b, b->visible ? r : Rect(r.min, r.min));
        return;
    }
    const f32 gap = Max(2.0f, Round(g.S(2.0f)));
    const f32 extent = n->horizontal ? r.Width() : r.Height();
    const f32 minSize = Min(g.S(60.0f), extent * 0.5f);
    f32 first = Round(Clamp(extent * n->ratio, minSize, Max(extent - minSize - gap, minSize)));
    if (n->horizontal) {
        Layout(g, a, Rect(r.min, Vec2(r.min.x + first, r.max.y)));
        Layout(g, b, Rect(Vec2(r.min.x + first + gap, r.min.y), r.max));
    } else {
        Layout(g, a, Rect(r.min, Vec2(r.max.x, r.min.y + first)));
        Layout(g, b, Rect(Vec2(r.min.x, r.min.y + first + gap), r.max));
    }
}

DockNode* LeafAt(DockNode* n, Vec2 p)
{
    if (!n->visible && n->parent)
        return nullptr;
    if (n->IsLeaf())
        return n->rect.Contains(p) ? n : nullptr;
    if (DockNode* a = LeafAt(n->child[0].Get(), p))
        return a;
    return LeafAt(n->child[1].Get(), p);
}

void UnregisterTree(Context::Impl& g, DockNode* n)
{
    g.dockNodes.Remove(n->id);
    for (UniquePtr<DockNode>& c : n->child)
        if (c)
            UnregisterTree(g, c.Get());
}

/// Removes an empty leaf: its sibling takes the parent's place.
void CollapseEmptyLeaf(Context::Impl& g, DockNode* node)
{
    DockNode* p = node->parent;
    if (!p || !node->IsLeaf() || !node->windows.Empty())
        return;
    const u32 idx = p->child[0].Get() == node ? 0 : 1;
    UniquePtr<DockNode> sibling = move(p->child[1 - idx]);
    g.dockNodes.Remove(node->id);
    p->child[idx].Reset();
    DockNode* gp = p->parent;
    sibling->parent = gp;
    g.dockNodes.Remove(p->id);
    if (gp) {
        const u32 pi = gp->child[0].Get() == p ? 0 : 1;
        gp->child[pi] = move(sibling); // destroys p
    } else {
        for (auto& kv : g.dockSpaces)
            if (kv.second->root.Get() == p) {
                kv.second->root = move(sibling);
                break;
            }
    }
}

/// Drop-target rectangles around a node's center: 0 center, 1 left, 2 right, 3 up, 4 down.
Rect TargetRect(Context::Impl& g, const DockNode* n, i32 dir)
{
    const f32 s = Round(g.S(30.0f)), step = Round(g.S(36.0f));
    const Vec2 c = Round(n->rect.Center());
    static const Vec2 Offsets[5] = {Vec2(0, 0), Vec2(-1, 0), Vec2(1, 0), Vec2(0, -1), Vec2(0, 1)};
    const Vec2 o = c + Offsets[dir] * step;
    return Rect(o - Vec2(s * 0.5f), o + Vec2(s * 0.5f));
}

Rect PreviewArea(const DockNode* n, i32 dir)
{
    const Rect r = n->rect;
    switch (dir) {
    case 1: return Rect(r.min, Vec2(r.min.x + r.Width() * 0.35f, r.max.y));
    case 2: return Rect(Vec2(r.max.x - r.Width() * 0.35f, r.min.y), r.max);
    case 3: return Rect(r.min, Vec2(r.max.x, r.min.y + r.Height() * 0.35f));
    case 4: return Rect(Vec2(r.min.x, r.max.y - r.Height() * 0.35f), r.max);
    default: return r;
    }
}

void DrawSplitters(Context::Impl& g, DockNode* n)
{
    if (n->IsLeaf())
        return;
    DockNode* a = n->child[0].Get();
    DockNode* b = n->child[1].Get();
    if (a->visible && b->visible) {
        const Rect r = n->horizontal ? Rect(Vec2(a->rect.max.x, n->rect.min.y), Vec2(b->rect.min.x, n->rect.max.y))
                                     : Rect(Vec2(n->rect.min.x, a->rect.max.y), Vec2(n->rect.max.x, b->rect.min.y));
        const f32 grow = g.S(3.0f);
        const Rect hit = n->horizontal ? Rect(r.min - Vec2(grow, 0.0f), r.max + Vec2(grow, 0.0f))
                                       : Rect(r.min - Vec2(0.0f, grow), r.max + Vec2(0.0f, grow));
        const ID id = HashWithSeed("#SPLIT", n->id);
        bool hovered, held;
        g.ButtonBehavior(hit, id, &hovered, &held);
        if (hovered || held)
            g.SetCursorShape(n->horizontal ? CursorShape::ResizeEW : CursorShape::ResizeNS);
        if (held) {
            const f32 extent = n->horizontal ? n->rect.Width() : n->rect.Height();
            const f32 pos = (n->horizontal ? g.mouse.x - n->rect.min.x : g.mouse.y - n->rect.min.y);
            n->ratio = Clamp(pos / Max(extent, 1.0f), 0.05f, 0.95f);
        }
        if (hovered || held)
            g.current->dl->AddRect(r, g.style.colors.accent);
    }
    DrawSplitters(g, a);
    DrawSplitters(g, b);
}

void DrawTabBars(Context::Impl& g, DockSpaceState& ds, DockNode* n)
{
    if (!n->visible)
        return;
    if (!n->IsLeaf()) {
        DrawTabBars(g, ds, n->child[0].Get());
        DrawTabBars(g, ds, n->child[1].Get());
        return;
    }
    const Theme& c = g.style.colors;
    DrawList& dl = *g.current->dl;
    const Rect bar(n->rect.min, Vec2(n->rect.max.x, n->content.min.y));
    dl.AddRect(bar, c.titleBg);
    dl.PushClipRect(bar);
    const f32 padX = Round(g.S(12.0f));
    const f32 closeW = Round(g.S(16.0f));
    const f32 rr = g.S(3.0f);
    f32 x = bar.min.x + g.S(2.0f);
    for (usize i = 0; i < n->windows.Size(); ++i) {
        const ID wid = n->windows[i];
        WindowState* w = FindWindow(g, wid);
        if (!w || !IsWindowAlive(g, wid))
            continue;
        const StringView label = DisplayLabel(w->name.View());
        const f32 width = Round(g.CalcTextSize(label).x + padX * 2.0f + (w->hasCloseButton ? closeW : 0.0f));
        const Rect tab(Vec2(x, bar.min.y + g.S(3.0f)), Vec2(x + width, bar.max.y));
        x += width + g.S(1.0f);
        const ID tabId = HashWithSeed("#TAB", wid);
        g.ItemAdd(tab, tabId);
        bool hovered, held;
        g.ButtonBehavior(tab, tabId, &hovered, &held);
        if (held && g.activeIdJustActivated) {
            n->selected = wid;
            g.focusedRoot = w;
        }
        const bool selected = n->selected == wid;
        const bool focused = g.focusedRoot == w;
        if (selected) {
            dl.AddRect(tab, c.windowBg, CornerRadii::Top(rr));
            if (focused)
                dl.AddRect(Rect(tab.min, Vec2(tab.max.x, tab.min.y + Max(2.0f, Round(g.S(2.0f))))), c.accent, CornerRadii::Top(rr));
        } else if (hovered) {
            dl.AddRect(tab, c.tabHovered, CornerRadii::Top(rr));
        }
        dl.AddTextInRect(g.Font(), g.FontSize(), Rect(Vec2(tab.min.x + padX, tab.min.y), Vec2(tab.max.x - padX * 0.5f - (w->hasCloseButton ? closeW : 0.0f), tab.max.y)),
                         selected ? c.text : (hovered ? c.text : c.textDim), label);
        if (w->hasCloseButton && (selected || hovered)) {
            const Rect cr(Vec2(tab.max.x - padX * 0.5f - closeW, tab.min.y + (tab.Height() - closeW) * 0.5f),
                          Vec2(tab.max.x - padX * 0.5f, tab.min.y + (tab.Height() + closeW) * 0.5f));
            const ID cid = HashWithSeed("#TABCLOSE", wid);
            bool ch, cheld;
            if (g.ButtonBehavior(cr, cid, &ch, &cheld))
                w->closeRequested = true;
            if (ch)
                dl.AddRect(cr, c.buttonHovered, CornerRadii(g.S(3.0f)));
            g.PushFontSmall();
            dl.AddTextInRect(g.Font(), g.FontSize(), cr, ch ? c.text : c.textDim, ZX_ICON_X, TextAlign::Center, false);
            g.fontStack.PopBack();
        }
        // Drag a tab out of the bar -> floating window that keeps following the mouse.
        if (held && !g.activeIdJustActivated) {
            const Vec2 d = g.mouse - g.mouseClickPos[0];
            const bool outside = !bar.Expanded(g.S(14.0f)).Contains(g.mouse);
            if (outside && LengthSq(d) > g.S(8.0f) * g.S(8.0f)) {
                const Vec2 size(Max(n->content.Width() * 0.6f, g.S(320.0f)), Max(n->content.Height() * 0.6f, g.S(220.0f)));
                g.UndockWindow(w);
                w->pos = g.mouse - g.S(Vec2(60.0f, 14.0f));
                w->size = Round(Min(size, g.display * 0.8f));
                g.ClearActive();
                g.SetActive(HashWithSeed("#MOVE", w->id), w);
                g.movingWindow = w;
                g.focusedRoot = w;
                g.BringToFront(w);
                break;
            }
        }
    }
    dl.PopClipRect();
}

} // namespace

DockNode* Context::Impl::NewDockNode(DockSpaceState& ds, DockNode* parent)
{
    UniquePtr<DockNode> owned(new DockNode());
    ID id;
    do {
        id = HashWithSeed(String::Format("{}#{}", ds.name, ds.nextNode++), ds.id);
    } while (dockNodes.Contains(id));
    owned->id = id;
    owned->parent = parent;
    DockNode* raw = owned.Release();
    dockNodes.Insert(ID(id), raw);
    return raw;
}

void Context::Impl::UndockWindow(WindowState* w)
{
    DockNode* node = w->dockNodeId ? FindDockNode(w->dockNodeId) : nullptr;
    w->dockNodeId = 0;
    w->dockHidden = false;
    if (!node)
        return;
    node->windows.Remove(w->id);
    if (node->selected == w->id)
        node->selected = node->windows.Empty() ? 0 : node->windows[0];
    CollapseEmptyLeaf(*this, node);
}

void Context::Impl::DockWindowInto(WindowState* w, DockNode* node, i32 dir)
{
    UndockWindow(w);
    if (dir <= 0 || (node->windows.Empty() && !node->parent && node->IsLeaf())) {
        node->windows.AddUnique(w->id);
        node->selected = w->id;
        w->dockNodeId = node->id;
        return;
    }
    static const DockDir Dirs[5] = {DockDir::Left, DockDir::Left, DockDir::Right, DockDir::Up, DockDir::Down};
    const ID created = DockSplitNode(node->id, Dirs[dir], 0.35f, nullptr);
    DockNode* target = FindDockNode(created);
    target->windows.PushBack(w->id);
    target->selected = w->id;
    w->dockNodeId = target->id;
}

ID DockSpace(StringView name, const Rect& rect)
{
    Context::Impl& g = G();
    const ID id = HashWithSeed(name, 0xD0C5u);
    UniquePtr<DockSpaceState>* found = g.dockSpaces.Find(id);
    if (!found) {
        UniquePtr<DockSpaceState> ds(new DockSpaceState());
        ds->id = id;
        ds->name = name;
        found = g.dockSpaces.Insert(ID(id), move(ds)).first;
    }
    DockSpaceState& ds = **found;
    if (!ds.root)
        ds.root.Reset(g.NewDockNode(ds, nullptr));
    ds.rect = rect;
    ds.lastFrame = g.frame;

    // Drop a window released over a target last frame.
    if (g.droppedWindow && ds.previewNode && ds.previewDir >= 0) {
        if (DockNode* target = g.FindDockNode(ds.previewNode); target && target->IsLeaf())
            g.DockWindowInto(g.droppedWindow, target, ds.previewDir);
    }
    ds.previewNode = 0;
    ds.previewDir = -1;

    UpdateVisibility(g, ds.root.Get());
    Layout(g, ds.root.Get(), rect);

    // Host window: background, splitters, tab bars.
    SetNextWindowPos(rect.min / g.scale, Cond::Always);
    SetNextWindowSize(rect.Size() / g.scale, Cond::Always);
    Begin(String::Format("##DockHost_{}", name), nullptr,
          WindowFlags::NoTitleBar | WindowFlags::NoResize | WindowFlags::NoMove | WindowFlags::NoScrollbar | WindowFlags::NoCollapse |
              WindowFlags::NoBringToFront | WindowFlags::NoBackground);
    WindowState* host = g.current;
    host->order = DockHostLayer;
    host->hiddenFrames = 0;
    host->dl->PopClipRect(); // draw over the whole host rect (Begin pushed the content clip)
    host->dl->PushClipRect(rect, 0.0f, false);
    const Rect savedClip = host->clip;
    host->clip = rect;
    host->dl->AddRect(rect, g.style.colors.titleBg);
    if (!ds.root->visible) {
        g.PushFontSmall();
        host->dl->AddTextInRect(g.Font(), g.FontSize(), rect, g.style.colors.textDisabled, "Drop a panel here", TextAlign::Center, false);
        g.fontStack.PopBack();
    }
    DrawSplitters(g, ds.root.Get());
    DrawTabBars(g, ds, ds.root.Get());
    host->clip = savedClip;
    End();

    // Drop targets while a floating window is dragged over this dock space.
    WindowState* moving = g.movingWindow;
    if (moving && !moving->dockNodeId && rect.Contains(g.mouse)) {
        DockNode* leaf = ds.root->visible ? LeafAt(ds.root.Get(), g.mouse) : ds.root.Get();
        if (leaf) {
            const Theme& c = g.style.colors;
            const i32 dirs = ds.root->visible ? 5 : 1;
            for (i32 d = 0; d < dirs; ++d) {
                const Rect t = TargetRect(g, leaf, d);
                const bool hot = t.Expanded(g.S(3.0f)).Contains(g.mouse);
                if (hot) {
                    ds.previewNode = leaf->id;
                    ds.previewDir = d;
                    g.foreground.AddRect(PreviewArea(leaf, d), WithAlpha(c.accent, 0.22f), CornerRadii(g.S(4.0f)), 2.0f, c.accent);
                }
                g.foreground.AddShadow(t, Color(0, 0, 0, 120), g.S(4.0f), g.S(6.0f));
                g.foreground.AddRect(t, hot ? c.accent : c.popupBg, CornerRadii(g.S(4.0f)), 1.0f, c.accent);
                // Glyph showing where the window goes.
                const Rect inner(t.min + Vec2(g.S(6.0f)), t.max - Vec2(g.S(6.0f)));
                Rect part = inner;
                if (d == 1) part.max.x = inner.min.x + inner.Width() * 0.45f;
                if (d == 2) part.min.x = inner.max.x - inner.Width() * 0.45f;
                if (d == 3) part.max.y = inner.min.y + inner.Height() * 0.45f;
                if (d == 4) part.min.y = inner.max.y - inner.Height() * 0.45f;
                g.foreground.AddRectOutline(inner, hot ? c.accentText : c.accent, 1.0f, CornerRadii(g.S(2.0f)));
                g.foreground.AddRect(part, WithAlpha(hot ? c.accentText : c.accent, 0.7f), CornerRadii(g.S(2.0f)));
            }
        }
    }
    return ds.root->id;
}

bool DockSpaceHasLayout(StringView name)
{
    Context::Impl& g = G();
    UniquePtr<DockSpaceState>* found = g.dockSpaces.Find(HashWithSeed(name, 0xD0C5u));
    if (!found || !(*found)->root)
        return false;
    const DockNode* r = (*found)->root.Get();
    return !r->IsLeaf() || !r->windows.Empty();
}

ID DockSplitNode(ID nodeId, DockDir dir, f32 ratio, ID* remaining)
{
    Context::Impl& g = G();
    DockNode* node = g.FindDockNode(nodeId);
    ZX_ASSERT(node && node->IsLeaf(), "DockSplitNode: unknown node or not a leaf");
    DockSpaceState* ds = FindDockSpaceOf(g, node);
    ZX_ASSERT(ds, "DockSplitNode: node without dock space");
    DockNode* a = g.NewDockNode(*ds, node);
    DockNode* b = g.NewDockNode(*ds, node);
    const bool first = dir == DockDir::Left || dir == DockDir::Up;
    DockNode* created = first ? a : b;
    DockNode* rest = first ? b : a;
    rest->windows = move(node->windows);
    rest->selected = node->selected;
    node->windows.Clear();
    node->selected = 0;
    for (ID wid : rest->windows)
        if (WindowState* w = FindWindow(g, wid))
            w->dockNodeId = rest->id;
    node->horizontal = dir == DockDir::Left || dir == DockDir::Right;
    node->ratio = first ? ratio : 1.0f - ratio;
    node->child[0].Reset(a);
    node->child[1].Reset(b);
    if (remaining)
        *remaining = rest->id;
    return created->id;
}

void DockWindow(StringView title, ID nodeId)
{
    Context::Impl& g = G();
    DockNode* node = g.FindDockNode(nodeId);
    ZX_ASSERT(node && node->IsLeaf(), "DockWindow: unknown node or not a leaf");
    const ID wid = HashWithSeed(title, 0);
    WindowState* w = g.FindOrCreateWindow(title, wid);
    if (w->name.Empty())
        w->name = title;
    g.UndockWindow(w);
    node->windows.AddUnique(wid);
    if (!node->selected)
        node->selected = wid;
    w->dockNodeId = node->id;
}

void FocusDockedWindow(StringView title)
{
    Context::Impl& g = G();
    WindowState* w = FindWindow(g, HashWithSeed(title, 0));
    if (!w || !w->dockNodeId)
        return;
    if (DockNode* n = g.FindDockNode(w->dockNodeId))
        n->selected = w->id;
    g.focusedRoot = w;
}

// --- Layout persistence ------------------------------------------------------------------------------------------------
// dockspace <name>
// node <id> <parent id|0> <horizontal 0/1> <ratio> <selected window id|0>     (pre-order; children follow parents)
// tab <node id> <window id> <window title>

namespace {

void SaveNode(Context::Impl& g, const DockNode* n, String& out)
{
    out += String::Format("node {:08x} {:08x} {} {:.4f} {:08x}\n", n->id, n->parent ? n->parent->id : 0u, n->horizontal ? 1 : 0, n->ratio,
                          n->selected);
    for (ID wid : n->windows)
        if (WindowState* w = FindWindow(g, wid))
            out += String::Format("tab {:08x} {:08x} {}\n", n->id, wid, w->name);
    for (const UniquePtr<DockNode>& c : n->child)
        if (c)
            SaveNode(g, c.Get(), out);
}

u32 ParseHex(StringView s)
{
    u32 v = 0;
    for (char ch : s) {
        v <<= 4;
        if (ch >= '0' && ch <= '9')
            v |= u32(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            v |= u32(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F')
            v |= u32(ch - 'A' + 10);
    }
    return v;
}

} // namespace

String SaveDockLayout()
{
    Context::Impl& g = G();
    String out;
    for (auto& kv : g.dockSpaces) {
        const DockSpaceState& ds = *kv.second;
        out += String::Format("dockspace {}\n", ds.name);
        if (ds.root)
            SaveNode(g, ds.root.Get(), out);
    }
    return out;
}

bool LoadDockLayout(StringView text)
{
    Context::Impl& g = G();
    for (auto& kv : g.windows)
        kv.second->dockNodeId = 0;
    for (auto& kv : g.dockSpaces)
        if (kv.second->root)
            UnregisterTree(g, kv.second->root.Get());
    g.dockSpaces.Clear();
    g.dockNodes.Clear();
    DockSpaceState* ds = nullptr;
    StringView rest = text;
    bool ok = true;
    while (!rest.Empty()) {
        StringView line;
        rest.Split('\n', line, rest); // no separator: line = remainder, rest = empty
        line = line.Trim();
        if (line.Empty())
            continue;
        StringView cmd, args;
        if (!line.Split(' ', cmd, args))
            continue;
        if (cmd == "dockspace") {
            UniquePtr<DockSpaceState> s(new DockSpaceState());
            s->name = args;
            s->id = HashWithSeed(args, 0xD0C5u);
            s->nextNode = 1000;
            ds = g.dockSpaces.Insert(ID(s->id), move(s)).first->Get();
        } else if ((cmd == "node" || cmd == "tab") && !ds) {
            ok = false; // nodes must follow a dockspace line
        } else if (cmd == "node") {
            StringView idS, parentS, horizS, ratioS, selS;
            StringView r = args;
            r.Split(' ', idS, r);
            r.Split(' ', parentS, r);
            r.Split(' ', horizS, r);
            r.Split(' ', ratioS, selS);
            UniquePtr<DockNode> owned(new DockNode());
            DockNode* n = owned.Get();
            n->id = ParseHex(idS);
            n->horizontal = horizS == "1";
            n->ratio = ParseFloat(ratioS).ValueOr(0.5f);
            n->selected = ParseHex(selS);
            const ID parentId = ParseHex(parentS);
            if (parentId == 0) {
                ds->root = move(owned);
            } else {
                DockNode* parent = g.FindDockNode(parentId);
                if (!parent) {
                    ok = false;
                    continue;
                }
                n->parent = parent;
                (parent->child[0] ? parent->child[1] : parent->child[0]) = move(owned);
            }
            g.dockNodes.Insert(ID(n->id), n);
        } else if (cmd == "tab") {
            StringView nodeS, winS, title;
            StringView r = args;
            r.Split(' ', nodeS, r);
            r.Split(' ', winS, title);
            DockNode* n = g.FindDockNode(ParseHex(nodeS));
            if (!n) {
                ok = false;
                continue;
            }
            const ID wid = ParseHex(winS);
            WindowState* w = g.FindOrCreateWindow(title, wid);
            if (w->name.Empty())
                w->name = title;
            n->windows.AddUnique(wid);
            w->dockNodeId = n->id;
        }
    }
    // Internal nodes must have two children; anything malformed falls back to an empty layout.
    for (auto& kv : g.dockNodes) {
        DockNode* n = kv.second;
        if ((n->child[0] && !n->child[1]) || (!n->child[0] && n->child[1]))
            ok = false;
    }
    if (!ok) {
        for (auto& kv : g.windows)
            kv.second->dockNodeId = 0;
        g.dockSpaces.Clear();
        g.dockNodes.Clear();
    }
    return ok;
}

Rect GetWindowContentRect() { return G().current->inner; }

} // namespace zx::ui
