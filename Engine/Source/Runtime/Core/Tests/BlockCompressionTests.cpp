// BC6H / BC7 encoder quality (bit layouts are checked against the GPU in RHI.BlockCompressionMatchesHardwareDecoder).

#include "Core/Image/TextureTools.h"
#include "Core/Math/Random.h"
#include "Core/Testing/Test.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace zx;

namespace {

Image TestImageLdr(u32 size, bool alpha)
{
    Random rng(17);
    Image img = Image::Create(size, size, ImageFormat::RGBA8);
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            u8* p = img.Row(y) + x * 4;
            const f32 fx = f32(x) / f32(size), fy = f32(y) / f32(size);
            const bool edge = ((x / 13) + (y / 9)) % 4 == 0; // hard color edges inside blocks
            p[0] = u8(Clamp((edge ? 0.9f : fx) * 255.0f + rng.Range(-6.0f, 6.0f), 0.0f, 255.0f));
            p[1] = u8(Clamp((edge ? 0.2f : fy) * 255.0f + rng.Range(-6.0f, 6.0f), 0.0f, 255.0f));
            p[2] = u8(Clamp(0.5f + 0.5f * std::sin(fx * 9.0f + fy * 4.0f), 0.0f, 1.0f) * 255.0f);
            p[3] = alpha ? u8(Clamp(std::sin(fx * 6.0f) * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f) : 255;
        }
    return img;
}

f64 Psnr(const Image& a, const Image& b, u32 channels)
{
    f64 se = 0.0;
    usize n = 0;
    for (usize i = 0; i < usize(a.width) * a.height; ++i)
        for (u32 c = 0; c < channels; ++c) {
            const f64 d = f64(a.pixels[i * 4 + c]) - f64(b.pixels[i * 4 + c]);
            se += d * d;
            ++n;
        }
    const f64 mse = se / f64(n);
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

} // namespace

ZX_TEST(TextureTools, Bc7BeatsBc1AndBc3)
{
    const Image opaque = TestImageLdr(64, false);
    const Array<u8> bc7 = texture::CompressBlocks(opaque, texture::BlockFormat::BC7);
    const Array<u8> bc1 = texture::CompressBlocks(opaque, texture::BlockFormat::BC1);
    ZX_EXPECT_EQ(bc7.Size(), usize(16 * 16 * 16));
    const f64 p7 = Psnr(opaque, texture::DecompressBlocks(bc7.View(), 64, 64, texture::BlockFormat::BC7), 3);
    const f64 p1 = Psnr(opaque, texture::DecompressBlocks(bc1.View(), 64, 64, texture::BlockFormat::BC1), 3);
    ZX_EXPECT(p7 > 38.0);
    ZX_EXPECT(p7 > p1 + 3.0);

    const Image alpha = TestImageLdr(64, true);
    const Array<u8> bc7a = texture::CompressBlocks(alpha, texture::BlockFormat::BC7);
    const Array<u8> bc3 = texture::CompressBlocks(alpha, texture::BlockFormat::BC3);
    const Image d7 = texture::DecompressBlocks(bc7a.View(), 64, 64, texture::BlockFormat::BC7);
    const Image d3 = texture::DecompressBlocks(bc3.View(), 64, 64, texture::BlockFormat::BC3);
    const f64 pa7 = Psnr(alpha, d7, 4), pa3 = Psnr(alpha, d3, 4);
    ZX_EXPECT(pa7 > 36.0);
    ZX_EXPECT(pa7 > pa3 + 2.0);
    if (test::CurrentTestFailed() || std::getenv("ZX_TEST_PRINT"))
        std::printf("BC7 %.1f dB vs BC1 %.1f dB; with alpha BC7 %.1f dB vs BC3 %.1f dB\n", p7, p1, pa7, pa3);
}

ZX_TEST(TextureTools, Bc6hKeepsHdrRange)
{
    // Sky-like gradient over five orders of magnitude, a sun spot, hard edges.
    constexpr u32 Size = 64;
    Random rng(5);
    Image img = Image::Create(Size, Size, ImageFormat::RGBA32F);
    for (u32 y = 0; y < Size; ++y)
        for (u32 x = 0; x < Size; ++x) {
            f32* p = reinterpret_cast<f32*>(img.Row(y) + x * 16);
            const f32 fx = f32(x) / Size, fy = f32(y) / Size;
            const f32 sky = std::pow(10.0f, -2.0f + 5.0f * fy);
            const f32 sun = std::exp(-((fx - 0.7f) * (fx - 0.7f) + (fy - 0.3f) * (fy - 0.3f)) * 400.0f) * 30000.0f;
            const bool ground = x < 10 && y > 40;
            p[0] = ground ? 0.05f : sky * (0.6f + 0.4f * fx) + sun;
            p[1] = ground ? 0.04f : sky * 0.8f + sun * 0.95f + rng.NextFloat() * 0.001f;
            p[2] = ground ? 0.02f : sky + sun * 0.9f;
            p[3] = 1.0f;
        }
    const Array<u8> blocks = texture::CompressBlocks(img, texture::BlockFormat::BC6H);
    ZX_EXPECT_EQ(blocks.Size(), usize((Size / 4) * (Size / 4) * 16));
    const Image back = texture::DecompressBlocks(blocks.View(), Size, Size, texture::BlockFormat::BC6H);
    ZX_REQUIRE(back.format == ImageFormat::RGBA32F);
    f64 sumLog = 0.0, maxLog = 0.0;
    for (usize i = 0; i < usize(Size) * Size; ++i)
        for (u32 c = 0; c < 3; ++c) {
            const f32 a = reinterpret_cast<const f32*>(img.pixels.Data())[i * 4 + c];
            const f32 b = reinterpret_cast<const f32*>(back.pixels.Data())[i * 4 + c];
            const f64 e = Abs(std::log2((f64(b) + 1e-3) / (f64(a) + 1e-3)));
            sumLog += e;
            maxLog = Max(maxLog, e);
        }
    const f64 meanLog = sumLog / f64(Size * Size * 3);
    // Mean error below 2 % (log2 0.03), no texel off by more than 2x.
    ZX_EXPECT(meanLog < 0.03);
    ZX_EXPECT(maxLog < 1.0);
    if (test::CurrentTestFailed() || std::getenv("ZX_TEST_PRINT"))
        std::printf("BC6H mean |log2 error| %.4f, max %.3f\n", meanLog, maxLog);
}

ZX_TEST(TextureTools, HdrMipChain)
{
    Image img = Image::Create(16, 8, ImageFormat::RGBA32F);
    f32* p = reinterpret_cast<f32*>(img.pixels.Data());
    for (usize i = 0; i < 16 * 8; ++i) {
        p[i * 4 + 0] = i == 37 ? 50000.0f : 1.0f; // energy of a bright texel is preserved by the box filter
        p[i * 4 + 1] = 2.0f;
        p[i * 4 + 2] = 3.0f;
        p[i * 4 + 3] = 1.0f;
    }
    texture::MipOptions o;
    o.filter = texture::MipFilter::Box;
    const Array<Image> mips = texture::GenerateMips(img, o);
    ZX_EXPECT_EQ(mips.Size(), usize(5));
    ZX_EXPECT(mips.Back().format == ImageFormat::RGBA32F && mips.Back().width == 1 && mips.Back().height == 1);
    const f32* last = reinterpret_cast<const f32*>(mips.Back().pixels.Data());
    // 16x8 -> 8x4 -> 4x2 -> 2x1 -> 1x1: the last step averages 2 texels of a 2x1 level (box clamps at the edge).
    ZX_EXPECT(Abs(last[0] - (1.0f * 127.0f + 50000.0f) / 128.0f) < 1.0f);
    ZX_EXPECT(Abs(last[1] - 2.0f) < 1e-4f && Abs(last[2] - 3.0f) < 1e-4f);
}
