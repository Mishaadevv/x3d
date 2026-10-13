// Font parsing, rasterization, font system and draw list tests (fonts from Engine/Content/Fonts, WOFF from Tests/Data).

#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Testing/Test.h"
#include "UI/DrawList.h"
#include "UI/Rasterizer.h"

using namespace zx;
using namespace zx::ui;

namespace {

// Tests run without the VFS: physical paths.
String InterLatinPath() { return FindEngineRootDirectory() + "/Engine/Content/Fonts/Inter/inter-latin-400-normal.ttf"; }
String InterCyrillicPath() { return FindEngineRootDirectory() + "/Engine/Content/Fonts/Inter/inter-cyrillic-400-normal.ttf"; }
String LucidePath() { return FindEngineRootDirectory() + "/Engine/Content/Fonts/Lucide/lucide.ttf"; }

f32 CoverageAt(const Array<u8>& pixels, u32 width, u32 x, u32 y) { return f32(pixels[y * width + x]) / 255.0f; }

} // namespace

ZX_TEST(Font, ParseInterMetrics)
{
    UniquePtr<FontFace> face = FontFace::LoadFile(InterLatinPath());
    ZX_REQUIRE(face);
    const FontMetrics& m = face->GetMetrics();
    ZX_EXPECT_EQ(m.unitsPerEm, 2048);
    ZX_EXPECT_EQ(m.ascender, 1984);
    ZX_EXPECT_EQ(m.descender, -494);
    ZX_EXPECT_GT(m.capHeight, 1000);
    ZX_EXPECT_GT(face->GlyphCount(), 100u);
    const u16 a = face->GlyphIndex('A');
    ZX_EXPECT_NE(a, 0);
    ZX_EXPECT_GT(face->GlyphAdvance(a), 1000);
    ZX_EXPECT_EQ(face->GlyphIndex(0x416), 0); // 'Ж' lives in the Cyrillic subset
    // Space has an advance but no outline.
    GlyphOutline outline;
    const u16 space = face->GlyphIndex(' ');
    ZX_EXPECT_NE(space, 0);
    ZX_EXPECT(!face->GetGlyphOutline(space, outline));
    ZX_EXPECT_GT(face->GlyphAdvance(space), 0);
}

ZX_TEST(Font, CyrillicAndOutlines)
{
    UniquePtr<FontFace> face = FontFace::LoadFile(InterCyrillicPath());
    ZX_REQUIRE(face);
    const u16 zhe = face->GlyphIndex(0x416);
    ZX_REQUIRE(zhe != 0);
    GlyphOutline outline;
    ZX_REQUIRE(face->GetGlyphOutline(zhe, outline));
    ZX_EXPECT_GT(outline.segments.Size(), 8u);
    // Outline is closed: each contour ends where it started -> segment endpoints chain.
    ZX_EXPECT_GT(outline.boundsMax.x, outline.boundsMin.x);
    ZX_EXPECT_GT(outline.boundsMax.y, 1000.0f); // cap height ~1490 units
    ZX_EXPECT_NEAR(outline.boundsMin.y, 0.0f, 40.0f);
    // Composite glyph: 'Й' = 'И' + breve.
    const u16 shortI = face->GlyphIndex(0x419);
    ZX_REQUIRE(shortI != 0);
    ZX_REQUIRE(face->GetGlyphOutline(shortI, outline));
    ZX_EXPECT_GT(outline.boundsMax.y, 1600.0f); // the breve sits above the cap height
}

ZX_TEST(Font, Kerning)
{
    UniquePtr<FontFace> face = FontFace::LoadFile(InterLatinPath());
    ZX_REQUIRE(face);
    const u16 a = face->GlyphIndex('A'), v = face->GlyphIndex('V'), t = face->GlyphIndex('T'), o = face->GlyphIndex('o');
    ZX_EXPECT_LT(face->Kerning(a, v), 0);
    ZX_EXPECT_LT(face->Kerning(t, o), 0);
    ZX_EXPECT_EQ(face->Kerning(face->GlyphIndex('H'), face->GlyphIndex('H')), 0);
    ZX_EXPECT_EQ(face->Kerning(a, v), face->Kerning(a, v)); // cached path
}

ZX_TEST(Font, WoffMatchesTtf)
{
    Array<u8> bytes;
    ZX_REQUIRE(fs::ReadFile(FindEngineRootDirectory() + "/Tests/Data/Fonts/inter-latin-400-normal.woff", bytes));
    UniquePtr<FontFace> woff = FontFace::Load(move(bytes), "woff");
    UniquePtr<FontFace> ttf = FontFace::LoadFile(InterLatinPath());
    ZX_REQUIRE(woff && ttf);
    ZX_EXPECT_EQ(woff->GetMetrics().unitsPerEm, ttf->GetMetrics().unitsPerEm);
    for (u32 cp = 'A'; cp <= 'z'; ++cp) {
        const u16 g = woff->GlyphIndex(cp);
        ZX_EXPECT_NE(g, 0);
        ZX_EXPECT_EQ(woff->GlyphAdvance(g), ttf->GlyphAdvance(ttf->GlyphIndex(cp)));
    }
}

