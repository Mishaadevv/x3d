#pragma once

// Offline texture processing: mip chains and block compression (BC1/BC3/BC4/BC5, BC6H for HDR, BC7). Used by the asset
// cooker.

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

/// Generates the full mip chain (level 0 = source) for RGBA8 or RGBA32F images (HDR: filtered as is, srgb ignored;
/// prefer the box filter there - the Kaiser lobes ring around very bright texels).
Array<Image> GenerateMips(const Image& source, const MipOptions& options = {});

enum class BlockFormat : u8
{
    BC1, // RGB (+1-bit alpha), 4 bpp
    BC3, // RGBA, 8 bpp
    BC4, // single channel (R), 4 bpp
    BC5, // two channels (RG, normal maps), 8 bpp
    BC6H, // unsigned half-float RGB (HDR), 8 bpp
    BC7, // RGBA, high quality, 8 bpp
};

u32 BlockBytes(BlockFormat format);

/// Compresses an image (any size; edge blocks are padded by clamping). Output: blocks in row-major order.
/// BC6H takes RGBA32F (alpha ignored, negatives clamped to 0), the other formats RGBA8.
Array<u8> CompressBlocks(const Image& image, BlockFormat format);

/// Decodes blocks (for tests/previews): RGBA32F for BC6H, RGBA8 otherwise.
Image DecompressBlocks(ByteSpan blocks, u32 width, u32 height, BlockFormat format);

// Single blocks (16 texels, row-major). forceMode (tests): BC6H mode 0..13 (= modes 1..14 of the spec), BC7 mode 1/4/5/6.
void EncodeBC6HBlock(const f32 rgb[16][3], u8* out, i32 forceMode = -1);
void DecodeBC6HBlock(const u8* in, u16 out[16][3]); // half-float bits
void EncodeBC7Block(const u8 rgba[16][4], u8* out, i32 forceMode = -1);
/// Modes 1, 4, 5 and 6 (what the encoder writes); other modes decode to magenta and return false.
bool DecodeBC7Block(const u8* in, u8 out[16][4]);

} // namespace zx::texture
