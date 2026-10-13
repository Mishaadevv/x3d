#include "Core/Image/Image.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/String/Parse.h"
#include "Core/String/Path.h"

#include <cmath>

namespace zx {

Image Image::Create(u32 width, u32 height, ImageFormat format)
{
    Image img;
    img.width = width;
    img.height = height;
    img.format = format;
    img.pixels.ResizeZeroed(usize(width) * height * ImageFormatBytesPerPixel(format));
    return img;
}

void Image::ConvertTo(ImageFormat target)
{
    if (target == format)
        return;
    const usize count = usize(width) * height * 4;
    Image out = Create(width, height, target);
    for (usize i = 0; i < count; ++i) {
        f32 v; // normalized value
        switch (format) {
        case ImageFormat::RGBA8: v = f32(pixels[i]) / 255.0f; break;
        case ImageFormat::RGBA16: v = f32(reinterpret_cast<const u16*>(pixels.Data())[i]) / 65535.0f; break;
        default: v = reinterpret_cast<const f32*>(pixels.Data())[i]; break;
        }
        switch (target) {
        case ImageFormat::RGBA8:
            out.pixels[i] = u8((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f + 0.5f);
            break;
        case ImageFormat::RGBA16:
            reinterpret_cast<u16*>(out.pixels.Data())[i] = u16((v < 0 ? 0 : v > 1 ? 1 : v) * 65535.0f + 0.5f);
            break;
        default:
            reinterpret_cast<f32*>(out.pixels.Data())[i] = v;
            break;
        }
    }
    *this = move(out);
}

bool Image::HasTransparency() const
{
    const usize count = usize(width) * height;
    for (usize i = 0; i < count; ++i) {
        switch (format) {
        case ImageFormat::RGBA8:
            if (pixels[i * 4 + 3] != 255)
                return true;
            break;
        case ImageFormat::RGBA16:
            if (reinterpret_cast<const u16*>(pixels.Data())[i * 4 + 3] != 65535)
                return true;
            break;
        default:
            if (reinterpret_cast<const f32*>(pixels.Data())[i * 4 + 3] < 1.0f)
                return true;
            break;
        }
    }
    return false;
}

ImageFileType DetectImageType(ByteSpan d, StringView hint)
{
    if (d.Size() >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
        return ImageFileType::Png;
    if (d.Size() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return ImageFileType::Jpeg;
    if (d.Size() >= 10 && (MemCompare(d.Data(), "#?RADIANCE", 10) == 0 || MemCompare(d.Data(), "#?RGBE", 6) == 0))
        return ImageFileType::Hdr;
    if (d.Size() >= 26 && d[0] == 'B' && d[1] == 'M')
        return ImageFileType::Bmp;
    if (Path::HasExtension(hint, "tga"))
        return ImageFileType::Tga;
    return ImageFileType::Unknown;
}

Result<Image> DecodeImage(ByteSpan data, StringView hint)
{
    switch (DetectImageType(data, hint)) {
    case ImageFileType::Png: return DecodePng(data);
    case ImageFileType::Jpeg: return DecodeJpeg(data);
    case ImageFileType::Tga: return DecodeTga(data);
    case ImageFileType::Bmp: return DecodeBmp(data);
    case ImageFileType::Hdr: return DecodeHdr(data);
    default: return Error::Make(ErrorCode::Unsupported, "Unknown image format '{}'", hint);
    }
}

Result<Image> LoadImageFile(StringView path)
{
    Array<u8> bytes;
    if (!fs::ReadFile(path, bytes))
        return Error::Make(ErrorCode::NotFound, "Image not found: {}", path);
    return DecodeImage(bytes.View(), path);
}

// --- TGA ------------------------------------------------------------------------------------------------------------

Result<Image> DecodeTga(ByteSpan d)
{
    if (d.Size() < 18)
        return Error::Make(ErrorCode::InvalidData, "TGA: truncated header");
    const u8 idLength = d[0];
    const u8 colorMapType = d[1];
    const u8 type = d[2];
    const u32 w = u32(d[12]) | (u32(d[13]) << 8);
    const u32 h = u32(d[14]) | (u32(d[15]) << 8);
    const u8 bpp = d[16];
    const u8 desc = d[17];
    const bool rle = type == 9 || type == 10 || type == 11;
    const u8 baseType = rle ? u8(type - 8) : type;
    if (colorMapType != 0 || (baseType != 2 && baseType != 3))
        return Error::Make(ErrorCode::Unsupported, "TGA: only true-color/grayscale images are supported");
    if (w == 0 || h == 0 || (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32))
        return Error::Make(ErrorCode::InvalidData, "TGA: invalid size or bit depth");
    const u32 bytesPerPixel = bpp / 8;
    usize pos = 18 + idLength;
    Image img = Image::Create(w, h, ImageFormat::RGBA8);
    const bool topDown = (desc & 0x20) != 0;
    auto readPixel = [&](u8* out) -> bool {
        if (pos + bytesPerPixel > d.Size())
            return false;
        const u8* p = d.Data() + pos;
        pos += bytesPerPixel;
        if (bytesPerPixel == 1) {
            out[0] = out[1] = out[2] = p[0];
            out[3] = 255;
        } else if (bytesPerPixel == 2) {
            const u16 v = u16(p[0] | (p[1] << 8));
            out[0] = u8(((v >> 10) & 31) * 255 / 31);
            out[1] = u8(((v >> 5) & 31) * 255 / 31);
            out[2] = u8((v & 31) * 255 / 31);
            out[3] = 255;
        } else {
            out[0] = p[2];
            out[1] = p[1];
            out[2] = p[0];
            out[3] = bytesPerPixel == 4 ? p[3] : 255;
        }
        return true;
    };
    const usize total = usize(w) * h;
    usize i = 0;
    while (i < total) {
        if (!rle) {
            if (!readPixel(&img.pixels[i * 4]))
                return Error::Make(ErrorCode::InvalidData, "TGA: truncated pixel data");
            ++i;
            continue;
        }
        if (pos >= d.Size())
            return Error::Make(ErrorCode::InvalidData, "TGA: truncated RLE data");
        const u8 header = d[pos++];
        const usize count = (header & 0x7F) + 1;
        if (i + count > total)
            return Error::Make(ErrorCode::InvalidData, "TGA: RLE overflow");
        if (header & 0x80) {
            u8 px[4];
            if (!readPixel(px))
                return Error::Make(ErrorCode::InvalidData, "TGA: truncated RLE packet");
            for (usize k = 0; k < count; ++k)
                MemCopy(&img.pixels[(i + k) * 4], px, 4);
        } else {
            for (usize k = 0; k < count; ++k)
                if (!readPixel(&img.pixels[(i + k) * 4]))
                    return Error::Make(ErrorCode::InvalidData, "TGA: truncated raw packet");
        }
        i += count;
    }
    if (!topDown) {
        // Bottom-up storage: flip rows.
        const usize pitch = img.RowPitch();
        Array<u8> tmp;
        tmp.ResizeUninitialized(pitch);
        for (u32 y = 0; y < h / 2; ++y) {
            MemCopy(tmp.Data(), img.Row(y), pitch);
            MemCopy(img.Row(y), img.Row(h - 1 - y), pitch);
            MemCopy(img.Row(h - 1 - y), tmp.Data(), pitch);
        }
    }
    return img;
}

// --- BMP ------------------------------------------------------------------------------------------------------------

namespace {

struct BmpChannel
{
    u32 mask = 0;
    u32 shift = 0;
    u32 max = 0; // (1 << bits) - 1
};

BmpChannel MakeBmpChannel(u32 mask)
{
    BmpChannel c;
    c.mask = mask;
    if (!mask)
        return c;
    while (!((mask >> c.shift) & 1))
        ++c.shift;
    c.max = mask >> c.shift;
    while (c.max & (c.max + 1)) // non-contiguous masks: keep the low contiguous run
        c.max &= c.max >> 1;
    return c;
}

u8 ExtractBmpChannel(const BmpChannel& c, u32 pixel)
{
    if (!c.max)
        return 0;
    const u32 v = (pixel >> c.shift) & c.max;
    return c.max == 255 ? u8(v) : u8((v * 255 + c.max / 2) / c.max);
}

} // namespace

Result<Image> DecodeBmp(ByteSpan d)
{
    auto rd16 = [&](usize o) { return u32(d[o]) | (u32(d[o + 1]) << 8); };
    auto rd32 = [&](usize o) { return rd16(o) | (rd16(o + 2) << 16); };
    if (d.Size() < 26 || d[0] != 'B' || d[1] != 'M')
        return Error::Make(ErrorCode::InvalidData, "BMP: bad magic");
    const u32 dataOffset = rd32(10);
    const u32 headerSize = rd32(14);
    if (headerSize < 12 || 14 + usize(headerSize) > d.Size())
        return Error::Make(ErrorCode::InvalidData, "BMP: truncated header");
    i32 w, h;
    u32 bpp, compression = 0, colorsUsed = 0;
    if (headerSize == 12) { // OS/2 BITMAPCOREHEADER
        w = i32(rd16(18));
        h = i32(i16(rd16(20)));
        bpp = rd16(24);
    } else {
        if (headerSize < 40)
            return Error::Make(ErrorCode::Unsupported, "BMP: unsupported header size {}", headerSize);
        w = i32(rd32(18));
        h = i32(rd32(22));
        bpp = rd16(28);
        compression = rd32(30);
        colorsUsed = rd32(46);
    }
    const bool topDown = h < 0;
    const u32 width = u32(w), height = u32(topDown ? -h : h);
    if (w <= 0 || height == 0 || u64(width) * height > (1ull << 28))
        return Error::Make(ErrorCode::InvalidData, "BMP: invalid size {}x{}", w, h);
    const bool rle = compression == 1 || compression == 2;
    const bool bitfields = compression == 3 || compression == 6;
    if (!(compression == 0 || (compression == 1 && bpp == 8) || (compression == 2 && bpp == 4) || (bitfields && (bpp == 16 || bpp == 32))))
        return Error::Make(ErrorCode::Unsupported, "BMP: unsupported compression {} at {} bpp", compression, bpp);
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return Error::Make(ErrorCode::Unsupported, "BMP: unsupported bit depth {}", bpp);

    // Channel masks (V3 BITFIELDS store them right after the 40-byte header, V4/V5 inside it - same offset).
    BmpChannel r, g, b, a;
    if (bitfields) {
        if (14 + 40 + 12 > d.Size())
            return Error::Make(ErrorCode::InvalidData, "BMP: truncated bitfield masks");
        r = MakeBmpChannel(rd32(54));
        g = MakeBmpChannel(rd32(58));
        b = MakeBmpChannel(rd32(62));
        if ((headerSize >= 56 || compression == 6) && 14 + 40 + 16 <= d.Size())
            a = MakeBmpChannel(rd32(66));
    } else if (bpp == 16) {
        r = MakeBmpChannel(0x7C00);
        g = MakeBmpChannel(0x03E0);
        b = MakeBmpChannel(0x001F);
    } else if (bpp == 32) {
        r = MakeBmpChannel(0x00FF0000);
        g = MakeBmpChannel(0x0000FF00);
        b = MakeBmpChannel(0x000000FF);
        a = MakeBmpChannel(0xFF000000); // BI_RGB alpha is unofficial: used only if not all zero (checked below)
    }

    // Palette.
    u8 palette[256][4] = {};
    if (bpp <= 8) {
        const u32 entrySize = headerSize == 12 ? 3 : 4;
        const u32 count = Min(colorsUsed ? colorsUsed : (1u << bpp), 256u);
        const usize palOffset = 14 + usize(headerSize) + (compression == 3 && headerSize == 40 ? 12 : 0);
        if (palOffset + usize(count) * entrySize > d.Size())
            return Error::Make(ErrorCode::InvalidData, "BMP: truncated palette");
        for (u32 i = 0; i < count; ++i) {
            const u8* p = d.Data() + palOffset + usize(i) * entrySize;
            palette[i][0] = p[2];
            palette[i][1] = p[1];
            palette[i][2] = p[0];
            palette[i][3] = 255;
        }
    }
    if (dataOffset >= d.Size())
        return Error::Make(ErrorCode::InvalidData, "BMP: pixel data offset out of range");

    Image img = Image::Create(width, height, ImageFormat::RGBA8);
    // File row y (in storage order) -> image row.
    auto outRow = [&](u32 fileRow) { return img.Row(topDown ? fileRow : height - 1 - fileRow); };

    if (rle) {
        // Unspecified pixels (delta / early end of line) stay index 0, like most decoders.
        Array<u8> indices;
        indices.ResizeZeroed(usize(width) * height);
        usize pos = dataOffset;
        u32 x = 0, y = 0;
        auto put = [&](u32 index) {
            if (x < width && y < height)
                indices[usize(y) * width + x] = u8(index);
            ++x;
        };
        while (pos + 1 < d.Size() && y < height) {
            const u32 count = d[pos], value = d[pos + 1];
            pos += 2;
            if (count > 0) {
                for (u32 i = 0; i < count; ++i)
                    put(bpp == 8 ? value : (i & 1) ? (value & 15) : (value >> 4));
            } else if (value == 0) { // end of line
                x = 0;
                ++y;
            } else if (value == 1) { // end of bitmap
                break;
            } else if (value == 2) { // delta
                if (pos + 2 > d.Size())
                    break;
                x += d[pos];
                y += d[pos + 1];
                pos += 2;
            } else { // absolute run of `value` pixels, padded to 16 bits
                const usize bytes = bpp == 8 ? value : (value + 1) / 2;
                if (pos + bytes > d.Size())
                    return Error::Make(ErrorCode::InvalidData, "BMP: truncated RLE run");
                for (u32 i = 0; i < value; ++i)
                    put(bpp == 8 ? d[pos + i] : (i & 1) ? (d[pos + i / 2] & 15) : (d[pos + i / 2] >> 4));
                pos += bytes + (bytes & 1);
            }
        }
        for (u32 fy = 0; fy < height; ++fy) {
            u8* dst = outRow(fy);
            for (u32 fx = 0; fx < width; ++fx)
                MemCopy(dst + fx * 4, palette[indices[usize(fy) * width + fx]], 4);
        }
        return img;
    }

    const usize stride = ((usize(width) * bpp + 31) / 32) * 4;
    if (dataOffset + stride * (height - 1) + (usize(width) * bpp + 7) / 8 > d.Size())
        return Error::Make(ErrorCode::InvalidData, "BMP: truncated pixel data");
    bool anyAlpha = false;
    for (u32 fy = 0; fy < height; ++fy) {
        const u8* src = d.Data() + dataOffset + stride * fy;
        u8* dst = outRow(fy);
        for (u32 x = 0; x < width; ++x, dst += 4) {
            switch (bpp) {
            case 1: MemCopy(dst, palette[(src[x >> 3] >> (7 - (x & 7))) & 1], 4); break;
            case 4: MemCopy(dst, palette[(src[x >> 1] >> ((x & 1) ? 0 : 4)) & 15], 4); break;
            case 8: MemCopy(dst, palette[src[x]], 4); break;
            case 24:
                dst[0] = src[x * 3 + 2];
                dst[1] = src[x * 3 + 1];
                dst[2] = src[x * 3 + 0];
                dst[3] = 255;
                break;
            default: {
                const u32 px = bpp == 16 ? u32(src[x * 2]) | (u32(src[x * 2 + 1]) << 8)
                                         : u32(src[x * 4]) | (u32(src[x * 4 + 1]) << 8) | (u32(src[x * 4 + 2]) << 16) |
                                               (u32(src[x * 4 + 3]) << 24);
                dst[0] = ExtractBmpChannel(r, px);
                dst[1] = ExtractBmpChannel(g, px);
                dst[2] = ExtractBmpChannel(b, px);
                dst[3] = a.max ? ExtractBmpChannel(a, px) : 255;
                anyAlpha |= a.max && dst[3] != 0;
                break;
            }
            }
        }
    }
    // A zero alpha channel everywhere means the file doesn't really use alpha (common for 32-bit BI_RGB).
    if (a.max && !anyAlpha)
        for (usize i = 3; i < img.pixels.Size(); i += 4)
            img.pixels[i] = 255;
    return img;
}

// --- Radiance HDR (RGBE) --------------------------------------------------------------------------------------------

Result<Image> DecodeHdr(ByteSpan d)
{
    usize pos = 0;
    auto readLine = [&](StringView& line) -> bool {
        if (pos >= d.Size())
            return false;
        const usize start = pos;
        while (pos < d.Size() && d[pos] != '\n')
            ++pos;
        line = StringView(reinterpret_cast<const char*>(d.Data()) + start, pos - start);
        ++pos;
        return true;
    };
    StringView line;
    bool rgbe = false;
    while (readLine(line) && !line.Empty())
        if (line.StartsWith("FORMAT=32-bit_rle_rgbe"))
            rgbe = true;
    if (!rgbe)
        return Error::Make(ErrorCode::Unsupported, "HDR: only 32-bit_rle_rgbe is supported");
    if (!readLine(line))
        return Error::Make(ErrorCode::InvalidData, "HDR: missing resolution");
    // "-Y height +X width"
    i64 w = 0, h = 0;
    {
        const Array<StringView> parts = SplitString(line.Trim(), ' ', true);
        if (parts.Size() != 4 || parts[0] != "-Y" || parts[2] != "+X")
            return Error::Make(ErrorCode::Unsupported, "HDR: unsupported orientation '{}'", line);
        ParseIntPrefix(parts[1], h);
        ParseIntPrefix(parts[3], w);
    }
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768)
        return Error::Make(ErrorCode::InvalidData, "HDR: invalid size");
    Image img = Image::Create(u32(w), u32(h), ImageFormat::RGBA32F);
    Array<u8> scan;
    scan.ResizeUninitialized(usize(w) * 4);
    for (i64 y = 0; y < h; ++y) {
        if (pos + 4 > d.Size())
            return Error::Make(ErrorCode::InvalidData, "HDR: truncated");
        const bool newRle = w >= 8 && w < 32768 && d[pos] == 2 && d[pos + 1] == 2 && ((d[pos + 2] << 8) | d[pos + 3]) == w;
        if (newRle) {
            pos += 4;
            for (u32 c = 0; c < 4; ++c) {
                i64 x = 0;
                while (x < w) {
                    if (pos >= d.Size())
                        return Error::Make(ErrorCode::InvalidData, "HDR: truncated RLE");
                    u32 count = d[pos++];
                    if (count > 128) {
                        count -= 128;
                        if (pos >= d.Size() || x + count > w)
                            return Error::Make(ErrorCode::InvalidData, "HDR: bad RLE run");
                        const u8 v = d[pos++];
                        for (u32 k = 0; k < count; ++k)
                            scan[usize(x++) * 4 + c] = v;
                    } else {
                        if (count == 0 || pos + count > d.Size() || x + count > w)
                            return Error::Make(ErrorCode::InvalidData, "HDR: bad RLE dump");
                        for (u32 k = 0; k < count; ++k)
                            scan[usize(x++) * 4 + c] = d[pos++];
                    }
                }
            }
        } else {
            if (pos + usize(w) * 4 > d.Size())
                return Error::Make(ErrorCode::InvalidData, "HDR: truncated flat scanline");
            MemCopy(scan.Data(), d.Data() + pos, usize(w) * 4);
            pos += usize(w) * 4;
        }
        f32* out = reinterpret_cast<f32*>(img.Row(u32(y)));
        for (i64 x = 0; x < w; ++x) {
            const u8* e = &scan[usize(x) * 4];
            const f32 scale = e[3] ? std::ldexp(1.0f, i32(e[3]) - 136) : 0.0f;
            out[x * 4 + 0] = f32(e[0]) * scale;
            out[x * 4 + 1] = f32(e[1]) * scale;
            out[x * 4 + 2] = f32(e[2]) * scale;
            out[x * 4 + 3] = 1.0f;
        }
    }
    return img;
}

} // namespace zx
