// UI Designer tool (U): builds game UI (UI Canvases) with the mouse in the viewport - click a widget to select it,
// drag it to move (anchored widgets) or to reorder (widgets in a layout), drag the handles to resize; a preview frame
// shows other screen shapes. Every drag is one undo step.

#include "EditorTools.h"
#include "Editor.h"
#include "Core/Containers/Algorithm.h"
#include "UI/UI.h"

namespace zx::editor {

namespace {

struct PreviewPreset
{
    const char* name;
    f32 width, height; // 0 = the viewport
};
const PreviewPreset Previews[] = {{"Viewport", 0, 0},          {"16:9  (1920 x 1080)", 1920, 1080}, {"21:9  (2560 x 1080)", 2560, 1080},
                                  {"4:3  (1024 x 768)", 1024, 768}, {"16:10  (1680 x 1050)", 1680, 1050}, {"9:16 phone  (1080 x 1920)", 1080, 1920}};

bool IsCanvasRoot(const ecs::World& world, ecs::Entity e) { return world.Has<UICanvas>(e); }

/// Canvases in drawing order.
void CanvasesInOrder(ecs::World& world, Array<ecs::Entity>& out)
{
    out.Clear();
    world.Query<const UICanvas>().Each([&](ecs::Entity e, const UICanvas&) { out.PushBack(e); });
    StableSort(out, [&](ecs::Entity a, ecs::Entity b) {
        const UICanvas& ca = *world.Get<const UICanvas>(a);
        const UICanvas& cb = *world.Get<const UICanvas>(b);
        if (ca.worldSpace != cb.worldSpace)
            return ca.worldSpace;
        return ca.order < cb.order;
    });
}

/// The UI elements under `p`, from the canvas down to the innermost one (the topmost canvas that has any).
void PickChain(ecs::World& world, Vec2 p, Array<ecs::Entity>& chain)
{
    chain.Clear();
    Array<ecs::Entity> path;
    Function<void(ecs::Entity)> visit = [&](ecs::Entity e) {
        const UIElement* el = world.Get<const UIElement>(e);
        if (!el || !el->visible)
            return;
        const bool inside = el->rect.Contains(p);
        path.PushBack(e);
        if (inside)
            chain = path; // later (drawn on top) wins
        if (!(el->clip || el->scroll) || inside)
            for (ecs::Entity c : scene::GetChildren(world, e))
                visit(c);
        path.PopBack();
    };
    Array<ecs::Entity> canvases;
    CanvasesInOrder(world, canvases);
    for (ecs::Entity c : canvases)
        if (const UICanvas* cv = world.Get<const UICanvas>(c); cv && cv->onScreen)
            visit(c);
}

/// What a click picks (like Figma): the widget right under the canvas; inside the selected widget - the selection
/// (double-click: one level deeper); a sibling of the selection - that sibling; Ctrl - the innermost widget.
ecs::Entity PickTarget(ecs::World& world, const Array<ecs::Entity>& chain, ecs::Entity sel, bool deepest, bool deeper)
{
    if (chain.Empty())
        return {};
    if (deepest)
        return chain.Back();
    const usize at = sel ? chain.Find(sel) : NPos;
    if (at != NPos)
        return deeper && at + 1 < chain.Size() ? chain[at + 1] : sel;
    if (const ecs::Entity parent = sel ? scene::GetParent(world, sel) : ecs::Entity()) {
        const usize p = chain.Find(parent);
        if (p != NPos && p + 1 < chain.Size())
            return chain[p + 1];
    }
    return chain.Size() > 1 ? chain[1] : chain[0];
}

/// Pixels per UI unit of the parent's layout (offsets and sizes are in these units).
f32 LayoutScale(const ecs::World& world, ecs::Entity e)
{
    const UIElement& el = *world.Get<const UIElement>(e);
    const f32 own = Max(el.shownScale * el.animScale, 1e-3f);
    return Max(el.pixelScale / own, 1e-4f);
}

/// Laid-out siblings (not anchored, visible) of `e`.
void FlowSiblings(const ecs::World& world, ecs::Entity e, Array<ecs::Entity>& out)
{
    out.Clear();
    const ecs::Entity parent = scene::GetParent(world, e);
    for (ecs::Entity c : scene::GetChildren(world, parent))
        if (const UIElement* el = world.Get<const UIElement>(c); el && el->visible && !el->absolute && c != e)
            out.PushBack(c);
}

/// Where `e` would go among its laid-out siblings for the mouse at `p` (index into FlowSiblings).
u32 InsertionIndex(const ecs::World& world, ecs::Entity e, Vec2 p, const Array<ecs::Entity>& siblings)
{
    const UIElement* parent = world.Get<const UIElement>(scene::GetParent(world, e));
    const bool row = parent && parent->direction == UIDirection::Row;
    u32 index = 0;
    for (ecs::Entity s : siblings) {
        const Rect r = world.Get<const UIElement>(s)->rect;
        if ((row ? p.x : p.y) > (row ? r.Center().x : r.Center().y))
            ++index;
    }
    return index;
}

f32 Snap(f32 v, const ToolState& ts, bool free) { return ts.uiSnap && !free ? Round(v / Max(ts.uiGrid, 0.5f)) * Max(ts.uiGrid, 0.5f) : Round(v * 10.0f) / 10.0f; }

/// Moves `e` among its siblings so it comes before laid-out sibling `index` (or after the last one). Undoable.
void Reorder(Editor& ed, ecs::Entity e, u32 index, const Array<ecs::Entity>& siblings, StringView label)
{
    const ecs::Entity parent = scene::GetParent(ed.world, e);
    scene::Children* children = parent ? ed.world.Get<scene::Children>(parent) : nullptr;
    if (!children)
        return;
    ed.undo.Begin(label);
    for (ecs::Entity c : children->entities)
        ed.undo.Record(c);
    children->entities.RemoveAt(children->entities.Find(e));
    usize at = children->entities.Size();
    if (index < siblings.Size())
        at = children->entities.Find(siblings[index]);
    else if (!siblings.Empty())
        at = children->entities.Find(siblings.Back()) + 1;
    children->entities.Insert(Min(at, children->entities.Size()), e);
    ed.undo.End();
}

} // namespace

Rect UIDesignerScreen(Editor& ed, const Rect& viewport)
{
    const ToolState& ts = ed.tools;
    if (ts.active != EditorTool::UIDesigner || ed.IsPlaying() || ts.uiPreview == 0 || ts.uiPreview >= ZX_ARRAY_COUNT(Previews))
        return viewport;
    const PreviewPreset& p = Previews[ts.uiPreview];
    const f32 margin = 24.0f;
    const f32 k = Min((viewport.Width() - margin * 2.0f) / p.width, (viewport.Height() - margin * 2.0f) / p.height);
    const Vec2 size = Round(Vec2(p.width, p.height) * Max(k, 0.01f));
    const Vec2 min = Round(viewport.Center() - size * 0.5f);
    return Rect(min, min + size);
}

bool UpdateUIDesigner(Editor& ed, const Rect& viewport, const Rect& screen, bool hovered, ui::DrawList& dl, f32 s)
{
    ToolState& ts = ed.tools;
    if (ts.active != EditorTool::UIDesigner || ed.IsPlaying())
        return false;
    const ui::Theme& theme = ui::GetCurrentContext()->GetStyle().colors;
    const ui::FontHandle font = ui::GetCurrentContext()->Fonts().FindFont("Inter SemiBold");
    // Preview frame: the screen shape being designed for.
    if (screen != viewport) {
        const Color dim(0, 0, 0, 150);
        dl.AddRect(Rect(viewport.min, Vec2(viewport.max.x, screen.min.y)), dim);
        dl.AddRect(Rect(Vec2(viewport.min.x, screen.max.y), viewport.max), dim);
        dl.AddRect(Rect(Vec2(viewport.min.x, screen.min.y), Vec2(screen.min.x, screen.max.y)), dim);
        dl.AddRect(Rect(Vec2(screen.max.x, screen.min.y), Vec2(viewport.max.x, screen.max.y)), dim);
        dl.AddRectOutline(screen, Color(255, 255, 255, 90), 1.0f);
        dl.AddTextInRect(font, 11.0f * s, Rect(screen.min - Vec2(0.0f, 18.0f * s), Vec2(screen.max.x, screen.min.y - 2.0f * s)), Color(255, 255, 255, 150),
                         Previews[ts.uiPreview].name);
    }
    const Vec2 mouse = ui::GetMousePos();
    const bool alt = HasAnyFlags(ed.input.Modifiers(), KeyModifiers::Alt); // Alt: no snapping
    ecs::Entity sel = ed.world.IsAlive(ed.selected) && ed.world.Has<UIElement>(ed.selected) ? ed.selected : ecs::Entity();
    if (ts.uiDragging && (!sel || sel != ts.uiDragged)) {
        ed.undo.End();
        ts.uiDragging = false;
    }
    const bool canEdit = sel && !IsCanvasRoot(ed.world, sel);

    // Handles: 0..7 corners and edges clockwise from the top left, 8 = the body.
    static const Vec2 HandleDirs[8] = {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}};
    auto handlePos = [](const Rect& r, u32 h) {
        return Vec2(Lerp(r.min.x, r.max.x, HandleDirs[h].x * 0.5f + 0.5f), Lerp(r.min.y, r.max.y, HandleDirs[h].y * 0.5f + 0.5f));
    };
    i32 hot = -1;
    if (!ts.uiDragging && hovered && canEdit) {
        const Rect r = ed.world.Get<const UIElement>(sel)->rect;
        for (u32 h = 0; h < 8 && hot < 0; ++h)
            if (Length(mouse - handlePos(r, h)) < 7.0f * s)
                hot = i32(h);
    }
    Array<ecs::Entity> chain;
    if (hovered && !ts.uiDragging)
        PickChain(ed.world, mouse, chain);
    const bool ctrl = HasAnyFlags(ed.input.Modifiers(), KeyModifiers::Control);
    const bool dbl = ui::IsMouseDoubleClicked(MouseButton::Left);
    ts.uiHover = PickTarget(ed.world, chain, sel, ctrl, false);
    if (hot < 0 && canEdit && !ts.uiDragging && ts.uiHover == sel && !dbl && !ctrl)
        hot = 8; // the body of the selection

