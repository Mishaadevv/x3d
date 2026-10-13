#pragma once

#include "Core/Containers/Function.h"
#include "UI/Rasterizer.h"

// Font system: fallback chains of faces, glyph cache with a dynamic atlas, text measurement.
//
//   FontSystem fonts;
//   FontHandle inter = fonts.AddFont("Inter", {"/Engine/Fonts/Inter/inter-latin-400-normal.ttf", ...});
//   fonts.AddIconFace(inter, "/Engine/Fonts/Lucide/lucide.ttf");   // icons as private-use codepoints
//   const Vec2 size = fonts.MeasureText(inter, 14.0f, "Hello, world");
//
// Glyphs are rasterized on the CPU (analytic coverage) at the exact pixel size with 4 horizontal sub-pixel phases, so
// text stays crisp at any DPI scale. The atlas is a fixed set of 1024x1024 R8 pages; when it is full the least recently
// used page is evicted (glyphs on it are re-rasterized on demand). The GPU copy is maintained by the UI renderer.

#include "Core/Containers/Array.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Geometry.h"
#include "Core/Memory/Pointers.h"
#include "Core/String/String.h"
#include "UI/FontFace.h"

namespace zx::ui {

struct FontHandle
{
    u16 index = 0xFFFF;
    bool IsValid() const { return index != 0xFFFF; }
    friend bool operator==(FontHandle, FontHandle) = default;
};

/// A rasterized glyph ready to draw.
struct GlyphEntry
{
    u16 page = 0;
    u16 x = 0, y = 0;      // atlas texel position
    u16 width = 0, height = 0;
    i16 left = 0, top = 0; // bitmap offset from the (pixel-snapped) pen position
};

/// Glyph after shaping: where to draw it relative to the line origin (left, baseline).
struct ShapedGlyph
{
    u32 face;      // global face index
    u16 glyph;
    f32 x;         // pen position (pixels)
    f32 y;         // baseline shift (pixels, y down)
    f32 size;      // pixel size of this glyph (icons can be scaled)
    f32 advance;
    u32 byteOffset; // in the source string
};

class FontSystem
{
public:
    static constexpr u32 AtlasPageSize = 1024;
    static constexpr u32 MaxAtlasPages = 4;

    FontSystem();
    ~FontSystem();
    ZX_NON_COPYABLE(FontSystem)

    /// Creates a font from a fallback chain of face files (first = primary). Returns invalid handle if none loads.
    FontHandle AddFont(StringView name, Span<const StringView> facePaths);
    /// Appends an icon face to a font's chain. Icons are scaled by `sizeScale` and centered on the cap height.
    bool AddIconFace(FontHandle font, StringView path, f32 sizeScale = 1.2f);
    FontHandle FindFont(StringView name) const;

    struct LineMetrics
    {
        f32 ascent;     // pixels above the baseline
        f32 descent;    // pixels below the baseline (positive)
        f32 lineHeight; // baseline-to-baseline
        f32 capHeight;
    };
    LineMetrics GetLineMetrics(FontHandle font, f32 size) const;

    /// Shapes one line (no wrapping; '\n' is treated as a space-width break by callers). Appends to `out`.
    void ShapeText(FontHandle font, f32 size, StringView text, Array<ShapedGlyph>& out) const;
    f32 MeasureWidth(FontHandle font, f32 size, StringView text) const;
    /// Size of text with explicit '\n' lines and optional word wrapping (wrapWidth > 0).
    Vec2 MeasureText(FontHandle font, f32 size, StringView text, f32 wrapWidth = 0.0f) const;
    /// Byte length of the prefix of `text` that fits into `width` when wrapping at word boundaries.
    usize WrapLine(FontHandle font, f32 size, StringView text, f32 width) const;
    /// Byte offset of the caret position closest to `x` (single line).
    usize HitTest(FontHandle font, f32 size, StringView text, f32 x) const;

    /// Returns the atlas entry of a glyph rasterized at `size` with the sub-pixel phase of `penX`
    /// (rasterizes on demand). Null for empty glyphs (spaces) or when the atlas is exhausted this frame.
    const GlyphEntry* GetGlyph(u32 face, u16 glyph, f32 size, f32 penX);
    /// Atlas entry for an application-rasterized bitmap (e.g. an SVG icon at one size), cached under `key`;
    /// `rasterize` runs only on a cache miss. Null if rasterization fails or the atlas is full this frame.
    const GlyphEntry* GetCustomGlyph(u64 key, const Function<bool(GlyphBitmap&)>& rasterize);

    // --- Atlas (consumed by the renderer) ---
    void BeginFrame(u64 frame) { m_frame = frame; }
    u32 AtlasPageCount() const { return u32(m_pages.Size()); }
    const u8* AtlasPagePixels(u32 page) const { return m_pages[page].pixels.Data(); }
    /// Dirty row range of a page since the last ClearDirty (empty when maxY <= minY).
    void GetDirtyRows(u32 page, u32& minY, u32& maxY) const
    {
        minY = m_pages[page].dirtyMinY;
        maxY = m_pages[page].dirtyMaxY;
    }
    void ClearDirty(u32 page)
    {
        m_pages[page].dirtyMinY = AtlasPageSize;
        m_pages[page].dirtyMaxY = 0;
    }
    u32 GlyphCacheSize() const { return u32(m_glyphs.Size()); }
    u32 Evictions() const { return m_evictions; }

private:
    struct FaceRef
    {
        u32 face;          // index into m_faces
        f32 sizeScale = 1.0f;
        bool icon = false;
    };
    struct FontData
    {
        String name;
        Array<FaceRef> chain;
        mutable HashMap<u32, u64> codepointCache; // codepoint -> (chain index << 16 | glyph)
    };
    struct Shelf
    {
        u32 y, height, x;
    };
    struct Page
    {
        Array<u8> pixels;
        Array<Shelf> shelves;
        u32 nextShelfY = 0;
        u64 lastUsedFrame = 0;
        u32 generation = 0;
        u32 dirtyMinY = AtlasPageSize;
        u32 dirtyMaxY = 0;
    };
    struct CachedGlyph
    {
        GlyphEntry entry;
        u32 generation = 0;
        bool empty = false;
    };

    u32 LoadFace(StringView path); // shared between fonts; U32Max on failure
    u64 ResolveCodepoint(const FontData& font, u32 cp) const;
    bool AllocateRect(u32 width, u32 height, u32& page, u32& x, u32& y);
    bool AllocateInPage(Page& page, u32 width, u32 height, u32& x, u32& y);

    Array<UniquePtr<FontFace>> m_faces;
    HashMap<String, u32> m_facePaths;
    Array<UniquePtr<FontData>> m_fonts;
    HashMap<u64, CachedGlyph> m_glyphs;
    Array<Page> m_pages;
    GlyphOutline m_outline; // scratch
    u64 m_frame = 0;
    u32 m_evictions = 0;
};

/// UTF-8 helpers for text editing.
usize Utf8PrevBoundary(StringView text, usize pos);
usize Utf8NextBoundary(StringView text, usize pos);

} // namespace zx::ui