ZX_TEST(Font, RejectsGarbage)
{
    Array<u8> junk;
    junk.Resize(64, 0xAB);
    ZX_EXPECT(!FontFace::Load(move(junk), "junk"));
    Array<u8> tiny;
    tiny.Resize(4, 0);
    ZX_EXPECT(!FontFace::Load(move(tiny), "tiny"));
    // Truncated real font: tables out of bounds must be rejected without crashing.
    Array<u8> bytes;
    ZX_REQUIRE(fs::ReadFile(InterLatinPath(), bytes));
    bytes.Resize(bytes.Size() / 3);
    ZX_EXPECT(!FontFace::Load(move(bytes), "truncated"));
}

ZX_TEST(Font, LucideIcons)
{
    UniquePtr<FontFace> face = FontFace::LoadFile(LucidePath());
    ZX_REQUIRE(face);
    const u16 activity = face->GlyphIndex(57400); // "activity" in codepoints.json
    ZX_REQUIRE(activity != 0);
    GlyphOutline outline;
    ZX_EXPECT(face->GetGlyphOutline(activity, outline));
    ZX_EXPECT_EQ(face->GlyphIndex('A'), 0);
}

ZX_TEST(Rasterizer, ExactAreaCoverage)
{
    // Rectangle x in [1.25, 3.75], y in [1, 3]: partial columns at both ends.
    PathRasterizer r(5, 4);
    r.MoveTo(Vec2(1.25f, 1.0f));
    r.LineTo(Vec2(3.75f, 1.0f));
    r.LineTo(Vec2(3.75f, 3.0f));
    r.LineTo(Vec2(1.25f, 3.0f));
    r.Close();
    Array<u8> px;
    px.Resize(20);
    r.Resolve(px.Data(), 5);
    ZX_EXPECT_NEAR(CoverageAt(px, 5, 0, 1), 0.0f, 0.01f);
    ZX_EXPECT_NEAR(CoverageAt(px, 5, 1, 1), 0.75f, 0.01f);
    ZX_EXPECT_NEAR(CoverageAt(px, 5, 2, 1), 1.0f, 0.01f);
    ZX_EXPECT_NEAR(CoverageAt(px, 5, 3, 2), 0.75f, 0.01f);
    ZX_EXPECT_NEAR(CoverageAt(px, 5, 2, 0), 0.0f, 0.01f);
    ZX_EXPECT_NEAR(CoverageAt(px, 5, 2, 3), 0.0f, 0.01f);
    // Opposite winding gives the same coverage.
    PathRasterizer r2(5, 4);
    r2.MoveTo(Vec2(1.25f, 1.0f));
    r2.LineTo(Vec2(1.25f, 3.0f));
    r2.LineTo(Vec2(3.75f, 3.0f));
    r2.LineTo(Vec2(3.75f, 1.0f));
    r2.Close();
    Array<u8> px2;
    px2.Resize(20);
    r2.Resolve(px2.Data(), 5);
    for (usize i = 0; i < 20; ++i)
        ZX_EXPECT_EQ(px[i], px2[i]);
}

ZX_TEST(Rasterizer, TriangleArea)
{
    // Total coverage equals the polygon area (diagonal edges exercise the multi-cell path).
    PathRasterizer r(16, 16);
    r.MoveTo(Vec2(1.3f, 2.1f));
    r.LineTo(Vec2(14.7f, 4.4f));
    r.LineTo(Vec2(6.2f, 14.9f));
    r.Close();
    Array<u8> px;
    px.Resize(256);
    r.Resolve(px.Data(), 16);
    f32 sum = 0.0f;
    for (u8 v : px)
        sum += f32(v) / 255.0f;
    const f32 area = 0.5f * Abs(Cross(Vec2(14.7f, 4.4f) - Vec2(1.3f, 2.1f), Vec2(6.2f, 14.9f) - Vec2(1.3f, 2.1f)));
    ZX_EXPECT_NEAR(sum, area, 0.5f);
}

