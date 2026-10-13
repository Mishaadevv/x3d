#include "Render/TextureLoader.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Math/MathCommon.h"
#include "Core/Math/Packing.h"
#include "Core/String/Path.h"

namespace zx::render {

using namespace rhi;

Result<TextureData> PrepareTexture(const Image& source, const TextureLoadOptions& options)
{
    if (!source.IsValid())
        return Error::Make(ErrorCode::InvalidArgument, "PrepareTexture: empty image");
    TextureData out;
    out.desc.width = source.width;
    out.desc.height = source.height;
    out.desc.usage = TextureUsage::ShaderResource;

    if (source.format == ImageFormat::RGBA32F) {
        // HDR: box-filtered mips, BC6H when the size allows, else half floats.
        Array<Image> levels;
        if (options.generateMips) {
            texture::MipOptions mo;
            mo.filter = texture::MipFilter::Box;
            levels = texture::GenerateMips(source, mo);
        } else {
            levels.PushBack(source);
        }
        const bool bc6 = options.compression != TextureCompression::None && source.width % 4 == 0 && source.height % 4 == 0;
        out.desc.format = bc6 ? Format::BC6H_UF16 : Format::R16G16B16A16_FLOAT;
        out.desc.mipLevels = u32(levels.Size());
        for (const Image& level : levels) {
            if (bc6) {
                out.mips.PushBack(texture::CompressBlocks(level, texture::BlockFormat::BC6H));
                continue;
            }
            Array<u8>& mip = out.mips.AddDefaulted();
            const usize count = usize(level.width) * level.height * 4;
            mip.ResizeUninitialized(count * 2);
            const f32* src = reinterpret_cast<const f32*>(level.pixels.Data());
            for (usize i = 0; i < count; ++i)
                reinterpret_cast<u16*>(mip.Data())[i] = FloatToHalf(src[i]);
        }
        return out;
    }
    Image rgba8 = source;
    rgba8.ConvertTo(ImageFormat::RGBA8);

    Array<Image> levels;
    if (options.generateMips) {
        texture::MipOptions mo;
        mo.filter = options.mipFilter;
        mo.srgb = options.srgb;
        mo.normalMap = options.normalMap;
        levels = texture::GenerateMips(rgba8, mo);
    } else {
        levels.PushBack(move(rgba8));
    }

    TextureCompression compression = options.compression;
    if (compression == TextureCompression::Auto)
        compression = options.normalMap ? TextureCompression::BC5
                                        : (levels[0].HasTransparency() ? TextureCompression::BC7 : TextureCompression::BC1);
    // Block-compressed textures need 4-aligned top-level dimensions in D3D12.
    if (compression != TextureCompression::None && (source.width % 4 != 0 || source.height % 4 != 0))
        compression = TextureCompression::None;

    switch (compression) {
    case TextureCompression::BC1: out.desc.format = options.srgb ? Format::BC1_SRGB : Format::BC1_UNORM; break;
    case TextureCompression::BC3: out.desc.format = options.srgb ? Format::BC3_SRGB : Format::BC3_UNORM; break;
    case TextureCompression::BC4: out.desc.format = Format::BC4_UNORM; break;
    case TextureCompression::BC5: out.desc.format = Format::BC5_UNORM; break;
    case TextureCompression::BC7: out.desc.format = options.srgb ? Format::BC7_SRGB : Format::BC7_UNORM; break;
    default: out.desc.format = options.srgb ? Format::R8G8B8A8_SRGB : Format::R8G8B8A8_UNORM; break;
    }
    out.desc.mipLevels = u32(levels.Size());
    for (const Image& level : levels) {
        switch (compression) {
        case TextureCompression::BC1: out.mips.PushBack(texture::CompressBlocks(level, texture::BlockFormat::BC1)); break;
        case TextureCompression::BC3: out.mips.PushBack(texture::CompressBlocks(level, texture::BlockFormat::BC3)); break;
        case TextureCompression::BC4: out.mips.PushBack(texture::CompressBlocks(level, texture::BlockFormat::BC4)); break;
        case TextureCompression::BC5: out.mips.PushBack(texture::CompressBlocks(level, texture::BlockFormat::BC5)); break;
        case TextureCompression::BC7: out.mips.PushBack(texture::CompressBlocks(level, texture::BlockFormat::BC7)); break;
        default: out.mips.PushBack(level.pixels); break;
        }
    }
    return out;
}

namespace {
constexpr u32 MakeFourCC(char a, char b, char c, char d) { return u32(u8(a)) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) | (u32(u8(d)) << 24); }

Format DxgiToFormat(u32 dxgi)
{
    switch (dxgi) {
    case 28: return Format::R8G8B8A8_UNORM;
    case 29: return Format::R8G8B8A8_SRGB;
    case 87: return Format::B8G8R8A8_UNORM;
    case 91: return Format::B8G8R8A8_SRGB;
    case 10: return Format::R16G16B16A16_FLOAT;
    case 2: return Format::R32G32B32A32_FLOAT;
    case 71: return Format::BC1_UNORM;
    case 72: return Format::BC1_SRGB;
    case 77: return Format::BC3_UNORM;
    case 78: return Format::BC3_SRGB;
    case 80: return Format::BC4_UNORM;
    case 83: return Format::BC5_UNORM;
    case 95: return Format::BC6H_UF16;
    case 98: return Format::BC7_UNORM;
    case 99: return Format::BC7_SRGB;
    default: return Format::Unknown;
    }
}
} // namespace