    // Press: grab a handle of the selection, or select what's under the mouse and grab it.
    if (hovered && ed.input.WasMousePressed(MouseButton::Left)) {
        if (hot < 0) {
            const ecs::Entity target = PickTarget(ed.world, chain, sel, ctrl, dbl);
            ed.Select(target);
            sel = target;
            hot = sel && !IsCanvasRoot(ed.world, sel) ? 8 : -1;
        }
        if (hot >= 0 && sel) {
            const UIElement& el = *ed.world.Get<const UIElement>(sel);
            ts.uiDragging = true;
            ts.uiDragged = sel;
            ts.uiHandle = hot;
            ts.uiPressMouse = mouse;
            ts.uiStartRect = el.rect;
            ts.uiStartOffset = el.offset;
            ts.uiStartSize = Vec2(el.size.x > 0.0f ? el.size.x : el.measured.x, el.size.y > 0.0f ? el.size.y : el.measured.y);
            ts.uiMoved = false;
            ts.uiInsert = -1;
            ed.undo.Begin(hot == 8 ? "Move UI Element" : "Resize UI Element");
            ed.undo.Record(sel);
        }
    }
    // Drag.
    Array<ecs::Entity> siblings;
    if (ts.uiDragging && sel) {
        UIElement& el = *ed.world.Get<UIElement>(sel);
        const Vec2 d = mouse - ts.uiPressMouse;
        if (Length(d) > 3.0f * s)
            ts.uiMoved = true;
        const f32 ps = LayoutScale(ed.world, sel);
        if (ts.uiMoved) {
            if (ts.uiHandle == 8) {
                if (el.absolute) {
                    el.offset = Vec2(Snap(ts.uiStartOffset.x + d.x / ps, ts, alt), Snap(ts.uiStartOffset.y + d.y / ps, ts, alt));
                } else {
                    FlowSiblings(ed.world, sel, siblings);
                    ts.uiInsert = i32(InsertionIndex(ed.world, sel, mouse, siblings));
                }
            } else {
                // Resize: the dragged edges follow the mouse, the opposite ones stay.
                const Vec2 dir = HandleDirs[ts.uiHandle];
                Rect r = ts.uiStartRect;
                const f32 minPx = 4.0f * s;
                if (dir.x < 0)
                    r.min.x = Min(r.min.x + d.x, r.max.x - minPx);
                if (dir.x > 0)
                    r.max.x = Max(r.max.x + d.x, r.min.x + minPx);
                if (dir.y < 0)
                    r.min.y = Min(r.min.y + d.y, r.max.y - minPx);
                if (dir.y > 0)
                    r.max.y = Max(r.max.y + d.y, r.min.y + minPx);
                const f32 own = Max(el.pixelScale, 1e-4f);
                Vec2 size = el.size;
                if (dir.x != 0)
                    size.x = Max(Snap(ts.uiStartSize.x + (r.Width() - ts.uiStartRect.Width()) / own, ts, alt), 1.0f);
                else if (size.x <= 0.0f && dir.y != 0)
                    size.x = 0.0f; // height only: the width keeps fitting its content
                if (dir.y != 0)
                    size.y = Max(Snap(ts.uiStartSize.y + (r.Height() - ts.uiStartRect.Height()) / own, ts, alt), 1.0f);
                el.size = size;
                if (el.absolute) {
                    // Keep the pivot where the resized box puts it (the opposite edge stays put).
                    const Vec2 startSize = ts.uiStartRect.Size();
                    const Vec2 newSize = Vec2(dir.x != 0 ? (el.size.x * own) : startSize.x, dir.y != 0 ? (el.size.y * own) : startSize.y);
                    const Vec2 newMin(dir.x < 0 ? ts.uiStartRect.max.x - newSize.x : ts.uiStartRect.min.x,
                                      dir.y < 0 ? ts.uiStartRect.max.y - newSize.y : ts.uiStartRect.min.y);
                    const Vec2 pivotStart = ts.uiStartRect.min + Vec2(startSize.x * el.pivot.x, startSize.y * el.pivot.y);
                    const Vec2 pivotNew = newMin + Vec2(newSize.x * el.pivot.x, newSize.y * el.pivot.y);
                    el.offset = ts.uiStartOffset + (pivotNew - pivotStart) / ps;
                }
            }
        }
        if (!ed.input.IsMouseDown(MouseButton::Left)) {
            const bool reorder = ts.uiHandle == 8 && !el.absolute && ts.uiMoved && ts.uiInsert >= 0;
            ed.undo.End();
            ts.uiDragging = false;
            if (reorder) {
                FlowSiblings(ed.world, sel, siblings);
                Reorder(ed, sel, u32(ts.uiInsert), siblings, "Reorder UI Element");
            }
            ts.uiInsert = -1;
        }
    }
    // Arrow keys nudge anchored widgets by the grid (Shift: x10).
    if (canEdit && !ts.uiDragging && hovered && ed.world.Get<const UIElement>(sel)->absolute) {
        Vec2 nudge;
        if (ed.input.WasKeyPressed(Key::Left))
            nudge.x = -1.0f;
        if (ed.input.WasKeyPressed(Key::Right))
            nudge.x = 1.0f;
        if (ed.input.WasKeyPressed(Key::Up))
            nudge.y = -1.0f;
        if (ed.input.WasKeyPressed(Key::Down))
            nudge.y = 1.0f;
        if (nudge.x != 0.0f || nudge.y != 0.0f) {
            const f32 step = Max(ts.uiGrid, 1.0f) * (HasAnyFlags(ed.input.Modifiers(), KeyModifiers::Shift) ? 10.0f : 1.0f);
            ed.undo.Begin("Nudge UI Element");
            ed.undo.Record(sel);
            ed.world.Get<UIElement>(sel)->offset += nudge * step;
            ed.undo.End();
        }
    }

