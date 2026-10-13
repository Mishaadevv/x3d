#include "UI/FontFace.h"
#include "Core/Compression/Compression.h"
#include "Core/Containers/Algorithm.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Log/Log.h"
#include "Core/String/Path.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogFont, Info);

namespace zx::ui {

namespace {

constexpr u32 Tag(const char* t) { return (u32(u8(t[0])) << 24) | (u32(u8(t[1])) << 16) | (u32(u8(t[2])) << 8) | u32(u8(t[3])); }

// Big-endian readers. All reads are bounds-checked against the table size; out-of-range reads return 0.
struct Reader
{
    const u8* data = nullptr;
    u32 size = 0;

    bool Has(u32 offset, u32 bytes) const { return u64(offset) + bytes <= size; }
    u8 U8(u32 o) const { return Has(o, 1) ? data[o] : 0; }
    u16 U16(u32 o) const { return Has(o, 2) ? u16((u32(data[o]) << 8) | data[o + 1]) : 0; }
    i16 I16(u32 o) const { return i16(U16(o)); }
    u32 U32(u32 o) const
    {
        return Has(o, 4) ? (u32(data[o]) << 24) | (u32(data[o + 1]) << 16) | (u32(data[o + 2]) << 8) | data[o + 3] : 0;
    }
    Reader Sub(u32 offset) const { return offset <= size ? Reader{data + offset, size - offset} : Reader{}; }
};

/// Index of `glyph` in an OpenType coverage table, or -1.
i32 CoverageIndex(const Reader& r, u16 glyph)
{
    const u16 format = r.U16(0);
    if (format == 1) {
        u32 lo = 0, hi = r.U16(2);
        while (lo < hi) {
            const u32 mid = (lo + hi) / 2;
            const u16 g = r.U16(4 + mid * 2);
            if (g == glyph)
                return i32(mid);
            if (g < glyph)
                lo = mid + 1;
            else
                hi = mid;
        }
    } else if (format == 2) {
        u32 lo = 0, hi = r.U16(2);
        while (lo < hi) {
            const u32 mid = (lo + hi) / 2;
            const u32 rec = 4 + mid * 6;
            const u16 start = r.U16(rec), end = r.U16(rec + 2);
            if (glyph < start)
                hi = mid;
            else if (glyph > end)
                lo = mid + 1;
            else
                return i32(r.U16(rec + 4) + (glyph - start));
        }
    }
    return -1;
}

u16 GlyphClass(const Reader& r, u16 glyph)
{
    const u16 format = r.U16(0);
    if (format == 1) {
        const u16 start = r.U16(2), count = r.U16(4);
        if (glyph >= start && glyph < start + count)
            return r.U16(6 + (glyph - start) * 2);
    } else if (format == 2) {
        u32 lo = 0, hi = r.U16(2);
        while (lo < hi) {
            const u32 mid = (lo + hi) / 2;
            const u32 rec = 4 + mid * 6;
            const u16 start = r.U16(rec), end = r.U16(rec + 2);
            if (glyph < start)
                hi = mid;
            else if (glyph > end)
                lo = mid + 1;
            else
                return r.U16(rec + 4);
        }
    }
    return 0;
}

u32 ValueRecordSize(u16 format) { return u32(std::popcount(u32(format & 0xFF))) * 2; }

/// XAdvance from a GPOS ValueRecord (0 if absent).
i32 ValueXAdvance(const Reader& r, u32 offset, u16 format)
{
    if (!(format & 0x0004))
        return 0;
    u32 o = offset;
    if (format & 0x0001)
        o += 2;
    if (format & 0x0002)
        o += 2;
    return r.I16(o);
}

/// Pair adjustment subtable (GPOS lookup type 2). Returns true if the pair is covered.
bool PairPosAdjustment(const Reader& st, u16 left, u16 right, i32& out)
{
    const u16 format = st.U16(0);
    const i32 coverage = CoverageIndex(st.Sub(st.U16(2)), left);
    if (coverage < 0)
        return false;
    const u16 vf1 = st.U16(4), vf2 = st.U16(6);
    const u32 size1 = ValueRecordSize(vf1), size2 = ValueRecordSize(vf2);
    if (format == 1) {
        if (u32(coverage) >= st.U16(8))
            return false;
        const Reader set = st.Sub(st.U16(10 + u32(coverage) * 2));
        const u32 recordSize = 2 + size1 + size2;
        u32 lo = 0, hi = set.U16(0);
        while (lo < hi) {
            const u32 mid = (lo + hi) / 2;
            const u32 rec = 2 + mid * recordSize;
            const u16 second = set.U16(rec);
            if (second == right) {
                out = ValueXAdvance(set, rec + 2, vf1);
                return true;
            }
            if (second < right)
                lo = mid + 1;
            else
                hi = mid;
        }
        return false;
    }
    if (format == 2) {
        const u16 c1 = GlyphClass(st.Sub(st.U16(8)), left);
        const u16 c2 = GlyphClass(st.Sub(st.U16(10)), right);
        const u16 class1Count = st.U16(12), class2Count = st.U16(14);
        if (c1 >= class1Count || c2 >= class2Count)
            return false;
        const u32 rec = 16 + (u32(c1) * class2Count + c2) * (size1 + size2);
        out = ValueXAdvance(st, rec, vf1);
        return true; // class-based subtables cover every pair of covered glyphs (class 0 included)
    }
    return false;
}

void ApplyTransform(const f32 m[6], Vec2& p)
{
    const Vec2 q(m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5]);
    p = q;
}

} // namespace

