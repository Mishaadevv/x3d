#pragma once

// Loads textures from image files (PNG/JPEG/TGA/HDR/EXR) or DDS into GPU textures, with optional mip generation and
// BCn compression (HDR images: BC6H). The asset pipeline cooks textures offline; this is the direct runtime path.

#include "Core/Image/TextureTools.h"
#include "RHI/RHI.h"

namespace zx::render {

enum class TextureCompression : u8
{
    None,  // RGBA8 (HDR: RGBA16F)
    Auto,  // BC1 if opaque, BC7 if transparent (color); BC5 for normal maps; HDR: BC6H
    BC1,
    BC3,
    BC4,
    BC5,
    BC7,
};

struct TextureLoadOptions
{
    bool srgb = true;
    bool generateMips = true;
    bool normalMap = false;
    TextureCompression compression = TextureCompression::Auto;
    texture::MipFilter mipFilter = texture::MipFilter::Kaiser;
};

/// CPU-side texture ready for upload (all mips, possibly block-compressed).
struct TextureData
{
    rhi::TextureDesc desc;
    Array<Array<u8>> mips; // one per mip level (array layer 0)
};

Result<TextureData> PrepareTexture(const Image& image, const TextureLoadOptions& options);
/// Parses a DDS file (DX9 FourCC BC1/BC3/BC4/BC5, DX10 header: BC1-BC7, RGBA8/16F/32F; 2D, mips).
Result<TextureData> ParseDds(ByteSpan data);

rhi::Texture* CreateTexture(rhi::Device& device, const TextureData& data, StringView debugName);
/// Loads a file through the VFS (DDS or image) and creates the GPU texture.
Result<rhi::Texture*> LoadTexture(rhi::Device& device, StringView path, const TextureLoadOptions& options = {});

} // namespace zx::render
