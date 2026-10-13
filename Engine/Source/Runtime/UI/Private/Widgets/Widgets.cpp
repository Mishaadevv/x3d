// Basic widgets: text, buttons, toggles, sliders/drags, combo, trees, tabs, menus, drag & drop, color, misc.

#include "UI/Svg.h"
#include "../Core/Internal.h"
#include "Core/String/Utf.h"

namespace zx::ui {

namespace {

Color ButtonColor(const Theme& c, ButtonVariant v, bool hovered, bool held)
{
    switch (v) {
    case ButtonVariant::Primary:
        return held ? c.accentActive : (hovered ? c.accentHovered : c.accent);
    case ButtonVariant::Danger:
        return held ? LerpColor(c.error, Color(0, 0, 0), 0.2f) : (hovered ? LerpColor(c.error, Color(255, 255, 255), 0.12f) : c.error);
    case ButtonVariant::Ghost:
        return held ? c.buttonActive : (hovered ? c.buttonHovered : Color(0, 0, 0, 0));
    default:
        return held ? c.buttonActive : (hovered ? c.buttonHovered : c.button);
    }
}

/// Label to the right of a frame of `frameWidth`; returns the total item rect.
Rect LayoutFrameWithLabel(Context::Impl& g, f32 frameWidth, StringView label, Rect& frame)
{
    WindowState* w = g.current;
    const f32 fh = g.FrameHeight();
    frame = Rect(w->cursor, w->cursor + Vec2(frameWidth, fh));
    const StringView shown = DisplayLabel(label);
    const f32 labelW = shown.Empty() ? 0.0f : g.S(g.style.itemInnerSpacing) + g.CalcTextSize(shown).x;
    return Rect(frame.min, frame.max + Vec2(labelW, 0.0f));
}

void RenderLabelRight(Context::Impl& g, const Rect& frame, const Rect& total, StringView label)
{
    const StringView shown = DisplayLabel(label);
    if (!shown.Empty())
        g.RenderTextClipped(Rect(Vec2(frame.max.x + g.S(g.style.itemInnerSpacing), frame.min.y), total.max), shown, g.TextColor());
}

void RenderChevron(Context::Impl& g, const Rect& r, bool open, Color color)
{
    g.PushFontSmall();
    g.current->dl->AddTextInRect(g.Font(), g.FontSize(), r, g.Col(color), open ? ZX_ICON_CHEVRON_DOWN : ZX_ICON_CHEVRON_RIGHT,
                                 TextAlign::Center, false);
    g.fontStack.PopBack();
}

} // namespace

// --- Text ------------------------------------------------------------------------------------------------------------

void AlignTextToFramePadding()
{
    Context::Impl& g = G();
    g.current->currLineHeight = Max(g.current->currLineHeight, g.FrameHeight());
}

void Text(StringView text)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const Vec2 size = g.CalcTextSize(text);
    const f32 offY = w->currLineHeight > size.y ? Round((w->currLineHeight - size.y) * 0.5f) : 0.0f;
    const Rect r(w->cursor + Vec2(0.0f, offY), w->cursor + Vec2(0.0f, offY) + size);
    g.ItemSize(Vec2(size.x, size.y + offY));
    if (g.ItemAdd(r, 0))
        w->dl->AddText(g.Font(), g.FontSize(), r.min, g.TextColor(), text);
}

void TextColored(Color color, StringView text)
{
    PushTextColor(color);
    Text(text);
    PopTextColor();
}

void TextDisabled(StringView text) { TextColored(G().style.colors.textDim, text); }

void TextWrapped(StringView text)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const f32 wrap = Max(w->workMaxX - w->cursor.x, g.S(20.0f));
    const Vec2 size = g.CalcTextSize(text, wrap);
    const Rect r(w->cursor, w->cursor + size);
    g.ItemSize(size);
    if (g.ItemAdd(r, 0))
        w->dl->AddText(g.Font(), g.FontSize(), r.min, g.TextColor(), text, wrap);
}

void Heading(StringView text)
{
    Context::Impl& g = G();
    PushFont(FontStyle::SemiBold, g.style.headingFontSize);
    Text(text);
    PopFont();
}

void BulletText(StringView text)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const f32 lh = g.LineHeight();
    const Vec2 ts = g.CalcTextSize(text);
    const f32 bulletW = Round(g.S(g.style.indentSpacing));
    const Rect r(w->cursor, w->cursor + Vec2(bulletW + ts.x, Max(lh, ts.y)));
    g.ItemSize(r.Size());
    if (!g.ItemAdd(r, 0))
        return;
    w->dl->AddCircle(Vec2(r.min.x + bulletW * 0.4f, r.min.y + lh * 0.5f), Max(2.0f, g.S(2.2f)), g.Col(g.style.colors.textDim));
    w->dl->AddText(g.Font(), g.FontSize(), Vec2(r.min.x + bulletW, r.min.y), g.TextColor(), text);
}

void LabelText(StringView label, StringView value)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const f32 fh = g.FrameHeight();
    const f32 width = w->workMaxX - w->cursor.x;
    const f32 split = Round(width * (1.0f - g.style.defaultItemWidthRatio));
    const Rect r(w->cursor, w->cursor + Vec2(width, fh));
    g.ItemSize(r.Size());
    if (!g.ItemAdd(r, 0))
        return;
    g.RenderTextClipped(Rect(r.min, Vec2(r.min.x + split - g.S(6.0f), r.max.y)), DisplayLabel(label), g.Col(g.style.colors.textDim));
    g.RenderTextClipped(Rect(Vec2(r.min.x + split, r.min.y), r.max), value, g.TextColor());
}

bool TextLink(StringView text)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(text);
    const Vec2 size = g.CalcTextSize(text);
    const Rect r(w->cursor, w->cursor + size);
    g.ItemSize(size);
    if (!g.ItemAdd(r, id))
        return false;
    bool hovered, held;
    const bool pressed = g.ButtonBehavior(r, id, &hovered, &held);
    if (hovered)
        g.SetCursorShape(CursorShape::Hand);
    const Color c = g.Col(hovered ? g.style.colors.accentHovered : g.style.colors.accent);
    w->dl->AddText(g.Font(), g.FontSize(), r.min, c, text);
    if (hovered)
        w->dl->AddRect(Rect(Vec2(r.min.x, r.max.y - g.S(2.0f)), Vec2(r.max.x, r.max.y - g.S(2.0f) + Max(1.0f, Round(g.S(1.0f))))), c);
    return pressed;
}

// --- Buttons ---------------------------------------------------------------------------------------------------------

bool Button(StringView label, Vec2 size, ButtonVariant variant)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    const StringView shown = DisplayLabel(label);
    const Vec2 ts = g.CalcTextSize(shown);
    const f32 padX = Round(g.S(g.style.framePadding.x + 4.0f));
    Vec2 sz(ts.x + padX * 2.0f, g.FrameHeight());
    if (size.x > 0.0f)
        sz.x = Round(g.S(size.x));
    else if (size.x < 0.0f)
        sz.x = Max(w->workMaxX - w->cursor.x + g.S(size.x + 1.0f), g.S(10.0f));
    if (size.y > 0.0f)
        sz.y = Round(g.S(size.y));
    const Rect r(w->cursor, w->cursor + sz);
    g.ItemSize(sz);
    if (!g.ItemAdd(r, id))
        return false;
    bool hovered, held;
    const bool pressed = g.ButtonBehavior(r, id, &hovered, &held);
    const Theme& c = g.style.colors;
    const bool filled = variant == ButtonVariant::Primary || variant == ButtonVariant::Danger;
    w->dl->AddRect(r, g.Col(ButtonColor(c, variant, hovered, held)), CornerRadii(g.S(g.style.frameRounding)),
                   variant == ButtonVariant::Ghost || filled ? 0.0f : 1.0f, g.Col(hovered ? c.borderStrong : c.border));
    g.RenderTextClipped(r, shown, filled ? g.Col(c.accentText) : g.TextColor(), TextAlign::Center);
    return pressed;
}