FontFace::~FontFace() = default;

UniquePtr<FontFace> FontFace::Load(Array<u8> data, StringView debugName)
{
    UniquePtr<FontFace> face(new FontFace());
    face->m_data = move(data);
    face->m_name = debugName;
    if (!face->Parse())
        return nullptr;
    return face;
}

UniquePtr<FontFace> FontFace::LoadFile(StringView path)
{
    Array<u8> bytes;
    if (!fs::ReadFile(path, bytes)) {
        ZX_LOG_ERROR(LogFont, "Cannot read font '{}'", path);
        return nullptr;
    }
    return Load(move(bytes), Path::FileName(path));
}

FontFace::Table FontFace::FindTable(const char* tag) const
{
    const Table* t = m_tables.Find(Tag(tag));
    return t ? *t : Table{};
}

bool FontFace::DecodeWoff()
{
    const Reader r{m_data.Data(), u32(m_data.Size())};
    const u32 flavor = r.U32(4);
    const u16 numTables = r.U16(12);
    const u32 totalSfntSize = r.U32(16);
    if (flavor == Tag("OTTO")) {
        ZX_LOG_ERROR(LogFont, "'{}': CFF-flavored WOFF is not supported", m_name);
        return false;
    }
    if (numTables == 0 || numTables > 256 || !r.Has(44, u32(numTables) * 20) || totalSfntSize > (64u << 20)) {
        ZX_LOG_ERROR(LogFont, "'{}': malformed WOFF header", m_name);
        return false;
    }
    // Rebuild a plain sfnt: offset table + directory + 4-byte aligned tables.
    Array<u8> sfnt;
    const u32 headerSize = 12 + u32(numTables) * 16;
    sfnt.Resize(headerSize);
    auto put16 = [&](u32 o, u32 v) {
        sfnt[o] = u8(v >> 8);
        sfnt[o + 1] = u8(v);
    };
    auto put32 = [&](u32 o, u32 v) {
        put16(o, v >> 16);
        put16(o + 2, v & 0xFFFF);
    };
    put32(0, flavor);
    put16(4, numTables);
    for (u32 i = 0; i < numTables; ++i) {
        const u32 e = 44 + i * 20;
        const u32 tag = r.U32(e), offset = r.U32(e + 4), compLength = r.U32(e + 8), origLength = r.U32(e + 12);
        if (!r.Has(offset, compLength) || compLength > origLength || origLength > (32u << 20)) {
            ZX_LOG_ERROR(LogFont, "'{}': malformed WOFF table directory", m_name);
            return false;
        }
        const u32 dst = u32(sfnt.Size());
        if (compLength < origLength) {
            Array<u8> table;
            if (!compression::ZlibDecompress(ByteSpan(r.data + offset, compLength), table, origLength) ||
                table.Size() != origLength) {
                ZX_LOG_ERROR(LogFont, "'{}': failed to decompress a WOFF table", m_name);
                return false;
            }
            sfnt.Append(table.View());
        } else {
            sfnt.Append(ByteSpan(r.data + offset, origLength));
        }
        while (sfnt.Size() % 4)
            sfnt.PushBack(0);
        const u32 d = 12 + i * 16;
        put32(d, tag);
        put32(d + 4, r.U32(e + 16));
        put32(d + 8, dst);
        put32(d + 12, origLength);
    }
    m_data = move(sfnt);
    return true;
}

