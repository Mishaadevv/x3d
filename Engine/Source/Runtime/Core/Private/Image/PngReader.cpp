// PNG decoder: all color types, bit depths 1-16, Adam7 interlacing, tRNS transparency. Output: RGBA8 (8-bit
// sources and palettes) or RGBA16 (16-bit sources).

#include "Core/Compression/Compression.h"
#include "Core/Hash/Hash.h"
#include "Core/Image/Image.h"

namespace zx {

namespace {

u32 ReadU32BE(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

u8 PaethPredictor(i32 a, i32 b, i32 c)
{
    const i32 p = a + b - c;
    const i32 pa = p > a ? p - a : a - p;
    const i32 pb = p > b ? p - b : b - p;
    const i32 pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc)
        return u8(a);
    return pb <= pc ? u8(b) : u8(c);
}

/// Reverses PNG filtering in place for one (sub)image. `bpp` = bytes per complete pixel (>= 1).
bool Unfilter(u8* data, usize rowBytes, u32 rows, u32 bpp, u8* outRows)
{
    // data: rows of (1 filter byte + rowBytes); outRows: rows * rowBytes.
    for (u32 y = 0; y < rows; ++y) {
        const u8 filter = data[usize(y) * (rowBytes + 1)];
        const u8* src = data + usize(y) * (rowBytes + 1) + 1;
        u8* dst = outRows + usize(y) * rowBytes;
        const u8* prev = y > 0 ? outRows + usize(y - 1) * rowBytes : nullptr;
        for (usize i = 0; i < rowBytes; ++i) {
            const i32 a = i >= bpp ? dst[i - bpp] : 0;
            const i32 b = prev ? prev[i] : 0;
            const i32 c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            switch (filter) {
            case 0: dst[i] = src[i]; break;
            case 1: dst[i] = u8(src[i] + a); break;
            case 2: dst[i] = u8(src[i] + b); break;
            case 3: dst[i] = u8(src[i] + ((a + b) >> 1)); break;
            case 4: dst[i] = u8(src[i] + PaethPredictor(a, b, c)); break;
            default: return false;
            }
        }
    }
    return true;
}

struct PngInfo
{
    u32 width = 0, height = 0;
    u8 bitDepth = 0, colorType = 0, interlace = 0;
    u8 palette[256][4];
    u32 paletteSize = 0;
    bool hasTrnsKey = false;
    u16 trnsKey[3] = {};
};

u32 Channels(u8 colorType)
{
    switch (colorType) {
    case 0: return 1; // gray
    case 2: return 3; // RGB
    case 3: return 1; // palette
    case 4: return 2; // gray + alpha
    case 6: return 4; // RGBA
    default: return 0;
    }
}

/// Reads sample `index` of a row with the given bit depth.
u16 Sample(const u8* row, usize index, u32 bitDepth)
{
    switch (bitDepth) {
    case 16: return u16((row[index * 2] << 8) | row[index * 2 + 1]);
    case 8: return row[index];
    default: {
        const usize bit = index * bitDepth;
        const u32 shift = 8 - bitDepth - u32(bit & 7);
        return u16((row[bit >> 3] >> shift) & ((1u << bitDepth) - 1));
    }
    }
}

/// Converts one unfiltered (sub)image row to output pixels at (x0 + i*dx, y).
void ExpandRow(const PngInfo& info, const u8* row, u32 pixelCount, Image& out, u32 y, u32 x0, u32 dx)
{
    const u32 ch = Channels(info.colorType);
    const bool wide = info.bitDepth == 16;
    const u32 maxValue = (1u << info.bitDepth) - 1;
    for (u32 i = 0; i < pixelCount; ++i) {
        u16 s[4] = {};
        for (u32 c = 0; c < ch; ++c)
            s[c] = Sample(row, usize(i) * ch + c, info.bitDepth);
        u32 r, g, b, a;
        u32 full = wide ? 65535 : 255;
        switch (info.colorType) {
        case 0: { // grayscale: scale to full range
            const u32 v = wide ? s[0] : (info.bitDepth == 8 ? s[0] : s[0] * 255 / maxValue);
            r = g = b = v;
            a = (info.hasTrnsKey && s[0] == info.trnsKey[0]) ? 0 : full;
            break;
        }
        case 2:
            r = s[0];
            g = s[1];
            b = s[2];
            a = (info.hasTrnsKey && s[0] == info.trnsKey[0] && s[1] == info.trnsKey[1] && s[2] == info.trnsKey[2]) ? 0 : full;
            break;
        case 3: {
            const u8* p = info.palette[s[0] < info.paletteSize ? s[0] : 0];
            r = p[0];
            g = p[1];
            b = p[2];
            a = p[3];
            full = 255;
            break;
        }
        case 4:
            r = g = b = s[0];
            a = s[1];
            break;
        default:
            r = s[0];
            g = s[1];
            b = s[2];
            a = s[3];
            break;
        }
        const u32 x = x0 + i * dx;
        if (out.format == ImageFormat::RGBA16) {
            u16* px = reinterpret_cast<u16*>(out.Row(y)) + usize(x) * 4;
            px[0] = u16(r);
            px[1] = u16(g);
            px[2] = u16(b);
            px[3] = u16(a);
        } else {
            u8* px = out.Row(y) + usize(x) * 4;
            px[0] = u8(r);
            px[1] = u8(g);
            px[2] = u8(b);
            px[3] = u8(a);
        }
    }
}

} // namespace

Result<Image> DecodePng(ByteSpan d)
{
    static const u8 signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (d.Size() < 8 || MemCompare(d.Data(), signature, 8) != 0)
        return Error::Make(ErrorCode::InvalidData, "PNG: bad signature");
    PngInfo info;
    Array<u8> idat;
    bool sawHeader = false, sawEnd = false;
    usize pos = 8;
    while (pos + 12 <= d.Size() && !sawEnd) {
        const u32 length = ReadU32BE(d.Data() + pos);
        const u8* type = d.Data() + pos + 4;
        if (length > d.Size() - pos - 12)
            return Error::Make(ErrorCode::InvalidData, "PNG: chunk exceeds file size");
        const u8* data = type + 4;
        const u32 crc = ReadU32BE(data + length);
        if (Crc32(type, length + 4) != crc)
            return Error::Make(ErrorCode::InvalidData, "PNG: CRC mismatch in chunk {}", StringView(reinterpret_cast<const char*>(type), 4));
        const StringView name(reinterpret_cast<const char*>(type), 4);
        if (name == "IHDR") {
            if (length != 13)
                return Error::Make(ErrorCode::InvalidData, "PNG: bad IHDR");
            info.width = ReadU32BE(data);
            info.height = ReadU32BE(data + 4);
            info.bitDepth = data[8];
            info.colorType = data[9];
            info.interlace = data[12];
            if (data[10] != 0 || data[11] != 0 || info.interlace > 1 || Channels(info.colorType) == 0)
                return Error::Make(ErrorCode::Unsupported, "PNG: unsupported compression/filter/color type");
            const u8 bd = info.bitDepth;
            const bool okDepth = (bd == 8 || bd == 16) || ((bd == 1 || bd == 2 || bd == 4) && (info.colorType == 0 || info.colorType == 3)) ||
                                 (bd == 8 && info.colorType == 3);
            if (!okDepth || info.width == 0 || info.height == 0 || info.width > 32768 || info.height > 32768)
                return Error::Make(ErrorCode::InvalidData, "PNG: invalid size or bit depth");
            sawHeader = true;
        } else if (name == "PLTE") {
            info.paletteSize = length / 3;
            if (info.paletteSize > 256 || length % 3)
                return Error::Make(ErrorCode::InvalidData, "PNG: bad palette");
            for (u32 i = 0; i < info.paletteSize; ++i) {
                info.palette[i][0] = data[i * 3];
                info.palette[i][1] = data[i * 3 + 1];
                info.palette[i][2] = data[i * 3 + 2];
                info.palette[i][3] = 255;
            }
        } else if (name == "tRNS") {
            if (info.colorType == 3) {
                for (u32 i = 0; i < length && i < 256; ++i)
                    info.palette[i][3] = data[i];
            } else if (info.colorType == 0 && length >= 2) {
                info.hasTrnsKey = true;
                info.trnsKey[0] = u16((data[0] << 8) | data[1]);
            } else if (info.colorType == 2 && length >= 6) {
                info.hasTrnsKey = true;
                for (u32 c = 0; c < 3; ++c)
                    info.trnsKey[c] = u16((data[c * 2] << 8) | data[c * 2 + 1]);
            }
        } else if (name == "IDAT") {
            idat.Append(Span<const u8>(data, length));
        } else if (name == "IEND") {
            sawEnd = true;
        } else if (!(type[0] & 0x20)) {
            return Error::Make(ErrorCode::Unsupported, "PNG: unknown critical chunk {}", name);
        }
        pos += 12 + length;
    }
    if (!sawHeader || idat.Empty())
        return Error::Make(ErrorCode::InvalidData, "PNG: missing IHDR or IDAT");
    if (info.colorType == 3 && info.paletteSize == 0)
        return Error::Make(ErrorCode::InvalidData, "PNG: palette image without PLTE");

    const u32 ch = Channels(info.colorType);
    const u32 bitsPerPixel = ch * info.bitDepth;
    const u32 bpp = Max(1u, bitsPerPixel / 8);
    auto rowBytesFor = [&](u32 w) { return (usize(w) * bitsPerPixel + 7) / 8; };

    // Expected decompressed size.
    usize expected = 0;
    static const u32 adam7[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    if (info.interlace) {
        for (const auto& p : adam7) {
            const u32 pw = (info.width + p[2] - 1 - p[0]) / p[2];
            const u32 ph = (info.height + p[3] - 1 - p[1]) / p[3];
            if (pw && ph)
                expected += (rowBytesFor(pw) + 1) * ph;
        }
    } else {
        expected = (rowBytesFor(info.width) + 1) * info.height;
    }
    Array<u8> raw;
    if (!compression::ZlibDecompress(idat.View(), raw, expected))
        return Error::Make(ErrorCode::InvalidData, "PNG: corrupt image data");
    if (raw.Size() < expected)
        return Error::Make(ErrorCode::InvalidData, "PNG: image data too short");

    Image img = Image::Create(info.width, info.height, info.bitDepth == 16 ? ImageFormat::RGBA16 : ImageFormat::RGBA8);
    Array<u8> rows;
    usize offset = 0;
    auto decodePass = [&](u32 pw, u32 ph, u32 x0, u32 y0, u32 dx, u32 dy) -> bool {
        const usize rb = rowBytesFor(pw);
        rows.ResizeUninitialized(rb * ph);
        if (!Unfilter(raw.Data() + offset, rb, ph, bpp, rows.Data()))
            return false;
        for (u32 y = 0; y < ph; ++y)
            ExpandRow(info, rows.Data() + usize(y) * rb, pw, img, y0 + y * dy, x0, dx);
        offset += (rb + 1) * ph;
        return true;
    };
    if (info.interlace) {
        for (const auto& p : adam7) {
            const u32 pw = (info.width + p[2] - 1 - p[0]) / p[2];
            const u32 ph = (info.height + p[3] - 1 - p[1]) / p[3];
            if (pw && ph && !decodePass(pw, ph, p[0], p[1], p[2], p[3]))
                return Error::Make(ErrorCode::InvalidData, "PNG: bad filter type");
        }
    } else if (!decodePass(info.width, info.height, 0, 0, 1, 1)) {
        return Error::Make(ErrorCode::InvalidData, "PNG: bad filter type");
    }
    return img;
}

} // namespace zx