bool IconButton(StringView icon, StringView tooltip, bool selected)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    PushID(tooltip.Empty() ? icon : tooltip);
    const ID id = g.MakeID("##icon");
    PopID();
    const f32 fh = g.FrameHeight();
    const Rect r(w->cursor, w->cursor + Vec2(fh, fh));
    g.ItemSize(r.Size());
    if (!g.ItemAdd(r, id))
        return false;
    bool hovered, held;
    const bool pressed = g.ButtonBehavior(r, id, &hovered, &held);
    const Theme& c = g.style.colors;
    Color bg = selected ? WithAlpha(c.accent, 0.35f) : ButtonColor(c, ButtonVariant::Ghost, hovered, held);
    if (selected && hovered)
        bg = WithAlpha(c.accent, 0.5f);
    w->dl->AddRect(r, g.Col(bg), CornerRadii(g.S(g.style.frameRounding)));
    g.RenderTextClipped(r, icon, selected ? g.Col(c.text) : (hovered ? g.TextColor() : g.Col(c.textDim)), TextAlign::Center);
    if (!tooltip.Empty())
        ItemTooltip(tooltip);
    return pressed;
}

bool InvisibleButton(StringView idStr, Vec2 size)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(idStr);
    const Rect r(w->cursor, w->cursor + Round(g.S(size)));
    g.ItemSize(r.Size());
    if (!g.ItemAdd(r, id))
        return false;
    return g.ButtonBehavior(r, id, nullptr, nullptr);
}

bool Checkbox(StringView label, bool* value)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    const f32 fh = g.FrameHeight();
    const f32 box = Round(g.LineHeight() * 0.95f);
    const StringView shown = DisplayLabel(label);
    const f32 labelW = shown.Empty() ? 0.0f : g.S(g.style.itemInnerSpacing) + g.CalcTextSize(shown).x;
    const Rect total(w->cursor, w->cursor + Vec2(box + labelW, fh));
    g.ItemSize(total.Size());
    if (!g.ItemAdd(total, id))
        return false;
    bool hovered, held;
    const bool pressed = g.ButtonBehavior(total, id, &hovered, &held);
    if (pressed) {
        *value = !*value;
        g.editedId = id;
    }
    const Theme& c = g.style.colors;
    const Rect br(Vec2(total.min.x, Round(total.min.y + (fh - box) * 0.5f)), Vec2(total.min.x + box, Round(total.min.y + (fh - box) * 0.5f) + box));
    if (*value) {
        w->dl->AddRect(br, g.Col(held ? c.accentActive : (hovered ? c.accentHovered : c.accent)), CornerRadii(g.S(3.5f)));
        g.RenderCheckMark(br, c.accentText);
    } else {
        w->dl->AddRect(br, g.Col(held ? c.frameBgActive : (hovered ? c.frameBgHovered : c.frameBg)), CornerRadii(g.S(3.5f)), 1.0f,
                       g.Col(hovered ? c.borderStrong : c.border));
    }
    if (!shown.Empty())
        g.RenderTextClipped(Rect(Vec2(br.max.x + g.S(g.style.itemInnerSpacing), total.min.y), total.max), shown, g.TextColor());
    return pressed;
}

bool Toggle(StringView label, bool* value)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    const f32 fh = g.FrameHeight();
    const f32 h = Round(g.LineHeight() * 0.95f);
    const f32 tw = Round(h * 1.8f);
    const StringView shown = DisplayLabel(label);
    const f32 labelW = shown.Empty() ? 0.0f : g.S(g.style.itemInnerSpacing) + g.CalcTextSize(shown).x;
    const Rect total(w->cursor, w->cursor + Vec2(tw + labelW, fh));
    g.ItemSize(total.Size());
    if (!g.ItemAdd(total, id))
        return false;
    bool hovered, held;
    const bool pressed = g.ButtonBehavior(total, id, &hovered, &held);
    if (pressed) {
        *value = !*value;
        g.editedId = id;
    }
    // Animated knob position.
    f32& t = g.floatStorage[id];
    const f32 target = *value ? 1.0f : 0.0f;
    t = Abs(t - target) < 0.01f ? target : Lerp(t, target, Saturate(g.dt * 18.0f));
    const Theme& c = g.style.colors;
    const Rect track(Vec2(total.min.x, Round(total.min.y + (fh - h) * 0.5f)), Vec2(total.min.x + tw, Round(total.min.y + (fh - h) * 0.5f) + h));
    const Color off = hovered ? c.frameBgHovered : c.frameBgActive;
    w->dl->AddRect(track, g.Col(LerpColor(off, hovered ? c.accentHovered : c.accent, t)), CornerRadii(h * 0.5f), 1.0f,
                   g.Col(LerpColor(c.borderStrong, c.accent, t)));
    const f32 knobR = h * 0.5f - g.S(2.5f);
    const Vec2 knobC(track.min.x + h * 0.5f + (tw - h) * t, track.min.y + h * 0.5f);
    w->dl->AddCircle(knobC, knobR, g.Col(LerpColor(c.textDim, Color(255, 255, 255), t)));
    if (!shown.Empty())
        g.RenderTextClipped(Rect(Vec2(track.max.x + g.S(g.style.itemInnerSpacing), total.min.y), total.max), shown, g.TextColor());
    return pressed;
}

bool RadioButton(StringView label, bool active)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    const f32 fh = g.FrameHeight();
    const f32 box = Round(g.LineHeight() * 0.95f);
    const StringView shown = DisplayLabel(label);
    const f32 labelW = shown.Empty() ? 0.0f : g.S(g.style.itemInnerSpacing) + g.CalcTextSize(shown).x;
    const Rect total(w->cursor, w->cursor + Vec2(box + labelW, fh));
    g.ItemSize(total.Size());
    if (!g.ItemAdd(total, id))
        return false;
    bool hovered, held;
    const bool pressed = g.ButtonBehavior(total, id, &hovered, &held);
    const Theme& c = g.style.colors;
    const Vec2 center(total.min.x + box * 0.5f, total.min.y + fh * 0.5f);
    if (active) {
        w->dl->AddCircle(center, box * 0.5f, g.Col(hovered ? c.accentHovered : c.accent));
        w->dl->AddCircle(center, box * 0.2f, g.Col(c.accentText));
    } else {
        w->dl->AddCircle(center, box * 0.5f, g.Col(hovered ? c.frameBgHovered : c.frameBg), 1.0f, g.Col(hovered ? c.borderStrong : c.border));
    }
    if (!shown.Empty())
        g.RenderTextClipped(Rect(Vec2(total.min.x + box + g.S(g.style.itemInnerSpacing), total.min.y), total.max), shown, g.TextColor());
    return pressed;
}

bool RadioButton(StringView label, i32* value, i32 buttonValue)
{
    const bool pressed = RadioButton(label, *value == buttonValue);
    if (pressed)
        *value = buttonValue;
    return pressed;
}