bool FontFace::Parse()
{
    if (m_data.Size() < 12) {
        ZX_LOG_ERROR(LogFont, "'{}': file too small", m_name);
        return false;
    }
    Reader file{m_data.Data(), u32(m_data.Size())};
    u32 version = file.U32(0);
    if (version == Tag("wOFF")) {
        if (!DecodeWoff())
            return false;
        file = Reader{m_data.Data(), u32(m_data.Size())};
        version = file.U32(0);
    }
    if (version == Tag("OTTO")) {
        ZX_LOG_ERROR(LogFont, "'{}': CFF outlines are not supported", m_name);
        return false;
    }
    if (version != 0x00010000 && version != Tag("true")) {
        ZX_LOG_ERROR(LogFont, "'{}': not a TrueType/WOFF font", m_name);
        return false;
    }
    const u16 numTables = file.U16(4);
    for (u32 i = 0; i < numTables; ++i) {
        const u32 e = 12 + i * 16;
        const u32 offset = file.U32(e + 8), length = file.U32(e + 12);
        if (!file.Has(e, 16) || !file.Has(offset, length)) {
            ZX_LOG_ERROR(LogFont, "'{}': table out of bounds", m_name);
            return false;
        }
        m_tables.InsertOrAssign(file.U32(e), Table{file.data + offset, length});
    }

    const Table headT = FindTable("head"), hheaT = FindTable("hhea"), maxpT = FindTable("maxp");
    const Table hmtxT = FindTable("hmtx"), locaT = FindTable("loca"), glyfT = FindTable("glyf"), cmapT = FindTable("cmap");
    if (!headT.data || !hheaT.data || !maxpT.data || !hmtxT.data || !locaT.data || !glyfT.data || !cmapT.data) {
        ZX_LOG_ERROR(LogFont, "'{}': missing required tables (head/hhea/maxp/hmtx/loca/glyf/cmap)", m_name);
        return false;
    }
    const Reader head{headT.data, headT.size}, hhea{hheaT.data, hheaT.size}, maxp{maxpT.data, maxpT.size};
    m_metrics.unitsPerEm = head.U16(18);
    if (m_metrics.unitsPerEm < 16 || m_metrics.unitsPerEm > 16384) {
        ZX_LOG_ERROR(LogFont, "'{}': invalid unitsPerEm", m_name);
        return false;
    }
    m_longLoca = head.I16(50) != 0;
    m_numGlyphs = maxp.U16(4);
    m_numHMetrics = hhea.U16(34);
    if (m_numHMetrics == 0 || m_numHMetrics > m_numGlyphs || hmtxT.size < m_numHMetrics * 4u) {
        ZX_LOG_ERROR(LogFont, "'{}': invalid hmtx", m_name);
        return false;
    }
    m_metrics.ascender = hhea.I16(4);
    m_metrics.descender = hhea.I16(6);
    m_metrics.lineGap = hhea.I16(8);
    if (const Table os2T = FindTable("OS/2"); os2T.data) {
        const Reader os2{os2T.data, os2T.size};
        if (os2.U16(0) >= 2) {
            m_metrics.xHeight = os2.I16(86);
            m_metrics.capHeight = os2.I16(88);
        }
        if (m_metrics.ascender == 0 && m_metrics.descender == 0) {
            m_metrics.ascender = os2.I16(68);
            m_metrics.descender = os2.I16(70);
            m_metrics.lineGap = os2.I16(72);
        }
    }
    if (m_metrics.capHeight == 0)
        m_metrics.capHeight = m_metrics.ascender * 7 / 10;
    if (m_metrics.xHeight == 0)
        m_metrics.xHeight = m_metrics.capHeight * 7 / 10;

    // cmap: prefer full Unicode (format 12), then BMP (format 4).
    const Reader cmap{cmapT.data, cmapT.size};
    const u16 numSub = cmap.U16(2);
    u32 bestScore = 0;
    for (u32 i = 0; i < numSub; ++i) {
        const u32 rec = 4 + i * 8;
        const u16 platform = cmap.U16(rec), encoding = cmap.U16(rec + 2);
        const u32 offset = cmap.U32(rec + 4);
        const u16 format = cmap.U16(offset);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode || (format != 4 && format != 12))
            continue;
        const u32 score = format == 12 ? 2 : 1;
        if (score > bestScore) {
            bestScore = score;
            m_cmapFormat = format;
            m_cmap = Table{cmap.data + offset, cmap.size - offset};
        }
    }
    if (!bestScore) {
        ZX_LOG_ERROR(LogFont, "'{}': no Unicode cmap (format 4/12)", m_name);
        return false;
    }

    // GPOS 'kern' feature lookups (pair adjustment, possibly wrapped in extension lookups).
    if (const Table gposT = FindTable("GPOS"); gposT.data) {
        const Reader gpos{gposT.data, gposT.size};
        const Reader features = gpos.Sub(gpos.U16(6));
        const Reader lookups = gpos.Sub(gpos.U16(8));
        Array<u16> lookupIndices;
        const u16 featureCount = features.U16(0);
        for (u32 i = 0; i < featureCount; ++i) {
            if (features.U32(2 + i * 6) != Tag("kern"))
                continue;
            const Reader feature = features.Sub(features.U16(2 + i * 6 + 4));
            const u16 count = feature.U16(2);
            for (u32 k = 0; k < count; ++k)
                lookupIndices.AddUnique(feature.U16(4 + k * 2));
        }
        Sort(lookupIndices); // lookups apply in LookupList order
        for (u16 li : lookupIndices) {
            if (li >= lookups.U16(0))
                continue;
            const u32 lookupOffset = u32(gpos.U16(8)) + lookups.U16(2 + li * 2);
            const Reader lookup = gpos.Sub(lookupOffset);
            const u16 type = lookup.U16(0), subCount = lookup.U16(4);
            for (u32 s = 0; s < subCount; ++s) {
                u32 sub = lookupOffset + lookup.U16(6 + s * 2);
                if (type == 9) { // extension
                    const Reader ext = gpos.Sub(sub);
                    if (ext.U16(2) != 2)
                        continue;
                    sub += ext.U32(4);
                } else if (type != 2) {
                    continue;
                }
                if (sub < gpos.size)
                    m_kernLookups.PushBack(sub);
            }
        }
    }
    return true;
}

