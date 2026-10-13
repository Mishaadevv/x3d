// Text fields: single/multi-line editing with selection, clipboard, undo/redo, word navigation.

#include "../Core/Internal.h"
#include "Core/String/Parse.h"
#include "Core/String/Utf.h"

namespace zx::ui {

namespace {

constexpr f64 UndoCoalesceSeconds = 1.0;
constexpr usize MaxUndo = 100;

enum class CharClass : u8
{
    Space,
    Word,
    Punct
};

CharClass Classify(StringView text, usize pos)
{
    const u8 c = u8(text[pos]);
    if (c == ' ' || c == '\t' || c == '\n')
        return CharClass::Space;
    if (c >= 0x80 || IsAlnumAscii(char(c)) || c == '_')
        return CharClass::Word;
    return CharClass::Punct;
}

usize PrevWord(StringView text, usize pos)
{
    pos = Utf8PrevBoundary(text, pos);
    while (pos > 0 && Classify(text, pos) == CharClass::Space)
        pos = Utf8PrevBoundary(text, pos);
    const CharClass cls = pos < text.Size() ? Classify(text, pos) : CharClass::Space;
    while (pos > 0) {
        const usize prev = Utf8PrevBoundary(text, pos);
        if (Classify(text, prev) != cls)
            break;
        pos = prev;
    }
    return pos;
}

usize NextWord(StringView text, usize pos)
{
    if (pos >= text.Size())
        return text.Size();
    const CharClass cls = Classify(text, pos);
    while (pos < text.Size() && Classify(text, pos) == cls)
        pos = Utf8NextBoundary(text, pos);
    while (pos < text.Size() && Classify(text, pos) == CharClass::Space && text[pos] != '\n')
        pos = Utf8NextBoundary(text, pos);
    return pos;
}

usize LineStart(StringView text, usize pos)
{
    while (pos > 0 && text[pos - 1] != '\n')
        --pos;
    return pos;
}

usize LineEnd(StringView text, usize pos)
{
    while (pos < text.Size() && text[pos] != '\n')
        ++pos;
    return pos;
}

u32 LineIndex(StringView text, usize pos)
{
    u32 line = 0;
    for (usize i = 0; i < pos && i < text.Size(); ++i)
        line += text[i] == '\n';
    return line;
}

usize LineStartByIndex(StringView text, u32 line)
{
    usize pos = 0;
    while (line > 0 && pos < text.Size()) {
        if (text[pos] == '\n')
            --line;
        ++pos;
    }
    return pos;
}

/// Password display: one bullet per codepoint; byte offsets map through codepoint indices.
String MaskText(StringView text)
{
    String s;
    for (u32 cp : Utf8Codepoints(text)) {
        ZX_UNUSED(cp);
        s += "\xE2\x80\xA2";
    }
    return s;
}

usize CodepointIndex(StringView text, usize byte)
{
    usize n = 0, pos = 0;
    while (pos < byte && pos < text.Size()) {
        pos = Utf8NextBoundary(text, pos);
        ++n;
    }
    return n;
}

usize ByteOfCodepoint(StringView text, usize index)
{
    usize pos = 0;
    while (index-- > 0 && pos < text.Size())
        pos = Utf8NextBoundary(text, pos);
    return pos;
}

bool AllowedChar(u32 cp, InputTextFlags flags)
{
    if (HasAnyFlags(flags, InputTextFlags::CharsDecimal))
        return (cp >= '0' && cp <= '9') || cp == '.' || cp == '-' || cp == '+' || cp == 'e' || cp == 'E';
    return true;
}

} // namespace

bool Context::Impl::TextEditBehavior(ID id, const Rect& frameRect, String& text, InputTextFlags flags, StringView hint, bool justFocused,
                                     const CodeEditorOptions* code)
{
    TextEditState& st = textEdit;
    const bool multiline = HasAnyFlags(flags, InputTextFlags::Multiline);
    const bool readOnly = HasAnyFlags(flags, InputTextFlags::ReadOnly);
    const bool password = HasAnyFlags(flags, InputTextFlags::Password) && !multiline;
    const Vec2 pad(Round(S(style.framePadding.x)), Round(S(style.framePadding.y)));
    const f32 lineH = LineHeight();
    const FontHandle font = Font();
    const f32 size = FontSize();
    // Code editors: line-number gutter left of the text, scroll kept while inactive.
    const bool isCode = code != nullptr && multiline;
    const f32 gutter = isCode && code->lineNumbers ? Round(fonts.MeasureWidth(font, size, "00000") + S(10.0f)) : 0.0f;
    Vec2* keptScroll = isCode ? &codeScroll[id] : nullptr;

    const bool hovered = ItemHoverable(frameRect, id);
    if (hovered || activeId == id)
        SetCursorShape(CursorShape::IBeam);
    bool active = activeId == id;
    bool changed = false, enterPressed = false;

    // Byte offset under the mouse (in `text` coordinates).
    auto hitTest = [&](Vec2 m) -> usize {
        const StringView view = text.View();
        if (multiline) {
            const f32 localY = m.y - (frameRect.min.y + pad.y) + st.scrollY;
            const u32 lines = LineIndex(view, view.Size()) + 1;
            const u32 line = u32(Clamp(i32(Floor(localY / lineH)), 0, i32(lines) - 1));
            const usize start = LineStartByIndex(view, line);
            const usize end = LineEnd(view, start);
            return start + fonts.HitTest(font, size, view.SubStr(start, end - start), m.x - (frameRect.min.x + pad.x + gutter) + st.scrollX);
        }
        const f32 localX = m.x - (frameRect.min.x + pad.x) + st.scrollX;
        if (password) {
            const String masked = MaskText(view);
            const usize maskedByte = fonts.HitTest(font, size, masked, localX);
            return ByteOfCodepoint(view, CodepointIndex(masked, maskedByte));
        }
        return fonts.HitTest(font, size, view, localX);
    };

    if ((hovered && mousePressed[0]) || justFocused) {
        if (!active) {
            SetActive(id, current);
            active = true;
            st.id = id;
            st.initial = text;
            st.scrollX = keptScroll ? keptScroll->x : 0.0f;
            st.scrollY = keptScroll ? keptScroll->y : 0.0f;
            st.undo.Clear();
            st.redo.Clear();
            st.cursor = st.anchor = text.Size();
            st.selectAll = HasAnyFlags(flags, InputTextFlags::AutoSelectAll) || justFocused;
            if (!justFocused && !st.selectAll)
                st.cursor = st.anchor = hitTest(mouse);
            if (st.selectAll) {
                st.anchor = 0;
                st.cursor = text.Size();
            }
        } else if (hovered) {
            st.cursor = hitTest(mouse);
            if (!HasAnyFlags(mods, KeyModifiers::Shift))
                st.anchor = st.cursor;
        }
        if (hovered && mouseDoubleClicked[0]) {
            st.anchor = PrevWord(text.View(), Utf8NextBoundary(text.View(), st.cursor));
            st.cursor = NextWord(text.View(), st.anchor);
        }
        st.dragging = hovered && !st.selectAll;
        st.blink = 0.0f;
    } else if (active && mousePressed[0] && !hovered) {
        ClearActive(); // click outside: commit
        active = false;
    }
    if (active && st.id != id) {
        // Activated through another path (e.g. stale state): reinitialize.
        st.id = id;
        st.initial = text;
        st.cursor = st.anchor = text.Size();
    }

    if (active) {
        activeIdAlive = true;
        st.blink += dt;
        if (st.cursor > text.Size())
            st.cursor = text.Size();
        if (st.anchor > text.Size())
            st.anchor = text.Size();
        if (st.dragging) {
            if (mouseDown[0])
                st.cursor = hitTest(mouse);
            else
                st.dragging = false;
        }

        auto pushUndo = [&](bool typing) {
            if (typing && !st.undo.Empty() && time - st.lastEditTime < UndoCoalesceSeconds) {
                st.lastEditTime = time;
                return;
            }
            st.undo.PushBack({text, st.cursor, st.anchor});
            if (st.undo.Size() > MaxUndo)
                st.undo.RemoveAt(0);
            st.redo.Clear();
            st.lastEditTime = typing ? time : 0.0;
        };
        auto deleteSelection = [&]() {
            const usize a = st.SelMin(), b = st.SelMax();
            text.Erase(a, b - a);
            st.cursor = st.anchor = a;
        };
        auto insert = [&](StringView s) {
            if (st.HasSelection())
                deleteSelection();
            text.Insert(st.cursor, s);
            st.cursor += s.Size();
            st.anchor = st.cursor;
        };

        const bool ctrl = HasAnyFlags(mods, KeyModifiers::Control);
        const bool shift = HasAnyFlags(mods, KeyModifiers::Shift);
        for (Key key : keysPressed) {
            const StringView view = text.View();
            usize newCursor = st.cursor;
            bool moveCaret = false;
            switch (key) {
            case Key::Left:
                if (st.HasSelection() && !shift)
                    newCursor = st.SelMin();
                else
                    newCursor = ctrl ? PrevWord(view, st.cursor) : Utf8PrevBoundary(view, st.cursor);
                moveCaret = true;
                break;
            case Key::Right:
                if (st.HasSelection() && !shift)
                    newCursor = st.SelMax();
                else
                    newCursor = ctrl ? NextWord(view, st.cursor) : Utf8NextBoundary(view, st.cursor);
                moveCaret = true;
                break;
            case Key::Home:
                newCursor = ctrl || !multiline ? 0 : LineStart(view, st.cursor);
                moveCaret = true;
                break;
            case Key::End:
                newCursor = ctrl || !multiline ? view.Size() : LineEnd(view, st.cursor);
                moveCaret = true;
                break;
            case Key::Up:
            case Key::Down:
                if (multiline) {
                    const usize ls = LineStart(view, st.cursor);
                    const f32 x = fonts.MeasureWidth(font, size, view.SubStr(ls, st.cursor - ls));
                    const u32 line = LineIndex(view, st.cursor);
                    const u32 lines = LineIndex(view, view.Size()) + 1;
                    if ((key == Key::Up && line > 0) || (key == Key::Down && line + 1 < lines)) {
                        const usize ts = LineStartByIndex(view, key == Key::Up ? line - 1 : line + 1);
                        const usize te = LineEnd(view, ts);
                        newCursor = ts + fonts.HitTest(font, size, view.SubStr(ts, te - ts), x);
                    } else {
                        newCursor = key == Key::Up ? 0 : view.Size();
                    }
                    moveCaret = true;
                }
                break;
            case Key::PageUp:
            case Key::PageDown:
                if (multiline && isCode) {
                    // One page of lines, keeping the column.
                    const u32 page = Max(1u, u32(frameRect.Height() / lineH) - 1u);
                    const u32 line = LineIndex(view, st.cursor);
                    const u32 lines = LineIndex(view, view.Size()) + 1;
                    const u32 target = key == Key::PageUp ? (line > page ? line - page : 0u) : Min(line + page, lines - 1);
                    const usize ls = LineStart(view, st.cursor);
                    const f32 x = fonts.MeasureWidth(font, size, view.SubStr(ls, st.cursor - ls));
                    const usize ts = LineStartByIndex(view, target);
                    newCursor = ts + fonts.HitTest(font, size, view.SubStr(ts, LineEnd(view, ts) - ts), x);
                    moveCaret = true;
                } else if (multiline) {
                    newCursor = key == Key::PageUp ? 0 : view.Size();
                    moveCaret = true;
                }
                break;
            case Key::Backspace:
                if (readOnly)
                    break;
                if (st.HasSelection()) {
                    pushUndo(false);
                    deleteSelection();
                } else if (st.cursor > 0) {
                    pushUndo(false);
                    const usize from = ctrl ? PrevWord(view, st.cursor) : Utf8PrevBoundary(view, st.cursor);
                    text.Erase(from, st.cursor - from);
                    st.cursor = st.anchor = from;
                }
                changed = true;
                break;
            case Key::Delete:
                if (readOnly)
                    break;
                if (st.HasSelection()) {
                    pushUndo(false);
                    deleteSelection();
                } else if (st.cursor < view.Size()) {
                    pushUndo(false);
                    const usize to = ctrl ? NextWord(view, st.cursor) : Utf8NextBoundary(view, st.cursor);
                    text.Erase(st.cursor, to - st.cursor);
                }
                changed = true;
                break;
            case Key::Enter:
            case Key::KeypadEnter:
                if (multiline && !ctrl) {
                    if (!readOnly) {
                        pushUndo(false);
                        String newline("\n");
                        if (isCode) {
                            // Auto-indent: keep the current line's indentation, one level more after '{'.
                            const usize ls = LineStart(view, st.SelMin());
                            for (usize i = ls; i < st.SelMin() && (view[i] == ' ' || view[i] == '\t'); ++i)
                                newline += view[i];
                            usize j = st.SelMin();
                            while (j > ls && view[j - 1] == ' ')
                                --j;
                            if (j > ls && view[j - 1] == '{')
                                newline += "    ";
                        }
                        insert(newline);
                        changed = true;
                    }
                } else {
                    enterPressed = true;
                    if (!HasAnyFlags(flags, InputTextFlags::KeepFocusOnEnter)) {
                        ClearActive();
                        active = false;
                    }
                }
                break;
            case Key::Escape:
                if (!readOnly && text != st.initial) {
                    text = st.initial;
                    changed = true;
                }
                ClearActive();
                active = false;
                break;
            case Key::Tab:
                if (multiline && !readOnly) {
                    pushUndo(false);
                    if (isCode && (shift || (st.HasSelection() && LineIndex(view, st.SelMin()) != LineIndex(view, st.SelMax())))) {
                        // Indent / unindent every selected line by one level.
                        const usize a = LineStart(view, st.SelMin());
                        const usize b = LineEnd(view, st.SelMax() > st.SelMin() && view[st.SelMax() - 1] == '\n' ? st.SelMax() - 1 : st.SelMax());
                        String out;
                        usize pos = a;
                        for (;;) {
                            const usize le = LineEnd(view, pos);
                            const StringView ln = view.SubStr(pos, le - pos);
                            if (shift) {
                                usize n = 0;
                                while (n < 4 && n < ln.Size() && ln[n] == ' ')
                                    ++n;
                                out += ln.SubStr(n);
                            } else {
                                if (!ln.Empty())
                                    out += "    ";
                                out += ln;
                            }
                            if (le >= b)
                                break;
                            out += '\n';
                            pos = le + 1;
                        }
                        String rebuilt(view.Left(a));
                        rebuilt += out;
                        rebuilt += view.SubStr(b);
                        text = move(rebuilt);
                        st.anchor = a;
                        st.cursor = a + out.Size();
                    } else {
                        insert("    ");
                    }
                    changed = true;
                }
                break;
            case Key::A:
                if (ctrl) {
                    st.anchor = 0;
                    st.cursor = text.Size();
                }
                break;
            case Key::C:
            case Key::X:
                if (ctrl && st.HasSelection() && !password) {
                    platform::SetClipboardText(view.SubStr(st.SelMin(), st.SelMax() - st.SelMin()));
                    if (key == Key::X && !readOnly) {
                        pushUndo(false);
                        deleteSelection();
                        changed = true;
                    }
                }
                break;
            case Key::V:
                if (ctrl && !readOnly) {
                    String clip = platform::GetClipboardText();
                    if (!multiline) {
                        clip.ReplaceAll("\r\n", " ");
                        clip.ReplaceAll("\n", " ");
                    } else {
                        clip.ReplaceAll("\r\n", "\n");
                    }
                    if (!clip.Empty()) {
                        pushUndo(false);
                        insert(clip);
                        changed = true;
                    }
                }
                break;
            case Key::Z:
            case Key::Y:
                if (ctrl && !readOnly) {
                    const bool redo = key == Key::Y || shift;
                    Array<TextEditState::Snapshot>& from = redo ? st.redo : st.undo;
                    Array<TextEditState::Snapshot>& to = redo ? st.undo : st.redo;
                    if (!from.Empty()) {
                        to.PushBack({text, st.cursor, st.anchor});
                        TextEditState::Snapshot s = from.PopBackValue();
                        text = move(s.text);
                        st.cursor = Min(s.cursor, text.Size());
                        st.anchor = Min(s.anchor, text.Size());
                        st.lastEditTime = 0.0;
                        changed = true;
                    }
                }
                break;
            default:
                break;
            }
            if (!active)
                break;
            if (moveCaret) {
                st.cursor = newCursor;
                if (!shift)
                    st.anchor = st.cursor;
                st.blink = 0.0f;
            }
        }
        if (active && !readOnly && !typed.Empty() && !HasAnyFlags(mods, KeyModifiers::Control)) {
            String filtered;
            for (u32 cp : Utf8Codepoints(typed.View()))
                if (AllowedChar(cp, flags))
                    Utf8Append(filtered, cp);
            if (!filtered.Empty()) {
                pushUndo(true);
                if (isCode && filtered == "}" && !st.HasSelection()) {
                    // A closing brace on an indentation-only line moves back one level.
                    const usize ls = LineStart(text.View(), st.cursor);
                    bool blank = st.cursor - ls >= 4;
                    for (usize i = ls; blank && i < st.cursor; ++i)
                        blank = text[i] == ' ';
                    if (blank) {
                        text.Erase(st.cursor - 4, 4);
                        st.cursor = st.anchor = st.cursor - 4;
                    }
                }
                insert(filtered);
                changed = true;
                st.blink = 0.0f;
            }
        }
    }

    // --- Render ---
    const Theme& col = style.colors;
    DrawList& dl = *current->dl;
    const f32 rounding = S(style.frameRounding);
    dl.AddRect(frameRect, Col(active ? col.frameBgActive : (hovered ? col.frameBgHovered : col.frameBg)), CornerRadii(rounding), 1.0f,
               Col(active ? col.accent : col.border));
    const Rect inner(frameRect.min + Vec2(pad.x + gutter, 1.0f), frameRect.max - Vec2(pad.x, 1.0f));
    if (gutter > 0.0f)
        dl.AddRect(Rect(frameRect.min + Vec2(1.0f, 1.0f), Vec2(frameRect.min.x + gutter, frameRect.max.y - 1.0f)),
                   Col(WithAlpha(col.windowBg, 0.6f)), CornerRadii::Left(rounding));
    dl.PushClipRect(inner);
    const StringView view = text.View();
    String masked;
    StringView shown = view;
    if (password) {
        masked = MaskText(view);
        shown = masked;
    }
    auto displayOffset = [&](usize byte) -> usize { return password ? ByteOfCodepoint(shown, CodepointIndex(view, byte)) : byte; };

    if (multiline) {
        if (isCode && code->scrollToLine > 0) {
            const f32 y = f32(code->scrollToLine - 1) * lineH;
            f32& sy = active ? st.scrollY : keptScroll->y;
            if (y < sy || y + lineH > sy + inner.Height() - pad.y)
                sy = Max(0.0f, y - inner.Height() * 0.35f);
        }
        // Keep the caret visible.
        if (active && !(isCode && code->scrollToLine > 0)) {
            const u32 line = LineIndex(view, st.cursor);
            const f32 caretY = f32(line) * lineH;
            const f32 viewH = inner.Height() - pad.y;
            if (caretY - st.scrollY < 0.0f)
                st.scrollY = caretY;
            else if (caretY + lineH - st.scrollY > viewH)
                st.scrollY = caretY + lineH - viewH;
            const usize ls = LineStart(view, st.cursor);
            const f32 caretX = fonts.MeasureWidth(font, size, view.SubStr(ls, st.cursor - ls));
            if (caretX - st.scrollX < 0.0f)
                st.scrollX = caretX;
            else if (caretX - st.scrollX > inner.Width() - S(2.0f))
                st.scrollX = caretX - inner.Width() + S(2.0f);
        }
        if (hovered && wheel != 0.0f && !wheelConsumed) {
            const u32 lines = LineIndex(view, view.Size()) + 1;
            f32& sy = active || !keptScroll ? st.scrollY : keptScroll->y;
            sy = Clamp(sy - wheel * lineH * 3.0f, 0.0f, Max(f32(lines) * lineH - inner.Height() + pad.y, 0.0f));
            wheelConsumed = true;
        }
        if (active && keptScroll)
            *keptScroll = Vec2(st.scrollX, st.scrollY);
        const Vec2 kept = keptScroll ? *keptScroll : Vec2(0.0f, 0.0f);
        const f32 scrollX = active ? st.scrollX : kept.x, scrollY = active ? st.scrollY : kept.y;
        const Vec2 origin(inner.min.x - scrollX, frameRect.min.y + pad.y - scrollY);
        usize start = 0;
        u32 line = 0;
        const usize selA = active ? st.SelMin() : 0, selB = active ? st.SelMax() : 0;
        const u32 caretLine = active ? LineIndex(view, st.cursor) : U32Max;
        Array<Color> colors;
        u32 colorState = 0;
        u32 firstVisible = U32Max, lastVisible = 0;
        for (;;) {
            const usize end = LineEnd(view, start);
            const f32 y = origin.y + f32(line) * lineH;
            const StringView ln = view.SubStr(start, end - start);
            if (isCode && code->colorizer) {
                colors.Resize(ln.Size());
                code->colorizer(ln, colors.Data(), colorState); // every line: block comments carry over
            }
            if (y + lineH >= inner.min.y && y <= inner.max.y) {
                firstVisible = Min(firstVisible, line);
                lastVisible = line;
                if (isCode && code->errorLine == line + 1)
                    dl.AddRect(Rect(Vec2(inner.min.x, y), Vec2(inner.max.x, y + lineH)), Col(Color(0xE5, 0x48, 0x4D, 0x38)));
                else if (isCode && line == caretLine && selA == selB)
                    dl.AddRect(Rect(Vec2(inner.min.x, y), Vec2(inner.max.x, y + lineH)), Col(WithAlpha(col.text, 0.05f)));
                if (selB > selA && selA <= end && selB >= start) {
                    const usize a = Max(selA, start), b = Min(selB, end);
                    const f32 x0 = fonts.MeasureWidth(font, size, view.SubStr(start, a - start));
                    f32 x1 = fonts.MeasureWidth(font, size, view.SubStr(start, b - start));
                    if (selB > end)
                        x1 += S(4.0f); // newline is selected
                    dl.AddRect(Rect(Vec2(origin.x + x0, y), Vec2(origin.x + x1, y + lineH)), col.textSelection);
                }
                if (isCode && code->colorizer && !ln.Empty()) {
                    // Runs of equal color.
                    f32 x = 0.0f;
                    usize runStart = 0;
                    for (usize i = 1; i <= ln.Size(); ++i) {
                        if (i < ln.Size() && colors[i] == colors[runStart])
                            continue;
                        const StringView run = ln.SubStr(runStart, i - runStart);
                        dl.AddText(font, size, Vec2(origin.x + x, y), Col(colors[runStart]), run);
                        x += fonts.MeasureWidth(font, size, run);
                        runStart = i;
                    }
                } else {
                    dl.AddText(font, size, Vec2(origin.x, y), Col(TextColor()), ln);
                }
            }
            if (end >= view.Size())
                break;
            start = end + 1;
            ++line;
        }
        if (gutter > 0.0f && firstVisible != U32Max) {
            dl.PopClipRect();
            dl.PushClipRect(Rect(frameRect.min + Vec2(1.0f, 1.0f), Vec2(frameRect.min.x + gutter, frameRect.max.y - 1.0f)));
            for (u32 l = firstVisible; l <= lastVisible; ++l) {
                char buf[16];
                const usize n = FormatToBuffer(buf, sizeof(buf), "{}", l + 1);
                const StringView num(buf, n);
                const f32 w = fonts.MeasureWidth(font, size, num);
                const bool isError = code->errorLine == l + 1;
                const Color c = isError ? Color(0xF0, 0x60, 0x60) : (l == caretLine ? col.textDim : col.textDisabled);
                dl.AddText(font, size, Vec2(frameRect.min.x + gutter - S(6.0f) - w, origin.y + f32(l) * lineH), Col(c), num);
            }
            dl.PopClipRect();
            dl.PushClipRect(inner);
        }
        if (view.Empty() && !active && !hint.Empty())
            dl.AddText(font, size, origin, Col(col.textDisabled), hint);
        if (active && Fmod(st.blink, 1.0f) < 0.6f) {
            const usize ls = LineStart(view, st.cursor);
            const f32 cx = origin.x + fonts.MeasureWidth(font, size, view.SubStr(ls, st.cursor - ls));
            const f32 cy = origin.y + f32(LineIndex(view, st.cursor)) * lineH;
            dl.AddRect(Rect(Vec2(Round(cx), cy + S(1.0f)), Vec2(Round(cx) + Max(1.0f, Round(S(1.0f))), cy + lineH - S(1.0f))), col.text);
        }
    } else {
        const f32 textY = Round(frameRect.min.y + (frameRect.Height() - lineH) * 0.5f);
        if (active) {
            const f32 caretX = fonts.MeasureWidth(font, size, shown.Left(displayOffset(st.cursor)));
            const f32 visible = inner.Width() - S(2.0f);
            if (caretX - st.scrollX < 0.0f)
                st.scrollX = caretX;
            else if (caretX - st.scrollX > visible)
                st.scrollX = caretX - visible;
            const f32 total = fonts.MeasureWidth(font, size, shown);
            st.scrollX = Clamp(st.scrollX, 0.0f, Max(total - visible, 0.0f));
        }
        const f32 scrollX = active ? st.scrollX : 0.0f;
        const f32 x = inner.min.x - scrollX;
        if (active && st.HasSelection()) {
            const f32 x0 = fonts.MeasureWidth(font, size, shown.Left(displayOffset(st.SelMin())));
            const f32 x1 = fonts.MeasureWidth(font, size, shown.Left(displayOffset(st.SelMax())));
            dl.AddRect(Rect(Vec2(x + x0, textY), Vec2(x + x1, textY + lineH)), col.textSelection);
        }
        if (shown.Empty() && !hint.Empty())
            dl.AddText(font, size, Vec2(x, textY), Col(col.textDisabled), hint);
        else
            dl.AddText(font, size, Vec2(x, textY), Col(TextColor()), shown);
        if (active && Fmod(st.blink, 1.0f) < 0.6f) {
            const f32 cx = Round(x + fonts.MeasureWidth(font, size, shown.Left(displayOffset(st.cursor))));
            dl.AddRect(Rect(Vec2(cx, textY + S(1.0f)), Vec2(cx + Max(1.0f, Round(S(1.0f))), textY + lineH - S(1.0f))), col.text);
        }
    }
    dl.PopClipRect();

    if (changed)
        editedId = id;
    if (HasAnyFlags(flags, InputTextFlags::EnterReturnsTrue))
        return enterPressed;
    return changed;
}

void SetKeyboardFocusHere() { G().focusNextTextField = true; }

void SetActiveTextCursorToEnd()
{
    Context::Impl& g = G();
    g.textEdit.cursor = g.textEdit.anchor = ~usize(0); // clamped to the text size by the widget
    g.textEdit.scrollX = 0.0f;
}

bool InputText(StringView label, String& text, InputTextFlags flags, StringView hint)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    const f32 width = g.CalcItemWidth();
    const f32 fh = g.FrameHeight();
    const Rect frame(w->cursor, w->cursor + Vec2(width, fh));
    const StringView shownLabel = DisplayLabel(label);
    const f32 labelW = shownLabel.Empty() ? 0.0f : g.S(g.style.itemInnerSpacing) + g.CalcTextSize(shownLabel).x;
    const Rect total(frame.min, frame.max + Vec2(labelW, 0.0f));
    g.ItemSize(total.Size());
    if (!g.ItemAdd(frame, id) && g.activeId != id)
        return false;
    const bool focus = g.focusNextTextField && g.activeId != id;
    g.focusNextTextField = false;
    const bool result = g.TextEditBehavior(id, frame, text, flags & ~InputTextFlags::Multiline, hint, focus);
    if (!shownLabel.Empty())
        g.RenderTextClipped(Rect(Vec2(frame.max.x + g.S(g.style.itemInnerSpacing), frame.min.y), total.max), shownLabel, g.TextColor());
    g.lastItemId = id;
    g.lastItemRect = frame;
    return result;
}

