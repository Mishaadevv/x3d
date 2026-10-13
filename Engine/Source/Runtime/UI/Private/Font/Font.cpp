#include "UI/Font.h"
#include "Core/Log/Log.h"
#include "Core/String/Utf.h"
#include "UI/Rasterizer.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogFontSystem, Info);

namespace zx::ui {

namespace {

constexpr u32 SubpixelPhases = 4;
constexpr u32 GlyphGutter = 1;

u64 GlyphKey(u32 face, u16 glyph, u32 sizeKey, u32 phase)
{
    return (u64(face) << 48) | (u64(glyph) << 32) | (u64(sizeKey) << 8) | phase;
}

} // namespace

usize Utf8PrevBoundary(StringView text, usize pos)
{
    if (pos == 0)
        return 0;
    --pos;
    while (pos > 0 && (u8(text[pos]) & 0xC0) == 0x80)
        --pos;
    return pos;
}

usize Utf8NextBoundary(StringView text, usize pos)
{
    if (pos >= text.Size())
        return text.Size();
    ++pos;
    while (pos < text.Size() && (u8(text[pos]) & 0xC0) == 0x80)
        ++pos;
    return pos;
}

FontSystem::FontSystem() = default;
FontSystem::~FontSystem() = default;

FontHandle FontSystem::AddFont(StringView name, Span<const StringView> facePaths)
{
    UniquePtr<FontData> font(new FontData());
    font->name = name;
    for (StringView path : facePaths) {
        const u32 face = LoadFace(path);
        if (face != U32Max)
            font->chain.PushBack(FaceRef{face, 1.0f, false});
    }
    if (font->chain.Empty()) {
        ZX_LOG_ERROR(LogFontSystem, "Font '{}': no face could be loaded", name);
        return FontHandle{};
    }
    const FontHandle handle{u16(m_fonts.Size())};
    m_fonts.PushBack(move(font));
    return handle;
}

bool FontSystem::AddIconFace(FontHandle font, StringView path, f32 sizeScale)
{
    if (!font.IsValid() || font.index >= m_fonts.Size())
        return false;
    const u32 face = LoadFace(path);
    if (face == U32Max)
        return false;
    FontData& data = *m_fonts[font.index];
    data.chain.PushBack(FaceRef{face, sizeScale, true});
    data.codepointCache.Clear();
    return true;
}

u32 FontSystem::LoadFace(StringView path)
{
    if (const u32* existing = m_facePaths.Find(path))
        return *existing;
    UniquePtr<FontFace> face = FontFace::LoadFile(path);
    if (!face)
        return U32Max;
    const u32 index = u32(m_faces.Size());
    m_faces.PushBack(move(face));
    m_facePaths.Insert(String(path), index);
    return index;
}

FontHandle FontSystem::FindFont(StringView name) const
{
    for (usize i = 0; i < m_fonts.Size(); ++i)
        if (m_fonts[i]->name == name)
            return FontHandle{u16(i)};
    return FontHandle{};
}

FontSystem::LineMetrics FontSystem::GetLineMetrics(FontHandle font, f32 size) const
{
    const FontFace& face = *m_faces[m_fonts[font.index]->chain[0].face];
    const FontMetrics& m = face.GetMetrics();
    const f32 s = face.ScaleForPixelHeight(size);
    LineMetrics lm;
    lm.ascent = f32(m.ascender) * s;
    lm.descent = f32(-m.descender) * s;
    lm.lineHeight = f32(m.ascender - m.descender + m.lineGap) * s;
    lm.capHeight = f32(m.capHeight) * s;
    return lm;
}

u64 FontSystem::ResolveCodepoint(const FontData& font, u32 cp) const
{
    if (const u64* cached = font.codepointCache.Find(cp))
        return *cached;
    u64 result = 0; // chain 0, .notdef
    for (usize i = 0; i < font.chain.Size(); ++i) {
        const u16 g = m_faces[font.chain[i].face]->GlyphIndex(cp);
        if (g != 0) {
            result = (u64(i) << 16) | g;
            break;
        }
    }
    font.codepointCache.Insert(cp, result);
    return result;
}

void FontSystem::ShapeText(FontHandle font, f32 size, StringView text, Array<ShapedGlyph>& out) const
{
    const FontData& data = *m_fonts[font.index];
    const FontFace& primary = *m_faces[data.chain[0].face];
    const f32 capHeight = f32(primary.GetMetrics().capHeight) * primary.ScaleForPixelHeight(size);
    f32 pen = 0.0f;
    u32 prevFace = U32Max;
    u16 prevGlyph = 0;
    usize pos = 0;
    while (pos < text.Size()) {
        const usize start = pos;
        u32 cp = Utf8Decode(text, pos);
        u32 repeat = 1;
        if (cp == '\t') {
            cp = ' ';
            repeat = 4;
        } else if (cp == '\n' || cp == '\r') {
            cp = ' ';
        }
        const u64 resolved = ResolveCodepoint(data, cp);
        const FaceRef& ref = data.chain[usize(resolved >> 16)];
        const FontFace& face = *m_faces[ref.face];
        const u16 glyph = u16(resolved & 0xFFFF);
        const f32 glyphSize = size * ref.sizeScale;
        const f32 scale = face.ScaleForPixelHeight(glyphSize);
        if (prevFace == ref.face && !ref.icon)
            pen += f32(face.Kerning(prevGlyph, glyph)) * scale;
        ShapedGlyph g;
        g.face = ref.face;
        g.glyph = glyph;
        g.x = pen;
        g.y = 0.0f;
        if (ref.icon) {
            const FontMetrics& m = face.GetMetrics();
            g.y = f32(m.ascender + m.descender) * 0.5f * scale - capHeight * 0.5f;
        }
        g.size = glyphSize;
        g.advance = f32(face.GlyphAdvance(glyph)) * scale * f32(repeat);
        g.byteOffset = u32(start);
        out.PushBack(g);
        pen += g.advance;
        prevFace = ref.face;
        prevGlyph = glyph;
    }
}

f32 FontSystem::MeasureWidth(FontHandle font, f32 size, StringView text) const
{
    if (text.Empty())
        return 0.0f;
    // Same arithmetic as ShapeText, without storing the glyphs.
    const FontData& data = *m_fonts[font.index];
    f32 pen = 0.0f;
    u32 prevFace = U32Max;
    u16 prevGlyph = 0;
    usize pos = 0;
    while (pos < text.Size()) {
        u32 cp = Utf8Decode(text, pos);
        f32 repeat = 1.0f;
        if (cp == '\t') {
            cp = ' ';
            repeat = 4.0f;
        } else if (cp == '\n' || cp == '\r') {
            cp = ' ';
        }
        const u64 resolved = ResolveCodepoint(data, cp);
        const FaceRef& ref = data.chain[usize(resolved >> 16)];
        const FontFace& face = *m_faces[ref.face];
        const u16 glyph = u16(resolved & 0xFFFF);
        const f32 scale = face.ScaleForPixelHeight(size * ref.sizeScale);
        if (prevFace == ref.face && !ref.icon)
            pen += f32(face.Kerning(prevGlyph, glyph)) * scale;
        pen += f32(face.GlyphAdvance(glyph)) * scale * repeat;
        prevFace = ref.face;
        prevGlyph = glyph;
    }
    return pen;
}

usize FontSystem::WrapLine(FontHandle font, f32 size, StringView text, f32 width) const
{
    // Greedy: extend word by word; a single word longer than the line is broken by codepoints.
    usize lineEnd = text.Find('\n');
    if (lineEnd == StringView::NPos)
        lineEnd = text.Size();
    const StringView line = text.Left(lineEnd);
    if (MeasureWidth(font, size, line) <= width)
        return lineEnd;
    usize best = 0;
    usize pos = 0;
    while (pos < line.Size()) {
        usize next = pos;
        while (next < line.Size() && line[next] == ' ')
            ++next;
        while (next < line.Size() && line[next] != ' ')
            ++next;
        if (MeasureWidth(font, size, line.Left(next)) > width)
            break;
        best = next;
        pos = next;
    }
    if (best == 0) {
        // No word fits: break inside the first word.
        usize p = Utf8NextBoundary(line, 0);
        best = p;
        while (p < line.Size()) {
            const usize n = Utf8NextBoundary(line, p);
            if (MeasureWidth(font, size, line.Left(n)) > width)
                break;
            best = p = n;
        }
    }
    return best;
}

Vec2 FontSystem::MeasureText(FontHandle font, f32 size, StringView text, f32 wrapWidth) const
{
    const LineMetrics lm = GetLineMetrics(font, size);
    f32 width = 0.0f;
    u32 lines = 0;
    StringView rest = text;
    for (;;) {
        usize len;
        if (wrapWidth > 0.0f) {
            len = WrapLine(font, size, rest, wrapWidth);
        } else {
            len = rest.Find('\n');
            if (len == StringView::NPos)
                len = rest.Size();
        }
        StringView line = rest.Left(len);
        while (!line.Empty() && line.Back() == ' ' && wrapWidth > 0.0f)
            line = line.DropRight(1);
        width = Max(width, MeasureWidth(font, size, line));
        ++lines;
        rest = rest.DropLeft(len);
        while (!rest.Empty() && rest[0] == ' ' && wrapWidth > 0.0f)
            rest = rest.DropLeft(1);
        if (rest.Empty())
            break;
        if (rest[0] == '\n') {
            rest = rest.DropLeft(1);
            if (rest.Empty()) {
                ++lines;
                break;
            }
        }
    }
    return Vec2(width, lm.lineHeight * f32(lines));
}

usize FontSystem::HitTest(FontHandle font, f32 size, StringView text, f32 x) const
{
    Array<ShapedGlyph> glyphs;
    ShapeText(font, size, text, glyphs);
    for (const ShapedGlyph& g : glyphs)
        if (x < g.x + g.advance * 0.5f)
            return g.byteOffset;
    return text.Size();
}

bool FontSystem::AllocateInPage(Page& page, u32 width, u32 height, u32& x, u32& y)
{
    const u32 w = width + GlyphGutter, h = height + GlyphGutter;
    // Best-fitting existing shelf (height within 30 %).
    Shelf* best = nullptr;
    for (Shelf& s : page.shelves)
        if (s.height >= h && s.height <= h + h * 3 / 10 + 1 && s.x + w <= AtlasPageSize && (!best || s.height < best->height))
            best = &s;
    if (!best) {
        if (page.nextShelfY + h > AtlasPageSize || w > AtlasPageSize)
            return false;
        page.shelves.PushBack(Shelf{page.nextShelfY, h, 0});
        page.nextShelfY += h;
        best = &page.shelves.Back();
    }
    x = best->x;
    y = best->y;
    best->x += w;
    return true;
}

bool FontSystem::AllocateRect(u32 width, u32 height, u32& pageIndex, u32& x, u32& y)
{
    for (u32 i = 0; i < m_pages.Size(); ++i)
        if (AllocateInPage(m_pages[i], width, height, x, y)) {
            pageIndex = i;
            return true;
        }
    if (m_pages.Size() < MaxAtlasPages) {
        Page& p = m_pages.AddDefaulted();
        p.pixels.Resize(usize(AtlasPageSize) * AtlasPageSize, 0);
        pageIndex = u32(m_pages.Size() - 1);
        return AllocateInPage(p, width, height, x, y);
    }
    // Evict the least recently used page that is not needed by the current frame.
    u32 victim = U32Max;
    for (u32 i = 0; i < m_pages.Size(); ++i)
        if (m_pages[i].lastUsedFrame < m_frame && (victim == U32Max || m_pages[i].lastUsedFrame < m_pages[victim].lastUsedFrame))
            victim = i;
    if (victim == U32Max)
        return false;
    Page& p = m_pages[victim];
    MemZero(p.pixels.Data(), p.pixels.Size());
    p.shelves.Clear();
    p.nextShelfY = 0;
    ++p.generation;
    p.dirtyMinY = 0;
    p.dirtyMaxY = AtlasPageSize;
    ++m_evictions;
    pageIndex = victim;
    return AllocateInPage(p, width, height, x, y);
}

const GlyphEntry* FontSystem::GetCustomGlyph(u64 key, const Function<bool(GlyphBitmap&)>& rasterize)
{
    key |= 0xFFFFull << 48; // never collides with font glyph keys (face index < 0xFFFF)
    if (CachedGlyph* cached = m_glyphs.Find(key)) {
        if (cached->empty)
            return nullptr;
        Page& page = m_pages[cached->entry.page];
        if (cached->generation == page.generation) {
            page.lastUsedFrame = m_frame;
            return &cached->entry;
        }
    }
    GlyphBitmap bitmap;
    CachedGlyph result;
    if (!rasterize(bitmap) || bitmap.width == 0 || bitmap.height == 0) {
        result.empty = true;
        m_glyphs.InsertOrAssign(key, result);
        return nullptr;
    }
    u32 pageIndex, x, y;
    if (!AllocateRect(bitmap.width, bitmap.height, pageIndex, x, y))
        return nullptr;
    Page& page = m_pages[pageIndex];
    for (u32 row = 0; row < bitmap.height; ++row)
        MemCopy(page.pixels.Data() + usize(y + row) * AtlasPageSize + x, bitmap.pixels.Data() + usize(row) * bitmap.width, bitmap.width);
    page.dirtyMinY = Min(page.dirtyMinY, y);
    page.dirtyMaxY = Max(page.dirtyMaxY, y + bitmap.height);
    page.lastUsedFrame = m_frame;
    result.entry = GlyphEntry{u16(pageIndex), u16(x), u16(y), u16(bitmap.width), u16(bitmap.height), i16(bitmap.left), i16(bitmap.top)};
    result.generation = page.generation;
    return &m_glyphs.InsertOrAssign(key, result).entry;
}

const GlyphEntry* FontSystem::GetGlyph(u32 faceIndex, u16 glyph, f32 size, f32 penX)
{
    const u32 sizeKey = u32(Clamp(Round(size * 4.0f), 1.0f, 16383.0f));
    const u32 phase = u32(Floor((penX - Floor(penX)) * f32(SubpixelPhases))) % SubpixelPhases;
    const u64 key = GlyphKey(faceIndex, glyph, sizeKey, phase);
    CachedGlyph* cached = m_glyphs.Find(key);
    if (cached) {
        if (cached->empty)
            return nullptr;
        Page& page = m_pages[cached->entry.page];
        if (cached->generation == page.generation) {
            page.lastUsedFrame = m_frame;
            return &cached->entry;
        }
    }
    // Rasterize.
    const FontFace& face = *m_faces[faceIndex];
    CachedGlyph result;
    GlyphBitmap bitmap;
    const f32 rasterSize = f32(sizeKey) * 0.25f;
    if (!face.GetGlyphOutline(glyph, m_outline) ||
        !RasterizeGlyph(m_outline, face.ScaleForPixelHeight(rasterSize), Vec2(f32(phase) / f32(SubpixelPhases), 0.0f), bitmap)) {
        result.empty = true;
        m_glyphs.InsertOrAssign(key, result);
        return nullptr;
    }
    u32 pageIndex, x, y;
    if (!AllocateRect(bitmap.width, bitmap.height, pageIndex, x, y))
        return nullptr; // atlas exhausted by this frame's glyphs: skip (retried next frame)
    Page& page = m_pages[pageIndex];
    for (u32 row = 0; row < bitmap.height; ++row)
        MemCopy(page.pixels.Data() + usize(y + row) * AtlasPageSize + x, bitmap.pixels.Data() + usize(row) * bitmap.width,
                bitmap.width);
    page.dirtyMinY = Min(page.dirtyMinY, y);
    page.dirtyMaxY = Max(page.dirtyMaxY, y + bitmap.height);
    page.lastUsedFrame = m_frame;
    result.entry = GlyphEntry{u16(pageIndex), u16(x), u16(y), u16(bitmap.width), u16(bitmap.height), i16(bitmap.left), i16(bitmap.top)};
    result.generation = page.generation;
    return &m_glyphs.InsertOrAssign(key, result).entry;
}

} // namespace zx::ui