u16 FontFace::GlyphIndex(u32 cp) const
{
    const Reader r{m_cmap.data, m_cmap.size};
    if (m_cmapFormat == 12) {
        u32 lo = 0, hi = r.U32(12);
        while (lo < hi) {
            const u32 mid = (lo + hi) / 2;
            const u32 rec = 16 + mid * 12;
            const u32 start = r.U32(rec), end = r.U32(rec + 4);
            if (cp < start)
                hi = mid;
            else if (cp > end)
                lo = mid + 1;
            else {
                const u32 g = r.U32(rec + 8) + (cp - start);
                return g < m_numGlyphs ? u16(g) : 0;
            }
        }
        return 0;
    }
    if (m_cmapFormat == 4) {
        if (cp > 0xFFFF)
            return 0;
        const u32 segCount = r.U16(6) / 2;
        const u32 endCodes = 14, startCodes = endCodes + segCount * 2 + 2;
        const u32 idDeltas = startCodes + segCount * 2, idRangeOffsets = idDeltas + segCount * 2;
        u32 lo = 0, hi = segCount;
        while (lo < hi) {
            const u32 mid = (lo + hi) / 2;
            const u16 end = r.U16(endCodes + mid * 2);
            if (cp > end) {
                lo = mid + 1;
                continue;
            }
            const u16 start = r.U16(startCodes + mid * 2);
            if (cp < start) {
                hi = mid;
                continue;
            }
            const u16 delta = r.U16(idDeltas + mid * 2);
            const u16 rangeOffset = r.U16(idRangeOffsets + mid * 2);
            u32 g;
            if (rangeOffset == 0) {
                g = (cp + delta) & 0xFFFF;
            } else {
                const u32 addr = idRangeOffsets + mid * 2 + rangeOffset + (cp - start) * 2;
                g = r.U16(addr);
                if (g != 0)
                    g = (g + delta) & 0xFFFF;
            }
            return g < m_numGlyphs ? u16(g) : 0;
        }
    }
    return 0;
}

