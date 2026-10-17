#include "Engine/GameUI.h"
#include "Core/Containers/Algorithm.h"
#include "Scene/Hierarchy.h"
#include "UI/DrawList.h"
#include "UI/Font.h"

namespace zx {

ZX_REFLECT_ENUM_BEGIN(UIDirection)
    ZX_ENUM_VALUE(Column)
    ZX_ENUM_VALUE(Row)
ZX_REFLECT_ENUM_END()
ZX_REFLECT_ENUM_BEGIN(UIJustify)
    ZX_ENUM_VALUE(Start)
    ZX_ENUM_VALUE(Center)
    ZX_ENUM_VALUE(End)
    ZX_ENUM_VALUE(SpaceBetween)
    ZX_ENUM_VALUE(SpaceAround)
ZX_REFLECT_ENUM_END()
ZX_REFLECT_ENUM_BEGIN(UIAlign)
    ZX_ENUM_VALUE(Stretch)
    ZX_ENUM_VALUE(Start)
    ZX_ENUM_VALUE(Center)
    ZX_ENUM_VALUE(End)
ZX_REFLECT_ENUM_END()
ZX_REFLECT_ENUM_BEGIN(UITextAlign)
    ZX_ENUM_VALUE(Left)
    ZX_ENUM_VALUE(Center)
    ZX_ENUM_VALUE(Right)
ZX_REFLECT_ENUM_END()
ZX_REFLECT_ENUM_BEGIN(UIFontWeight)
    ZX_ENUM_VALUE(Regular)
    ZX_ENUM_VALUE(Medium)
    ZX_ENUM_VALUE(SemiBold)
    ZX_ENUM_VALUE(Bold)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(UICanvas)
    ZX_FIELD(referenceHeight, Meta::Range(100, 4320), Meta::Tooltip("UI units from the top of the screen to the bottom: everything scales with the window height"))
    ZX_FIELD(order, Meta::Tooltip("Canvases with a higher order draw on top"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(UIElement)
    ZX_FIELD(visible)
    ZX_FIELD(size, Meta::Tooltip("Width and height in UI units; 0 = fit the content"))
    ZX_FIELD(grow, Meta::Range(0, 10), Meta::Tooltip("Share of the parent's free space along its direction"))
    ZX_FIELD(margin, Meta::Tooltip("Left, top, right, bottom (UI units)"))
    ZX_FIELD(absolute, Meta::Category("Placement"), Meta::Tooltip("Anchored in the parent instead of laid out by it (HUD corners, popups)"))
    ZX_FIELD(anchor, Meta::EditCondition("absolute"), Meta::Tooltip("Point of the parent, 0..1 (0, 0 = top left)"))
    ZX_FIELD(pivot, Meta::EditCondition("absolute"), Meta::Tooltip("Point of this box put on the anchor, 0..1"))
    ZX_FIELD(offset, Meta::EditCondition("absolute"), Meta::Tooltip("UI units from the anchor"))
    ZX_FIELD(direction, Meta::Category("Children Layout"))
    ZX_FIELD(justify, Meta::Tooltip("Along the direction"))
    ZX_FIELD(align, Meta::Tooltip("Across the direction"))
    ZX_FIELD(gap, Meta::Range(0, 200))
    ZX_FIELD(padding, Meta::Tooltip("Left, top, right, bottom (UI units)"))
    ZX_FIELD(clip, Meta::Tooltip("Cut the children at this box"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(UIPanel)
    ZX_FIELD(color)
    ZX_FIELD(cornerRadius, Meta::Range(0, 200))
    ZX_FIELD(borderColor)
    ZX_FIELD(borderWidth, Meta::Range(0, 20))
    ZX_FIELD(shadow, Meta::Range(0, 100), Meta::Tooltip("Blur of a drop shadow (0 = none)"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(UIText)
    ZX_FIELD(text, Meta::Multiline())
    ZX_FIELD(fontSize, Meta::Range(6, 300))
    ZX_FIELD(color)
    ZX_FIELD(weight)
    ZX_FIELD(align)
    ZX_FIELD(wrap, Meta::Tooltip("Break lines at the box width"))
    ZX_FIELD(shadow, Meta::Tooltip("Soft dark outline: readable over any background"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(UIImage)
    ZX_FIELD(texture, Meta::AssetType("Texture"))
    ZX_FIELD(tint)
    ZX_FIELD(cornerRadius, Meta::Range(0, 200))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(UIButton)
    ZX_FIELD(color)
    ZX_FIELD(hoverColor)
    ZX_FIELD(pressedColor)
    ZX_FIELD(cornerRadius, Meta::Range(0, 200))
    ZX_FIELD(interactable)
    ZX_FIELD(clickSound, Meta::Tooltip("Played on click (variants _0.._N picked at random); empty = silent"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(UIProgressBar)
    ZX_FIELD(value, Meta::Range(0, 1))
    ZX_FIELD(backColor)
    ZX_FIELD(fillColor)
    ZX_FIELD(cornerRadius, Meta::Range(0, 100))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(UISlider)
    ZX_FIELD(value)
    ZX_FIELD(minValue)
    ZX_FIELD(maxValue)
    ZX_FIELD(trackColor)
    ZX_FIELD(fillColor)
    ZX_FIELD(handleColor)
    ZX_FIELD(interactable)
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(UIToggle)
    ZX_FIELD(on)
    ZX_FIELD(offColor)
    ZX_FIELD(onColor)
    ZX_FIELD(knobColor)
    ZX_FIELD(interactable)
ZX_REFLECT_STRUCT_END()

Color ToDisplayColor(const LinearColor& c) { return Color::FromLinear(c); }

namespace {

ui::FontHandle FontOf(ui::FontSystem& fonts, UIFontWeight w)
{
    static const char* const Names[] = {"Inter", "Inter Medium", "Inter SemiBold", "Inter Bold"};
    ui::FontHandle f = fonts.FindFont(Names[u32(w)]);
    return f.IsValid() ? f : fonts.FindFont("Inter");
}

/// Layout pass state: pixels per UI unit of the canvas being laid out.
struct LayoutPass
{
    ecs::World& world;
    ui::FontSystem& fonts;
    f32 scale = 1.0f;

    /// Children that take part in the layout (UI Element, visible), in order.
    void Children(ecs::Entity e, Array<ecs::Entity>& out) const
    {
        out.Clear();
        for (ecs::Entity c : scene::GetChildren(world, e))
            if (const UIElement* el = world.Get<const UIElement>(c); el && el->visible)
                out.PushBack(c);
    }

    /// Content size in UI units (bottom-up); `width` = known box width for wrapped text (0 = unknown).
    Vec2 Measure(ecs::Entity e, f32 width = 0.0f)
    {
        UIElement& el = *world.Get<UIElement>(e);
        Vec2 content;
        if (const UIText* t = world.Get<const UIText>(e)) {
            const ui::FontHandle font = FontOf(fonts, t->weight);
            const f32 wrap = t->wrap && (el.size.x > 0.0f || width > 0.0f) ? (el.size.x > 0.0f ? el.size.x : width) - el.padding.x - el.padding.z : 0.0f;
            if (font.IsValid())
                content = fonts.MeasureText(font, t->fontSize * scale, t->text.View(), wrap * scale) / scale;
        }
        if (world.Has<UISlider>(e))
            content = Vec2(Max(content.x, 240.0f), Max(content.y, 28.0f));
        if (world.Has<UIToggle>(e))
            content = Vec2(Max(content.x, 64.0f), Max(content.y, 34.0f));
        if (world.Has<UIProgressBar>(e))
            content = Vec2(Max(content.x, 240.0f), Max(content.y, 16.0f));
        if (world.Has<UIImage>(e) && content.x == 0.0f)
            content = Vec2(64.0f, 64.0f);
        // Children along the direction.
        Array<ecs::Entity> kids;
        Children(e, kids);
        const bool row = el.direction == UIDirection::Row;
        f32 main = 0.0f, cross = 0.0f;
        u32 flow = 0;
        for (ecs::Entity c : kids) {
            const UIElement& ce = *world.Get<const UIElement>(c);
            const Vec2 m = Measure(c, !row && el.size.x > 0.0f ? el.size.x - el.padding.x - el.padding.z : 0.0f);
            if (ce.absolute)
                continue;
            const Vec2 outer = m + Vec2(ce.margin.x + ce.margin.z, ce.margin.y + ce.margin.w);
            main += row ? outer.x : outer.y;
            cross = Max(cross, row ? outer.y : outer.x);
            ++flow;
        }
        if (flow > 1)
            main += el.gap * f32(flow - 1);
        const Vec2 children = row ? Vec2(main, cross) : Vec2(cross, main);
        content = Vec2(Max(content.x, children.x), Max(content.y, children.y)) + Vec2(el.padding.x + el.padding.z, el.padding.y + el.padding.w);
        el.measured = Vec2(el.size.x > 0.0f ? el.size.x : content.x, el.size.y > 0.0f ? el.size.y : content.y);
        return el.measured;
    }

    /// Places `e` at `rect` (pixels) and its children (top-down).
    void Arrange(ecs::Entity e, const Rect& rect)
    {
        UIElement& el = *world.Get<UIElement>(e);
        el.rect = rect;
        const Rect inner(rect.min + Vec2(el.padding.x, el.padding.y) * scale, rect.max - Vec2(el.padding.z, el.padding.w) * scale);
        Array<ecs::Entity> kids;
        Children(e, kids);
        const bool row = el.direction == UIDirection::Row;
        const f32 avail = row ? inner.Width() : inner.Height();
        const f32 crossAvail = row ? inner.Height() : inner.Width();
        // Main-axis sizes: measured + share of the free space by grow.
        Array<f32> sizes;
        f32 used = 0.0f, grow = 0.0f;
        u32 flow = 0;
        for (ecs::Entity c : kids) {
            const UIElement& ce = *world.Get<const UIElement>(c);
            if (ce.absolute) {
                sizes.PushBack(0.0f);
                continue;
            }
            const f32 s = (row ? ce.measured.x : ce.measured.y) * scale;
            sizes.PushBack(s);
            used += s + (row ? ce.margin.x + ce.margin.z : ce.margin.y + ce.margin.w) * scale;
            grow += ce.grow;
            ++flow;
        }
        if (flow > 1)
            used += el.gap * scale * f32(flow - 1);
        f32 free = avail - used;
        if (grow > 0.0f && free > 0.0f) {
            for (usize i = 0; i < kids.Size(); ++i) {
                const UIElement& ce = *world.Get<const UIElement>(kids[i]);
                if (!ce.absolute)
                    sizes[i] += free * ce.grow / grow;
            }
            free = 0.0f;
        }
        f32 cursor = 0.0f, spacing = el.gap * scale;
        if (free > 0.0f) {
            switch (el.justify) {
            case UIJustify::Start: break;
            case UIJustify::Center: cursor = free * 0.5f; break;
            case UIJustify::End: cursor = free; break;
            case UIJustify::SpaceBetween:
                if (flow > 1)
                    spacing += free / f32(flow - 1);
                break;
            case UIJustify::SpaceAround:
                spacing += free / f32(Max(flow, 1u));
                cursor = free / f32(Max(flow, 1u)) * 0.5f;
                break;
            }
        }
        for (usize i = 0; i < kids.Size(); ++i) {
            const ecs::Entity c = kids[i];
            const UIElement& ce = *world.Get<const UIElement>(c);
            if (ce.absolute) {
                const Vec2 size = ce.measured * scale;
                const Vec2 at = inner.min + Vec2(inner.Width() * ce.anchor.x, inner.Height() * ce.anchor.y) + ce.offset * scale -
                                Vec2(size.x * ce.pivot.x, size.y * ce.pivot.y);
                Arrange(c, Rect(at, at + size));
                continue;
            }
            const Vec4 m = ce.margin * scale;
            const f32 mainBefore = row ? m.x : m.y, mainAfter = row ? m.z : m.w;
            const f32 crossBefore = row ? m.y : m.x, crossAfter = row ? m.w : m.z;
            const f32 crossSize0 = (row ? ce.measured.y : ce.measured.x) * scale;
            const bool fixedCross = (row ? ce.size.y : ce.size.x) > 0.0f;
            const f32 crossRoom = crossAvail - crossBefore - crossAfter;
            f32 crossSize = crossSize0, crossPos = crossBefore;
            switch (el.align) {
            case UIAlign::Stretch:
                if (!fixedCross)
                    crossSize = crossRoom;
                break;
            case UIAlign::Start: break;
            case UIAlign::Center: crossPos = crossBefore + (crossRoom - crossSize) * 0.5f; break;
            case UIAlign::End: crossPos = crossBefore + crossRoom - crossSize; break;
            }
            cursor += mainBefore;
            Rect r;
            if (row)
                r = Rect(Vec2(inner.min.x + cursor, inner.min.y + crossPos), Vec2(inner.min.x + cursor + sizes[i], inner.min.y + crossPos + crossSize));
            else
                r = Rect(Vec2(inner.min.x + crossPos, inner.min.y + cursor), Vec2(inner.min.x + crossPos + crossSize, inner.min.y + cursor + sizes[i]));
            // Wrapped text stretched wider / narrower than measured: re-measure its height for the final width.
            if (!row && world.Has<UIText>(c) && world.Get<const UIText>(c)->wrap && ce.size.y <= 0.0f) {
                Measure(c, r.Width() / scale);
                r.max.y = r.min.y + world.Get<const UIElement>(c)->measured.y * scale;
            }
            Arrange(c, r);
            cursor += (row ? r.Width() : r.Height()) + mainAfter + spacing;
        }
    }
};

void TextLines(ui::FontSystem& fonts, ui::FontHandle font, f32 size, StringView text, f32 wrap, Array<StringView>& out)
{
    out.Clear();
    usize start = 0;
    for (usize i = 0; i <= text.Size(); ++i) {
        if (i < text.Size() && text[i] != '\n')
            continue;
        StringView line = text.SubStr(start, i - start);
        while (wrap > 0.0f && !line.Empty()) {
            const usize n = Max<usize>(fonts.WrapLine(font, size, line, wrap), 1);
            if (n >= line.Size())
                break;
            out.PushBack(line.SubStr(0, n).TrimRight());
            line = line.SubStr(n).TrimLeft();
        }
        out.PushBack(line);
        start = i + 1;
    }
}

struct DrawPass
{
    ecs::World& world;
    ui::FontSystem& fonts;
    ui::DrawList& dl;
    const Function<u32(asset::AssetId)>& textureSrv;
    f32 scale = 1.0f;

    void Draw(ecs::Entity e)
    {
        const UIElement* el = world.Get<const UIElement>(e);
        if (!el || !el->visible)
            return;
        const Rect r = el->rect;
        const f32 s = scale;
        if (const UIPanel* p = world.Get<const UIPanel>(e)) {
            if (p->shadow > 0.0f)
                dl.AddShadow(r, Color(0, 0, 0, 110), p->cornerRadius * s, p->shadow * s, Vec2(0.0f, p->shadow * s * 0.25f));
            dl.AddRect(r, ToDisplayColor(p->color), ui::CornerRadii(p->cornerRadius * s), p->borderWidth * s, ToDisplayColor(p->borderColor));
        }
        if (const UIButton* b = world.Get<const UIButton>(e)) {
            LinearColor c = !b->interactable ? b->color * LinearColor(0.6f, 0.6f, 0.6f, 0.6f)
                            : el->pressed    ? b->pressedColor
                            : el->hovered    ? b->hoverColor
                                             : b->color;
            dl.AddRect(r, ToDisplayColor(c), ui::CornerRadii(b->cornerRadius * s));
        }
        if (const UIImage* img = world.Get<const UIImage>(e); img && img->texture.IsValid() && textureSrv) {
            const u32 srv = textureSrv(img->texture);
            if (srv != 0 && srv != ~0u)
                dl.AddImage(srv, r, Vec2(0, 0), Vec2(1, 1), ToDisplayColor(img->tint), img->cornerRadius * s);
        }
        if (const UIProgressBar* p = world.Get<const UIProgressBar>(e)) {
            const ui::CornerRadii radii(Min(p->cornerRadius * s, r.Height() * 0.5f));
            dl.AddRect(r, ToDisplayColor(p->backColor), radii);
            const f32 v = Clamp(p->value, 0.0f, 1.0f);
            if (v > 0.0f)
                dl.AddRect(Rect(r.min, Vec2(r.min.x + r.Width() * v, r.max.y)), ToDisplayColor(p->fillColor), radii);
        }
        if (const UISlider* sl = world.Get<const UISlider>(e)) {
            const f32 h = Min(r.Height(), 8.0f * s);
            const Rect track(Vec2(r.min.x + r.Height() * 0.5f, r.Center().y - h * 0.5f), Vec2(r.max.x - r.Height() * 0.5f, r.Center().y + h * 0.5f));
            const f32 t = sl->maxValue > sl->minValue ? Clamp((sl->value - sl->minValue) / (sl->maxValue - sl->minValue), 0.0f, 1.0f) : 0.0f;
            const f32 x = track.min.x + track.Width() * t;
            dl.AddRect(track, ToDisplayColor(sl->trackColor), ui::CornerRadii(h * 0.5f));
            dl.AddRect(Rect(track.min, Vec2(x, track.max.y)), ToDisplayColor(sl->fillColor), ui::CornerRadii(h * 0.5f));
            const f32 knob = r.Height() * (el->hovered || el->pressed ? 0.5f : 0.42f);
            dl.AddShadow(Rect(Vec2(x - knob, r.Center().y - knob), Vec2(x + knob, r.Center().y + knob)), Color(0, 0, 0, 90), knob, 4.0f * s);
            dl.AddCircle(Vec2(x, r.Center().y), knob, ToDisplayColor(sl->handleColor));
        }
        if (const UIToggle* tg = world.Get<const UIToggle>(e)) {
            const f32 h = r.Height(), w = Min(r.Width(), h * 1.9f);
            const Rect pill(Vec2(r.min.x, r.min.y), Vec2(r.min.x + w, r.max.y));
            dl.AddRect(pill, ToDisplayColor(tg->on ? tg->onColor : tg->offColor), ui::CornerRadii(h * 0.5f));
            const f32 knob = h * 0.5f - 3.0f * s;
            dl.AddCircle(Vec2(tg->on ? pill.max.x - h * 0.5f : pill.min.x + h * 0.5f, pill.Center().y), knob, ToDisplayColor(tg->knobColor));
        }
        if (const UIText* t = world.Get<const UIText>(e); t && !t->text.Empty()) {
            const ui::FontHandle font = FontOf(fonts, t->weight);
            if (font.IsValid()) {
                const f32 size = t->fontSize * s;
                const Rect inner(r.min + Vec2(el->padding.x, el->padding.y) * s, r.max - Vec2(el->padding.z, el->padding.w) * s);
                Array<StringView> lines;
                TextLines(fonts, font, size, t->text.View(), t->wrap ? inner.Width() : 0.0f, lines);
                const f32 lineHeight = fonts.GetLineMetrics(font, size).lineHeight;
                const f32 block = lineHeight * f32(lines.Size());
                f32 y = inner.min.y + Max(0.0f, (inner.Height() - block) * 0.5f);
                for (StringView line : lines) {
                    const f32 w = fonts.MeasureWidth(font, size, line);
                    const f32 x = t->align == UITextAlign::Left     ? inner.min.x
                                  : t->align == UITextAlign::Center ? inner.Center().x - w * 0.5f
                                                                    : inner.max.x - w;
                    if (t->shadow) {
                        const f32 o = Max(1.0f, size * 0.06f);
                        dl.AddText(font, size, Vec2(x + o, y + o), Color(0, 0, 0, u8(Clamp(t->color.a, 0.0f, 1.0f) * 150.0f)), line);
                    }
                    dl.AddText(font, size, Vec2(x, y), ToDisplayColor(t->color), line);
                    y += lineHeight;
                }
            }
        }
        if (el->clip)
            dl.PushClipRect(r);
        for (ecs::Entity c : scene::GetChildren(world, e))
            Draw(c);
        if (el->clip)
            dl.PopClipRect();
    }
};

/// Canvases by order (stable), each with its scale.
void Canvases(ecs::World& world, Array<ecs::Entity>& out)
{
    out.Clear();
    world.Query<const UICanvas>().Each([&](ecs::Entity e, const UICanvas&) { out.PushBack(e); });
    StableSort(out, [&](ecs::Entity a, ecs::Entity b) { return world.Get<const UICanvas>(a)->order < world.Get<const UICanvas>(b)->order; });
}

} // namespace

void GameUISystem::Layout(ecs::World& world, ui::FontSystem& fonts, const Rect& screen)
{
    Array<ecs::Entity> canvases;
    Canvases(world, canvases);
    for (ecs::Entity c : canvases) {
        if (!world.Has<UIElement>(c))
            world.Add(c, UIElement{});
        const UICanvas& canvas = *world.Get<const UICanvas>(c);
        LayoutPass pass{world, fonts, screen.Height() / Max(canvas.referenceHeight, 1.0f)};
        pass.Measure(c);
        pass.Arrange(c, screen);
    }
}

void GameUISystem::Draw(ecs::World& world, ui::FontSystem& fonts, ui::DrawList& dl, const Function<u32(asset::AssetId)>& textureSrv)
{
    Array<ecs::Entity> canvases;
    Canvases(world, canvases);
    for (ecs::Entity c : canvases) {
        const UIElement* el = world.Get<const UIElement>(c);
        if (!el)
            continue;
        DrawPass pass{world, fonts, dl, textureSrv, el->rect.Height() / Max(world.Get<const UICanvas>(c)->referenceHeight, 1.0f)};
        pass.Draw(c);
    }
}

void GameUISystem::Interact(ecs::World& world, Vec2 mouse, bool down, bool pressed, bool released)
{
    // The topmost interactive widget under the mouse (later canvases / later children draw on top).
    ecs::Entity hot;
    Array<ecs::Entity> canvases;
    Canvases(world, canvases);
    Function<void(ecs::Entity)> visit = [&](ecs::Entity e) {
        const UIElement* el = world.Get<const UIElement>(e);
        if (!el || !el->visible)
            return;
        const bool interactive = (world.Has<UIButton>(e) && world.Get<const UIButton>(e)->interactable) ||
                                 (world.Has<UISlider>(e) && world.Get<const UISlider>(e)->interactable) ||
                                 (world.Has<UIToggle>(e) && world.Get<const UIToggle>(e)->interactable);
        if (interactive && el->rect.Contains(mouse))
            hot = e;
        if (el->clip && !el->rect.Contains(mouse))
            return;
        for (ecs::Entity c : scene::GetChildren(world, e))
            visit(c);
    };
    for (ecs::Entity c : canvases)
        visit(c);
    m_mouseOver = bool(hot);
    if (pressed && hot)
        m_pressed = hot;
    world.Query<UIElement>().Each([&](ecs::Entity e, UIElement& el) {
        el.hovered = e == hot && (!m_pressed || m_pressed == e);
        el.pressed = e == m_pressed && down;
    });
    // Sliders follow the mouse while held.
    if (m_pressed && down && world.IsAlive(m_pressed))
        if (UISlider* sl = world.Get<UISlider>(m_pressed)) {
            const Rect r = world.Get<const UIElement>(m_pressed)->rect;
            const f32 left = r.min.x + r.Height() * 0.5f, width = Max(r.Width() - r.Height(), 1.0f);
            const f32 v = sl->minValue + Clamp((mouse.x - left) / width, 0.0f, 1.0f) * (sl->maxValue - sl->minValue);
            if (v != sl->value) {
                sl->value = v;
                m_events.PushBack(UIEvent{UIEvent::Type::Changed, m_pressed, v});
            }
        }
    if (released && m_pressed) {
        if (m_pressed == hot && world.IsAlive(hot)) {
            if (world.Has<UIButton>(hot))
                m_events.PushBack(UIEvent{UIEvent::Type::Click, hot, 0.0f});
            if (UIToggle* tg = world.Get<UIToggle>(hot)) {
                tg->on = !tg->on;
                m_events.PushBack(UIEvent{UIEvent::Type::Changed, hot, tg->on ? 1.0f : 0.0f});
            }
        }
        m_pressed = {};
    }
    if (!down && !released)
        m_pressed = {};
}

} // namespace zx
