#include "RHI/RHITypes.h"
#include "Core/Assert.h"

namespace zx::rhi {

namespace {
// name, bytesPerBlock, blockSize, isDepth, hasStencil, isSRGB, isCompressed
constexpr FormatInfo FormatTable[] = {
    {"Unknown", 0, 1, false, false, false, false},
    {"R8_UNORM", 1, 1, false, false, false, false},
    {"R8_UINT", 1, 1, false, false, false, false},
    {"R8G8_UNORM", 2, 1, false, false, false, false},
    {"R8G8B8A8_UNORM", 4, 1, false, false, false, false},
    {"R8G8B8A8_SRGB", 4, 1, false, false, true, false},
    {"R8G8B8A8_SNORM", 4, 1, false, false, false, false},
    {"R8G8B8A8_UINT", 4, 1, false, false, false, false},
    {"B8G8R8A8_UNORM", 4, 1, false, false, false, false},
    {"B8G8R8A8_SRGB", 4, 1, false, false, true, false},
    {"R16_UNORM", 2, 1, false, false, false, false},
    {"R16_UINT", 2, 1, false, false, false, false},
    {"R16_FLOAT", 2, 1, false, false, false, false},
    {"R16G16_UNORM", 4, 1, false, false, false, false},
    {"R16G16_FLOAT", 4, 1, false, false, false, false},
    {"R16G16B16A16_UNORM", 8, 1, false, false, false, false},
    {"R16G16B16A16_FLOAT", 8, 1, false, false, false, false},
    {"R32_UINT", 4, 1, false, false, false, false},
    {"R32_SINT", 4, 1, false, false, false, false},
    {"R32_FLOAT", 4, 1, false, false, false, false},
    {"R32G32_UINT", 8, 1, false, false, false, false},
    {"R32G32_FLOAT", 8, 1, false, false, false, false},
    {"R32G32B32_FLOAT", 12, 1, false, false, false, false},
    {"R32G32B32A32_UINT", 16, 1, false, false, false, false},
    {"R32G32B32A32_FLOAT", 16, 1, false, false, false, false},
    {"R10G10B10A2_UNORM", 4, 1, false, false, false, false},
    {"R11G11B10_FLOAT", 4, 1, false, false, false, false},
    {"R9G9B9E5_SHAREDEXP", 4, 1, false, false, false, false},
    {"D16_UNORM", 2, 1, true, false, false, false},
    {"D24_UNORM_S8_UINT", 4, 1, true, true, false, false},
    {"D32_FLOAT", 4, 1, true, false, false, false},
    {"D32_FLOAT_S8X24_UINT", 8, 1, true, true, false, false},
    {"BC1_UNORM", 8, 4, false, false, false, true},
    {"BC1_SRGB", 8, 4, false, false, true, true},
    {"BC3_UNORM", 16, 4, false, false, false, true},
    {"BC3_SRGB", 16, 4, false, false, true, true},
    {"BC4_UNORM", 8, 4, false, false, false, true},
    {"BC5_UNORM", 16, 4, false, false, false, true},
    {"BC6H_UF16", 16, 4, false, false, false, true},
    {"BC7_UNORM", 16, 4, false, false, false, true},
    {"BC7_SRGB", 16, 4, false, false, true, true},
};
static_assert(ZX_ARRAY_COUNT(FormatTable) == usize(Format::Count), "Format table out of sync");
} // namespace

const FormatInfo& GetFormatInfo(Format format)
{
    ZX_ASSERT(u32(format) < u32(Format::Count));
    return FormatTable[u32(format)];
}

u64 GetRowPitch(Format format, u32 width)
{
    const FormatInfo& f = GetFormatInfo(format);
    const u32 blocks = (width + f.blockSize - 1) / f.blockSize;
    return u64(blocks) * f.bytesPerBlock;
}

u64 GetSliceSize(Format format, u32 width, u32 height)
{
    const FormatInfo& f = GetFormatInfo(format);
    const u32 rows = (height + f.blockSize - 1) / f.blockSize;
    return GetRowPitch(format, width) * rows;
}

} // namespace zx::rhi