i32 FontFace::GlyphAdvance(u16 glyph) const
{
    const Table t = FindTable("hmtx");
    const Reader r{t.data, t.size};
    return r.U16((glyph < m_numHMetrics ? glyph : m_numHMetrics - 1) * 4);
}

i32 FontFace::GlyphLeftSideBearing(u16 glyph) const
{
    const Table t = FindTable("hmtx");
    const Reader r{t.data, t.size};
    if (glyph < m_numHMetrics)
        return r.I16(glyph * 4 + 2);
    return r.I16(m_numHMetrics * 4 + (glyph - m_numHMetrics) * 2);
}

i32 FontFace::Kerning(u16 left, u16 right) const
{
    const u32 key = (u32(left) << 16) | right;
    if (const i16* cached = m_kernCache.Find(key))
        return *cached;
    const i32 value = m_kernLookups.Empty() ? LookupKernTable(left, right) : LookupGposKerning(left, right);
    m_kernCache.Insert(key, i16(Clamp(value, -32768, 32767)));
    return value;
}

i32 FontFace::LookupGposKerning(u16 left, u16 right) const
{
    const Table t = FindTable("GPOS");
    const Reader gpos{t.data, t.size};
    i32 total = 0;
    // Subtables of one lookup: the first one covering the pair wins; separate lookups accumulate.
    for (u32 offset : m_kernLookups) {
        i32 v = 0;
        if (PairPosAdjustment(gpos.Sub(offset), left, right, v))
            total += v;
    }
    return total;
}

i32 FontFace::LookupKernTable(u16 left, u16 right) const
{
    const Table t = FindTable("kern");
    if (!t.data)
        return 0;
    const Reader kern{t.data, t.size};
    if (kern.U16(0) != 0)
        return 0; // only the Microsoft version 0 layout
    const u16 nTables = kern.U16(2);
    u32 offset = 4;
    i32 total = 0;
    for (u32 i = 0; i < nTables; ++i) {
        const u16 length = kern.U16(offset + 2), coverage = kern.U16(offset + 4);
        if ((coverage >> 8) == 0 && (coverage & 1)) { // format 0, horizontal
            const Reader st = kern.Sub(offset + 6);
            const u32 key = (u32(left) << 16) | right;
            u32 lo = 0, hi = st.U16(0);
            while (lo < hi) {
                const u32 mid = (lo + hi) / 2;
                const u32 rec = 8 + mid * 6;
                const u32 k = st.U32(rec);
                if (k == key) {
                    total += st.I16(rec + 4);
                    break;
                }
                if (k < key)
                    lo = mid + 1;
                else
                    hi = mid;
            }
        }
        if (length == 0)
            break;
        offset += length;
    }
    return total;
}

