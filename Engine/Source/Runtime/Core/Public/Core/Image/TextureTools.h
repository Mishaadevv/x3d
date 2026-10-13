#pragma once

// Offline texture processing: mip chains and block compression (BC1/BC3/BC4/BC5). Used by the asset cooker.

#include "Core/Image/Image.h"

namespace zx::texture {

enum class MipFilter : u8
{
    Box,    // 2x2 average (fast)
    Kaiser, // 4x4 windowed sinc-like kernel (sharper)
};

struct MipOptions
{
    MipFilter filter = MipFilter::Kaiser;
    /// RGB stored as sRGB: filter in linear space.
    bool srgb = true;
    /// Keep the fraction of texels with alpha >= cutoff constant across mips (alpha-tested foliage).
    bool preserveAlphaCoverage = false;
    f32 alphaCutoff = 0.5f;
    /// Renormalize RGB as tangent-space normals ([0,1] encoded) after filtering.
    bool normalMap = false;
};

/// Generates the full mip chain (level 0 = source) for RGBA8 images.
Array<Image> GenerateMips(const Image& source, const MipOptions& options = {});

enum class BlockFormat : u8
{
    BC1, // RGB (+1-bit alpha), 4 bpp
    BC3, // RGBA, 8 bpp
    BC4, // single channel (R), 4 bpp
    BC5, // two channels (RG, normal maps), 8 bpp
};

u32 BlockBytes(BlockFormat format);

/// Compresses an RGBA8 image (any size; edge blocks are padded by clamping). Output: blocks in row-major order.
Array<u8> CompressBlocks(const Image& image, BlockFormat format);

/// Decodes blocks back to RGBA8 (for tests/previews).
Image DecompressBlocks(ByteSpan blocks, u32 width, u32 height, BlockFormat format);

} // namespace zx::texture
