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

ZX_REFLECT_ENUM_BEGIN(UIEasing)
    ZX_ENUM_VALUE(Linear)
    ZX_ENUM_VALUE(EaseIn)
    ZX_ENUM_VALUE(EaseOut)
    ZX_ENUM_VALUE(EaseInOut)
    ZX_ENUM_VALUE(Back)
    ZX_ENUM_VALUE(Elastic)
    ZX_ENUM_VALUE(Bounce)
ZX_REFLECT_ENUM_END()
ZX_REFLECT_ENUM_BEGIN(UIAnimationPreset)
    ZX_ENUM_VALUE(FadeIn)
    ZX_ENUM_VALUE(FadeOut)
    ZX_ENUM_VALUE(SlideInLeft)
    ZX_ENUM_VALUE(SlideInRight)
    ZX_ENUM_VALUE(SlideInTop)
    ZX_ENUM_VALUE(SlideInBottom)
    ZX_ENUM_VALUE(PopIn)
    ZX_ENUM_VALUE(Pulse)
    ZX_ENUM_VALUE(Float)
    ZX_ENUM_VALUE(Blink)
    ZX_ENUM_VALUE(Shake)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(UIAnimation)
    ZX_FIELD(preset)
    ZX_FIELD(duration, Meta::Range(0.01, 30), Meta::Units("s"), Meta::Tooltip("Seconds (looping presets: one cycle)"))
    ZX_FIELD(delay, Meta::Range(0, 30), Meta::Units("s"))
    ZX_FIELD(easing)
    ZX_FIELD(distance, Meta::Range(0, 2000), Meta::Tooltip("UI units for slides; Float and Shake move a tenth of it"))
    ZX_FIELD(playOnShow, Meta::Tooltip("Play when the game starts and every time the element becomes visible"))
    ZX_FIELD(loop, Meta::Tooltip("Repeat (Pulse, Float and Blink always loop)"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(UICanvas)
    ZX_FIELD(referenceHeight, Meta::Range(100, 4320), Meta::Tooltip("UI units from the top of the screen to the bottom: everything scales with the window height"))
    ZX_FIELD(order, Meta::Tooltip("Canvases with a higher order draw on top"))
    ZX_FIELD(worldSpace, Meta::Category("World Space"), Meta::Tooltip("Float over this entity in the 3D world: name plates, health bars"))
    ZX_FIELD(worldOffset, Meta::EditCondition("worldSpace"), Meta::Units("m"), Meta::Tooltip("From the entity (world axes): above its head"))
    ZX_FIELD(maxDistance, Meta::EditCondition("worldSpace"), Meta::Range(1, 10000), Meta::Units("m"), Meta::Tooltip("Hidden farther away"))
    ZX_FIELD(fullSizeDistance, Meta::EditCondition("worldSpace"), Meta::Range(0, 1000), Meta::Units("m"),
             Meta::Tooltip("Full size closer than this, smaller farther away (0 = always full size)"))
    ZX_FIELD(minScale, Meta::EditCondition("worldSpace"), Meta::Range(0.05, 1), Meta::Tooltip("Smallest size far away"))
    ZX_FIELD(style, Meta::Category("Style"),
             Meta::Tooltip("A .zstyle sheet in Content (e.g. UI/Menu.zstyle): CSS-like rules for the look of every widget on this canvas"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(UIInputField)
    ZX_FIELD(text)
    ZX_FIELD(placeholder, Meta::Tooltip("Shown greyed out while empty"))
    ZX_FIELD(maxLength, Meta::Range(1, 4096))
    ZX_FIELD(password, Meta::Tooltip("Show dots instead of the characters"))
    ZX_FIELD(fontSize, Meta::Range(6, 200))
    ZX_FIELD(textColor)
    ZX_FIELD(placeholderColor)
    ZX_FIELD(backColor)
    ZX_FIELD(focusColor, Meta::Tooltip("Border while typing"))
    ZX_FIELD(cornerRadius, Meta::Range(0, 100))
    ZX_FIELD(clearOnSubmit, Meta::Tooltip("Empty the field after Enter (chat)"))
    ZX_FIELD(interactable)
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
    ZX_FIELD(wrap, Meta::Tooltip("Row: children that don't fit start a new line"))
    ZX_FIELD(scroll, Meta::Tooltip("Column: the children scroll (mouse wheel, scroll bar) instead of squeezing"))
    ZX_FIELD(styleClass, Meta::Category("Look"), Meta::Tooltip("Classes for the canvas' style sheet, separated by spaces: \"title big\""))
    ZX_FIELD(opacity, Meta::Range(0, 1))
    ZX_FIELD(scale, Meta::Range(0, 4), Meta::Tooltip("Drawn bigger / smaller around the center (children too); the layout doesn't change"))
    ZX_FIELD(translate, Meta::Tooltip("Drawn moved by this many UI units; the layout doesn't change"))
    ZX_FIELD(transition, Meta::Range(0, 5), Meta::Units("s"), Meta::Tooltip("Seconds to blend Opacity / Scale / Translate changes (style states)"))
    ZX_FIELD(navigable, Meta::Tooltip("Buttons, sliders, toggles and input fields: the gamepad / keyboard focus can land here"))
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
    f32 opacity = 1.0f;

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
        if (const UIInputField* in = world.Get<const UIInputField>(e))
            content = Vec2(Max(content.x, 320.0f), Max(content.y, in->fontSize * 1.7f));
        // Children along the direction.
        Array<ecs::Entity> kids;
        Children(e, kids);
        const bool row = el.direction == UIDirection::Row;
        const f32 boxWidth = el.size.x > 0.0f ? el.size.x : width;
        const f32 innerWidth = boxWidth > 0.0f ? boxWidth - el.padding.x - el.padding.z : 0.0f;
        f32 main = 0.0f, cross = 0.0f;
        u32 flow = 0;
        // Wrapped rows: lines as wide as the box allows.
        f32 lineMain = 0.0f, lineCross = 0.0f, linesCross = 0.0f;
        u32 lineCount = 0, inLine = 0;
        for (ecs::Entity c : kids) {
            const UIElement& ce = *world.Get<const UIElement>(c);
            const Vec2 m = Measure(c, !row && innerWidth > 0.0f ? innerWidth : 0.0f);
            if (ce.absolute)
                continue;
            const Vec2 outer = m + Vec2(ce.margin.x + ce.margin.z, ce.margin.y + ce.margin.w);
            if (row && el.wrap && innerWidth > 0.0f) {
                if (inLine > 0 && lineMain + el.gap + outer.x > innerWidth) {
                    main = Max(main, lineMain);
                    linesCross += lineCross + (lineCount > 0 ? el.gap : 0.0f);
                    ++lineCount;
                    lineMain = lineCross = 0.0f;
                    inLine = 0;
                }
                lineMain += (inLine > 0 ? el.gap : 0.0f) + outer.x;
                lineCross = Max(lineCross, outer.y);
                ++inLine;
                ++flow;
                continue;
            }
            main += row ? outer.x : outer.y;
            cross = Max(cross, row ? outer.y : outer.x);
            ++flow;
        }
        if (row && el.wrap && innerWidth > 0.0f) {
            if (inLine > 0) {
                main = Max(main, lineMain);
                linesCross += lineCross + (lineCount > 0 ? el.gap : 0.0f);
            }
            cross = linesCross;
        } else if (flow > 1) {
            main += el.gap * f32(flow - 1);
        }
        el.contentHeight = row ? cross : main;
        const Vec2 children = row ? Vec2(main, cross) : Vec2(cross, main);
        content = Vec2(Max(content.x, children.x), Max(content.y, children.y)) + Vec2(el.padding.x + el.padding.z, el.padding.y + el.padding.w);
        el.measured = Vec2(el.size.x > 0.0f ? el.size.x : content.x, el.size.y > 0.0f ? el.size.y : content.y);
        return el.measured;
    }

    /// Places `e` at `rect` (pixels) and its children (top-down).
    void Arrange(ecs::Entity e, Rect rect)
    {
        UIElement& el = *world.Get<UIElement>(e);
        // The Look: scaled around the center and moved (children with it), faded.
        const f32 k = Max(el.shownScale * el.animScale, 0.0f);
        const Vec2 move = (el.shownTranslate + el.animTranslate) * scale;
        if (k != 1.0f || move.x != 0.0f || move.y != 0.0f) {
            const Vec2 c = rect.Center();
            rect = Rect(c + (rect.min - c) * k + move, c + (rect.max - c) * k + move);
        }
        const f32 savedScale = scale, savedOpacity = opacity;
        scale *= k;
        opacity *= Saturate(el.shownOpacity * el.animOpacity);
        el.pixelScale = scale;
        el.drawOpacity = opacity;
        ArrangeInside(e, el, rect);
        scale = savedScale;
        opacity = savedOpacity;
    }

    void ArrangeInside(ecs::Entity e, UIElement& el, const Rect& rect)
    {
        el.rect = rect;
        const Rect inner(rect.min + Vec2(el.padding.x, el.padding.y) * scale, rect.max - Vec2(el.padding.z, el.padding.w) * scale);
        Array<ecs::Entity> kids;
        Children(e, kids);
        const bool row = el.direction == UIDirection::Row;
        if (row && el.wrap) {
            ArrangeWrapped(el, inner, kids);
            return;
        }
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
        f32 scrolled = 0.0f;
        if (el.scroll && !row) {
            // Scrolling: the children keep their sizes; the view moves over them.
            el.contentHeight = used / scale;
            const f32 maxScroll = Max(used - avail, 0.0f) / scale;
            el.scrollOffset = Clamp(el.scrollOffset, 0.0f, maxScroll);
            scrolled = el.scrollOffset * scale;
            free = Max(free, 0.0f);
            grow = free > 0.0f ? grow : 0.0f;
        }
        if (grow > 0.0f && free > 0.0f) {
            for (usize i = 0; i < kids.Size(); ++i) {
                const UIElement& ce = *world.Get<const UIElement>(kids[i]);
                if (!ce.absolute)
                    sizes[i] += free * ce.grow / grow;
            }
            free = 0.0f;
        }
        f32 cursor = -scrolled, spacing = el.gap * scale;
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

    /// Row with Wrap: children flow left to right and onto new lines (each line aligned by `align`).
    void ArrangeWrapped(UIElement& el, const Rect& inner, const Array<ecs::Entity>& kids)
    {
        const f32 gap = el.gap * scale;
        f32 x = 0.0f, y = 0.0f, lineHeight = 0.0f;
        usize lineStart = 0;
        Array<Rect> rects;
        rects.Resize(kids.Size());
        auto finishLine = [&](usize end) {
            for (usize k = lineStart; k < end; ++k) {
                const UIElement& ce = *world.Get<const UIElement>(kids[k]);
                if (ce.absolute)
                    continue;
                Rect& r = rects[k];
                const f32 h = r.Height();
                const f32 offset = el.align == UIAlign::Center ? (lineHeight - h) * 0.5f : el.align == UIAlign::End ? lineHeight - h : 0.0f;
                r.min.y += offset;
                r.max.y += offset;
                if (el.align == UIAlign::Stretch && ce.size.y <= 0.0f)
                    r.max.y = r.min.y + lineHeight - (ce.margin.y + ce.margin.w) * scale;
            }
        };
        for (usize i = 0; i < kids.Size(); ++i) {
            const UIElement& ce = *world.Get<const UIElement>(kids[i]);
            if (ce.absolute) {
                const Vec2 size = ce.measured * scale;
                const Vec2 at = inner.min + Vec2(inner.Width() * ce.anchor.x, inner.Height() * ce.anchor.y) + ce.offset * scale -
                                Vec2(size.x * ce.pivot.x, size.y * ce.pivot.y);
                rects[i] = Rect(at, at + size);
                continue;
            }
            const Vec2 size = ce.measured * scale;
            const Vec4 m = ce.margin * scale;
            const f32 outerW = size.x + m.x + m.z, outerH = size.y + m.y + m.w;
            if (x > 0.0f && x + outerW > inner.Width() + 0.5f) {
                finishLine(i);
                y += lineHeight + gap;
                x = 0.0f;
                lineHeight = 0.0f;
                lineStart = i;
            }
            const Vec2 at = inner.min + Vec2(x + m.x, y + m.y);
            rects[i] = Rect(at, at + size);
            x += outerW + gap;
            lineHeight = Max(lineHeight, outerH);
        }
        finishLine(kids.Size());
        for (usize i = 0; i < kids.Size(); ++i)
            Arrange(kids[i], rects[i]);
    }
};

/// UTF-8 code points in `s`.
usize CodepointCount(StringView s)
{
    usize n = 0;
    for (usize i = 0; i < s.Size(); ++i)
        n += (u8(s[i]) & 0xC0) != 0x80 ? 1 : 0;
    return n;
}

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
    ecs::Entity focusedInput;
    f32 time = 0.0f;

    bool navActive = false;

    /// The element's color faded by its (and its ancestors') Opacity.
    static Color Fade(Color c, f32 a) { return Color(c.r, c.g, c.b, u8(f32(c.a) * a + 0.5f)); }

    void Draw(ecs::Entity e)
    {
        const UIElement* el = world.Get<const UIElement>(e);
        if (!el || !el->visible || el->drawOpacity <= 0.003f)
            return;
        const Rect r = el->rect;
        const f32 s = el->pixelScale;
        const f32 a = el->drawOpacity;
        auto C = [a](const LinearColor& c) { return Fade(ToDisplayColor(c), a); };
        if (const UIPanel* p = world.Get<const UIPanel>(e)) {
            if (p->shadow > 0.0f)
                dl.AddShadow(r, Fade(Color(0, 0, 0, 110), a), p->cornerRadius * s, p->shadow * s, Vec2(0.0f, p->shadow * s * 0.25f));
            dl.AddRect(r, C(p->color), ui::CornerRadii(p->cornerRadius * s), p->borderWidth * s, C(p->borderColor));
        }
        if (const UIButton* b = world.Get<const UIButton>(e)) {
            LinearColor c = !b->interactable ? b->color * LinearColor(0.6f, 0.6f, 0.6f, 0.6f)
                            : el->pressed    ? b->pressedColor
                            : el->hovered    ? b->hoverColor
                                             : b->color;
            dl.AddRect(r, C(c), ui::CornerRadii(b->cornerRadius * s));
        }
        if (const UIImage* img = world.Get<const UIImage>(e); img && img->texture.IsValid() && textureSrv) {
            const u32 srv = textureSrv(img->texture);
            if (srv != 0 && srv != ~0u)
                dl.AddImage(srv, r, Vec2(0, 0), Vec2(1, 1), C(img->tint), img->cornerRadius * s);
        }
        if (const UIProgressBar* p = world.Get<const UIProgressBar>(e)) {
            const ui::CornerRadii radii(Min(p->cornerRadius * s, r.Height() * 0.5f));
            dl.AddRect(r, C(p->backColor), radii);
            const f32 v = Clamp(p->value, 0.0f, 1.0f);
            if (v > 0.0f)
                dl.AddRect(Rect(r.min, Vec2(r.min.x + r.Width() * v, r.max.y)), C(p->fillColor), radii);
        }
        if (const UISlider* sl = world.Get<const UISlider>(e)) {
            const f32 h = Min(r.Height(), 8.0f * s);
            const Rect track(Vec2(r.min.x + r.Height() * 0.5f, r.Center().y - h * 0.5f), Vec2(r.max.x - r.Height() * 0.5f, r.Center().y + h * 0.5f));
            const f32 t = sl->maxValue > sl->minValue ? Clamp((sl->value - sl->minValue) / (sl->maxValue - sl->minValue), 0.0f, 1.0f) : 0.0f;
            const f32 x = track.min.x + track.Width() * t;
            dl.AddRect(track, C(sl->trackColor), ui::CornerRadii(h * 0.5f));
            dl.AddRect(Rect(track.min, Vec2(x, track.max.y)), C(sl->fillColor), ui::CornerRadii(h * 0.5f));
            const f32 knob = r.Height() * (el->hovered || el->pressed ? 0.5f : 0.42f);
            dl.AddShadow(Rect(Vec2(x - knob, r.Center().y - knob), Vec2(x + knob, r.Center().y + knob)), Fade(Color(0, 0, 0, 90), a), knob, 4.0f * s);
            dl.AddCircle(Vec2(x, r.Center().y), knob, C(sl->handleColor));
        }
        if (const UIToggle* tg = world.Get<const UIToggle>(e)) {
            const f32 h = r.Height(), w = Min(r.Width(), h * 1.9f);
            const Rect pill(Vec2(r.min.x, r.min.y), Vec2(r.min.x + w, r.max.y));
            dl.AddRect(pill, C(tg->on ? tg->onColor : tg->offColor), ui::CornerRadii(h * 0.5f));
            const f32 knob = h * 0.5f - 3.0f * s;
            dl.AddCircle(Vec2(tg->on ? pill.max.x - h * 0.5f : pill.min.x + h * 0.5f, pill.Center().y), knob, C(tg->knobColor));
        }
        if (const UIInputField* in = world.Get<const UIInputField>(e)) {
            const bool focused = e == focusedInput;
            const ui::CornerRadii radii(in->cornerRadius * s);
            dl.AddRect(r, C(in->backColor), radii, focused ? 2.0f * s : 1.0f * s,
                       focused ? C(in->focusColor) : Fade(Color(255, 255, 255, el->hovered ? 70 : 35), a));
            const ui::FontHandle font = FontOf(fonts, UIFontWeight::Regular);
            if (font.IsValid()) {
                const f32 size = in->fontSize * s;
                String shown;
                if (in->password)
                    for (usize k = 0, n = CodepointCount(in->text.View()); k < n; ++k)
                        shown += "\xE2\x80\xA2"; // bullet
                const StringView text = in->password ? shown.View() : in->text.View();
                const f32 pad = 12.0f * s;
                const Rect inner(Vec2(r.min.x + pad, r.min.y), Vec2(r.max.x - pad, r.max.y));
                const f32 lineHeight = fonts.GetLineMetrics(font, size).lineHeight;
                const f32 y = inner.Center().y - lineHeight * 0.5f;
                const f32 w = fonts.MeasureWidth(font, size, text);
                const f32 x = inner.min.x + Min(0.0f, inner.Width() - w - 2.0f * s); // keep the end in view while typing
                dl.PushClipRect(inner);
                if (text.Empty() && !focused)
                    dl.AddText(font, size, Vec2(inner.min.x, y), C(in->placeholderColor), in->placeholder.View());
                else
                    dl.AddText(font, size, Vec2(x, y), C(in->textColor), text);
                if (focused && Frac(time * 1.6f) < 0.6f) // caret
                    dl.AddRect(Rect(Vec2(x + w + 1.0f * s, y + lineHeight * 0.15f), Vec2(x + w + 3.0f * s, y + lineHeight * 0.85f)),
                               C(in->textColor));
                dl.PopClipRect();
            }
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
                        dl.AddText(font, size, Vec2(x + o, y + o), Fade(Color(0, 0, 0, u8(Clamp(t->color.a, 0.0f, 1.0f) * 150.0f)), a), line);
                    }
                    dl.AddText(font, size, Vec2(x, y), C(t->color), line);
                    y += lineHeight;
                }
            }
        }
        const bool clip = el->clip || el->scroll;
        if (clip)
            dl.PushClipRect(r);
        for (ecs::Entity c : scene::GetChildren(world, e))
            Draw(c);
        if (clip)
            dl.PopClipRect();
        // Scroll bar: where the view is over the content.
        const f32 viewH = (r.Height() / s) - el->padding.y - el->padding.w;
        if (el->scroll && el->contentHeight > viewH + 0.5f) {
            const f32 trackH = r.Height() - 8.0f * s;
            const f32 thumbH = Max(trackH * viewH / el->contentHeight, 24.0f * s);
            const f32 t = el->scrollOffset / Max(el->contentHeight - viewH, 1.0f);
            const f32 top = r.min.y + 4.0f * s + (trackH - thumbH) * t;
            dl.AddRect(Rect(Vec2(r.max.x - 7.0f * s, top), Vec2(r.max.x - 3.0f * s, top + thumbH)), Fade(Color(255, 255, 255, 90), a), ui::CornerRadii(2.0f * s));
        }
        // Gamepad / keyboard focus ring.
        if (navActive && el->focused) {
            f32 radius = 6.0f;
            if (const UIButton* b = world.Get<const UIButton>(e))
                radius = b->cornerRadius;
            else if (const UIInputField* in = world.Get<const UIInputField>(e))
                radius = in->cornerRadius;
            else if (world.Has<UIToggle>(e) || world.Has<UISlider>(e))
                radius = r.Height() * 0.5f / s;
            const f32 o = 3.0f * s;
            dl.AddRectOutline(Rect(r.min - Vec2(o), r.max + Vec2(o)), Fade(Color(255, 255, 255, 220), a), 2.0f * s, ui::CornerRadii(radius * s + o));
        }
    }
};

