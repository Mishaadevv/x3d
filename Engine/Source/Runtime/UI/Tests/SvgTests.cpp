#include "Core/Testing/Test.h"
#include "UI/Svg.h"

using namespace zx;
using namespace zx::ui;

namespace {

u8 At(const GlyphBitmap& b, u32 x, u32 y) { return b.pixels[usize(y) * b.width + x]; }

// Lucide-style icon: no fill, 2-unit round strokes, 24 x 24 view box.
constexpr StringView RingIcon = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none"
  stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/></svg>)svg";

} // namespace

ZX_TEST(Svg, StrokedCircleIsARing)
{
    Result<SvgImage> img = ParseSvg(RingIcon);
    ZX_REQUIRE(img.IsOk());
    ZX_EXPECT_EQ(img->shapes.Size(), usize(1));
    ZX_EXPECT(!img->shapes[0].fill && img->shapes[0].strokeWidth == 2.0f);
    GlyphBitmap b;
    ZX_REQUIRE(RasterizeSvg(*img, 48, b));
    ZX_EXPECT_EQ(b.width, 48u);
    ZX_EXPECT_EQ(At(b, 24, 24), u8(0));        // hollow center
    ZX_EXPECT(At(b, 24, 4) > 200);              // on the ring (r = 10 units = 20 px)
    ZX_EXPECT(At(b, 44, 24) > 200);
    ZX_EXPECT_EQ(At(b, 0, 0), u8(0));
}

ZX_TEST(Svg, PathCommandsArcsAndFill)
{
    // Full circle drawn with two arcs (compact flag syntax), filled, plus a relative-path square hole (evenodd-free:
    // reversed winding).
    const StringView svg = R"svg(<svg viewBox="0 0 24 24"><path d="M2 12a10 10 0 1 0 20 0a10 10 0 1 0-20 0z M8 8h8v8H8z"/></svg>)svg";
    Result<SvgImage> img = ParseSvg(svg);
    ZX_REQUIRE(img.IsOk());
    ZX_EXPECT(img->shapes.Size() == 1 && img->shapes[0].fill);
    ZX_EXPECT_EQ(img->shapes[0].contours.Size(), usize(2));
    GlyphBitmap b;
    ZX_REQUIRE(RasterizeSvg(*img, 24, b));
    ZX_EXPECT(At(b, 4, 12) > 200);   // inside the disc
    ZX_EXPECT(At(b, 12, 12) < 40);   // inside the square: opposite winding cancels
    ZX_EXPECT(At(b, 0, 0) == 0);

    // Rounded rect, polyline, line, group style inheritance, transform.
    const StringView shapes = R"svg(<svg viewBox="0 0 24 24" fill="none" stroke="#000" stroke-width="2">
        <g transform="translate(1, 0)"><rect x="3" y="3" width="16" height="16" rx="3"/></g>
        <polyline points="4 20 8 16 12 20"/><line x1="2" y1="2" x2="22" y2="2"/><!-- comment -->
        <polygon points="12,6 15,12 9,12" fill="black" stroke="none"/></svg>)svg";
    Result<SvgImage> img2 = ParseSvg(shapes);
    ZX_REQUIRE(img2.IsOk());
    ZX_EXPECT_EQ(img2->shapes.Size(), usize(4));
    ZX_EXPECT(img2->shapes[3].fill && img2->shapes[3].strokeWidth == 0.0f);
    ZX_EXPECT(img2->shapes[0].contours[0][0].x > 4.0f + 0.9f); // translated by 1
    ZX_REQUIRE(RasterizeSvg(*img2, 24, b));
    ZX_EXPECT(At(b, 12, 2) > 150); // the line
    ZX_EXPECT(At(b, 12, 10) > 150); // the filled triangle

    ZX_EXPECT(!ParseSvg("<html></html>").IsOk());
}
