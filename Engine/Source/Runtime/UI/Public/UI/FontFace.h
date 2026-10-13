#pragma once

// TrueType font parsing (own implementation, no third-party code).
//
//   Array<u8> bytes;  fs::ReadFile("/Engine/Fonts/Inter/inter-latin-400-normal.ttf", bytes);
//   UniquePtr<FontFace> face = FontFace::Load(move(bytes));   // TTF (glyf outlines) or WOFF 1.0
//   const u16 glyph = face->GlyphIndex('A');
//   const f32 scale = face->ScaleForPixelHeight(16.0f);         // em size in pixels -> scale per font unit
//   GlyphOutline outline;  face->GetGlyphOutline(glyph, outline);
//
// Supported: cmap formats 4 / 12, glyf/loca (simple + composite glyphs), hmtx, kern (format 0) and GPOS pair
// adjustment (formats 1 and 2, extension lookups) for the 'kern' feature. CFF outlines ('OTTO') are not supported.

#include "Core/Containers/Array.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Vector.h"
#include "Core/Memory/Pointers.h"
#include "Core/String/String.h"

namespace zx::ui {

/// Contours of a glyph in font units (y up). Quadratic Bezier segments; lines have control == midpoint.
struct GlyphOutline
{
    struct Segment
    {
        Vec2 p0;
        Vec2 control;
        Vec2 p1;
        bool isLine;
    };
    Array<Segment> segments;
    Vec2 boundsMin;
    Vec2 boundsMax;

    void Clear()
    {
        segments.Clear();
        boundsMin = boundsMax = Vec2::Zero();
    }
    bool Empty() const { return segments.Empty(); }
};

struct FontMetrics
{
    i32 unitsPerEm = 1000;
    i32 ascender = 0;  // font units, positive up
    i32 descender = 0; // negative
    i32 lineGap = 0;
    i32 capHeight = 0;
    i32 xHeight = 0;
};

class FontFace
{
public:
    /// Parses a TTF or WOFF 1.0 file. Takes ownership of the bytes. Returns null (and logs) on failure.
    static UniquePtr<FontFace> Load(Array<u8> data, StringView debugName = {});
    static UniquePtr<FontFace> LoadFile(StringView path);

    ~FontFace();
    ZX_NON_COPYABLE(FontFace)

    const String& GetName() const { return m_name; }
    const FontMetrics& GetMetrics() const { return m_metrics; }
    u32 GlyphCount() const { return m_numGlyphs; }

    /// Glyph index for a Unicode codepoint (0 = .notdef / missing).
    u16 GlyphIndex(u32 codepoint) const;
    bool HasGlyph(u32 codepoint) const { return GlyphIndex(codepoint) != 0; }

    /// Horizontal metrics in font units.
    i32 GlyphAdvance(u16 glyph) const;
    i32 GlyphLeftSideBearing(u16 glyph) const;
    /// Pair adjustment in font units (GPOS 'kern' feature, falls back to the 'kern' table). Cached (not thread-safe: UI thread only).
    i32 Kerning(u16 left, u16 right) const;

    /// Outline in font units. Returns false for empty glyphs (space) or on malformed data.
    bool GetGlyphOutline(u16 glyph, GlyphOutline& out) const;

    /// Scale from font units to pixels so that the em square is `pixels` high.
    f32 ScaleForPixelHeight(f32 pixels) const { return pixels / f32(m_metrics.unitsPerEm); }

private:
    FontFace() = default;
    struct Table
    {
        const u8* data = nullptr;
        u32 size = 0;
    };
    bool Parse();
    bool DecodeWoff();
    Table FindTable(const char* tag) const;
    bool AddGlyphOutline(u16 glyph, const f32 transform[6], GlyphOutline& out, u32 depth) const;
    i32 LookupGposKerning(u16 left, u16 right) const;
    i32 LookupKernTable(u16 left, u16 right) const;

    Array<u8> m_data;      // original file (TTF) or decoded sfnt (WOFF)
    String m_name;
    HashMap<u32, Table> m_tables;
    FontMetrics m_metrics;
    u32 m_numGlyphs = 0;
    u32 m_numHMetrics = 0;
    bool m_longLoca = false;
    Table m_cmap;      // selected subtable
    u16 m_cmapFormat = 0;
    Array<u32> m_kernLookups; // GPOS lookup offsets (absolute within GPOS) of 'kern' pair-adjustment subtables
    mutable HashMap<u32, i16> m_kernCache;
};

} // namespace zx::ui