ZX_TEST(Rasterizer, GlyphWithHole)
{
    UniquePtr<FontFace> face = FontFace::LoadFile(InterLatinPath());
    ZX_REQUIRE(face);
    GlyphOutline outline;
    ZX_REQUIRE(face->GetGlyphOutline(face->GlyphIndex('O'), outline));
    GlyphBitmap bmp;
    ZX_REQUIRE(RasterizeGlyph(outline, face->ScaleForPixelHeight(64.0f), Vec2::Zero(), bmp));
    ZX_EXPECT_GT(bmp.width, 30u);
    ZX_EXPECT_GT(bmp.height, 40u);
    ZX_EXPECT_LT(bmp.top, -40); // the glyph is above the baseline
    // Center of 'O' is empty, the left stroke at mid height is solid.
    const u32 cy = bmp.height / 2;
    ZX_EXPECT_LT(CoverageAt(bmp.pixels, bmp.width, bmp.width / 2, cy), 0.05f);
    f32 maxLeft = 0.0f;
    for (u32 x = 0; x < bmp.width / 4; ++x)
        maxLeft = Max(maxLeft, CoverageAt(bmp.pixels, bmp.width, x, cy));
    ZX_EXPECT_GT(maxLeft, 0.95f);
    // Sub-pixel offset shifts the coverage but keeps the total ink.
    GlyphBitmap shifted;
    ZX_REQUIRE(RasterizeGlyph(outline, face->ScaleForPixelHeight(64.0f), Vec2(0.5f, 0.0f), shifted));
    f32 inkA = 0.0f, inkB = 0.0f;
    for (u8 v : bmp.pixels)
        inkA += f32(v);
    for (u8 v : shifted.pixels)
        inkB += f32(v);
    ZX_EXPECT_NEAR(inkA / inkB, 1.0f, 0.01f);
}

ZX_TEST(FontSystem, ShapingAndMeasurement)
{
    FontSystem fonts;
    const String latin = InterLatinPath(), cyrillic = InterCyrillicPath();
    const StringView faces[] = {latin, cyrillic};
    const FontHandle font = fonts.AddFont("Inter", Span<const StringView>(faces, 2));
    ZX_REQUIRE(font.IsValid());
    ZX_EXPECT(fonts.FindFont("Inter") == font);
    const f32 a = fonts.MeasureWidth(font, 16.0f, "A");
    const f32 v = fonts.MeasureWidth(font, 16.0f, "V");
    ZX_EXPECT_LT(fonts.MeasureWidth(font, 16.0f, "AV"), a + v - 0.1f); // kerning
    // Mixed scripts resolve through the fallback chain.
    Array<ShapedGlyph> glyphs;
    fonts.ShapeText(font, 16.0f, "Hi Мир", glyphs);
    ZX_REQUIRE(glyphs.Size() == 6u);
    ZX_EXPECT_NE(glyphs[0].face, glyphs[3].face);
    ZX_EXPECT_NE(glyphs[3].glyph, 0);
    ZX_EXPECT_EQ(glyphs[3].byteOffset, 3u);
    ZX_EXPECT_EQ(glyphs[4].byteOffset, 5u); // 'М' is 2 bytes
    // Measurement: two lines, wrapping.
    const FontSystem::LineMetrics lm = fonts.GetLineMetrics(font, 16.0f);
    ZX_EXPECT_NEAR(lm.lineHeight, 16.0f * (1984 + 494) / 2048.0f, 0.01f);
    const Vec2 two = fonts.MeasureText(font, 16.0f, "first\nsecond line");
    ZX_EXPECT_NEAR(two.y, lm.lineHeight * 2.0f, 0.01f);
    const f32 lineWidth = fonts.MeasureWidth(font, 16.0f, "one two three four");
    const Vec2 wrapped = fonts.MeasureText(font, 16.0f, "one two three four", lineWidth * 0.6f);
    ZX_EXPECT_GE(wrapped.y, lm.lineHeight * 2.0f - 0.01f);
    ZX_EXPECT_LE(wrapped.x, lineWidth * 0.6f);
    const usize wrap = fonts.WrapLine(font, 16.0f, "one two three four", lineWidth * 0.6f);
    ZX_EXPECT(wrap > 0 && wrap < 18);
    // Hit testing: caret positions at glyph boundaries.
    const StringView word = "Привет";
    ZX_EXPECT_EQ(fonts.HitTest(font, 16.0f, word, -5.0f), 0u);
    ZX_EXPECT_EQ(fonts.HitTest(font, 16.0f, word, 1000.0f), word.Size());
    const f32 w2 = fonts.MeasureWidth(font, 16.0f, "Пр");
    ZX_EXPECT_EQ(fonts.HitTest(font, 16.0f, word, w2 + 0.5f), 4u);
}

