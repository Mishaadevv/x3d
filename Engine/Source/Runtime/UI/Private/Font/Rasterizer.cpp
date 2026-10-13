#include "UI/Rasterizer.h"

namespace zx::ui {

PathRasterizer::PathRasterizer(u32 width, u32 height) : m_width(width), m_height(height), m_stride(width + 2)
{
    m_accum.Resize(usize(m_stride) * height, 0.0f);
}

void PathRasterizer::MoveTo(Vec2 p)
{
    m_start = m_current = p;
}

void PathRasterizer::LineTo(Vec2 p)
{
    Line(m_current, p);
    m_current = p;
}

void PathRasterizer::QuadTo(Vec2 c, Vec2 p)
{
    // Subdivide so the flattening error stays below ~1/8 pixel.
    const Vec2 dd = m_current - c * 2.0f + p;
    const f32 devSq = LengthSq(dd);
    const u32 n = devSq < 1e-6f ? 1 : Clamp(u32(1 + Floor(Sqrt(Sqrt(devSq) * 2.0f))), 1u, 64u);
    const Vec2 p0 = m_current;
    for (u32 i = 1; i <= n; ++i) {
        const f32 t = f32(i) / f32(n), u = 1.0f - t;
        LineTo(p0 * (u * u) + c * (2.0f * u * t) + p * (t * t));
    }
}

void PathRasterizer::CubicTo(Vec2 c0, Vec2 c1, Vec2 p)
{
    const Vec2 p0 = m_current;
    const f32 dev = Max(Length(p0 - c0 * 2.0f + c1), Length(c0 - c1 * 2.0f + p));
    const u32 n = Clamp(u32(1 + Floor(Sqrt(dev * 3.0f))), 1u, 96u);
    for (u32 i = 1; i <= n; ++i) {
        const f32 t = f32(i) / f32(n), u = 1.0f - t;
        LineTo(p0 * (u * u * u) + c0 * (3.0f * u * u * t) + c1 * (3.0f * u * t * t) + p * (t * t * t));
    }
}

void PathRasterizer::Close()
{
    if (m_current != m_start)
        LineTo(m_start);
}

// Signed area accumulation (as in font-rs): every edge adds, per scanline it crosses, the covered area to the cells
// it passes through and the remainder to the next cell; a running sum along each row yields the coverage.
void PathRasterizer::Line(Vec2 p0, Vec2 p1)
{
    if (Abs(p0.y - p1.y) <= 1e-7f)
        return;
    f32 dir = 1.0f;
    if (p0.y > p1.y) {
        swap(p0, p1);
        dir = -1.0f;
    }
    // Clip horizontally to the bitmap (contributions left of 0 land in column 0, right of width in the slack column).
    const f32 maxX = f32(m_width) + 0.999f;
    const f32 dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    f32 x = p0.x;
    if (p0.y < 0.0f)
        x -= p0.y * dxdy;
    const i32 yStart = Max(0, i32(Floor(p0.y)));
    const i32 yEnd = Min(i32(m_height), i32(Ceil(p1.y)));
    for (i32 y = yStart; y < yEnd; ++y) {
        f32* row = m_accum.Data() + usize(y) * m_stride;
        const f32 dy = Min(f32(y + 1), p1.y) - Max(f32(y), p0.y);
        const f32 xNext = x + dxdy * dy;
        const f32 d = dy * dir;
        f32 x0 = Clamp(Min(x, xNext), 0.0f, maxX);
        f32 x1 = Clamp(Max(x, xNext), 0.0f, maxX);
        const f32 x0Floor = Floor(x0);
        const i32 x0i = i32(x0Floor);
        const f32 x1Ceil = Ceil(x1);
        const i32 x1i = i32(x1Ceil);
        if (x1i <= x0i + 1) {
            const f32 xmf = 0.5f * (x0 + x1) - x0Floor;
            row[x0i] += d - d * xmf;
            row[x0i + 1] += d * xmf;
        } else {
            const f32 s = 1.0f / (x1 - x0);
            const f32 x0f = x0 - x0Floor;
            const f32 a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
            const f32 x1f = x1 - x1Ceil + 1.0f;
            const f32 am = 0.5f * s * x1f * x1f;
            row[x0i] += d * a0;
            if (x1i == x0i + 2) {
                row[x0i + 1] += d * (1.0f - a0 - am);
            } else {
                const f32 a1 = s * (1.5f - x0f);
                row[x0i + 1] += d * (a1 - a0);
                for (i32 xi = x0i + 2; xi < x1i - 1; ++xi)
                    row[xi] += d * s;
                const f32 a2 = a1 + f32(x1i - x0i - 3) * s;
                row[x1i - 1] += d * (1.0f - a2 - am);
            }
            row[x1i] += d * am;
        }
        x = xNext;
    }
}

void PathRasterizer::Resolve(u8* out, u32 stride) const
{
    for (u32 y = 0; y < m_height; ++y) {
        const f32* row = m_accum.Data() + usize(y) * m_stride;
        u8* dst = out + usize(y) * stride;
        f32 acc = 0.0f;
        for (u32 x = 0; x < m_width; ++x) {
            acc += row[x];
            const f32 c = Min(Abs(acc), 1.0f);
            dst[x] = u8(c * 255.0f + 0.5f);
        }
    }
}

bool RasterizeGlyph(const GlyphOutline& outline, f32 scale, Vec2 offset, GlyphBitmap& out, u32 padding)
{
    out = GlyphBitmap();
    if (outline.Empty())
        return false;
    const f32 pad = f32(padding);
    const i32 x0 = i32(Floor(outline.boundsMin.x * scale + offset.x - pad));
    const i32 x1 = i32(Ceil(outline.boundsMax.x * scale + offset.x + pad));
    const i32 y0 = i32(Floor(-outline.boundsMax.y * scale + offset.y - pad));
    const i32 y1 = i32(Ceil(-outline.boundsMin.y * scale + offset.y + pad));
    if (x1 <= x0 || y1 <= y0 || x1 - x0 > 2048 || y1 - y0 > 2048)
        return false;
    out.width = u32(x1 - x0);
    out.height = u32(y1 - y0);
    out.left = x0;
    out.top = y0;
    PathRasterizer r(out.width, out.height);
    const Vec2 origin(offset.x - f32(x0), offset.y - f32(y0));
    auto toPixel = [&](Vec2 p) { return Vec2(p.x * scale + origin.x, -p.y * scale + origin.y); };
    for (const GlyphOutline::Segment& s : outline.segments) {
        r.MoveTo(toPixel(s.p0));
        if (s.isLine)
            r.LineTo(toPixel(s.p1));
        else
            r.QuadTo(toPixel(s.control), toPixel(s.p1));
    }
    out.pixels.Resize(usize(out.width) * out.height);
    r.Resolve(out.pixels.Data(), out.width);
    return true;
}

} // namespace zx::ui