bool Selectable(StringView label, bool selected, Vec2 size)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    const StringView shown = DisplayLabel(label);
    const Vec2 ts = g.CalcTextSize(shown);
    const f32 padX = Round(g.S(g.style.framePadding.x));
    const Theme& c = g.style.colors;
    TableState* table = !g.tableStack.Empty() && g.tableStack.Back()->column >= 0 ? g.tableStack.Back() : nullptr;
    if (table) {
        // Inside a table: highlight the whole row from this cell to the right edge.
        const f32 rowH = g.LineHeight() + table->cellPadY * 2.0f;
        const TableColumn& col = table->columns[u32(table->column)];
        const Rect r(Vec2(col.x, table->rowY), Vec2(table->rect.max.x, table->rowY + rowH));
        g.ItemSize(Vec2(ts.x, g.LineHeight()));
        if (!g.ItemAdd(r, id))
            return false;
        bool hovered, held;
        const bool pressed = g.ButtonBehavior(r, id, &hovered, &held);
        if (selected || hovered) {
            Rect clip = r.Intersect(w->clip);
            clip.max = Max(clip.max, clip.min);
            w->dl->PushClipRect(clip, 0.0f, false);
            w->dl->AddRect(r, g.Col(selected ? c.selection : c.headerHovered));
            w->dl->PopClipRect();
        }
        g.RenderTextClipped(Rect(Vec2(col.x + table->cellPadX, r.min.y), Vec2(col.x + col.width - table->cellPadX, r.max.y)), shown,
                            g.TextColor());
        return pressed;
    }
    const f32 h = size.y > 0.0f ? Round(g.S(size.y)) : Round(g.LineHeight() + g.S(6.0f));
    const f32 avail = w->workMaxX - w->cursor.x;
    const f32 width = size.x > 0.0f ? Round(g.S(size.x)) : Max(avail, ts.x + padX * 2.0f);
    const Rect r(w->cursor, w->cursor + Vec2(width, h));
    // Content size only counts the text, so auto-sized popups don't grow.
    g.ItemSize(Vec2(size.x > 0.0f ? width : ts.x + padX * 2.0f, h));
    if (!g.ItemAdd(r, id))
        return false;
    bool hovered, held;
    const bool pressed = g.ButtonBehavior(r, id, &hovered, &held);
    if (selected || hovered)
        w->dl->AddRect(r, g.Col(selected ? (hovered ? LerpColor(c.selection, Color(255, 255, 255), 0.06f) : c.selection) : c.headerHovered),
                       CornerRadii(g.S(3.0f)));
    g.RenderTextClipped(Rect(r.min + Vec2(padX, 0.0f), r.max - Vec2(padX, 0.0f)), shown, g.TextColor());
    return pressed;
}

void ProgressBar(f32 fraction, Vec2 size, StringView overlay)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const f32 avail = w->workMaxX - w->cursor.x;
    const Vec2 sz(size.x > 0.0f ? g.S(size.x) : (size.x < 0.0f ? avail : g.CalcItemWidth()), size.y > 0.0f ? g.S(size.y) : g.FrameHeight());
    const Rect r(w->cursor, w->cursor + Round(sz));
    g.ItemSize(r.Size());
    if (!g.ItemAdd(r, 0))
        return;
    const Theme& c = g.style.colors;
    const f32 rr = g.S(g.style.frameRounding);
    w->dl->AddRect(r, g.Col(c.frameBg), CornerRadii(rr), 1.0f, g.Col(c.border));
    const f32 f = Saturate(fraction);
    if (f > 0.0f) {
        w->dl->PushClipRect(Rect(r.min, Vec2(r.min.x + r.Width() * f, r.max.y)));
        w->dl->AddRectGradient(Rect(r.min + Vec2(1.0f), r.max - Vec2(1.0f)), g.Col(c.accentHovered), g.Col(c.accent),
                               GradientDirection::Vertical, CornerRadii(rr - 1.0f));
        w->dl->PopClipRect();
    }
    const String text = overlay.Empty() ? String::Format("{}%", i32(f * 100.0f + 0.5f)) : String(overlay);
    g.RenderTextClipped(r, text, g.TextColor(), TextAlign::Center);
}

void Spinner(f32 radius)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const f32 r = g.S(radius);
    const Rect rect(w->cursor, w->cursor + Vec2(r * 2.0f + g.S(2.0f)));
    g.ItemSize(rect.Size());
    if (!g.ItemAdd(rect, 0))
        return;
    const Vec2 center = rect.Center();
    constexpr u32 Dots = 8;
    const f32 phase = f32(Fmod(f32(g.time) * 1.2f, 1.0f));
    for (u32 i = 0; i < Dots; ++i) {
        const f32 a = TwoPi * f32(i) / f32(Dots) - HalfPi;
        const f32 t = Frac(phase - f32(i) / f32(Dots));
        const Vec2 p = center + Vec2(Cos(a), Sin(a)) * (r - g.S(2.0f));
        w->dl->AddCircle(p, g.S(1.4f) + g.S(1.0f) * (1.0f - t), WithAlpha(g.style.colors.accent, 0.25f + 0.75f * (1.0f - t)));
    }
}

// --- Sliders & drags ---------------------------------------------------------------------------------------------------

namespace {

/// Shared slider core over doubles. Returns true when the value changed.
bool SliderCore(Context::Impl& g, StringView label, f64& value, f64 min, f64 max, bool isInt, StringView format)
{
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    Rect frame;
    const Rect total = LayoutFrameWithLabel(g, g.CalcItemWidth(), label, frame);
    g.ItemSize(total.Size());
    if (!g.ItemAdd(total, id) && g.activeId != id)
        return false;
    // Ctrl+click / double-click: type a value.
    TextEditState& st = g.textEdit;
    const bool typing = g.activeId == id && st.id == id && st.numeric;
    bool changed = false;
    if (typing) {
        changed = NumericTextEdit(id, frame, value, isInt, format, false);
        if (changed)
            value = Clamp(value, min, max);
        RenderLabelRight(g, frame, total, label);
        return changed;
    }
    const bool hovered = g.ItemHoverable(frame, id);
    if (hovered && g.mousePressed[0] && (HasAnyFlags(g.mods, KeyModifiers::Control) || g.mouseDoubleClicked[0])) {
        NumericTextEdit(id, frame, value, isInt, format, true);
        RenderLabelRight(g, frame, total, label);
        return false;
    }
    if (hovered && g.mousePressed[0])
        g.SetActive(id, w);
    const bool held = g.activeId == id && g.mouseDown[0];
    if (g.activeId == id && !g.mouseDown[0])
        g.ClearActive();
    if (held) {
        g.activeIdAlive = true;
        const f32 inset = g.S(4.0f);
        const f64 t = Saturate((g.mouse.x - frame.min.x - inset) / Max(frame.Width() - inset * 2.0f, 1.0f));
        f64 nv = min + (max - min) * t;
        if (isInt)
            nv = Floor(f32(nv) + 0.5f);
        if (nv != value) {
            value = nv;
            changed = true;
        }
        g.SetCursorShape(CursorShape::ResizeEW);
    }
    const Theme& c = g.style.colors;
    const f32 rr = g.S(g.style.frameRounding);
    g.current->dl->AddRect(frame, g.Col(held ? c.frameBgActive : (hovered ? c.frameBgHovered : c.frameBg)), CornerRadii(rr), 1.0f,
                           g.Col(hovered || held ? c.borderStrong : c.border));
    const f32 t = max > min ? f32((value - min) / (max - min)) : 0.0f;
    if (t > 0.0f) {
        const Rect fill(frame.min + Vec2(1.0f), Vec2(frame.min.x + 1.0f + (frame.Width() - 2.0f) * Saturate(t), frame.max.y - 1.0f));
        g.current->dl->PushClipRect(fill);
        g.current->dl->AddRect(Rect(frame.min + Vec2(1.0f), frame.max - Vec2(1.0f)), g.Col(WithAlpha(held ? c.accentActive : c.accent, 0.55f)),
                               CornerRadii(rr - 1.0f));
        g.current->dl->PopClipRect();
    }
    const String text = isInt ? String::Format("{}", i64(value)) : String::Format(format, value);
    g.RenderTextClipped(frame, text, g.TextColor(), TextAlign::Center);
    RenderLabelRight(g, frame, total, label);
    if (changed)
        g.editedId = id;
    return changed;
}

/// Drag core: horizontal mouse drag changes the value by `speed` per pixel (Shift x10, Alt x0.1).
bool DragCore(Context::Impl& g, ID id, const Rect& frame, f64& value, f64 speed, f64 min, f64 max, bool isInt, StringView format,
              Color marker)
{
    TextEditState& st = g.textEdit;
    if (g.activeId == id && st.id == id && st.numeric) {
        const bool changed = NumericTextEdit(id, frame, value, isInt, format, false);
        if (changed && max > min)
            value = Clamp(value, min, max);
        return changed;
    }
    const bool hovered = g.ItemHoverable(frame, id);
    if (hovered && g.mousePressed[0] && (HasAnyFlags(g.mods, KeyModifiers::Control) || g.mouseDoubleClicked[0])) {
        NumericTextEdit(id, frame, value, isInt, format, true);
        return false;
    }
    if (hovered && g.mousePressed[0]) {
        g.SetActive(id, g.current);
        g.activeDragValue = 0.0f;
    }
    if (hovered || g.activeId == id)
        g.SetCursorShape(CursorShape::ResizeEW);
    bool changed = false;
    const bool held = g.activeId == id && g.mouseDown[0];
    if (g.activeId == id && !g.mouseDown[0])
        g.ClearActive();
    if (held) {
        g.activeIdAlive = true;
        f64 s = speed;
        if (HasAnyFlags(g.mods, KeyModifiers::Shift))
            s *= 10.0;
        if (HasAnyFlags(g.mods, KeyModifiers::Alt))
            s *= 0.1;
        // Accumulate fractional steps so slow drags still move integer values.
        g.activeDragValue += g.mouseDelta.x / g.scale;
        f64 delta = f64(g.activeDragValue) * s;
        if (isInt) {
            delta = f64(i64(delta));
            g.activeDragValue -= f32(delta / s);
        } else {
            g.activeDragValue = 0.0f;
        }
        if (delta != 0.0) {
            f64 nv = value + delta;
            if (max > min)
                nv = Clamp(nv, min, max);
            if (nv != value) {
                value = nv;
                changed = true;
            }
        }
    }
    const Theme& c = g.style.colors;
    const f32 rr = g.S(g.style.frameRounding);
    g.current->dl->AddRect(frame, g.Col(held ? c.frameBgActive : (hovered ? c.frameBgHovered : c.frameBg)), CornerRadii(rr), 1.0f,
                           g.Col(hovered || held ? c.borderStrong : c.border));
    if (marker.a != 0)
        g.current->dl->AddRect(Rect(frame.min + Vec2(1.0f), Vec2(frame.min.x + Max(2.0f, Round(g.S(3.0f))), frame.max.y - 1.0f)),
                               g.Col(marker), CornerRadii::Left(rr - 1.0f));
    const String text = isInt ? String::Format("{}", i64(value)) : String::Format(format, value);
    g.RenderTextClipped(frame, text, g.TextColor(), TextAlign::Center);
    if (changed)
        g.editedId = id;
    return changed;
}

bool DragN(StringView label, f32* values, u32 count, f32 speed, f32 min, f32 max)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    Rect frame;
    const Rect total = LayoutFrameWithLabel(g, g.CalcItemWidth(), label, frame);
    g.ItemSize(total.Size());
    if (!g.ItemAdd(total, id) && (g.activeId == 0 || g.activeWindow != w))
        return false;
    static const Color Markers[4] = {Color(0xE0, 0x4B, 0x4B), Color(0x6C, 0xC0, 0x4A), Color(0x4A, 0x83, 0xE0), Color(0xB0, 0xB0, 0xB0)};
    const f32 gap = Round(g.S(4.0f));
    const f32 each = Floor((frame.Width() - gap * f32(count - 1)) / f32(count));
    bool changed = false;
    PushID(label);
    for (u32 i = 0; i < count; ++i) {
        const ID cid = g.MakeID(i32(i));
        const Rect r(Vec2(frame.min.x + f32(i) * (each + gap), frame.min.y), Vec2(frame.min.x + f32(i) * (each + gap) + each, frame.max.y));
        f64 v = values[i];
        if (DragCore(g, cid, r, v, speed, min, max, false, "{:.3f}", Markers[i])) {
            values[i] = f32(v);
            changed = true;
        }
    }
    PopID();
    RenderLabelRight(g, frame, total, label);
    g.lastItemId = id;
    g.lastItemRect = total;
    if (changed)
        g.editedId = id;
    return changed;
}

} // namespace