bool FontFace::GetGlyphOutline(u16 glyph, GlyphOutline& out) const
{
    out.Clear();
    static constexpr f32 Identity[6] = {1, 0, 0, 1, 0, 0};
    if (!AddGlyphOutline(glyph, Identity, out, 0) || out.segments.Empty()) {
        out.Clear();
        return false;
    }
    Vec2 mn(F32Max), mx(-F32Max);
    for (const GlyphOutline::Segment& s : out.segments) {
        mn = Min(mn, Min(s.p0, Min(s.control, s.p1)));
        mx = Max(mx, Max(s.p0, Max(s.control, s.p1)));
    }
    out.boundsMin = mn;
    out.boundsMax = mx;
    return true;
}

bool FontFace::AddGlyphOutline(u16 glyph, const f32 m[6], GlyphOutline& out, u32 depth) const
{
    if (glyph >= m_numGlyphs || depth > 8)
        return false;
    const Table locaT = FindTable("loca"), glyfT = FindTable("glyf");
    const Reader loca{locaT.data, locaT.size}, glyf{glyfT.data, glyfT.size};
    u32 start, end;
    if (m_longLoca) {
        start = loca.U32(glyph * 4);
        end = loca.U32(glyph * 4 + 4);
    } else {
        start = u32(loca.U16(glyph * 2)) * 2;
        end = u32(loca.U16(glyph * 2 + 2)) * 2;
    }
    if (end <= start)
        return true; // empty glyph (space)
    if (end > glyf.size)
        return false;
    const Reader g = glyf.Sub(start);
    const i16 numContours = g.I16(0);

    if (numContours < 0) {
        // Composite glyph.
        u32 o = 10;
        for (;;) {
            const u16 flags = g.U16(o), component = g.U16(o + 2);
            o += 4;
            f32 dx, dy;
            if (flags & 0x0001) { // ARG_1_AND_2_ARE_WORDS
                dx = g.I16(o);
                dy = g.I16(o + 2);
                o += 4;
            } else {
                dx = f32(i8(g.U8(o)));
                dy = f32(i8(g.U8(o + 1)));
                o += 2;
            }
            if (!(flags & 0x0002)) // point matching (rare in practice): no offset
                dx = dy = 0;
            f32 a = 1, b = 0, c = 0, d = 1;
            if (flags & 0x0008) { // WE_HAVE_A_SCALE
                a = d = f32(g.I16(o)) / 16384.0f;
                o += 2;
            } else if (flags & 0x0040) { // X_AND_Y_SCALE
                a = f32(g.I16(o)) / 16384.0f;
                d = f32(g.I16(o + 2)) / 16384.0f;
                o += 4;
            } else if (flags & 0x0080) { // TWO_BY_TWO
                a = f32(g.I16(o)) / 16384.0f;
                b = f32(g.I16(o + 2)) / 16384.0f;
                c = f32(g.I16(o + 4)) / 16384.0f;
                d = f32(g.I16(o + 6)) / 16384.0f;
                o += 8;
            }
            // child -> parent: [a c dx; b d dy], then the parent's transform.
            const f32 local[6] = {a, b, c, d, dx, dy};
            const f32 combined[6] = {m[0] * local[0] + m[2] * local[1], m[1] * local[0] + m[3] * local[1],
                                     m[0] * local[2] + m[2] * local[3], m[1] * local[2] + m[3] * local[3],
                                     m[0] * local[4] + m[2] * local[5] + m[4], m[1] * local[4] + m[3] * local[5] + m[5]};
            if (!AddGlyphOutline(component, combined, out, depth + 1))
                return false;
            if (!(flags & 0x0020)) // MORE_COMPONENTS
                break;
            if (!g.Has(o, 4))
                return false;
        }
        return true;
    }

    // Simple glyph.
    const u32 contours = u32(numContours);
    if (contours == 0)
        return true;
    const u32 endPtsOffset = 10;
    const u32 numPoints = u32(g.U16(endPtsOffset + (contours - 1) * 2)) + 1;
    const u32 instructionLength = g.U16(endPtsOffset + contours * 2);
    u32 o = endPtsOffset + contours * 2 + 2 + instructionLength;
    if (numPoints > 0xFFFF || !g.Has(o, 0))
        return false;

    struct Point
    {
        Vec2 p;
        bool onCurve;
    };
    Array<Point> points;
    points.Resize(numPoints);
    Array<u8> flags;
    flags.Resize(numPoints);
    for (u32 i = 0; i < numPoints;) {
        const u8 f = g.U8(o++);
        flags[i++] = f;
        if (f & 0x08) { // REPEAT
            u32 repeat = g.U8(o++);
            while (repeat-- && i < numPoints)
                flags[i++] = f;
        }
    }
    i32 x = 0;
    for (u32 i = 0; i < numPoints; ++i) {
        const u8 f = flags[i];
        if (f & 0x02) {
            const i32 dx = g.U8(o++);
            x += (f & 0x10) ? dx : -dx;
        } else if (!(f & 0x10)) {
            x += g.I16(o);
            o += 2;
        }
        points[i].p.x = f32(x);
        points[i].onCurve = (f & 0x01) != 0;
    }
    i32 y = 0;
    for (u32 i = 0; i < numPoints; ++i) {
        const u8 f = flags[i];
        if (f & 0x04) {
            const i32 dy = g.U8(o++);
            y += (f & 0x20) ? dy : -dy;
        } else if (!(f & 0x20)) {
            y += g.I16(o);
            o += 2;
        }
        points[i].p.y = f32(y);
    }
    if (o > g.size)
        return false;
    for (Point& pt : points)
        ApplyTransform(m, pt.p);

    // Contours -> quadratic segments (implied on-curve points between consecutive off-curve points).
    u32 first = 0;
    for (u32 c = 0; c < contours; ++c) {
        const u32 last = g.U16(endPtsOffset + c * 2);
        if (last < first || last >= numPoints)
            return false;
        const u32 count = last - first + 1;
        if (count < 2) {
            first = last + 1;
            continue;
        }
        auto at = [&](u32 i) -> const Point& { return points[first + (i % count)]; };
        // Start at an on-curve point (or the midpoint of the first two off-curve points).
        u32 startIndex = 0;
        while (startIndex < count && !at(startIndex).onCurve)
            ++startIndex;
        Vec2 startPoint;
        if (startIndex == count) {
            startPoint = (at(0).p + at(1).p) * 0.5f;
            startIndex = 1;
        } else {
            startPoint = at(startIndex).p;
            ++startIndex;
        }
        Vec2 current = startPoint;
        Vec2 control;
        bool hasControl = false;
        for (u32 k = 0; k < count; ++k) {
            const Point& pt = at(startIndex + k);
            if (pt.onCurve) {
                if (hasControl)
                    out.segments.PushBack({current, control, pt.p, false});
                else
                    out.segments.PushBack({current, (current + pt.p) * 0.5f, pt.p, true});
                current = pt.p;
                hasControl = false;
            } else {
                if (hasControl) {
                    const Vec2 mid = (control + pt.p) * 0.5f;
                    out.segments.PushBack({current, control, mid, false});
                    current = mid;
                }
                control = pt.p;
                hasControl = true;
            }
        }
        // Close the contour.
        if (hasControl)
            out.segments.PushBack({current, control, startPoint, false});
        else if (current != startPoint)
            out.segments.PushBack({current, (current + startPoint) * 0.5f, startPoint, true});
        first = last + 1;
    }
    return true;
}

} // namespace zx::ui
