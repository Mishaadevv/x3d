#include "UI/DrawList.h"
#include "UI/Svg.h"
#include "Core/String/Utf.h"

#include <bit>

namespace zx::ui {

Color LerpColor(Color a, Color b, f32 t)
{
    auto l = [t](u8 x, u8 y) { return u8(Clamp(f32(x) + (f32(y) - f32(x)) * t, 0.0f, 255.0f) + 0.5f); };
    return Color(l(a.r, b.r), l(a.g, b.g), l(a.b, b.b), l(a.a, b.a));
}

void DrawList::Reset(FontSystem* fonts, Rect viewport)
{
    m_fonts = fonts;
    m_prims.Clear();
    m_clips.Clear();
    m_clipStack.Clear();
    m_clips.PushBack(GpuClip{Vec4(viewport.min.x, viewport.min.y, viewport.max.x, viewport.max.y), 0.0f, {}});
    m_clipStack.PushBack(ClipState{viewport, 0});
}

void DrawList::PushClipRect(Rect rect, f32 radius, bool intersect)
{
    if (intersect)
        rect = rect.Intersect(m_clipStack.Back().rect);
    rect.max = Max(rect.max, rect.min);
    // Reuse the current entry for identical rectangular clips (common for nested layout scopes).
    const ClipState& cur = m_clipStack.Back();
    if (radius <= 0.0f && rect == cur.rect && m_clips[cur.index].radius <= 0.0f) {
        m_clipStack.PushBack(cur);
        return;
    }
    const u32 index = u32(m_clips.Size());
    ZX_ASSERT(index < 0xFFFF, "Too many clip rects in one draw list");
    m_clips.PushBack(GpuClip{Vec4(rect.min.x, rect.min.y, rect.max.x, rect.max.y), radius, {}});
    m_clipStack.PushBack(ClipState{rect, index});
}

void DrawList::PopClipRect()
{
    ZX_ASSERT(m_clipStack.Size() > 1, "PopClipRect without PushClipRect");
    m_clipStack.PopBack();
}

GpuPrimitive& DrawList::Emit(PrimitiveType type, const Rect& bounds)
{
    const ClipState& clip = m_clipStack.Back();
    if (!bounds.Intersects(clip.rect))
        return m_discard;
    GpuPrimitive& p = m_prims.AddDefaulted();
    p = GpuPrimitive{};
    p.header = u32(type) | (clip.index << 16);
    return p;
}

void DrawList::AddRect(const Rect& rect, Color fill, CornerRadii r, f32 borderWidth, Color border)
{
    if (rect.IsEmpty() || (fill.a == 0 && (border.a == 0 || borderWidth <= 0.0f)))
        return;
    GpuPrimitive& p = Emit(PrimitiveType::Rect, rect.Expanded(1.0f));
    p.rect = Vec4(rect.min.x, rect.min.y, rect.max.x, rect.max.y);
    const f32 maxR = Min(rect.Width(), rect.Height()) * 0.5f;
    p.data = Vec4(Min(r.topLeft, maxR), Min(r.topRight, maxR), Min(r.bottomRight, maxR), Min(r.bottomLeft, maxR));
    p.color0 = p.color1 = PackColor(fill);
    p.borderColor = PackColor(border);
    p.borderWidth = borderWidth;
    // Border-only rects: don't rasterize the empty interior.
    if (fill.a == 0 && borderWidth > 0.0f) {
        const f32 inset = Max(Max(Max(p.data.x, p.data.y), Max(p.data.z, p.data.w)), borderWidth) + 1.0f;
        if (rect.Width() > inset * 4.0f && rect.Height() > inset * 4.0f)
            p.extra = inset;
    }
}

void DrawList::AddRectGradient(const Rect& rect, Color from, Color to, GradientDirection direction, CornerRadii r,
                               f32 borderWidth, Color border)
{
    if (rect.IsEmpty())
        return;
    GpuPrimitive& p = Emit(PrimitiveType::Rect, rect.Expanded(1.0f));
    p.rect = Vec4(rect.min.x, rect.min.y, rect.max.x, rect.max.y);
    const f32 maxR = Min(rect.Width(), rect.Height()) * 0.5f;
    p.data = Vec4(Min(r.topLeft, maxR), Min(r.topRight, maxR), Min(r.bottomRight, maxR), Min(r.bottomLeft, maxR));
    p.color0 = PackColor(from);
    p.color1 = PackColor(to);
    p.header |= u32(direction) << 4;
    p.borderColor = PackColor(border);
    p.borderWidth = borderWidth;
}

void DrawList::InsertRectAt(u32 index, const Rect& rect, Color fill, CornerRadii radii)
{
    const u32 before = PrimitiveCount();
    AddRect(rect, fill, radii);
    if (PrimitiveCount() == before || index >= before)
        return;
    const GpuPrimitive p = m_prims.Back();
    m_prims.PopBack();
    m_prims.Insert(index, p);
}

void DrawList::AddRectOutline(const Rect& rect, Color color, f32 thickness, CornerRadii radii)
{
    AddRect(rect, Color(0, 0, 0, 0), radii, thickness, color);
}

void DrawList::AddShadow(const Rect& rect, Color color, f32 radius, f32 blur, Vec2 offset, bool hollow)
{
    if (rect.IsEmpty() || color.a == 0)
        return;
    const Rect r = rect.Translated(offset);
    GpuPrimitive& p = Emit(PrimitiveType::Shadow, r.Expanded(blur + 1.0f));
    p.rect = Vec4(r.min.x, r.min.y, r.max.x, r.max.y);
    const f32 rr = Min(radius, Min(r.Width(), r.Height()) * 0.5f);
    p.data = Vec4(rr, rr, rr, rr);
    p.color0 = p.color1 = PackColor(color);
    p.blur = Max(blur, 0.5f);
    if (hollow) {
        // The part of the shadow covered by the shape itself (shadow rect inset by the corner radius and the offset).
        const f32 inset = rr + Max(Abs(offset.x), Abs(offset.y)) + 1.0f;
        if (r.Width() > inset * 3.0f && r.Height() > inset * 3.0f)
            p.extra = inset;
    }
}

void DrawList::AddCircle(Vec2 center, f32 radius, Color fill, f32 borderWidth, Color border)
{
    AddRect(Rect(center - Vec2(radius), center + Vec2(radius)), fill, CornerRadii(radius), borderWidth, border);
}

void DrawList::AddCircleOutline(Vec2 center, f32 radius, Color color, f32 thickness)
{
    AddRect(Rect(center - Vec2(radius), center + Vec2(radius)), Color(0, 0, 0, 0), CornerRadii(radius), thickness, color);
}

void DrawList::AddLine(Vec2 p0, Vec2 p1, Color color, f32 thickness)
{
    if (color.a == 0)
        return;
    const f32 h = thickness * 0.5f + 1.0f;
    GpuPrimitive& p = Emit(PrimitiveType::Line, Rect(Min(p0, p1) - Vec2(h), Max(p0, p1) + Vec2(h)));
    p.rect = Vec4(p0.x, p0.y, p1.x, p1.y);
    p.data = Vec4(thickness, 0, 0, 0);
    p.color0 = p.color1 = PackColor(color);
}

void DrawList::AddPolyline(Span<const Vec2> points, Color color, f32 thickness, bool closed)
{
    if (points.Size() < 2)
        return;
    for (usize i = 0; i + 1 < points.Size(); ++i)
        AddLine(points[i], points[i + 1], color, thickness);
    if (closed && points.Size() > 2)
        AddLine(points[points.Size() - 1], points[0], color, thickness);
}

void DrawList::AddBezier(Vec2 p0, Vec2 c0, Vec2 c1, Vec2 p1, Color color, f32 thickness, u32 segments)
{
    if (segments == 0) {
        const f32 len = Length(c0 - p0) + Length(c1 - c0) + Length(p1 - c1);
        segments = Clamp(u32(len / 8.0f), 4u, 64u);
    }
    Vec2 prev = p0;
    for (u32 i = 1; i <= segments; ++i) {
        const f32 t = f32(i) / f32(segments), u = 1.0f - t;
        const Vec2 p = p0 * (u * u * u) + c0 * (3.0f * u * u * t) + c1 * (3.0f * u * t * t) + p1 * (t * t * t);
        AddLine(prev, p, color, thickness);
        prev = p;
    }
}

void DrawList::AddTriangle(Vec2 a, Vec2 b, Vec2 c, Color color, u32 aaEdges)
{
    if (color.a == 0)
        return;
    // The shader expects clockwise winding in screen space (y down); fix the edge mask when swapping.
    if (Cross(b - a, c - a) < 0.0f) {
        swap(b, c);
        // Edges (a,b),(b,c),(c,a) become (a,c),(c,b),(b,a): edge0 <- old edge2, edge1 <- old edge1, edge2 <- old edge0.
        aaEdges = ((aaEdges & 1) << 2) | (aaEdges & 2) | ((aaEdges >> 2) & 1);
    }
    const Rect bounds(Min(a, Min(b, c)) - Vec2(1.0f), Max(a, Max(b, c)) + Vec2(1.0f));
    GpuPrimitive& p = Emit(PrimitiveType::Triangle, bounds);
    p.rect = Vec4(a.x, a.y, b.x, b.y);
    p.data = Vec4(c.x, c.y, std::bit_cast<f32>(aaEdges), 0.0f);
    p.color0 = p.color1 = PackColor(color);
}

void DrawList::AddConvexPolygon(Span<const Vec2> points, Color color)
{
    const usize n = points.Size();
    if (n < 3)
        return;
    for (usize i = 1; i + 1 < n; ++i) {
        u32 mask = 2; // edge (p_i, p_i+1) is always on the outline
        if (i == 1)
            mask |= 1; // (p0, p1)
        if (i + 2 == n)
            mask |= 4; // (p_n-1, p0)
        AddTriangle(points[0], points[i], points[i + 1], color, mask);
    }
}

void DrawList::AddImage(u32 textureSrv, const Rect& rect, Vec2 uv0, Vec2 uv1, Color tint, f32 radius)
{
    if (rect.IsEmpty() || tint.a == 0)
        return;
    GpuPrimitive& p = Emit(PrimitiveType::Image, rect);
    p.rect = Vec4(rect.min.x, rect.min.y, rect.max.x, rect.max.y);
    p.data = Vec4(uv0.x, uv0.y, uv1.x, uv1.y);
    p.color0 = p.color1 = PackColor(tint);
    p.texture = textureSrv;
    p.extra = Min(radius, Min(rect.Width(), rect.Height()) * 0.5f);
}

void DrawList::AddSvg(const SvgImage& image, const Rect& rect, Color color)
{
    const u32 px = u32(Clamp(Round(Min(rect.Width(), rect.Height())), 1.0f, 1024.0f));
    const u64 key = (image.id & 0x0000FFFFFFFFF000ull) ^ u64(px);
    const GlyphEntry* e = m_fonts->GetCustomGlyph(key, [&](GlyphBitmap& out) { return RasterizeSvg(image, px, out); });
    if (!e)
        return;
    const Vec2 pos = Round(rect.Center() - Vec2(f32(e->width), f32(e->height)) * 0.5f);
    const Rect r(pos, pos + Vec2(f32(e->width), f32(e->height)));
    GpuPrimitive& p = Emit(PrimitiveType::Glyph, r);
    p.rect = Vec4(r.min.x, r.min.y, r.max.x, r.max.y);
    p.data = Vec4(f32(e->x), f32(e->y), f32(e->x + e->width), f32(e->y + e->height));
    p.color0 = p.color1 = PackColor(color);
    p.header |= u32(e->page) << 8;
}

void DrawList::DrawLine(FontHandle font, f32 size, f32 x, f32 baseline, Color color, StringView line)
{
    m_shaped.Clear();
    m_fonts->ShapeText(font, size, line, m_shaped);
    const Rect clip = m_clipStack.Back().rect;
    const u32 packed = PackColor(color);
    for (const ShapedGlyph& g : m_shaped) {
        const f32 penX = x + g.x;
        if (penX > clip.max.x)
            break;
        if (penX + g.advance + g.size < clip.min.x)
            continue;
        const GlyphEntry* e = m_fonts->GetGlyph(g.face, g.glyph, g.size, penX);
        if (!e)
            continue;
        const f32 gx = Floor(penX) + f32(e->left);
        const f32 gy = baseline + Round(g.y) + f32(e->top);
        const Rect r(Vec2(gx, gy), Vec2(gx + f32(e->width), gy + f32(e->height)));
        GpuPrimitive& p = Emit(PrimitiveType::Glyph, r);
        p.rect = Vec4(r.min.x, r.min.y, r.max.x, r.max.y);
        p.data = Vec4(f32(e->x), f32(e->y), f32(e->x + e->width), f32(e->y + e->height));
        p.color0 = p.color1 = packed;
        p.header |= u32(e->page) << 8;
    }
}

Vec2 DrawList::AddText(FontHandle font, f32 size, Vec2 pos, Color color, StringView text, f32 wrapWidth)
{
    if (!font.IsValid() || text.Empty())
        return Vec2::Zero();
    const FontSystem::LineMetrics lm = m_fonts->GetLineMetrics(font, size);
    const Rect clip = m_clipStack.Back().rect;
    f32 y = pos.y;
    f32 width = 0.0f;
    StringView rest = text;
    const bool visibleColor = color.a != 0;
    for (;;) {
        usize len;
        if (wrapWidth > 0.0f) {
            len = m_fonts->WrapLine(font, size, rest, wrapWidth);
        } else {
            len = rest.Find('\n');
            if (len == StringView::NPos)
                len = rest.Size();
        }
        StringView line = rest.Left(len);
        while (!line.Empty() && line.Back() == ' ' && wrapWidth > 0.0f)
            line = line.DropRight(1);
        // Lines outside the clip are skipped without shaping (long logs / documents).
        if (visibleColor && y + lm.lineHeight >= clip.min.y && y <= clip.max.y)
            DrawLine(font, size, pos.x, Round(y + lm.ascent), color, line);
        width = Max(width, m_fonts->MeasureWidth(font, size, line));
        y += lm.lineHeight;
        rest = rest.DropLeft(len);
        while (!rest.Empty() && rest[0] == ' ' && wrapWidth > 0.0f)
            rest = rest.DropLeft(1);
        if (rest.Empty())
            break;
        if (rest[0] == '\n') {
            rest = rest.DropLeft(1);
            if (rest.Empty()) {
                y += lm.lineHeight;
                break;
            }
        }
    }
    return Vec2(width, y - pos.y);
}

void DrawList::AddTextInRect(FontHandle font, f32 size, const Rect& rect, Color color, StringView text, TextAlign align,
                             bool ellipsis)
{
    if (!font.IsValid() || text.Empty() || !IsVisible(rect))
        return;
    const FontSystem::LineMetrics lm = m_fonts->GetLineMetrics(font, size);
    f32 width = m_fonts->MeasureWidth(font, size, text);
    StringView shown = text;
    bool truncated = false;
    if (ellipsis && width > rect.Width() + 0.5f) {
        const f32 dotsWidth = m_fonts->MeasureWidth(font, size, "\xE2\x80\xA6");
        const f32 avail = rect.Width() - dotsWidth;
        usize end = 0;
        while (end < text.Size()) {
            const usize next = Utf8NextBoundary(text, end);
            if (m_fonts->MeasureWidth(font, size, text.Left(next)) > avail)
                break;
            end = next;
        }
        shown = text.Left(end);
        truncated = true;
        width = m_fonts->MeasureWidth(font, size, shown) + dotsWidth;
    }
    f32 x = rect.min.x;
    if (align == TextAlign::Center)
        x = rect.min.x + (rect.Width() - width) * 0.5f;
    else if (align == TextAlign::Right)
        x = rect.max.x - width;
    // Center the cap height (visually centered for both Latin and Cyrillic).
    const f32 baseline = Round(rect.min.y + (rect.Height() + lm.capHeight) * 0.5f);
    PushClipRect(rect);
    DrawLine(font, size, x, baseline, color, shown);
    if (truncated)
        DrawLine(font, size, x + m_fonts->MeasureWidth(font, size, shown), baseline, color, "\xE2\x80\xA6");
    PopClipRect();
}

} // namespace zx::ui
