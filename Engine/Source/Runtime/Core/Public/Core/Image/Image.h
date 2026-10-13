#pragma once

// CPU-side images and codecs (own implementations): PNG, JPEG (baseline + progressive), TGA, BMP,
// Radiance HDR.
// Decoders return RGBA8 (LDR) or RGBA32F (HDR). Mip generation and BCn compression live in TextureTools.h.

#include "Core/Containers/Array.h"
#include "Core/Result.h"

namespace zx {

enum class ImageFormat : u8
{
    RGBA8,   // 4 x u8 (sRGB or linear, decided by the importer)
    RGBA16,  // 4 x u16
    RGBA32F, // 4 x f32 (HDR)
};

inline constexpr u32 ImageFormatBytesPerPixel(ImageFormat f)
{
    return f == ImageFormat::RGBA8 ? 4 : f == ImageFormat::RGBA16 ? 8 : 16;
}

struct Image
{
    u32 width = 0;
    u32 height = 0;
    ImageFormat format = ImageFormat::RGBA8;
    Array<u8> pixels; // tightly packed rows

    bool IsValid() const { return width > 0 && height > 0 && !pixels.Empty(); }
    usize RowPitch() const { return usize(width) * ImageFormatBytesPerPixel(format); }
    u8* Row(u32 y) { return pixels.Data() + usize(y) * RowPitch(); }
    const u8* Row(u32 y) const { return pixels.Data() + usize(y) * RowPitch(); }

    static Image Create(u32 width, u32 height, ImageFormat format);
    /// Converts in place (RGBA16 -> RGBA8 truncates, LDR -> RGBA32F divides by 255/65535).
    void ConvertTo(ImageFormat target);
    /// True if any pixel has alpha < 1.
    bool HasTransparency() const;
};

enum class ImageFileType : u8
{
    Unknown,
    Png,
    Jpeg,
    Tga,
    Bmp,
    Hdr
};

/// Detects the type from the first bytes (falls back to the extension for TGA which has no magic).
ImageFileType DetectImageType(ByteSpan data, StringView fileNameHint = {});

Result<Image> DecodeImage(ByteSpan data, StringView fileNameHint = {});
Result<Image> DecodePng(ByteSpan data);
Result<Image> DecodeJpeg(ByteSpan data);
Result<Image> DecodeTga(ByteSpan data);
/// BMP: 1/4/8-bit palette (incl. RLE4/RLE8), 16/24/32-bit (incl. BI_BITFIELDS with alpha), V3-V5 headers, top-down.
Result<Image> DecodeBmp(ByteSpan data);
Result<Image> DecodeHdr(ByteSpan data);

/// Loads through the virtual file system.
Result<Image> LoadImageFile(StringView path);

} // namespace zx