bool InputTextMultiline(StringView label, String& text, Vec2 size, InputTextFlags flags)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    const Vec2 avail(w->workMaxX - w->cursor.x, Max(w->clip.max.y - w->cursor.y, g.FrameHeight() * 3.0f));
    const Vec2 sz(size.x > 0.0f ? g.S(size.x) : avail.x, size.y > 0.0f ? g.S(size.y) : g.LineHeight() * 6.0f + g.S(g.style.framePadding.y) * 2.0f);
    const Rect frame(w->cursor, w->cursor + Round(sz));
    g.ItemSize(frame.Size());
    if (!g.ItemAdd(frame, id) && g.activeId != id)
        return false;
    const bool result = g.TextEditBehavior(id, frame, text, flags | InputTextFlags::Multiline, {}, false);
    g.lastItemId = id;
    g.lastItemRect = frame;
    return result;
}

bool InputTextCode(StringView label, String& text, Vec2 size, const CodeEditorOptions& options)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    const Vec2 avail(w->workMaxX - w->cursor.x, Max(w->clip.max.y - w->cursor.y, g.FrameHeight() * 3.0f));
    const Vec2 sz(size.x > 0.0f ? g.S(size.x) : avail.x, size.y > 0.0f ? g.S(size.y) : avail.y);
    const Rect frame(w->cursor, w->cursor + Round(sz));
    g.ItemSize(frame.Size());
    if (!g.ItemAdd(frame, id) && g.activeId != id)
        return false;
    const bool result = g.TextEditBehavior(id, frame, text, InputTextFlags::Multiline, {}, false, &options);
    g.lastItemId = id;
    g.lastItemRect = frame;
    return result;
}