    // Overlay.
    const Color accent = theme.accentHovered;
    if (ts.uiHover && ts.uiHover != sel)
        if (const UIElement* h = ed.world.Get<const UIElement>(ts.uiHover))
            dl.AddRectOutline(h->rect, WithAlpha(accent, 0.55f), 1.0f);
    if (sel) {
        const UIElement& el = *ed.world.Get<const UIElement>(sel);
        const Rect r = el.rect;
        dl.AddRectOutline(r, accent, 2.0f);
        if (canEdit) {
            for (u32 h = 0; h < 8; ++h) {
                const Vec2 c = handlePos(r, h);
                const f32 k = (i32(h) == hot || (ts.uiDragging && ts.uiHandle == i32(h))) ? 5.0f * s : 4.0f * s;
                dl.AddRect(Rect(c - Vec2(k), c + Vec2(k)), Color(255, 255, 255), ui::CornerRadii(1.5f * s), 1.0f * s, accent);
            }
            // Anchor of an anchored widget: a diamond on the parent.
            if (el.absolute)
                if (const UIElement* parent = ed.world.Get<const UIElement>(scene::GetParent(ed.world, sel))) {
                    const f32 ps = parent->pixelScale;
                    const Rect inner(parent->rect.min + Vec2(parent->padding.x, parent->padding.y) * ps,
                                     parent->rect.max - Vec2(parent->padding.z, parent->padding.w) * ps);
                    const Vec2 a(Lerp(inner.min.x, inner.max.x, el.anchor.x), Lerp(inner.min.y, inner.max.y, el.anchor.y));
                    const f32 k = 6.0f * s;
                    const Vec2 pts[4] = {a + Vec2(0, -k), a + Vec2(k, 0), a + Vec2(0, k), a + Vec2(-k, 0)};
                    dl.AddPolyline(Span<const Vec2>(pts, 4), Color(255, 200, 80), 1.5f * s, true);
                    const Vec2 pivot = r.min + Vec2(r.Width() * el.pivot.x, r.Height() * el.pivot.y);
                    dl.AddLine(a, pivot, Color(255, 200, 80, 120), 1.0f * s);
                }
        }
        // Name and size (UI units) above the box.
        const scene::EntityName* n = ed.world.Get<const scene::EntityName>(sel);
        const f32 own = Max(el.pixelScale, 1e-4f);
        const String label = String::Format("{}   {} x {}", n ? n->value.View() : StringView("UI Element"), i32(Round(r.Width() / own)), i32(Round(r.Height() / own)));
        const f32 w = ui::GetCurrentContext()->Fonts().MeasureWidth(font, 11.0f * s, label) + 14.0f * s;
        const Vec2 at(Clamp(r.min.x, viewport.min.x, Max(viewport.max.x - w, viewport.min.x)), Max(r.min.y - 22.0f * s, viewport.min.y));
        const Rect chip(at, at + Vec2(w, 18.0f * s));
        dl.AddRect(chip, accent, ui::CornerRadii(4.0f * s));
        dl.AddTextInRect(font, 11.0f * s, chip, Color(255, 255, 255), label, ui::TextAlign::Center, false);
        // Reorder: where it would land.
        if (ts.uiDragging && ts.uiInsert >= 0) {
            FlowSiblings(ed.world, sel, siblings);
            const UIElement* parent = ed.world.Get<const UIElement>(scene::GetParent(ed.world, sel));
            const bool row = parent && parent->direction == UIDirection::Row;
            if (!siblings.Empty() && parent) {
                const bool after = u32(ts.uiInsert) >= siblings.Size();
                const Rect sr = ed.world.Get<const UIElement>(siblings[after ? siblings.Size() - 1 : usize(ts.uiInsert)])->rect;
                const f32 gap = parent->gap * parent->pixelScale * 0.5f;
                if (row) {
                    const f32 x = after ? sr.max.x + gap : sr.min.x - gap;
                    dl.AddLine(Vec2(x, sr.min.y), Vec2(x, sr.max.y), Color(255, 200, 80), 3.0f * s);
                } else {
                    const f32 y = after ? sr.max.y + gap : sr.min.y - gap;
                    dl.AddLine(Vec2(sr.min.x, y), Vec2(sr.max.x, y), Color(255, 200, 80), 3.0f * s);
                }
            }
        }
    }
    // The cursor tells what a drag will do.
    if (hot >= 0 && hot < 8)
        ui::GetCurrentContext()->SetCursorShape((hot == 1 || hot == 5) ? CursorShape::ResizeNS
                                                : (hot == 3 || hot == 7)  ? CursorShape::ResizeEW
                                                : (hot == 0 || hot == 4)  ? CursorShape::ResizeNWSE
                                                                          : CursorShape::ResizeNESW);
    else if (hot == 8)
        ui::GetCurrentContext()->SetCursorShape(CursorShape::ResizeAll);
    return hovered || ts.uiDragging;
}