bool SliderFloat(StringView label, f32* value, f32 min, f32 max, StringView format)
{
    Context::Impl& g = G();
    f64 v = *value;
    const bool changed = SliderCore(g, label, v, min, max, false, format);
    *value = f32(v);
    return changed;
}

bool SliderInt(StringView label, i32* value, i32 min, i32 max)
{
    Context::Impl& g = G();
    f64 v = *value;
    const bool changed = SliderCore(g, label, v, min, max, true, "{}");
    *value = i32(v);
    return changed;
}

bool DragFloat(StringView label, f32* value, f32 speed, f32 min, f32 max, StringView format)
{
    Context::Impl& g = G();
    const ID id = g.MakeID(label);
    Rect frame;
    const Rect total = LayoutFrameWithLabel(g, g.CalcItemWidth(), label, frame);
    g.ItemSize(total.Size());
    if (!g.ItemAdd(total, id) && g.activeId != id)
        return false;
    f64 v = *value;
    const bool changed = DragCore(g, id, frame, v, speed, min, max, false, format, Color(0, 0, 0, 0));
    *value = f32(v);
    RenderLabelRight(g, frame, total, label);
    return changed;
}

bool DragFloat2(StringView label, f32 value[2], f32 speed, f32 min, f32 max) { return DragN(label, value, 2, speed, min, max); }
bool DragFloat3(StringView label, f32 value[3], f32 speed, f32 min, f32 max) { return DragN(label, value, 3, speed, min, max); }

bool DragInt(StringView label, i32* value, f32 speed, i32 min, i32 max)
{
    Context::Impl& g = G();
    const ID id = g.MakeID(label);
    Rect frame;
    const Rect total = LayoutFrameWithLabel(g, g.CalcItemWidth(), label, frame);
    g.ItemSize(total.Size());
    if (!g.ItemAdd(total, id) && g.activeId != id)
        return false;
    f64 v = *value;
    const bool changed = DragCore(g, id, frame, v, speed, min, max, true, "{}", Color(0, 0, 0, 0));
    *value = i32(v);
    RenderLabelRight(g, frame, total, label);
    return changed;
}

// --- Combo ---------------------------------------------------------------------------------------------------------------

bool Combo(StringView label, i32* current, Span<const StringView> items)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    Rect frame;
    const Rect total = LayoutFrameWithLabel(g, g.CalcItemWidth(), label, frame);
    g.ItemSize(total.Size());
    if (!g.ItemAdd(total, id))
        return false;
    const String popupId = String::Format("##combo_{:08x}", id);
    const bool open = IsPopupOpen(popupId);
    bool hovered, held;
    if (g.ButtonBehavior(frame, id, &hovered, &held, true) && !open) {
        g.lastItemRect = frame;
        OpenPopup(popupId);
        g.openPopups.Back().isCombo = true;
    }
    const Theme& c = g.style.colors;
    const f32 fh = g.FrameHeight();
    w->dl->AddRect(frame, g.Col(open ? c.frameBgActive : (hovered ? c.frameBgHovered : c.frameBg)), CornerRadii(g.S(g.style.frameRounding)),
                   1.0f, g.Col(open ? c.accent : (hovered ? c.borderStrong : c.border)));
    const StringView preview = *current >= 0 && usize(*current) < items.Size() ? items[usize(*current)] : StringView();
    const f32 padX = Round(g.S(g.style.framePadding.x));
    g.RenderTextClipped(Rect(frame.min + Vec2(padX, 0.0f), Vec2(frame.max.x - fh, frame.max.y)), preview, g.TextColor());
    RenderChevron(g, Rect(Vec2(frame.max.x - fh, frame.min.y), frame.max), true, c.textDim);
    RenderLabelRight(g, frame, total, label);

    bool changed = false;
    if (BeginPopup(popupId)) {
        g.current->minAutoWidth = frame.Width();
        for (usize i = 0; i < items.Size(); ++i) {
            PushID(i32(i));
            const bool selected = i32(i) == *current;
            if (Selectable(items[i], selected)) {
                *current = i32(i);
                changed = true;
                CloseCurrentPopup();
            }
            if (selected && g.current->appearing)
                SetScrollHereY();
            PopID();
        }
        EndPopup();
    }
    if (changed)
        g.editedId = id;
    g.lastItemId = id;
    g.lastItemRect = total;
    return changed;
}

// --- Trees -------------------------------------------------------------------------------------------------------------

void SetNextItemOpen(bool open, Cond cond)
{
    Context::Impl& g = G();
    g.nextOpenSet = true;
    g.nextOpen = open;
    g.nextOpenCond = cond;
}

