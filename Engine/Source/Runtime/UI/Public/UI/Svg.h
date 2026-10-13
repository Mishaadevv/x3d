#pragma once

// Monochrome SVG icons for ZUI: a parser for the subset icon sets use (<svg viewBox>, <g>, <path> with every command
// incl. arcs, <circle>, <ellipse>, <rect rx>, <line>, <polyline>, <polygon>; fill / stroke / stroke-width inherited
// from parents; colors only decide whether something is painted - icons are tinted when drawn). Rasterized at the
// requested pixel size into the font atlas and cached like glyphs.
//
//   Result<SvgImage> icon = ParseSvg(text);                // or LoadSvg("/Engine/Icons/Logo.svg")
//   drawList.AddSvg(*icon, rect, color);                  // ui::SvgIcon(*icon, Vec2(16)) as a widget

#include "Core/Math/Math.h"
#include "Core/Result.h"
#include "UI/Rasterizer.h"

namespace zx::ui {

struct SvgShape
{
    Array<Array<Vec2>> contours; // flattened, in viewBox units
    Array<bool> closed;          // per contour
    bool fill = false;
    f32 strokeWidth = 0.0f; // 0 = no stroke
    bool roundJoins = true;
};

struct SvgImage
{
    Vec2 origin;               // viewBox min
    Vec2 size = Vec2(24.0f);   // viewBox size
    Array<SvgShape> shapes;
    u64 id = 0;                // content hash (atlas cache key)
};

Result<SvgImage> ParseSvg(StringView text);
Result<SvgImage> LoadSvg(StringView path);

/// Coverage bitmap of the image fitted into `pixelSize` x `pixelSize` (aspect kept, centered).
bool RasterizeSvg(const SvgImage& image, u32 pixelSize, GlyphBitmap& out);

} // namespace zx::ui