void DrawUIDesignerPanel(Editor& ed)
{
    ToolState& ts = ed.tools;
    ui::Heading(ZX_ICON_LAYOUT_DASHBOARD "  UI Designer");
    ui::PushTextColor(ui::GetCurrentContext()->GetStyle().colors.textDisabled);
    ui::TextWrapped("Click a widget to select it (double-click: the one inside, Ctrl+click: the innermost). Drag to move it "
                    "(anchored widgets) or to change its place in the layout; drag the handles to resize. Alt: no snapping, arrows "
                    "nudge. Details shows all its settings.");
    ui::PopTextColor();
    ui::Spacing();
    // Preview screen shape.
    StringView names[ZX_ARRAY_COUNT(Previews)];
    for (u32 i = 0; i < ZX_ARRAY_COUNT(Previews); ++i)
        names[i] = Previews[i].name;
    i32 preview = i32(ts.uiPreview);
    ui::AlignTextToFramePadding();
    ui::Text("Preview");
    ui::SameLine(90.0f);
    ui::SetNextItemWidth(-1.0f);
    if (ui::Combo("##uiPreview", &preview, Span<const StringView>(names, ZX_ARRAY_COUNT(Previews))))
        ts.uiPreview = u32(preview);
    ui::Checkbox("Snap to grid", &ts.uiSnap);
    ui::SameLine();
    ui::SetNextItemWidth(-1.0f);
    ui::SliderFloat("##uiGrid", &ts.uiGrid, 1.0f, 32.0f, "{:.0f} units");

    // Palette: under the selected element (or the first canvas; a canvas when there's none).
    ui::Spacing();
    ui::TextDisabled("ADD");
    struct Item
    {
        const char* icon;
        const char* label;
        const char* kind;
    };
    static const Item Items[] = {{ZX_ICON_MONITOR, "Canvas", "canvas"},       {ZX_ICON_RECTANGLE_HORIZONTAL, "Panel", "panel"},
                                 {ZX_ICON_TYPE, "Text", "text"},              {ZX_ICON_MOUSE_POINTER_CLICK, "Button", "button"},
                                 {ZX_ICON_IMAGE, "Image", "image"},           {ZX_ICON_SLIDERS_HORIZONTAL, "Slider", "slider"},
                                 {ZX_ICON_TOGGLE_LEFT, "Toggle", "toggle"},   {ZX_ICON_GAUGE, "Progress", "progress"},
                                 {ZX_ICON_TEXT_CURSOR_INPUT, "Input", "input"}};
    const f32 w = (ui::GetContentRegionAvail().x / ui::GetCurrentContext()->GetScale() - 8.0f) / 3.0f;
    for (u32 i = 0; i < ZX_ARRAY_COUNT(Items); ++i) {
        if (i % 3 != 0)
            ui::SameLine(0.0f, 4.0f);
        if (ui::Button(String::Format("{} {}##add{}", Items[i].icon, Items[i].label, i), Vec2(w, 28.0f)))
            ed.CreateUIWidget(Items[i].kind);
    }

    const ecs::Entity sel = ed.world.IsAlive(ed.selected) && ed.world.Has<UIElement>(ed.selected) ? ed.selected : ecs::Entity();
    if (!sel) {
        ui::Spacing();
        ui::TextDisabled(ed.world.Query<const UICanvas>().Count() ? "Select a widget in the viewport." : "Add a Canvas to start.");
        return;
    }
    ui::Spacing();
    ui::Separator();
    const scene::EntityName* n = ed.world.Get<const scene::EntityName>(sel);
    ui::Text(n ? n->value.View() : StringView("UI Element"));
    const ecs::Entity parent = scene::GetParent(ed.world, sel);
    if (parent && ed.world.Has<UIElement>(parent)) {
        ui::SameLine();
        if (ui::Button(ZX_ICON_ARROW_UP_LEFT "  Parent"))
            ed.Select(parent);
    }
    if (IsCanvasRoot(ed.world, sel))
        return;
    UIElement& el = *ed.world.Get<UIElement>(sel);
    auto edit = [&](StringView label, auto&& fn) {
        ed.undo.Begin(label);
        ed.undo.Record(sel);
        fn(*ed.world.Get<UIElement>(sel));
        ed.undo.End();
    };
    bool absolute = el.absolute;
    if (ui::Checkbox("Anchored (free position)", &absolute)) {
        // Keep it where it is on screen: the offset from the chosen anchor.
        const Rect r = el.rect;
        edit("Anchor UI Element", [&](UIElement& e) {
            e.absolute = absolute;
            if (absolute)
                if (const UIElement* p = ed.world.Get<const UIElement>(parent)) {
                    const f32 ps = LayoutScale(ed.world, sel);
                    const Rect inner(p->rect.min + Vec2(p->padding.x, p->padding.y) * p->pixelScale,
                                     p->rect.max - Vec2(p->padding.z, p->padding.w) * p->pixelScale);
                    const Vec2 a(Lerp(inner.min.x, inner.max.x, e.anchor.x), Lerp(inner.min.y, inner.max.y, e.anchor.y));
                    const Vec2 pivot = r.min + Vec2(r.Width() * e.pivot.x, r.Height() * e.pivot.y);
                    e.offset = Round((pivot - a) / ps);
                    if (e.size.x <= 0.0f)
                        e.size.x = Round(r.Width() / ps);
                    if (e.size.y <= 0.0f)
                        e.size.y = Round(r.Height() / ps);
                }
        });
    }
    ui::ItemTooltip("Anchored widgets sit at a point of the parent (HUD corners, popups); others follow the parent's layout");
    if (el.absolute) {
        // 3 x 3 anchor presets: the widget snaps to that point of its parent.
        ui::TextDisabled("ANCHOR");
        static const char* const Icons[9] = {ZX_ICON_ARROW_UP_LEFT,   ZX_ICON_ARROW_UP,   ZX_ICON_ARROW_UP_RIGHT,
                                             ZX_ICON_ARROW_LEFT,      ZX_ICON_CROSSHAIR,  ZX_ICON_ARROW_RIGHT,
                                             ZX_ICON_ARROW_DOWN_LEFT, ZX_ICON_ARROW_DOWN, ZX_ICON_ARROW_DOWN_RIGHT};
        for (u32 i = 0; i < 9; ++i) {
            const Vec2 a(f32(i % 3) * 0.5f, f32(i / 3) * 0.5f);
            if (i % 3 != 0)
                ui::SameLine(0.0f, 4.0f);
            const bool on = el.anchor == a && el.pivot == a;
            if (ui::Button(String::Format("{}##anchor{}", Icons[i], i), Vec2(34.0f, 30.0f), on ? ui::ButtonVariant::Primary : ui::ButtonVariant::Default))
                edit("Anchor UI Element", [&](UIElement& e) {
                    e.anchor = e.pivot = a;
                    e.offset = Vec2();
                });
        }
        ui::ItemTooltip("Puts the widget at that point of its parent (anchor and pivot), offset 0");
    } else {
        // Place in the layout.
        Array<ecs::Entity> siblings;
        FlowSiblings(ed.world, sel, siblings);
        const u32 index = InsertionIndex(ed.world, sel, el.rect.Center(), siblings);
        ui::BeginDisabled(index == 0);
        if (ui::Button(ZX_ICON_ARROW_UP "  Earlier"))
            Reorder(ed, sel, index - 1, siblings, "Reorder UI Element");
        ui::EndDisabled();
        ui::SameLine(0.0f, 4.0f);
        ui::BeginDisabled(index >= siblings.Size());
        if (ui::Button(ZX_ICON_ARROW_DOWN "  Later"))
            Reorder(ed, sel, index + 1, siblings, "Reorder UI Element");
        ui::EndDisabled();
    }
    if (ui::Button(ZX_ICON_MINIMIZE_2 "  Fit Content"))
        edit("Fit UI Element", [](UIElement& e) { e.size = Vec2(); });
    ui::ItemTooltip("Size 0: as big as its text / children");
    // The canvas' style sheet and its problems.
    ecs::Entity canvas = sel;
    while (canvas && !ed.world.Has<UICanvas>(canvas))
        canvas = scene::GetParent(ed.world, canvas);
    if (const UICanvas* cv = canvas ? ed.world.Get<const UICanvas>(canvas) : nullptr; cv && !cv->style.Empty()) {
        ui::Spacing();
        ui::TextDisabled(String::Format("STYLE  {}", cv->style));
        for (const String& e : ed.gameUI.StyleErrors())
            ui::TextColored(ui::GetCurrentContext()->GetStyle().colors.warning, e);
    }
}

} // namespace zx::editor