/// Shared by InputFloat/InputInt and the typing mode of Drag/Slider widgets: edits `st.temp`, applies on commit.
bool NumericTextEdit(ID id, const Rect& frame, f64& value, bool isInt, StringView format, bool justFocused)
{
    Context::Impl& g = G();
    TextEditState& st = g.textEdit;
    if (justFocused || st.id != id || !st.numeric) {
        st.temp = isInt ? String::Format("{}", i64(value)) : String::Format(format, value);
        st.numeric = true;
    }
    String& buffer = st.temp;
    const bool wasActive = g.activeId == id;
    g.TextEditBehavior(id, frame, buffer, InputTextFlags::CharsDecimal | InputTextFlags::AutoSelectAll, {}, justFocused);
    const bool nowActive = g.activeId == id;
    if (wasActive && !nowActive) {
        st.numeric = false;
        // Escape restores `initial`, which then parses back to the original value.
        if (Optional<f64> v = ParseDouble(buffer.View()); v.HasValue()) {
            const f64 nv = isInt ? f64(i64(Round(f32(*v)))) : *v;
            if (nv != value) {
                value = nv;
                g.editedId = id;
                return true;
            }
        }
    }
    return false;
}

bool InputFloat(StringView label, f32* value, StringView format)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    const f32 width = g.CalcItemWidth();
    const Rect frame(w->cursor, w->cursor + Vec2(width, g.FrameHeight()));
    const StringView shownLabel = DisplayLabel(label);
    const f32 labelW = shownLabel.Empty() ? 0.0f : g.S(g.style.itemInnerSpacing) + g.CalcTextSize(shownLabel).x;
    g.ItemSize(frame.Size() + Vec2(labelW, 0.0f));
    if (!g.ItemAdd(frame, id) && g.activeId != id)
        return false;
    bool changed = false;
    if (g.activeId == id || (g.ItemHoverable(frame, id) && g.mousePressed[0])) {
        f64 v = *value;
        changed = NumericTextEdit(id, frame, v, false, format, g.activeId != id);
        *value = f32(v);
    } else {
        g.RenderFrame(frame, g.style.colors.frameBg);
        g.RenderTextClipped(Rect(frame.min + Vec2(g.S(g.style.framePadding.x), 0.0f), frame.max), String::Format(format, *value),
                            g.TextColor());
    }
    if (!shownLabel.Empty())
        g.RenderTextClipped(Rect(Vec2(frame.max.x + g.S(g.style.itemInnerSpacing), frame.min.y), Vec2(frame.max.x + labelW, frame.max.y)),
                            shownLabel, g.TextColor());
    g.lastItemId = id;
    g.lastItemRect = frame;
    return changed;
}