namespace {

bool TreeNodeBehavior(Context::Impl& g, ID id, StringView label, TreeNodeFlags flags)
{
    WindowState* w = g.current;
    const bool framed = HasAnyFlags(flags, TreeNodeFlags::Framed);
    const bool leaf = HasAnyFlags(flags, TreeNodeFlags::Leaf);
    // Open state.
    i32* stored = g.intStorage.Find(id);
    if (g.nextOpenSet) {
        if (g.nextOpenCond == Cond::Always || !stored)
            stored = &g.intStorage.InsertOrAssign(id, g.nextOpen ? 1 : 0);
        g.nextOpenSet = false;
    }
    if (!stored)
        stored = &g.intStorage.InsertOrAssign(id, HasAnyFlags(flags, TreeNodeFlags::DefaultOpen) ? 1 : 0);
    bool open = !leaf && *stored != 0;

    const f32 h = framed ? g.FrameHeight() : Round(g.LineHeight() + g.S(5.0f));
    const f32 x0 = framed ? w->workMinX : w->cursor.x;
    const Rect row(Vec2(x0, w->cursor.y), Vec2(w->workMaxX, w->cursor.y + h));
    const f32 arrowW = Round(g.S(16.0f));
    const StringView shown = DisplayLabel(label);
    if (framed)
        g.fontStack.PushBack({g.fontHandles[u32(FontStyle::SemiBold)], g.FontSize()});
    const f32 textW = g.CalcTextSize(shown).x;
    g.ItemSize(Vec2(arrowW + textW + g.S(8.0f), h));
    if (!g.ItemAdd(row, id)) {
        if (framed)
            g.fontStack.PopBack();
        return open;
    }
    bool hovered, held;
    const bool pressed = g.ButtonBehavior(row, id, &hovered, &held);
    if (pressed && !leaf) {
        const bool onArrow = g.mouse.x < row.min.x + arrowW + (framed ? g.S(4.0f) : 0.0f);
        if (!HasAnyFlags(flags, TreeNodeFlags::OpenOnArrow) || onArrow) {
            open = !open;
            *g.intStorage.Find(id) = open ? 1 : 0;
        }
    }
    if (hovered && g.mouseDoubleClicked[0] && !leaf && HasAnyFlags(flags, TreeNodeFlags::OpenOnArrow)) {
        open = !open;
        *g.intStorage.Find(id) = open ? 1 : 0;
    }
    const Theme& c = g.style.colors;
    if (framed) {
        w->dl->AddRect(row, g.Col(held ? c.headerActive : (hovered ? c.headerHovered : c.header)), CornerRadii(g.S(g.style.frameRounding)));
    } else if (HasAnyFlags(flags, TreeNodeFlags::Selected)) {
        w->dl->AddRect(Rect(Vec2(w->workMinX, row.min.y), row.max), g.Col(c.selection), CornerRadii(g.S(3.0f)));
    } else if (hovered) {
        w->dl->AddRect(Rect(Vec2(w->workMinX, row.min.y), row.max), g.Col(c.headerHovered), CornerRadii(g.S(3.0f)));
    }
    const f32 ax = row.min.x + (framed ? g.S(4.0f) : 0.0f);
    if (!leaf)
        RenderChevron(g, Rect(Vec2(ax, row.min.y), Vec2(ax + arrowW, row.max.y)), open, hovered ? c.text : c.textDim);
    g.RenderTextClipped(Rect(Vec2(ax + arrowW + g.S(2.0f), row.min.y), row.max), shown, g.TextColor());
    if (framed)
        g.fontStack.PopBack();
    g.lastItemHovered = hovered;
    return open;
}

} // namespace

bool TreeNode(StringView label, TreeNodeFlags flags)
{
    Context::Impl& g = G();
    const ID id = g.MakeID(label);
    const bool open = TreeNodeBehavior(g, id, label, flags);
    if (open && !HasAnyFlags(flags, TreeNodeFlags::NoTreePushOnOpen)) {
        Indent();
        g.current->idStack.PushBack(id);
    }
    return open;
}

void TreePop()
{
    Context::Impl& g = G();
    Unindent();
    g.current->idStack.PopBack();
}

bool CollapsingHeader(StringView label, TreeNodeFlags flags)
{
    Context::Impl& g = G();
    const ID id = g.MakeID(label);
    return TreeNodeBehavior(g, id, label, flags | TreeNodeFlags::Framed | TreeNodeFlags::NoTreePushOnOpen);
}

// --- Tabs ----------------------------------------------------------------------------------------------------------------

bool BeginTabBar(StringView idStr)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(idStr);
    UniquePtr<TabBarState>* found = g.tabBars.Find(id);
    if (!found)
        found = g.tabBars.Insert(ID(id), UniquePtr<TabBarState>(new TabBarState())).first;
    TabBarState* tb = found->Get();
    tb->id = id;
    if (tb->nextSelected) {
        tb->selected = tb->nextSelected;
        tb->nextSelected = 0;
    }
    const f32 h = g.FrameHeight() + g.S(2.0f);
    tb->barRect = Rect(Vec2(w->workMinX + w->indent, w->cursor.y), Vec2(w->workMaxX, w->cursor.y + h));
    tb->nextTabX = tb->barRect.min.x;
    tb->anyContent = false;
    tb->sawSelected = false;
    tb->firstTab = 0;
    w->dl->AddRect(Rect(Vec2(tb->barRect.min.x, tb->barRect.max.y - 1.0f), tb->barRect.max), g.Col(g.style.colors.border));
    g.ItemSize(Vec2(tb->barRect.Width(), h));
    tb->contentStartY = w->cursor.y;
    g.tabBarStack.PushBack(tb);
    w->idStack.PushBack(id);
    return true;
}

bool BeginTabItem(StringView label, bool* open)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    ZX_ASSERT(!g.tabBarStack.Empty(), "BeginTabItem outside BeginTabBar");
    TabBarState* tb = g.tabBarStack.Back();
    if (open && !*open)
        return false;
    const ID id = g.MakeID(label);
    if (tb->firstTab == 0)
        tb->firstTab = id;
    const StringView shown = DisplayLabel(label);
    const f32 padX = Round(g.S(12.0f));
    const f32 closeW = open ? Round(g.S(18.0f)) : 0.0f;
    const f32 width = g.CalcTextSize(shown).x + padX * 2.0f + closeW;
    const Rect r(Vec2(tb->nextTabX, tb->barRect.min.y), Vec2(tb->nextTabX + width, tb->barRect.max.y));
    tb->nextTabX += width + g.S(2.0f);
    // Tabs are drawn in the bar row without moving the layout cursor.
    const Vec2 savedCursor = w->cursor;
    g.ItemAdd(r, id);
    bool hovered, held;
    if (g.ButtonBehavior(r, id, &hovered, &held, true))
        tb->selected = id;
    if (tb->selected == 0)
        tb->selected = id;
    const bool selected = tb->selected == id;
    const Theme& c = g.style.colors;
    const f32 rr = g.S(g.style.frameRounding);
    if (selected) {
        w->dl->AddRect(Rect(r.min, r.max), g.Col(c.tabActive), CornerRadii::Top(rr), 1.0f, g.Col(c.border));
        w->dl->AddRect(Rect(Vec2(r.min.x + 1.0f, r.max.y - 1.0f), Vec2(r.max.x - 1.0f, r.max.y + 1.0f)), g.Col(c.tabActive));
        w->dl->AddRect(Rect(Vec2(r.min.x + 1.0f, r.min.y), Vec2(r.max.x - 1.0f, r.min.y + Max(2.0f, Round(g.S(2.0f))))), g.Col(c.accent),
                       CornerRadii::Top(rr));
    } else if (hovered) {
        w->dl->AddRect(Rect(r.min + Vec2(0.0f, g.S(2.0f)), r.max - Vec2(0.0f, 1.0f)), g.Col(c.tabHovered), CornerRadii::Top(rr));
    }
    g.RenderTextClipped(Rect(r.min + Vec2(padX, 0.0f), Vec2(r.max.x - padX - closeW, r.max.y)), shown,
                        selected ? g.TextColor() : g.Col(hovered ? c.text : c.textDim));
    if (open) {
        const Rect cr(Vec2(r.max.x - padX * 0.5f - closeW, r.min.y + (r.Height() - closeW) * 0.5f),
                      Vec2(r.max.x - padX * 0.5f, r.min.y + (r.Height() + closeW) * 0.5f));
        const ID cid = id + 0x51;
        bool ch, cheld;
        if (g.ButtonBehavior(cr, cid, &ch, &cheld))
            *open = false;
        if (ch)
            w->dl->AddRect(cr, g.Col(c.buttonHovered), CornerRadii(g.S(3.0f)));
        if (selected || hovered || ch) {
            PushFont(FontStyle::Regular, g.style.smallFontSize);
            g.RenderTextClipped(cr, ZX_ICON_X, g.Col(c.textDim), TextAlign::Center);
            PopFont();
        }
    }
    w->cursor = savedCursor;
    if (selected) {
        tb->sawSelected = true;
        tb->anyContent = true;
        w->cursor.y = tb->contentStartY;
        w->idStack.PushBack(id);
        return true;
    }
    return false;
}