ZX_TEST(FontSystem, AtlasAndEviction)
{
    FontSystem fonts;
    const String latin = InterLatinPath();
    const StringView faces[] = {latin};
    const FontHandle font = fonts.AddFont("Inter", Span<const StringView>(faces, 1));
    ZX_REQUIRE(font.IsValid());
    Array<ShapedGlyph> glyphs;
    fonts.ShapeText(font, 14.0f, "W", glyphs);
    ZX_REQUIRE(glyphs.Size() == 1u);
    u64 frame = 1;
    fonts.BeginFrame(frame);
    const GlyphEntry* e = fonts.GetGlyph(glyphs[0].face, glyphs[0].glyph, 14.0f, 10.0f);
    ZX_REQUIRE(e);
    const GlyphEntry first = *e;
    ZX_EXPECT_GT(first.width, 8u);
    // Same glyph, same phase -> cached; other phase -> new entry.
    ZX_EXPECT_EQ(fonts.GetGlyph(glyphs[0].face, glyphs[0].glyph, 14.0f, 25.0f)->x, first.x);
    const GlyphEntry* phase = fonts.GetGlyph(glyphs[0].face, glyphs[0].glyph, 14.0f, 10.5f);
    ZX_REQUIRE(phase);
    ZX_EXPECT(phase->x != first.x || phase->y != first.y);
    u32 minY, maxY;
    fonts.GetDirtyRows(0, minY, maxY);
    ZX_EXPECT_LT(minY, maxY);
    // Fill the atlas with huge glyphs across frames: pages fill up, then old pages are evicted.
    for (u32 i = 0; i < 400; ++i) {
        fonts.BeginFrame(++frame);
        fonts.GetGlyph(glyphs[0].face, glyphs[0].glyph, 120.0f + f32(i), 0.0f);
    }
    ZX_EXPECT_EQ(fonts.AtlasPageCount(), FontSystem::MaxAtlasPages);
    ZX_EXPECT_GT(fonts.Evictions(), 0u);
    // The first glyph is re-rasterized after its page was evicted.
    fonts.BeginFrame(++frame);
    ZX_EXPECT(fonts.GetGlyph(glyphs[0].face, glyphs[0].glyph, 14.0f, 10.0f) != nullptr);
}

ZX_TEST(DrawList, PrimitivesAndClipping)
{
    FontSystem fonts;
    const String latin = InterLatinPath();
    const StringView faces[] = {latin};
    const FontHandle font = fonts.AddFont("Inter", Span<const StringView>(faces, 1));
    ZX_REQUIRE(font.IsValid());
    DrawList dl;
    dl.Reset(&fonts, Rect(Vec2(0, 0), Vec2(800, 600)));
    dl.AddRect(Rect(Vec2(10, 10), Vec2(100, 50)), Color(255, 0, 0), CornerRadii(4.0f));
    ZX_EXPECT_EQ(dl.PrimitiveCount(), 1u);
    dl.AddRect(Rect(Vec2(900, 10), Vec2(950, 50)), Color(255, 0, 0)); // outside the viewport: culled
    dl.AddRect(Rect(Vec2(10, 10), Vec2(100, 50)), Color(0, 0, 0, 0)); // invisible: dropped
    ZX_EXPECT_EQ(dl.PrimitiveCount(), 1u);
    dl.PushClipRect(Rect(Vec2(0, 0), Vec2(50, 50)), 6.0f);
    ZX_EXPECT_EQ(dl.Clips().Size(), 2u);
    dl.AddLine(Vec2(60, 60), Vec2(80, 80), Color(255, 255, 255)); // fully outside the clip
    ZX_EXPECT_EQ(dl.PrimitiveCount(), 1u);
    dl.AddCircle(Vec2(25, 25), 10.0f, Color(0, 255, 0));
    ZX_EXPECT_EQ(dl.Primitives()[1].header >> 16, 1u);
    dl.PopClipRect();
    const u32 before = dl.PrimitiveCount();
    const Vec2 size = dl.AddText(font, 16.0f, Vec2(100, 100), Color(255, 255, 255), "Ab c");
    ZX_EXPECT_EQ(dl.PrimitiveCount(), before + 3); // the space emits nothing
    ZX_EXPECT_GT(size.x, 20.0f);
    for (u32 i = before; i < dl.PrimitiveCount(); ++i)
        ZX_EXPECT_EQ(dl.Primitives()[i].header & 0xF, u32(PrimitiveType::Glyph));
    const Vec2 pts[] = {Vec2(0, 0), Vec2(10, 0), Vec2(10, 10), Vec2(0, 10)};
    const u32 beforePoly = dl.PrimitiveCount();
    dl.AddConvexPolygon(Span<const Vec2>(pts, 4), Color(255, 255, 255));
    ZX_EXPECT_EQ(dl.PrimitiveCount(), beforePoly + 2);
}