/// Canvases by order (stable), each with its scale.
void Canvases(ecs::World& world, Array<ecs::Entity>& out)
{
    out.Clear();
    world.Query<const UICanvas>().Each([&](ecs::Entity e, const UICanvas&) { out.PushBack(e); });
    StableSort(out, [&](ecs::Entity a, ecs::Entity b) {
        const UICanvas& ca = *world.Get<const UICanvas>(a);
        const UICanvas& cb = *world.Get<const UICanvas>(b);
        if (ca.worldSpace != cb.worldSpace)
            return ca.worldSpace; // under the screen UI
        if (ca.order != cb.order)
            return ca.order < cb.order;
        return ca.worldSpace && ca.depth > cb.depth; // far ones first
    });
}

} // namespace

void GameUISystem::Layout(ecs::World& world, ui::FontSystem& fonts, const Rect& screen)
{
    Array<ecs::Entity> canvases;
    Canvases(world, canvases);
    // Styles (state rules from the last frame's hover / press / focus are undone first).
    RestoreOverrides(world);
    for (ecs::Entity c : canvases) {
        if (!world.Has<UIElement>(c))
            world.Add(c, UIElement{});
        ApplyStyles(world, c);
    }
    // The Look's targets; without Update (editing) it snaps there.
    world.Query<UIElement>().Each([&](ecs::Entity, UIElement& el) {
        el.targetOpacity = el.opacity;
        el.targetScale = el.scale;
        el.targetTranslate = el.translate;
        if (!m_playing) {
            el.shownOpacity = el.opacity;
            el.shownScale = el.scale;
            el.shownTranslate = el.translate;
            el.animOpacity = el.animScale = 1.0f;
            el.animTranslate = Vec2();
        }
    });
    for (ecs::Entity c : canvases) {
        UICanvas& canvas = *world.Get<UICanvas>(c);
        const f32 base = screen.Height() / Max(canvas.referenceHeight, 1.0f);
        if (!canvas.worldSpace) {
            canvas.onScreen = true;
            LayoutPass pass{world, fonts, base};
            pass.Measure(c);
            pass.Arrange(c, screen);
            continue;
        }
        // World space: over the entity, sized by distance.
        canvas.onScreen = false;
        if (!m_hasCamera)
            continue;
        const Vec3 at = scene::ComputeWorldMatrix(world, c).GetTranslation() + canvas.worldOffset;
        const f32 distance = Length(at - m_cameraPosition);
        canvas.depth = distance;
        const Vec4 clip = m_viewProj * Vec4(at.x, at.y, at.z, 1.0f);
        if (clip.w <= 0.05f || distance > canvas.maxDistance)
            continue;
        const Vec2 ndc(clip.x / clip.w, clip.y / clip.w);
        if (Abs(ndc.x) > 1.3f || Abs(ndc.y) > 1.3f)
            continue;
        const Vec2 point(screen.min.x + (ndc.x * 0.5f + 0.5f) * screen.Width(), screen.min.y + (0.5f - ndc.y * 0.5f) * screen.Height());
        const f32 k = canvas.fullSizeDistance > 0.0f ? Clamp(canvas.fullSizeDistance / Max(distance, 0.01f), Clamp(canvas.minScale, 0.01f, 1.0f), 1.0f) : 1.0f;
        LayoutPass pass{world, fonts, base * k};
        const Vec2 size = pass.Measure(c) * pass.scale;
        const UIElement& root = *world.Get<const UIElement>(c);
        const Vec2 min = Round(point - Vec2(size.x * root.pivot.x, size.y * root.pivot.y));
        pass.Arrange(c, Rect(min, min + size));
        canvas.onScreen = true;
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
        const UICanvas& canvas = *world.Get<const UICanvas>(c);
        if (!canvas.onScreen)
            continue;
        DrawPass pass{world, fonts, dl, textureSrv, el->pixelScale, m_focused, time};
        pass.navActive = m_navActive;
        pass.Draw(c);
    }
    RestoreOverrides(world); // state styles last only for the frame
}