void EndTabItem()
{
    Context::Impl& g = G();
    g.current->idStack.PopBack();
}

void EndTabBar()
{
    Context::Impl& g = G();
    ZX_ASSERT(!g.tabBarStack.Empty(), "EndTabBar without BeginTabBar");
    TabBarState* tb = g.tabBarStack.PopBackValue();
    if (!tb->sawSelected && tb->firstTab)
        tb->selected = tb->firstTab; // the selected tab disappeared
    g.current->idStack.PopBack();
}

// --- Menus ---------------------------------------------------------------------------------------------------------------

bool BeginMenuBarInRect(const Rect& bar)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    w->inMenuBar = true;
    w->menuBarSavedCursor = w->cursor;
    w->menuBarSavedLineHeight = w->currLineHeight;
    w->dl->PushClipRect(bar, 0.0f, false);
    w->savedClip = w->clip;
    w->clip = bar;
    w->menuBarCursorX = bar.min.x;
    w->cursor = Vec2(bar.min.x, Round(bar.min.y + (bar.Height() - g.FrameHeight()) * 0.5f));
    PushID("##menubar");
    return true;
}

bool BeginMenuBar()
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    if (!HasAnyFlags(w->flags, WindowFlags::MenuBar))
        return false;
    w->inMenuBar = true;
    w->menuBarSavedCursor = w->cursor;
    w->menuBarSavedLineHeight = w->currLineHeight;
    const f32 y = w->outer.min.y + w->titleHeight;
    const Rect bar(Vec2(w->outer.min.x, y), Vec2(w->outer.max.x, y + w->menuBarHeight));
    w->dl->PushClipRect(bar, 0.0f, false);
    w->savedClip = w->clip;
    w->clip = bar;
    w->cursor = Vec2(w->menuBarCursorX, Round(y + (w->menuBarHeight - g.FrameHeight()) * 0.5f));
    PushID("##menubar");
    return true;
}

void EndMenuBar()
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    PopID();
    w->dl->PopClipRect();
    w->clip = w->savedClip;
    w->cursor = w->menuBarSavedCursor;
    w->currLineHeight = w->menuBarSavedLineHeight;
    w->inMenuBar = false;
}

bool BeginMainMenuBar()
{
    Context::Impl& g = G();
    const f32 h = g.FrameHeight() + g.S(4.0f);
    SetNextWindowPos(Vec2::Zero(), Cond::Always);
    SetNextWindowSize(Vec2(g.display.x, h) / g.scale, Cond::Always);
    Begin("##MainMenuBar", nullptr,
          WindowFlags::NoTitleBar | WindowFlags::NoResize | WindowFlags::NoMove | WindowFlags::NoScrollbar | WindowFlags::MenuBar |
              WindowFlags::NoCollapse | WindowFlags::NoBringToFront);
    g.current->order = (1 << 20) - 1; // above regular windows, below popups
    g.current->hiddenFrames = 0;
    return BeginMenuBar();
}

void EndMainMenuBar()
{
    EndMenuBar();
    End();
}

bool BeginMenu(StringView label, bool enabled)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    const StringView shown = DisplayLabel(label);
    const Theme& c = g.style.colors;
    const String popupId = String::Format("##menu_{:08x}", id);
    const bool isOpen = IsPopupOpen(popupId);
    bool hovered = false, held = false;
    if (w->inMenuBar) {
        const f32 padX = Round(g.S(9.0f));
        const Vec2 ts = g.CalcTextSize(shown);
        const Rect r(w->cursor, w->cursor + Vec2(ts.x + padX * 2.0f, g.FrameHeight()));
        w->cursor.x = r.max.x + g.S(2.0f);
        w->menuBarCursorX = w->cursor.x;
        g.ItemAdd(r, id);
        if (!enabled)
            BeginDisabled();
        const bool pressed = g.ButtonBehavior(r, id, &hovered, &held, true);
        // Switching between menus of the same bar by hovering.
        const bool barHasOpenMenu = g.menuBarActiveId == w->id && !g.openPopups.Empty();
        if (enabled && ((pressed && !isOpen) || (hovered && barHasOpenMenu && !isOpen))) {
            g.lastItemRect = r;
            g.ClosePopupsFrom(0);
            OpenPopup(popupId);
            g.openPopups.Back().isMenu = true;
            g.menuBarActiveId = w->id;
        } else if (pressed && isOpen) {
            g.ClosePopupsFrom(0);
        }
        const bool openNow = IsPopupOpen(popupId);
        if (openNow || hovered)
            w->dl->AddRect(r, g.Col(openNow ? c.headerActive : c.headerHovered), CornerRadii(g.S(g.style.frameRounding)));
        g.RenderTextClipped(r, shown, g.TextColor(), TextAlign::Center);
        if (!enabled)
            EndDisabled();
    } else {
        // Submenu row inside a popup.
        const f32 h = Round(g.LineHeight() + g.S(8.0f));
        const f32 padX = Round(g.S(10.0f));
        const Vec2 ts = g.CalcTextSize(shown);
        const Rect r(w->cursor, Vec2(Max(w->workMaxX, w->cursor.x + ts.x + padX * 3.0f + g.S(16.0f)), w->cursor.y + h));
        g.ItemSize(Vec2(ts.x + padX * 2.0f + g.S(40.0f), h));
        g.ItemAdd(r, id);
        if (!enabled)
            BeginDisabled();
        g.ButtonBehavior(r, id, &hovered, &held, true);
        if (enabled && hovered && !isOpen) {
            g.lastItemRect = r;
            OpenPopup(popupId);
            g.openPopups.Back().isMenu = true;
        }
        const bool openNow = IsPopupOpen(popupId);
        if (hovered || openNow)
            w->dl->AddRect(r, g.Col(c.headerHovered), CornerRadii(g.S(4.0f)));
        g.RenderTextClipped(Rect(r.min + Vec2(padX + g.S(18.0f), 0.0f), r.max), shown, g.TextColor());
        RenderChevron(g, Rect(Vec2(r.max.x - g.S(22.0f), r.min.y), r.max), false, c.textDim);
        if (!enabled)
            EndDisabled();
    }
    if (!enabled)
        return false;
    return BeginPopup(popupId);
}

void EndMenu() { EndPopup(); }