bool InputInt(StringView label, i32* value)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const ID id = g.MakeID(label);
    const f32 width = g.CalcItemWidth();
    const f32 fh = g.FrameHeight();
    const Rect frame(w->cursor, w->cursor + Vec2(width, fh));
    const StringView shownLabel = DisplayLabel(label);
    const f32 labelW = shownLabel.Empty() ? 0.0f : g.S(g.style.itemInnerSpacing) + g.CalcTextSize(shownLabel).x;
    g.ItemSize(frame.Size() + Vec2(labelW, 0.0f));
    if (!g.ItemAdd(frame, id) && g.activeId != id)
        return false;
    bool changed = false;
    // -/+ step buttons on the right.
    const Rect textRect(frame.min, Vec2(frame.max.x - fh * 2.0f, frame.max.y));
    const Rect minus(Vec2(textRect.max.x, frame.min.y), Vec2(textRect.max.x + fh, frame.max.y));
    const Rect plus(Vec2(minus.max.x, frame.min.y), frame.max);
    if (g.activeId == id || (g.ItemHoverable(textRect, id) && g.mousePressed[0])) {
        f64 v = *value;
        changed = NumericTextEdit(id, textRect, v, true, "{}", g.activeId != id);
        *value = i32(v);
    } else {
        g.RenderFrame(textRect, g.style.colors.frameBg, true, g.S(g.style.frameRounding));
        g.RenderTextClipped(Rect(textRect.min + Vec2(g.S(g.style.framePadding.x), 0.0f), textRect.max), String::Format("{}", *value),
                            g.TextColor());
    }
    for (u32 i = 0; i < 2; ++i) {
        const Rect r = i == 0 ? minus : plus;
        const ID bid = id + 1 + i;
        bool hovered, held;
        if (g.ButtonBehavior(r, bid, &hovered, &held)) {
            *value += i == 0 ? -1 : 1;
            changed = true;
            g.editedId = id;
        }
        const Theme& c = g.style.colors;
        g.current->dl->AddRect(r, g.Col(held ? c.buttonActive : (hovered ? c.buttonHovered : c.button)),
                               i == 1 ? CornerRadii::Right(g.S(g.style.frameRounding)) : CornerRadii(), 1.0f, g.Col(c.border));
        g.RenderTextClipped(r, i == 0 ? ZX_ICON_MINUS : ZX_ICON_PLUS, g.TextColor(), TextAlign::Center);
    }
    if (!shownLabel.Empty())
        g.RenderTextClipped(Rect(Vec2(frame.max.x + g.S(g.style.itemInnerSpacing), frame.min.y), Vec2(frame.max.x + labelW, frame.max.y)),
                            shownLabel, g.TextColor());
    g.lastItemId = id;
    g.lastItemRect = frame;
    return changed;
}

} // namespace zx::ui