void GameUISystem::Interact(ecs::World& world, Vec2 mouse, bool down, bool pressed, bool released, f32 wheel)
{
    // The topmost interactive widget under the mouse (later canvases / later children draw on top).
    ecs::Entity hot, scrollTarget;
    Array<ecs::Entity> canvases;
    Canvases(world, canvases);
    Function<void(ecs::Entity)> visit = [&](ecs::Entity e) {
        const UIElement* el = world.Get<const UIElement>(e);
        if (!el || !el->visible)
            return;
        const bool interactive = (world.Has<UIButton>(e) && world.Get<const UIButton>(e)->interactable) ||
                                 (world.Has<UISlider>(e) && world.Get<const UISlider>(e)->interactable) ||
                                 (world.Has<UIToggle>(e) && world.Get<const UIToggle>(e)->interactable) ||
                                 (world.Has<UIInputField>(e) && world.Get<const UIInputField>(e)->interactable);
        if (interactive && el->rect.Contains(mouse))
            hot = e;
        if (el->scroll && el->rect.Contains(mouse))
            scrollTarget = e; // the innermost scroll view under the mouse
        if ((el->clip || el->scroll) && !el->rect.Contains(mouse))
            return;
        for (ecs::Entity c : scene::GetChildren(world, e))
            visit(c);
    };
    for (ecs::Entity c : canvases)
        if (const UICanvas* cv = world.Get<const UICanvas>(c); cv && cv->onScreen)
            visit(c);
    m_mouseOver = bool(hot) || bool(scrollTarget);
    if (wheel != 0.0f && scrollTarget)
        if (UIElement* el = world.Get<UIElement>(scrollTarget))
            el->scrollOffset = Clamp(el->scrollOffset - wheel * 60.0f, 0.0f, Max(el->contentHeight - el->rect.Height(), 0.0f) + 1e4f);
    if (pressed && hot)
        m_pressed = hot;
    // A click focuses an input field; anywhere else leaves it.
    if (pressed)
        m_focused = hot && world.Has<UIInputField>(hot) ? hot : ecs::Entity();
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

namespace zx {

void GameUISystem::TypeText(ecs::World& world, StringView typed, bool backspace, bool enter, bool escape)
{
    if (!m_focused || !world.IsAlive(m_focused)) {
        m_focused = {};
        return;
    }
    UIInputField* in = world.Get<UIInputField>(m_focused);
    if (!in || !in->interactable) {
        m_focused = {};
        return;
    }
    bool changed = false;
    // Printable characters only (the platform also reports control characters as text).
    for (usize i = 0; i < typed.Size(); ++i) {
        const u8 c = u8(typed[i]);
        if (c < 0x20 || c == 0x7F)
            continue;
        if ((c & 0xC0) != 0x80 && CodepointCount(in->text.View()) >= usize(Max(in->maxLength, 1)))
            break;
        usize n = 1;
        if (c >= 0xF0)
            n = 4;
        else if (c >= 0xE0)
            n = 3;
        else if (c >= 0xC0)
            n = 2;
        in->text += typed.SubStr(i, Min(n, typed.Size() - i));
        i += n - 1;
        changed = true;
    }
    if (backspace && !in->text.Empty()) {
        usize end = in->text.Size();
        do {
            --end;
        } while (end > 0 && (u8(in->text[end]) & 0xC0) == 0x80);
        in->text = String(in->text.View().SubStr(0, end));
        changed = true;
    }
    if (changed)
        m_events.PushBack(UIEvent{UIEvent::Type::Changed, m_focused, f32(CodepointCount(in->text.View())), {}});
    if (enter || escape)
        m_skipNav = true; // the input field used the key
    if (enter) {
        m_events.PushBack(UIEvent{UIEvent::Type::Submit, m_focused, 0.0f, in->text});
        if (in->clearOnSubmit)
            in->text.Clear();
        m_focused = {};
    }
    if (escape)
        m_focused = {};
}

} // namespace zx