bool MenuItem(StringView label, StringView shortcut, bool selected, bool enabled)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    const StringView shown = DisplayLabel(label);
    const f32 h = Round(g.LineHeight() + g.S(8.0f));
    const f32 padX = Round(g.S(10.0f));
    const f32 checkW = Round(g.S(18.0f));
    const Vec2 ts = g.CalcTextSize(shown);
    const f32 sw = shortcut.Empty() ? 0.0f : g.CalcTextSize(shortcut).x + g.S(28.0f);
    const f32 minW = padX * 2.0f + checkW + ts.x + sw;
    const Rect r(w->cursor, Vec2(Max(w->workMaxX, w->cursor.x + minW), w->cursor.y + h));
    g.ItemSize(Vec2(minW, h));
    if (!g.ItemAdd(r, id))
        return false;
    if (!enabled)
        BeginDisabled();
    bool hovered, held;
    const bool pressed = g.ButtonBehavior(r, id, &hovered, &held);
    // Hovering a plain item closes sibling submenus.
    if (hovered && !g.popupBeginStack.Empty() && g.popupBeginStack.Back() + 1 < g.openPopups.Size())
        g.ClosePopupsFrom(g.popupBeginStack.Back() + 1);
    const Theme& c = g.style.colors;
    if (hovered)
        w->dl->AddRect(r, g.Col(c.accent), CornerRadii(g.S(4.0f)));
    const Color textColor = hovered ? c.accentText : g.TextColor();
    if (selected)
        g.RenderCheckMark(Rect(Vec2(r.min.x + padX * 0.6f, r.min.y + (h - checkW) * 0.5f),
                               Vec2(r.min.x + padX * 0.6f + checkW, r.min.y + (h + checkW) * 0.5f)),
                          textColor);
    g.RenderTextClipped(Rect(Vec2(r.min.x + padX + checkW, r.min.y), r.max), shown, g.Col(textColor));
    if (!shortcut.Empty())
        g.RenderTextClipped(Rect(r.min, r.max - Vec2(padX, 0.0f)), shortcut, g.Col(hovered ? c.accentText : c.textDim), TextAlign::Right);
    if (!enabled)
        EndDisabled();
    if (pressed && enabled) {
        g.ClosePopupsFrom(0);
        return true;
    }
    return false;
}

bool MenuItem(StringView label, StringView shortcut, bool* selected, bool enabled)
{
    if (MenuItem(label, shortcut, selected && *selected, enabled)) {
        if (selected)
            *selected = !*selected;
        return true;
    }
    return false;
}

// --- Drag & drop ---------------------------------------------------------------------------------------------------------

bool BeginDragDropSource(StringView type, const void* data, usize size, StringView preview)
{
    Context::Impl& g = G();
    DragDropState& dd = g.dragDrop;
    if (dd.active)
        return dd.sourceId == g.lastItemId;
    if (g.lastItemId == 0 || g.activeId != g.lastItemId || !g.mouseDown[0])
        return false;
    if (Distance(g.mouse, g.mouseClickPos[0]) < g.S(5.0f))
        return false;
    dd.active = true;
    dd.delivered = dd.dropped = false;
    dd.sourceId = g.lastItemId;
    dd.type = type;
    dd.data.Clear();
    dd.data.Append(ByteSpan(static_cast<const u8*>(data), size));
    dd.preview = preview;
    g.ClearActive(); // let drop targets get hovered
    return true;
}

const void* AcceptDragDropPayload(StringView type, usize* size)
{
    Context::Impl& g = G();
    DragDropState& dd = g.dragDrop;
    if (!dd.active || dd.type != type || dd.sourceId == g.lastItemId)
        return nullptr;
    if (!g.current->contentHoverable || !g.IsHoveringRect(g.lastItemRect))
        return nullptr;
    g.current->dl->AddRectOutline(g.lastItemRect.Expanded(1.0f), g.style.colors.accent, Max(1.0f, Round(g.S(1.5f))),
                                  CornerRadii(g.S(3.0f)));
    if (!dd.dropped)
        return nullptr;
    dd.delivered = true;
    if (size)
        *size = dd.data.Size();
    return dd.data.Data();
}

bool IsDragDropActive(StringView type)
{
    const DragDropState& dd = G().dragDrop;
    return dd.active && (type.Empty() || dd.type == type);
}

// --- Misc ------------------------------------------------------------------------------------------------------------------

bool Splitter(StringView idStr, bool vertical, f32* size1, f32* size2, f32 min1, f32 min2, f32 thickness)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(idStr);
    const f32 t = g.S(thickness);
    Rect r;
    if (vertical)
        r = Rect(Vec2(w->cursor.x + g.S(*size1), w->cursor.y), Vec2(w->cursor.x + g.S(*size1) + t, w->clip.max.y));
    else
        r = Rect(Vec2(w->cursor.x, w->cursor.y + g.S(*size1)), Vec2(w->workMaxX, w->cursor.y + g.S(*size1) + t));
    g.ItemAdd(r, id);
    bool hovered, held;
    g.ButtonBehavior(r, id, &hovered, &held);
    if (hovered || held)
        g.SetCursorShape(vertical ? CursorShape::ResizeEW : CursorShape::ResizeNS);
    if (held) {
        f32 d = (vertical ? g.mouseDelta.x : g.mouseDelta.y) / g.scale;
        d = Clamp(d, min1 - *size1, *size2 - min2);
        *size1 += d;
        *size2 -= d;
    }
    if (hovered || held)
        w->dl->AddRect(vertical ? Rect(Vec2(r.Center().x - 1.0f, r.min.y), Vec2(r.Center().x + 1.0f, r.max.y))
                                : Rect(Vec2(r.min.x, r.Center().y - 1.0f), Vec2(r.max.x, r.Center().y + 1.0f)),
                       g.style.colors.accent);
    return held;
}

void Image(u32 textureSrv, Vec2 size, Vec2 uv0, Vec2 uv1, Color tint)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const Rect r(w->cursor, w->cursor + Round(g.S(size)));
    g.ItemSize(r.Size());
    if (g.ItemAdd(r, 0))
        w->dl->AddImage(textureSrv, r, uv0, uv1, tint);
}

void SvgIcon(const SvgImage& image, Vec2 size, Color color)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const Rect r(w->cursor, w->cursor + Round(g.S(size)));
    g.ItemSize(r.Size());
    if (g.ItemAdd(r, 0))
        w->dl->AddSvg(image, r, color.a == 0 ? g.TextColor() : color);
}

// --- Color -----------------------------------------------------------------------------------------------------------------

void ColorSwatch(Color color, Vec2 size)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const Rect r(w->cursor, w->cursor + Round(g.S(size)));
    g.ItemSize(r.Size());
    if (!g.ItemAdd(r, 0))
        return;
    g.RenderColorSwatch(r, color);
}

void Context::Impl::RenderColorSwatch(const Rect& r, Color color)
{
    DrawList& dl = *current->dl;
    const f32 rr = S(style.frameRounding);
    if (color.a < 255) {
        // Checkerboard behind transparent colors.
        dl.PushClipRect(r, rr);
        const f32 cell = Max(Round(S(5.0f)), 2.0f);
        dl.AddRect(r, Color(0xCC, 0xCC, 0xCC));
        for (f32 y = r.min.y; y < r.max.y; y += cell)
            for (f32 x = r.min.x + (i32((y - r.min.y) / cell) % 2) * cell; x < r.max.x; x += cell * 2.0f)
                dl.AddRect(Rect(Vec2(x, y), Min(Vec2(x + cell, y + cell), r.max)), Color(0x88, 0x88, 0x88));
        dl.PopClipRect();
        // Left half opaque, right half with alpha (shows both).
        dl.AddRect(Rect(r.min, Vec2(r.Center().x, r.max.y)), Color(color.r, color.g, color.b, 255), CornerRadii::Left(rr));
        dl.AddRect(Rect(Vec2(r.Center().x, r.min.y), r.max), color, CornerRadii::Right(rr));
        dl.AddRectOutline(r, style.colors.border, 1.0f, CornerRadii(rr));
    } else {
        dl.AddRect(r, color, CornerRadii(rr), 1.0f, style.colors.border);
    }
}