Result<TextureData> ParseDds(ByteSpan d)
{
    if (d.Size() < 128 || MemCompare(d.Data(), "DDS ", 4) != 0)
        return Error::Make(ErrorCode::InvalidData, "DDS: bad magic");
    auto u32At = [&](usize offset) {
        u32 v;
        MemCopy(&v, d.Data() + offset, 4);
        return v;
    };
    const u32 height = u32At(12), width = u32At(16);
    const u32 mipCount = Max(1u, u32At(28));
    const u32 pfFlags = u32At(80), fourCC = u32At(84);
    usize offset = 128;
    Format format = Format::Unknown;
    if ((pfFlags & 0x4) && fourCC == MakeFourCC('D', 'X', '1', '0')) {
        if (d.Size() < 148)
            return Error::Make(ErrorCode::InvalidData, "DDS: truncated DX10 header");
        format = DxgiToFormat(u32At(128));
        const u32 dimension = u32At(132), arraySize = u32At(140);
        if (dimension != 3 || arraySize != 1)
            return Error::Make(ErrorCode::Unsupported, "DDS: only single 2D textures are supported");
        offset = 148;
    } else if (pfFlags & 0x4) {
        switch (fourCC) {
        case MakeFourCC('D', 'X', 'T', '1'): format = Format::BC1_UNORM; break;
        case MakeFourCC('D', 'X', 'T', '5'): format = Format::BC3_UNORM; break;
        case MakeFourCC('A', 'T', 'I', '1'):
        case MakeFourCC('B', 'C', '4', 'U'): format = Format::BC4_UNORM; break;
        case MakeFourCC('A', 'T', 'I', '2'):
        case MakeFourCC('B', 'C', '5', 'U'): format = Format::BC5_UNORM; break;
        default: break;
        }
    } else if ((pfFlags & 0x40) && u32At(88) == 32 && u32At(92) == 0xFF && u32At(100) == 0xFF0000) {
        format = Format::R8G8B8A8_UNORM;
    }
    if (format == Format::Unknown)
        return Error::Make(ErrorCode::Unsupported, "DDS: unsupported pixel format");
    TextureData out;
    out.desc.width = width;
    out.desc.height = height;
    out.desc.format = format;
    out.desc.mipLevels = mipCount;
    out.desc.usage = TextureUsage::ShaderResource;
    for (u32 m = 0; m < mipCount; ++m) {
        const u64 size = GetSliceSize(format, Max(1u, width >> m), Max(1u, height >> m));
        if (offset + size > d.Size())
            return Error::Make(ErrorCode::InvalidData, "DDS: truncated mip {}", m);
        out.mips.PushBack(Array<u8>(Span<const u8>(d.Data() + offset, usize(size))));
        offset += usize(size);
    }
    return out;
}

Texture* CreateTexture(Device& device, const TextureData& data, StringView debugName)
{
    TextureDesc desc = data.desc;
    desc.debugName = debugName;
    Array<SubresourceData> subresources;
    for (u32 m = 0; m < data.mips.Size(); ++m)
        subresources.PushBack(SubresourceData{data.mips[m].Data(), GetRowPitch(desc.format, Max(1u, desc.width >> m)), 0});
    return device.CreateTexture(desc, subresources.View());
}

Result<Texture*> LoadTexture(Device& device, StringView path, const TextureLoadOptions& options)
{
    Array<u8> bytes;
    if (!fs::ReadFile(path, bytes))
        return Error::Make(ErrorCode::NotFound, "Texture not found: {}", path);
    Result<TextureData> data = Path::HasExtension(path, "dds") ? ParseDds(bytes.View()) : [&]() -> Result<TextureData> {
        Result<Image> img = DecodeImage(bytes.View(), path);
        if (!img)
            return img.TakeError();
        return PrepareTexture(*img, options);
    }();
    if (!data)
        return data.TakeError();
    Texture* t = CreateTexture(device, *data, Path::FileName(path));
    if (!t)
        return Error::Make(ErrorCode::Internal, "Failed to create GPU texture for {}", path);
    return t;
}

} // namespace zx::render
