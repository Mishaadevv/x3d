#pragma once

// Anti-aliased path rasterizer with exact (analytic) area coverage, non-zero-ish winding (|sum| clamped to 1).
// Used for glyphs and vector icons.
//
//   PathRasterizer r(width, height);
//   r.MoveTo(p0); r.LineTo(p1); r.QuadTo(c, p2); r.Close();   // pixel coordinates, y down
//   r.Resolve(pixels, width);                                   // 8-bit coverage

#include "Core/Containers/Array.h"
#include "Core/Math/Vector.h"
#include "UI/FontFace.h"

namespace zx::ui {

class PathRasterizer
{
public:
    PathRasterizer(u32 width, u32 height);

    void MoveTo(Vec2 p);
    void LineTo(Vec2 p);
    void QuadTo(Vec2 control, Vec2 p);
    void CubicTo(Vec2 c0, Vec2 c1, Vec2 p);
    void Close();
    /// Adds one edge directly (no current point bookkeeping).
    void Line(Vec2 p0, Vec2 p1);

    /// Writes coverage (0..255) into `out` (rows of `stride` bytes).
    void Resolve(u8* out, u32 stride) const;

    u32 Width() const { return m_width; }
    u32 Height() const { return m_height; }

private:
    u32 m_width;
    u32 m_height;
    u32 m_stride; // width + 2: room for contributions right of the last pixel
    Array<f32> m_accum;
    Vec2 m_start;
    Vec2 m_current;
};

struct GlyphBitmap
{
    u32 width = 0;
    u32 height = 0;
    /// Offset of the bitmap's top-left pixel from the pen position on the baseline (pixels, y down).
    i32 left = 0;
    i32 top = 0;
    Array<u8> pixels; // width * height coverage
};

/// Rasterizes an outline (font units) at `scale` (pixels per unit), shifted by a sub-pixel `offset` in [0,1).
/// Returns false (empty bitmap) for empty outlines.
bool RasterizeGlyph(const GlyphOutline& outline, f32 scale, Vec2 offset, GlyphBitmap& out, u32 padding = 1);

} // namespace zx::ui