bool ColorPicker(StringView label, LinearColor* color, bool alpha)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    // HSV is kept between frames so hue survives zero saturation/value.
    f32 h, s, v;
    color->ToHSV(h, s, v);
    f32* sh = &g.floatStorage[id ^ 0x1];
    f32* ss = &g.floatStorage[id ^ 0x2];
    f32* sv = &g.floatStorage[id ^ 0x3];
    const LinearColor fromStored = LinearColor::FromHSV(*sh, *ss, *sv, color->a);
    if (Color::FromLinear(fromStored) == Color::FromLinear(*color)) {
        h = *sh;
        s = *ss;
        v = *sv;
    } else {
        if (s <= 0.0f || v <= 0.0f)
            h = *sh;
        *sh = h;
        *ss = s;
        *sv = v;
    }
    const f32 sq = Round(g.S(180.0f));
    const f32 bar = Round(g.S(16.0f));
    const f32 gap = Round(g.S(8.0f));
    const Rect svRect(w->cursor, w->cursor + Vec2(sq, sq));
    const Rect hueRect(Vec2(svRect.max.x + gap, svRect.min.y), Vec2(svRect.max.x + gap + bar, svRect.max.y));
    const Rect alphaRect(Vec2(hueRect.max.x + gap, svRect.min.y), Vec2(hueRect.max.x + gap + bar, svRect.max.y));
    const f32 totalW = (alpha ? alphaRect.max.x : hueRect.max.x) - svRect.min.x;
    g.ItemSize(Vec2(totalW, sq));
    g.ItemAdd(Rect(svRect.min, Vec2(svRect.min.x + totalW, svRect.max.y)), id);
    bool changed = false;
    bool hov, held;
    // SV square.
    g.ButtonBehavior(svRect, id + 1, &hov, &held);
    if (held) {
        s = Saturate((g.mouse.x - svRect.min.x) / sq);
        v = 1.0f - Saturate((g.mouse.y - svRect.min.y) / sq);
        changed = true;
    }
    g.ButtonBehavior(hueRect, id + 2, &hov, &held);
    if (held) {
        h = Saturate((g.mouse.y - hueRect.min.y) / sq) * 0.9999f;
        changed = true;
    }
    if (alpha) {
        g.ButtonBehavior(alphaRect, id + 3, &hov, &held);
        if (held) {
            color->a = 1.0f - Saturate((g.mouse.y - alphaRect.min.y) / sq);
            changed = true;
        }
    }
    if (changed) {
        const f32 a = color->a;
        *color = LinearColor::FromHSV(h, s, v, a);
        *sh = h;
        *ss = s;
        *sv = v;
        g.editedId = id;
    }
    DrawList& dl = *w->dl;
    const f32 rr = g.S(3.0f);
    const Color hueColor = Color::FromLinear(LinearColor::FromHSV(h, 1.0f, 1.0f));
    dl.AddRectGradient(svRect, Color(255, 255, 255), hueColor, GradientDirection::Horizontal, CornerRadii(rr));
    dl.AddRectGradient(svRect, Color(0, 0, 0, 0), Color(0, 0, 0, 255), GradientDirection::Vertical, CornerRadii(rr), 1.0f, g.style.colors.border);
    // Hue bar: 6 gradient segments.
    for (u32 i = 0; i < 6; ++i) {
        const f32 y0 = hueRect.min.y + sq * f32(i) / 6.0f, y1 = hueRect.min.y + sq * f32(i + 1) / 6.0f;
        dl.AddRectGradient(Rect(Vec2(hueRect.min.x, Floor(y0)), Vec2(hueRect.max.x, Ceil(y1))),
                           Color::FromLinear(LinearColor::FromHSV(f32(i) / 6.0f, 1, 1)),
                           Color::FromLinear(LinearColor::FromHSV(f32(i + 1) / 6.0f * 0.9999f, 1, 1)), GradientDirection::Vertical,
                           i == 0 ? CornerRadii::Top(rr) : (i == 5 ? CornerRadii::Bottom(rr) : CornerRadii()));
    }
    dl.AddRectOutline(hueRect, g.style.colors.border, 1.0f, CornerRadii(rr));
    const f32 hy = Round(hueRect.min.y + h * sq);
    dl.AddRect(Rect(Vec2(hueRect.min.x - g.S(2.0f), hy - g.S(2.0f)), Vec2(hueRect.max.x + g.S(2.0f), hy + g.S(2.0f))), Color(0, 0, 0, 0),
               CornerRadii(g.S(2.0f)), Max(1.0f, Round(g.S(1.5f))), Color(255, 255, 255));
    if (alpha) {
        const Color opaque = Color::FromLinear(LinearColor(color->r, color->g, color->b, 1.0f));
        g.RenderColorSwatch(alphaRect, Color(0, 0, 0, 0));
        dl.AddRectGradient(alphaRect, opaque, Color(opaque.r, opaque.g, opaque.b, 0), GradientDirection::Vertical, CornerRadii(rr), 1.0f,
                           g.style.colors.border);
        const f32 ay = Round(alphaRect.min.y + (1.0f - color->a) * sq);
        dl.AddRect(Rect(Vec2(alphaRect.min.x - g.S(2.0f), ay - g.S(2.0f)), Vec2(alphaRect.max.x + g.S(2.0f), ay + g.S(2.0f))),
                   Color(0, 0, 0, 0), CornerRadii(g.S(2.0f)), Max(1.0f, Round(g.S(1.5f))), Color(255, 255, 255));
    }
    const Vec2 cursorPos(svRect.min.x + s * sq, svRect.min.y + (1.0f - v) * sq);
    dl.AddCircle(cursorPos, g.S(5.0f), Color(0, 0, 0, 0), Max(1.0f, Round(g.S(1.5f))), Color(255, 255, 255));
    dl.AddCircleOutline(cursorPos, g.S(6.5f), Color(0, 0, 0, 120), 1.0f);

    // Hex + preview row.
    const Color srgb = Color::FromLinear(*color);
    String hex = alpha ? String::Format("{:02X}{:02X}{:02X}{:02X}", srgb.r, srgb.g, srgb.b, srgb.a)
                       : String::Format("{:02X}{:02X}{:02X}", srgb.r, srgb.g, srgb.b);
    SetNextItemWidth(110.0f);
    PushID(label);
    if (InputText("Hex", hex, InputTextFlags::EnterReturnsTrue)) {
        const LinearColor parsed = LinearColor::FromHex(hex);
        if (!(parsed == LinearColor(1, 0, 1, 1)) || hex.View().EqualsIgnoreCase("FF00FF") || hex.View().EqualsIgnoreCase("FF00FFFF")) {
            *color = alpha ? parsed : LinearColor(parsed.r, parsed.g, parsed.b, color->a);
            changed = true;
            g.editedId = id;
        }
    }
    PopID();
    SameLine();
    ColorSwatch(srgb, Vec2(40.0f, g.FrameHeight() / g.scale));
    return changed;
}

bool ColorEdit(StringView label, LinearColor* color, bool alpha)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    Rect frame;
    const Rect total = LayoutFrameWithLabel(g, g.CalcItemWidth(), label, frame);
    g.ItemSize(total.Size());
    if (!g.ItemAdd(total, id))
        return false;
    const String popupId = String::Format("##color_{:08x}", id);
    bool hovered, held;
    if (g.ButtonBehavior(frame, id, &hovered, &held, true)) {
        g.lastItemRect = frame;
        OpenPopup(popupId);
        g.openPopups.Back().isCombo = true;
    }
    const Color srgb = Color::FromLinear(*color);
    const f32 sw = Round(frame.Height() * 1.6f);
    g.RenderColorSwatch(Rect(frame.min, Vec2(frame.min.x + sw, frame.max.y)), alpha ? srgb : Color(srgb.r, srgb.g, srgb.b, 255));
    const Rect textR(Vec2(frame.min.x + sw + g.S(6.0f), frame.min.y), frame.max);
    w->dl->AddRect(Rect(Vec2(frame.min.x + sw + g.S(2.0f), frame.min.y), frame.max), g.Col(hovered ? g.style.colors.frameBgHovered : g.style.colors.frameBg),
                   CornerRadii(g.S(g.style.frameRounding)), 1.0f, g.Col(g.style.colors.border));
    g.RenderTextClipped(textR, alpha ? String::Format("#{:02X}{:02X}{:02X}  {}%", srgb.r, srgb.g, srgb.b, i32(color->a * 100.0f + 0.5f))
                                     : String::Format("#{:02X}{:02X}{:02X}", srgb.r, srgb.g, srgb.b),
                        g.TextColor());
    RenderLabelRight(g, frame, total, label);
    bool changed = false;
    if (BeginPopup(popupId)) {
        changed = ColorPicker("##picker", color, alpha);
        EndPopup();
    }
    if (changed)
        g.editedId = id;
    g.lastItemId = id;
    g.lastItemRect = total;
    return changed;
}

} // namespace zx::ui
