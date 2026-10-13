// OpenEXR decoder (own implementation of the format): single-part scanline and tiled images (level 0 of mip/rip
// chains), compression NONE / RLE / ZIPS / ZIP / PIZ / PXR24, HALF / FLOAT / UINT channels, any data window.
// Output: RGBA32F from R/G/B(/A) or Y (a layer such as "beauty.R" is used when there are no plain channels).

#include "Core/Compression/Compression.h"
#include "Core/Image/Exr.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Math/Packing.h"

#include <atomic>

namespace zx {

namespace {

enum ExrPixelType : u32
{
    PixelUint = 0,
    PixelHalf = 1,
    PixelFloat = 2,
};

enum ExrCompression : u8
{
    CompNone = 0,
    CompRle = 1,
    CompZips = 2,
    CompZip = 3,
    CompPiz = 4,
    CompPxr24 = 5,
    CompB44 = 6,
    CompB44A = 7,
    CompDwaa = 8,
    CompDwab = 9,
};

struct ExrChannel
{
    String name;
    u32 type = PixelHalf;
    i32 xSampling = 1;
    i32 ySampling = 1;
    i32 output = -1; // RGBA component it feeds (-1 = ignored)
    bool luminance = false;
};

u32 PixelBytes(u32 type) { return type == PixelHalf ? 2 : 4; }

u32 LinesPerBlock(u8 compression)
{
    switch (compression) {
    case CompZip:
    case CompPxr24: return 16;
    case CompPiz:
    case CompB44:
    case CompB44A:
    case CompDwaa: return 32;
    case CompDwab: return 256;
    default: return 1;
    }
}

i32 FloorDiv(i32 a, i32 b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
/// Number of samples of a channel with sampling `s` in [a, b].
i32 NumSamples(i32 s, i32 a, i32 b) { return FloorDiv(b, s) - FloorDiv(a - 1, s); }
bool HasSample(i32 v, i32 s) { return v - FloorDiv(v, s) * s == 0; }

u32 ReadU32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
u64 ReadU64(const u8* p) { return u64(ReadU32(p)) | (u64(ReadU32(p + 4)) << 32); }

/// One block (scanline group or tile) in uncompressed "line by line, channel by channel" layout.
struct BlockLayout
{
    i32 x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    usize bytes = 0;
};

usize BlockBytes(Span<const ExrChannel> channels, i32 x0, i32 x1, i32 y0, i32 y1)
{
    usize n = 0;
    for (i32 y = y0; y <= y1; ++y)
        for (const ExrChannel& c : channels)
            if (HasSample(y, c.ySampling))
                n += usize(NumSamples(c.xSampling, x0, x1)) * PixelBytes(c.type);
    return n;
}

// --- ZIP / RLE: byte predictor + interleaving ------------------------------------------------------------------------

void UndoPredictorAndInterleave(const Array<u8>& tmp, u8* out, usize size)
{
    Array<u8> t = tmp;
    for (usize i = 1; i < size; ++i)
        t[i] = u8(i32(t[i - 1]) + i32(t[i]) - 128);
    const u8* t1 = t.Data();
    const u8* t2 = t.Data() + (size + 1) / 2;
    for (usize i = 0; i < size; ++i)
        out[i] = (i & 1) ? *t2++ : *t1++;
}

bool RleDecompress(ByteSpan in, Array<u8>& out, usize expected)
{
    out.Clear();
    out.Reserve(expected);
    usize i = 0;
    while (i < in.Size()) {
        const i8 count = i8(in[i++]);
        if (count < 0) {
            const usize n = usize(-i32(count));
            if (i + n > in.Size() || out.Size() + n > expected)
                return false;
            out.Append(Span<const u8>(in.Data() + i, n));
            i += n;
        } else {
            const usize n = usize(count) + 1;
            if (i >= in.Size() || out.Size() + n > expected)
                return false;
            const u8 v = in[i++];
            for (usize k = 0; k < n; ++k)
                out.PushBack(v);
        }
    }
    return out.Size() == expected;
}

// --- PIZ: Huffman + Haar wavelet ---------------------------------------------------------------------------------------

constexpr u32 HufEncBits = 16;
constexpr u32 HufEncSize = (1u << HufEncBits) + 1;
constexpr u32 HufTableBits = 14;
constexpr u32 ShortZeroRun = 59, LongZeroRun = 63;
constexpr u32 ShortestLongRun = 2 + LongZeroRun - ShortZeroRun;

struct BitStream
{
    const u8* data = nullptr;
    usize size = 0;

    /// n (<= 58) bits starting at bit `pos` (MSB first), zero-padded past the end.
    u64 Peek(u64 pos, u32 n) const
    {
        const usize byte = usize(pos >> 3);
        const u32 off = u32(pos & 7);
        u64 v = 0;
        for (usize i = 0; i < 8; ++i)
            v = (v << 8) | (byte + i < size ? data[byte + i] : 0);
        if (off + n <= 64)
            return (v << off) >> (64 - n);
        const u8 extra = byte + 8 < size ? data[byte + 8] : 0;
        v = (v << off) | (u64(extra) >> (8 - off));
        return v >> (64 - n);
    }
};

bool HufUncompress(const u8* data, usize size, u16* out, usize outCount)
{
    if (size == 0)
        return outCount == 0;
    if (size < 20)
        return false;
    const u32 im = ReadU32(data), iM = ReadU32(data + 4), nBits = ReadU32(data + 12);
    if (im >= HufEncSize || iM >= HufEncSize || im > iM)
        return false;
    // Code lengths: 6 bits each, with runs of zero lengths.
    Array<u8> lengths;
    lengths.ResizeZeroed(HufEncSize);
    BitStream table{data + 20, size - 20};
    u64 pos = 0;
    const u64 tableBits = u64(size - 20) * 8;
    for (u32 s = im; s <= iM; ++s) {
        if (pos + 6 > tableBits)
            return false;
        const u32 l = u32(table.Peek(pos, 6));
        pos += 6;
        if (l == LongZeroRun) {
            if (pos + 8 > tableBits)
                return false;
            const u32 run = u32(table.Peek(pos, 8)) + ShortestLongRun;
            pos += 8;
            if (s + run > iM + 1)
                return false;
            s += run - 1; // lengths stay 0
        } else if (l >= ShortZeroRun) {
            const u32 run = l - ShortZeroRun + 2;
            if (s + run > iM + 1)
                return false;
            s += run - 1;
        } else {
            lengths[s] = u8(l);
        }
    }
    const usize tableBytes = usize((pos + 7) / 8);
    const u8* stream = data + 20 + tableBytes;
    const usize streamSize = size - 20 - tableBytes;
    if (u64(nBits) > u64(streamSize) * 8)
        return false;

    // Canonical codes (OpenEXR assignment: longest codes get the smallest values).
    u64 counts[59] = {};
    for (u32 s = im; s <= iM; ++s)
        counts[lengths[s]]++;
    u64 c = 0;
    for (i32 l = 58; l > 0; --l) {
        const u64 nc = (c + counts[l]) >> 1;
        counts[l] = c;
        c = nc;
    }
    // Short codes: a 14-bit lookup table. Long codes: codes of one length are consecutive (assigned in symbol order),
    // so a length-ordered range check finds them (a prefix code never matches a shorter range first).
    u64 firstCode[59], lengthCount[59] = {};
    u32 firstIndex[59] = {};
    for (u32 l = 0; l <= 58; ++l)
        firstCode[l] = counts[l];
    Array<u32> longSymbols;
    Array<u32> fast; // symbol << 8 | length (0 = no short code)
    fast.ResizeZeroed(1u << HufTableBits);
    {
        u32 offset = 0;
        for (u32 s = im; s <= iM; ++s)
            if (lengths[s] > HufTableBits)
                ++lengthCount[lengths[s]];
        for (u32 l = HufTableBits + 1; l <= 58; ++l) {
            firstIndex[l] = offset;
            offset += u32(lengthCount[l]);
        }
        longSymbols.ResizeZeroed(offset);
    }
    u32 longFill[59] = {};
    for (u32 s = im; s <= iM; ++s) {
        const u32 l = lengths[s];
        if (l == 0)
            continue;
        const u64 code = counts[l]++;
        if (code >> l)
            return false; // invalid table
        if (l <= HufTableBits) {
            const u32 first = u32(code << (HufTableBits - l));
            const u32 n = 1u << (HufTableBits - l);
            for (u32 k = 0; k < n; ++k) {
                if (fast[first + k])
                    return false; // not prefix-free
                fast[first + k] = (s << 8) | l;
            }
        } else {
            longSymbols[firstIndex[l] + longFill[l]++] = s;
        }
    }

    BitStream bits{stream, streamSize};
    u64 p = 0;
    usize n = 0;
    while (p < nBits) {
        u32 symbol = 0, length = 0;
        const u32 entry = fast[u32(bits.Peek(p, HufTableBits))];
        if (entry) {
            symbol = entry >> 8;
            length = entry & 0xFF;
        } else {
            const u64 v = bits.Peek(p, 58);
            for (u32 l = HufTableBits + 1; l <= 58; ++l) {
                if (!lengthCount[l])
                    continue;
                const u64 code = v >> (58 - l);
                if (code - firstCode[l] < lengthCount[l]) {
                    symbol = longSymbols[firstIndex[l] + u32(code - firstCode[l])];
                    length = l;
                    break;
                }
            }
            if (length == 0)
                return false;
        }
        if (p + length > nBits)
            return false;
        p += length;
        if (symbol == iM) {
            // Run-length code: repeat the previous value (8-bit count).
            if (p + 8 > nBits || n == 0)
                return false;
            const u32 run = u32(bits.Peek(p, 8));
            p += 8;
            if (n + run > outCount)
                return false;
            const u16 v = out[n - 1];
            for (u32 k = 0; k < run; ++k)
                out[n++] = v;
        } else {
            if (n >= outCount)
                return false;
            out[n++] = u16(symbol);
        }
    }
    return n == outCount;
}

inline void Wdec14(u16 l, u16 h, u16& a, u16& b)
{
    const i16 ls = i16(l);
    const i32 hi = i16(h);
    const i32 ai = ls + (hi & 1) + (hi >> 1);
    a = u16(i16(ai));
    b = u16(i16(ai - hi));
}

inline void Wdec16(u16 l, u16 h, u16& a, u16& b)
{
    constexpr i32 AOffset = 1 << 15, ModMask = (1 << 16) - 1;
    const i32 m = l, d = h;
    const i32 bb = (m - (d >> 1)) & ModMask;
    const i32 aa = (d + bb - AOffset) & ModMask;
    b = u16(bb);
    a = u16(aa);
}

/// Inverse 2D Haar wavelet (OpenEXR wav2Decode). ox/oy = element/row strides in u16 units.
void Wav2Decode(u16* in, i32 nx, i32 ox, i32 ny, i32 oy, u16 mx)
{
    const bool w14 = mx < (1 << 14);
    const i32 n = nx > ny ? ny : nx;
    i32 p = 1;
    while (p <= n)
        p <<= 1;
    p >>= 1;
    i32 p2 = p;
    p >>= 1;
    auto dec = [w14](u16 l, u16 h, u16& a, u16& b) {
        if (w14)
            Wdec14(l, h, a, b);
        else
            Wdec16(l, h, a, b);
    };
    while (p >= 1) {
        u16* py = in;
        u16* ey = in + isize(oy) * (ny - p2);
        const isize oy1 = isize(oy) * p, oy2 = isize(oy) * p2, ox1 = isize(ox) * p, ox2 = isize(ox) * p2;
        u16 i00, i01, i10, i11;
        for (; py <= ey; py += oy2) {
            u16* px = py;
            u16* ex = py + isize(ox) * (nx - p2);
            for (; px <= ex; px += ox2) {
                u16* p01 = px + ox1;
                u16* p10 = px + oy1;
                u16* p11 = p10 + ox1;
                dec(*px, *p10, i00, i10);
                dec(*p01, *p11, i01, i11);
                dec(i00, i01, *px, *p01);
                dec(i10, i11, *p10, *p11);
            }
            if (nx & p) {
                u16* p10 = px + oy1;
                dec(*px, *p10, i00, *p10);
                *px = i00;
            }
        }
        if (ny & p) {
            u16* px = py;
            u16* ex = py + isize(ox) * (nx - p2);
            for (; px <= ex; px += ox2) {
                u16* p01 = px + ox1;
                dec(*px, *p01, i00, *p01);
                *px = i00;
            }
        }
        p2 = p;
        p >>= 1;
    }
}

bool PizDecompress(ByteSpan in, Span<const ExrChannel> channels, const BlockLayout& b, u8* out)
{
    constexpr u32 BitmapSize = 8192;
    if (in.Size() < 4)
        return false;
    const u16 minNonZero = u16(in[0] | (in[1] << 8)), maxNonZero = u16(in[2] | (in[3] << 8));
    usize pos = 4;
    Array<u8> bitmap;
    bitmap.ResizeZeroed(BitmapSize);
    if (minNonZero <= maxNonZero) {
        if (maxNonZero >= BitmapSize || pos + (maxNonZero - minNonZero + 1) > in.Size())
            return false;
        MemCopy(bitmap.Data() + minNonZero, in.Data() + pos, usize(maxNonZero - minNonZero + 1));
        pos += usize(maxNonZero - minNonZero + 1);
    }
    Array<u16> lut;
    lut.ResizeZeroed(65536);
    u32 k = 0;
    for (u32 i = 0; i < 65536; ++i)
        if (i == 0 || (bitmap[i >> 3] & (1u << (i & 7))))
            lut[k++] = u16(i);
    const u16 maxValue = u16(k - 1);
    if (pos + 4 > in.Size())
        return false;
    const u32 length = ReadU32(in.Data() + pos);
    pos += 4;
    if (pos + length > in.Size())
        return false;

    struct Plane
    {
        usize start;
        i32 nx, ny, ys, size;
    };
    Array<Plane> planes;
    usize words = 0;
    for (const ExrChannel& c : channels) {
        Plane pl;
        pl.start = words;
        pl.nx = NumSamples(c.xSampling, b.x0, b.x1);
        pl.ny = NumSamples(c.ySampling, b.y0, b.y1);
        pl.ys = c.ySampling;
        pl.size = i32(PixelBytes(c.type) / 2);
        words += usize(pl.nx) * usize(pl.ny) * usize(pl.size);
        planes.PushBack(pl);
    }
    if (words * 2 != b.bytes)
        return false;
    Array<u16> tmp;
    tmp.ResizeZeroed(words);
    if (!HufUncompress(in.Data() + pos, length, tmp.Data(), words))
        return false;
    for (const Plane& pl : planes)
        for (i32 j = 0; j < pl.size; ++j)
            if (pl.nx > 0 && pl.ny > 0)
                Wav2Decode(tmp.Data() + pl.start + j, pl.nx, pl.size, pl.ny, pl.nx * pl.size, maxValue);
    for (u16& v : tmp)
        v = lut[v];
    // Channel planes -> line by line, channel by channel.
    usize o = 0;
    Array<usize> cursor;
    for (const Plane& pl : planes)
        cursor.PushBack(pl.start);
    for (i32 y = b.y0; y <= b.y1; ++y)
        for (usize c = 0; c < planes.Size(); ++c) {
            const Plane& pl = planes[c];
            if (!HasSample(y, pl.ys))
                continue;
            const usize n = usize(pl.nx) * usize(pl.size);
            for (usize i = 0; i < n; ++i) {
                const u16 v = tmp[cursor[c] + i];
                out[o++] = u8(v);
                out[o++] = u8(v >> 8);
            }
            cursor[c] += n;
        }
    return o == b.bytes;
}

// --- PXR24: zlib + per-channel byte planes of differences ------------------------------------------------------------

bool Pxr24Decompress(ByteSpan in, Span<const ExrChannel> channels, const BlockLayout& b, u8* out)
{
    // Planes: HALF 2 bytes, FLOAT 3 (24-bit), UINT 4 per sample.
    usize planeBytes = 0;
    for (i32 y = b.y0; y <= b.y1; ++y)
        for (const ExrChannel& c : channels)
            if (HasSample(y, c.ySampling))
                planeBytes += usize(NumSamples(c.xSampling, b.x0, b.x1)) * (c.type == PixelFloat ? 3 : PixelBytes(c.type));
    Array<u8> tmp;
    if (!compression::ZlibDecompress(in, tmp, planeBytes) || tmp.Size() != planeBytes)
        return false;
    const u8* t = tmp.Data();
    usize o = 0;
    for (i32 y = b.y0; y <= b.y1; ++y)
        for (const ExrChannel& c : channels) {
            if (!HasSample(y, c.ySampling))
                continue;
            const usize n = usize(NumSamples(c.xSampling, b.x0, b.x1));
            u32 pixel = 0;
            switch (c.type) {
            case PixelUint:
                for (usize j = 0; j < n; ++j) {
                    pixel += (u32(t[j]) << 24) | (u32(t[n + j]) << 16) | (u32(t[2 * n + j]) << 8) | u32(t[3 * n + j]);
                    for (u32 k = 0; k < 4; ++k)
                        out[o++] = u8(pixel >> (8 * k));
                }
                t += 4 * n;
                break;
            case PixelHalf:
                for (usize j = 0; j < n; ++j) {
                    pixel += (u32(t[j]) << 8) | u32(t[n + j]);
                    out[o++] = u8(pixel);
                    out[o++] = u8(pixel >> 8);
                }
                t += 2 * n;
                break;
            default:
                for (usize j = 0; j < n; ++j) {
                    pixel += (u32(t[j]) << 24) | (u32(t[n + j]) << 16) | (u32(t[2 * n + j]) << 8);
                    for (u32 k = 0; k < 4; ++k)
                        out[o++] = u8(pixel >> (8 * k));
                }
                t += 3 * n;
                break;
            }
        }
    return o == b.bytes;
}

// --- Header -------------------------------------------------------------------------------------------------------------

struct ExrHeader
{
    Array<ExrChannel> channels;
    u8 compression = CompNone;
    i32 xMin = 0, yMin = 0, xMax = -1, yMax = -1;
    bool tiled = false;
    u32 tileW = 0, tileH = 0;
    u8 levelMode = 0;
    usize headerEnd = 0;
};

const char* CompressionName(u8 c)
{
    switch (c) {
    case CompB44: return "B44";
    case CompB44A: return "B44A";
    case CompDwaa: return "DWAA";
    case CompDwab: return "DWAB";
    default: return "unknown";
    }
}

Result<ExrHeader> ParseHeader(ByteSpan d)
{
    if (d.Size() < 8 || ReadU32(d.Data()) != 20000630u)
        return Error::Make(ErrorCode::InvalidData, "EXR: bad magic");
    const u32 version = ReadU32(d.Data() + 4);
    if ((version & 0xFF) != 2)
        return Error::Make(ErrorCode::Unsupported, "EXR: unsupported version {}", version & 0xFF);
    if (version & 0x800)
        return Error::Make(ErrorCode::Unsupported, "EXR: deep data is not supported");
    if (version & 0x1000)
        return Error::Make(ErrorCode::Unsupported, "EXR: multi-part files are not supported");
    ExrHeader h;
    h.tiled = (version & 0x200) != 0;
    usize p = 8;
    auto readString = [&](String& out) {
        const usize start = p;
        while (p < d.Size() && d[p] != 0)
            ++p;
        if (p >= d.Size() || p - start > 255)
            return false;
        out = String(StringView(reinterpret_cast<const char*>(d.Data() + start), p - start));
        ++p;
        return true;
    };
    bool hasChannels = false, hasWindow = false;
    for (;;) {
        String name, type;
        if (!readString(name))
            return Error::Make(ErrorCode::InvalidData, "EXR: truncated header");
        if (name.Empty())
            break;
        if (!readString(type) || p + 4 > d.Size())
            return Error::Make(ErrorCode::InvalidData, "EXR: truncated header");
        const u32 size = ReadU32(d.Data() + p);
        p += 4;
        if (p + size > d.Size())
            return Error::Make(ErrorCode::InvalidData, "EXR: truncated attribute '{}'", name);
        const u8* v = d.Data() + p;
        if (name == "channels" && type == "chlist") {
            usize q = 0;
            while (q < size && v[q] != 0) {
                ExrChannel c;
                const usize start = q;
                while (q < size && v[q] != 0)
                    ++q;
                if (q + 17 > size)
                    return Error::Make(ErrorCode::InvalidData, "EXR: bad channel list");
                c.name = String(StringView(reinterpret_cast<const char*>(v + start), q - start));
                ++q;
                c.type = ReadU32(v + q);
                c.xSampling = i32(ReadU32(v + q + 8));
                c.ySampling = i32(ReadU32(v + q + 12));
                q += 16;
                if (c.type > PixelFloat || c.xSampling < 1 || c.ySampling < 1)
                    return Error::Make(ErrorCode::InvalidData, "EXR: bad channel '{}'", c.name);
                h.channels.PushBack(move(c));
            }
            hasChannels = true;
        } else if (name == "compression" && size >= 1) {
            h.compression = v[0];
        } else if (name == "dataWindow" && size >= 16) {
            h.xMin = i32(ReadU32(v));
            h.yMin = i32(ReadU32(v + 4));
            h.xMax = i32(ReadU32(v + 8));
            h.yMax = i32(ReadU32(v + 12));
            hasWindow = true;
        } else if (name == "tiles" && size >= 9) {
            h.tileW = ReadU32(v);
            h.tileH = ReadU32(v + 4);
            h.levelMode = v[8] & 0x0F;
        } else if (name == "type") {
            const StringView t(reinterpret_cast<const char*>(v), size);
            if (t.StartsWith("deep"))
                return Error::Make(ErrorCode::Unsupported, "EXR: deep data is not supported");
        }
        p += size;
    }
    h.headerEnd = p;
    if (!hasChannels || !hasWindow || h.channels.Empty())
        return Error::Make(ErrorCode::InvalidData, "EXR: missing channels or dataWindow");
    if (h.xMax < h.xMin || h.yMax < h.yMin || i64(h.xMax) - h.xMin >= 65536 || i64(h.yMax) - h.yMin >= 65536)
        return Error::Make(ErrorCode::InvalidData, "EXR: invalid data window");
    if (h.compression > CompPxr24)
        return Error::Make(ErrorCode::Unsupported, "EXR: {} compression is not supported (use PIZ, ZIP, RLE, PXR24 or none)",
                           CompressionName(h.compression));
    if (h.tiled && (h.tileW == 0 || h.tileH == 0 || h.tileW > 65536 || h.tileH > 65536))
        return Error::Make(ErrorCode::InvalidData, "EXR: invalid tile size");
    return h;
}

/// Picks the channels that make up RGBA: plain R/G/B/A/Y, else the first layer that has an R channel.
bool MapChannels(Array<ExrChannel>& channels)
{
    StringView prefix;
    bool plain = false;
    for (const ExrChannel& c : channels)
        plain |= c.name == "R" || c.name == "G" || c.name == "B" || c.name == "Y";
    if (!plain) {
        for (const ExrChannel& c : channels)
            if (c.name.View().EndsWith(".R") || c.name.View().EndsWith(".Y")) {
                prefix = c.name.View().SubStr(0, c.name.Size() - 1);
                break;
            }
        if (prefix.Empty())
            return false;
    }
    bool any = false;
    for (ExrChannel& c : channels) {
        if (!c.name.View().StartsWith(prefix) || c.name.Size() != prefix.Size() + 1)
            continue;
        const char n = c.name[prefix.Size()];
        c.output = n == 'R' ? 0 : n == 'G' ? 1 : n == 'B' ? 2 : n == 'A' ? 3 : n == 'Y' ? 0 : -1;
        c.luminance = n == 'Y';
        any |= c.output >= 0;
    }
    return any;
}

} // namespace

Result<Image> DecodeExr(ByteSpan d)
{
    Result<ExrHeader> hr = ParseHeader(d);
    if (!hr)
        return hr.GetError();
    ExrHeader& h = *hr;
    if (!MapChannels(h.channels))
        return Error::Make(ErrorCode::Unsupported, "EXR: no R/G/B or Y channels");
    bool hasAlpha = false, hasRgb = false;
    for (const ExrChannel& c : h.channels) {
        if (c.output >= 0 && (c.xSampling != 1 || c.ySampling != 1))
            return Error::Make(ErrorCode::Unsupported, "EXR: subsampled color channels (luminance/chroma) are not supported");
        hasAlpha |= c.output == 3;
        hasRgb |= c.output >= 0 && c.output < 3 && !c.luminance;
    }
    if (hasRgb)
        for (ExrChannel& c : h.channels)
            if (c.luminance)
                c.output = -1; // RGB wins over Y
    const u32 width = u32(h.xMax - h.xMin + 1), height = u32(h.yMax - h.yMin + 1);

    // Chunk list (level 0 only for tiled mip/rip chains: its tiles come first in the offset table).
    struct Chunk
    {
        BlockLayout layout;
        u64 offset;
    };
    Array<Chunk> chunks;
    if (h.tiled) {
        const u32 tx = (width + h.tileW - 1) / h.tileW, ty = (height + h.tileH - 1) / h.tileH;
        chunks.Resize(usize(tx) * ty);
        for (u32 y = 0; y < ty; ++y)
            for (u32 x = 0; x < tx; ++x) {
                BlockLayout& b = chunks[usize(y) * tx + x].layout;
                b.x0 = h.xMin + i32(x * h.tileW);
                b.y0 = h.yMin + i32(y * h.tileH);
                b.x1 = Min(b.x0 + i32(h.tileW) - 1, h.xMax);
                b.y1 = Min(b.y0 + i32(h.tileH) - 1, h.yMax);
            }
    } else {
        const u32 lines = LinesPerBlock(h.compression);
        chunks.Resize((height + lines - 1) / lines);
        for (usize i = 0; i < chunks.Size(); ++i) {
            BlockLayout& b = chunks[i].layout;
            b.x0 = h.xMin;
            b.x1 = h.xMax;
            b.y0 = h.yMin + i32(i * lines);
            b.y1 = Min(b.y0 + i32(lines) - 1, h.yMax);
        }
    }
    if (h.headerEnd + chunks.Size() * 8 > d.Size())
        return Error::Make(ErrorCode::InvalidData, "EXR: truncated offset table");
    // Sanity limits before allocating: 128 M pixels, and no codec expands data more than ~1000x.
    const u64 rawBytes = u64(BlockBytes(h.channels.View(), h.xMin, h.xMax, h.yMin, h.yMin)) * height;
    if (u64(width) * height > (1ull << 27) || rawBytes > u64(d.Size()) * 1100 + 65536)
        return Error::Make(ErrorCode::InvalidData, "EXR: image too large for the file ({} x {})", width, height);
    for (usize i = 0; i < chunks.Size(); ++i) {
        chunks[i].offset = ReadU64(d.Data() + h.headerEnd + i * 8);
        if (chunks[i].offset >= d.Size())
            return Error::Make(ErrorCode::InvalidData, "EXR: bad chunk offset");
        chunks[i].layout.bytes = BlockBytes(h.channels.View(), chunks[i].layout.x0, chunks[i].layout.x1, chunks[i].layout.y0,
                                            chunks[i].layout.y1);
    }

    Image img = Image::Create(width, height, ImageFormat::RGBA32F);
    f32* pixels = reinterpret_cast<f32*>(img.pixels.Data());
    if (!hasAlpha)
        for (usize i = 0; i < usize(width) * height; ++i)
            pixels[i * 4 + 3] = 1.0f;

    std::atomic<u32> failures{0};
    auto decodeChunk = [&](const Chunk& chunk) -> bool {
        const usize headerBytes = h.tiled ? 20 : 8;
        if (chunk.offset + headerBytes > d.Size())
            return false;
        const u8* c = d.Data() + chunk.offset;
        BlockLayout b = chunk.layout;
        if (h.tiled) {
            // The chunk names its tile (level 0 only).
            const u32 tx = ReadU32(c), ty = ReadU32(c + 4);
            if (ReadU32(c + 8) != 0 || ReadU32(c + 12) != 0 || tx >= (width + h.tileW - 1) / h.tileW ||
                ty >= (height + h.tileH - 1) / h.tileH)
                return false;
            b.x0 = h.xMin + i32(tx * h.tileW);
            b.y0 = h.yMin + i32(ty * h.tileH);
            b.x1 = Min(b.x0 + i32(h.tileW) - 1, h.xMax);
            b.y1 = Min(b.y0 + i32(h.tileH) - 1, h.yMax);
            b.bytes = BlockBytes(h.channels.View(), b.x0, b.x1, b.y0, b.y1);
        } else if (i32(ReadU32(c)) != b.y0) {
            return false;
        }
        const u32 packed = ReadU32(c + headerBytes - 4);
        if (chunk.offset + headerBytes + packed > d.Size())
            return false;
        const ByteSpan in(c + headerBytes, packed);
        Array<u8> raw;
        raw.ResizeUninitialized(b.bytes);
        if (packed >= b.bytes || h.compression == CompNone) {
            if (packed < b.bytes)
                return false;
            MemCopy(raw.Data(), in.Data(), b.bytes);
        } else {
            switch (h.compression) {
            case CompRle: {
                Array<u8> tmp;
                if (!RleDecompress(in, tmp, b.bytes))
                    return false;
                UndoPredictorAndInterleave(tmp, raw.Data(), b.bytes);
                break;
            }
            case CompZips:
            case CompZip: {
                Array<u8> tmp;
                if (!compression::ZlibDecompress(in, tmp, b.bytes) || tmp.Size() != b.bytes)
                    return false;
                UndoPredictorAndInterleave(tmp, raw.Data(), b.bytes);
                break;
            }
            case CompPiz:
                if (!PizDecompress(in, h.channels.View(), b, raw.Data()))
                    return false;
                break;
            case CompPxr24:
                if (!Pxr24Decompress(in, h.channels.View(), b, raw.Data()))
                    return false;
                break;
            default: return false;
            }
        }
        // Raw layout -> RGBA32F.
        const u8* r = raw.Data();
        for (i32 y = b.y0; y <= b.y1; ++y)
            for (const ExrChannel& ch : h.channels) {
                if (!HasSample(y, ch.ySampling))
                    continue;
                const i32 n = NumSamples(ch.xSampling, b.x0, b.x1);
                const u32 bytes = PixelBytes(ch.type);
                if (ch.output >= 0) {
                    f32* dst = pixels + (usize(y - h.yMin) * width + usize(b.x0 - h.xMin)) * 4;
                    for (i32 x = 0; x < n; ++x) {
                        const u8* s = r + usize(x) * bytes;
                        f32 v;
                        if (ch.type == PixelHalf)
                            v = HalfToFloat(u16(s[0] | (s[1] << 8)));
                        else if (ch.type == PixelFloat)
                            v = std::bit_cast<f32>(ReadU32(s));
                        else
                            v = f32(ReadU32(s));
                        if (ch.luminance)
                            dst[x * 4] = dst[x * 4 + 1] = dst[x * 4 + 2] = v;
                        else
                            dst[x * 4 + ch.output] = v;
                    }
                }
                r += usize(n) * bytes;
            }
        return true;
    };
    auto run = [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i)
            if (!decodeChunk(chunks[i]))
                failures.fetch_add(1, std::memory_order_relaxed);
    };
    if (jobs::IsInitialized() && chunks.Size() > 4)
        jobs::ParallelFor(u32(chunks.Size()), 4, run);
    else
        run(0, u32(chunks.Size()));
    if (failures.load() != 0)
        return Error::Make(ErrorCode::InvalidData, "EXR: {} corrupt chunk(s)", failures.load());
    return img;
}

// --- Writer -------------------------------------------------------------------------------------------------------------

namespace exr {

Array<u8> Encode(const Image& image, bool compress)
{
    ZX_ASSERT(image.format == ImageFormat::RGBA32F, "EXR encoder expects RGBA32F");
    const bool alpha = image.HasTransparency();
    Array<u8> out;
    auto u8s = [&](StringView s) {
        out.Append(Span<const u8>(reinterpret_cast<const u8*>(s.Data()), s.Size()));
        out.PushBack(0);
    };
    auto u32s = [&](u32 v) {
        for (u32 k = 0; k < 4; ++k)
            out.PushBack(u8(v >> (8 * k)));
    };
    auto attr = [&](StringView name, StringView type, u32 size) {
        u8s(name);
        u8s(type);
        u32s(size);
    };
    u32s(20000630u);
    u32s(2);
    // Channels in alphabetical order: A, B, G, R.
    const char* names[] = {"A", "B", "G", "R"};
    const u32 components[] = {3, 2, 1, 0};
    const u32 first = alpha ? 0 : 1;
    attr("channels", "chlist", (4 - first) * 18 + 1);
    for (u32 c = first; c < 4; ++c) {
        u8s(names[c]);
        u32s(PixelHalf);
        u32s(0); // pLinear + reserved
        u32s(1);
        u32s(1);
    }
    out.PushBack(0);
    attr("compression", "compression", 1);
    out.PushBack(compress ? CompZip : CompNone);
    for (StringView w : {StringView("dataWindow"), StringView("displayWindow")}) {
        attr(w, "box2i", 16);
        u32s(0);
        u32s(0);
        u32s(image.width - 1);
        u32s(image.height - 1);
    }
    attr("lineOrder", "lineOrder", 1);
    out.PushBack(0);
    attr("pixelAspectRatio", "float", 4);
    u32s(std::bit_cast<u32>(1.0f));
    attr("screenWindowCenter", "v2f", 8);
    u32s(0);
    u32s(0);
    attr("screenWindowWidth", "float", 4);
    u32s(std::bit_cast<u32>(1.0f));
    out.PushBack(0);

    const u32 lines = compress ? 16 : 1;
    const u32 chunkCount = (image.height + lines - 1) / lines;
    const usize tableAt = out.Size();
    out.ResizeZeroed(out.Size() + usize(chunkCount) * 8);
    const f32* px = reinterpret_cast<const f32*>(image.pixels.Data());
    Array<u8> raw, tmp;
    for (u32 chunk = 0; chunk < chunkCount; ++chunk) {
        const u32 y0 = chunk * lines, y1 = Min(y0 + lines, image.height);
        raw.Clear();
        for (u32 y = y0; y < y1; ++y)
            for (u32 c = first; c < 4; ++c)
                for (u32 x = 0; x < image.width; ++x) {
                    const u16 hv = FloatToHalf(px[(usize(y) * image.width + x) * 4 + components[c]]);
                    raw.PushBack(u8(hv));
                    raw.PushBack(u8(hv >> 8));
                }
        Array<u8> data;
        if (compress) {
            // Interleave the two halves of the byte stream, then the predictor (inverse of the decoder).
            tmp.ResizeUninitialized(raw.Size());
            const usize half = (raw.Size() + 1) / 2;
            for (usize i = 0; i < raw.Size(); ++i)
                tmp[(i & 1) ? half + i / 2 : i / 2] = raw[i];
            for (usize i = tmp.Size(); i-- > 1;)
                tmp[i] = u8(i32(tmp[i]) - i32(tmp[i - 1]) + 128);
            data = compression::ZlibCompress(tmp.View(), 6);
            if (data.Size() >= raw.Size())
                data = raw;
        } else {
            data = raw;
        }
        const u64 offset = out.Size();
        for (u32 k = 0; k < 8; ++k)
            out[tableAt + usize(chunk) * 8 + k] = u8(offset >> (8 * k));
        u32s(y0);
        u32s(u32(data.Size()));
        out.Append(data.View());
    }
    return out;
}

} // namespace exr

} // namespace zx
