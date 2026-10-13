// Image codec tests against references produced by PIL/libpng/libjpeg (Tests/Data/Images, see gen script in docs).

#include "Core/CoreInit.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Image/Image.h"
#include "Core/Image/Png.h"
#include "Core/Testing/Test.h"

#include <cmath>

using namespace zx;

namespace {

String DataPath(StringView name) { return FindEngineRootDirectory() + "/Tests/Data/Images/" + name; }

bool ReadAll(StringView path, Array<u8>& out)
{
    platform::File f;
    if (!f.Open(path, platform::FileMode::Read))
        return false;
    out.ResizeUninitialized(usize(f.Size()));
    return f.Read(out.Data(), out.Size()) == out.Size();
}

struct Diff
{
    f64 mean = 0.0;
    u32 max = 0;
};

Diff Compare(const Image& img, const Array<u8>& ref)
{
    Diff d;
    const usize n = Min(img.pixels.Size(), ref.Size());
    u64 sum = 0;
    for (usize i = 0; i < n; ++i) {
        const u32 e = u32(img.pixels[i] > ref[i] ? img.pixels[i] - ref[i] : ref[i] - img.pixels[i]);
        sum += e;
        d.max = Max(d.max, e);
    }
    d.mean = n ? f64(sum) / f64(n) : 0.0;
    return d;
}

void CheckDecode(StringView file, StringView reference, bool exact)
{
    Array<u8> data, ref;
    ZX_REQUIRE(ReadAll(DataPath(file), data));
    ZX_REQUIRE(ReadAll(DataPath(reference), ref));
    Result<Image> img = DecodeImage(data.View(), file);
    if (!img) {
        test::ReportFailure(__FILE__, __LINE__, String::Format("{}: {}", file, img.GetError().message));
        return;
    }
    img->ConvertTo(ImageFormat::RGBA8);
    ZX_EXPECT_EQ(img->pixels.Size(), ref.Size());
    const Diff d = Compare(*img, ref);
    if (exact ? d.max != 0 : (d.mean > 2.5 || d.max > 64))
        test::ReportFailure(__FILE__, __LINE__, String::Format("{}: mean error {:.3f}, max {}", file, d.mean, d.max));
}

} // namespace

ZX_TEST(Image, PngColorTypes)
{
    for (StringView name : {"rgba8", "rgb8", "gray8", "graya8", "gray1", "gray2", "gray4", "palette_trns", "rgba8_interlaced"})
        CheckDecode(String(name) + ".png", String(name) + ".rgba", true);
}

ZX_TEST(Image, Png16Bit)
{
    Array<u8> data, ref;
    ZX_REQUIRE(ReadAll(DataPath("gray16.png"), data));
    ZX_REQUIRE(ReadAll(DataPath("gray16.raw16"), ref));
    Result<Image> img = DecodePng(data.View());
    ZX_REQUIRE(img.IsOk());
    ZX_EXPECT(img->format == ImageFormat::RGBA16);
    const u16* px = reinterpret_cast<const u16*>(img->pixels.Data());
    const u16* expected = reinterpret_cast<const u16*>(ref.Data());
    bool ok = true;
    for (usize i = 0; i < ref.Size() / 2; ++i)
        ok &= px[i * 4] == expected[i] && px[i * 4 + 3] == 65535;
    ZX_EXPECT(ok);
}

ZX_TEST(Image, JpegVariants)
{
    for (StringView name : {"jpeg420", "jpeg422", "jpeg444", "jpeg_gray", "jpeg_progressive", "jpeg_restart"})
        CheckDecode(String(name) + ".jpg", String(name) + ".rgba", false);
}

ZX_TEST(Image, Tga)
{
    CheckDecode("rgba_rle.tga", "rgba_rle.rgba", true);
    CheckDecode("rgb.tga", "rgb.rgba", true);
}

ZX_TEST(Image, Bmp)
{
    for (StringView name : {"rgb24", "pal8", "mono1", "argb32", "rgb565", "pal4", "rle8"})
        CheckDecode(String(name) + ".bmp", String(name) + ".rgba", true);
}

ZX_TEST(Image, HdrFlatScanlines)
{
    // 2x1 image, flat (uncompressed) RGBE: (1.0, 0.5, 0.25) and (2, 2, 2).
    String text = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
    Array<u8> data;
    data.Append(Span<const u8>(reinterpret_cast<const u8*>(text.Data()), text.Size()));
    const u8 pixels[] = {128, 64, 32, 129, 128, 128, 128, 130};
    data.Append(Span<const u8>(pixels));
    Result<Image> img = DecodeHdr(data.View());
    ZX_REQUIRE(img.IsOk());
    const f32* p = reinterpret_cast<const f32*>(img->pixels.Data());
    ZX_EXPECT_NEAR(p[0], 1.0, 1e-6);
    ZX_EXPECT_NEAR(p[1], 0.5, 1e-6);
    ZX_EXPECT_NEAR(p[2], 0.25, 1e-6);
    ZX_EXPECT_NEAR(p[4], 2.0, 1e-6);
}

ZX_TEST(Image, PngRoundTripThroughOwnEncoder)
{
    Image img = Image::Create(53, 17, ImageFormat::RGBA8);
    for (usize i = 0; i < img.pixels.Size(); ++i)
        img.pixels[i] = u8(i * 31 + (i >> 5));
    const Array<u8> png = png::Encode(img.pixels.Data(), img.width, img.height, 4);
    Result<Image> back = DecodePng(png.View());
    ZX_REQUIRE(back.IsOk());
    ZX_EXPECT(back->pixels == img.pixels);
}

ZX_TEST(Image, CorruptDataFailsCleanly)
{
    Array<u8> data;
    ZX_REQUIRE(ReadAll(DataPath("rgba8.png"), data));
    data[40] ^= 0x55; // breaks a CRC
    ZX_EXPECT(!DecodePng(data.View()));
    ZX_REQUIRE(ReadAll(DataPath("jpeg420.jpg"), data));
    data.Resize(data.Size() / 3);
    // Truncated JPEG either fails or decodes partially - it must never crash.
    (void)DecodeJpeg(data.View());
    ZX_EXPECT(!DecodeImage(ByteSpan(data.Data(), 2)));
}

#include "Core/Image/TextureTools.h"
#include "Core/Math/Color.h"

namespace {
f64 Psnr(const Image& a, const Image& b, u32 channels)
{
    f64 mse = 0.0;
    usize n = 0;
    for (usize i = 0; i < usize(a.width) * a.height; ++i)
        for (u32 c = 0; c < channels; ++c) {
            const f64 d = f64(a.pixels[i * 4 + c]) - f64(b.pixels[i * 4 + c]);
            mse += d * d;
            ++n;
        }
    mse /= f64(n);
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

Image MakeTestImage(u32 w, u32 h)
{
    Image img = Image::Create(w, h, ImageFormat::RGBA8);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u8* p = img.Row(y) + usize(x) * 4;
            p[0] = u8(x * 255 / (w - 1));
            p[1] = u8(y * 255 / (h - 1));
            p[2] = u8(128 + 100 * Sin(f32(x + y) * 0.05f));
            p[3] = u8(((x / 8) + (y / 8)) % 2 ? 255 : 40);
        }
    return img;
}
} // namespace

ZX_TEST(TextureTools, BlockCompressionQuality)
{
    const Image img = MakeTestImage(70, 45); // non-multiple-of-4 size
    struct Case
    {
        texture::BlockFormat format;
        u32 channels;
        f64 minPsnr;
    } cases[] = {{texture::BlockFormat::BC1, 3, 30.0}, {texture::BlockFormat::BC3, 4, 30.0},
                 {texture::BlockFormat::BC4, 1, 38.0}, {texture::BlockFormat::BC5, 2, 38.0}};
    Image opaque = img; // BC1 maps alpha < 0.5 to transparent black, so measure color quality on opaque data
    for (usize i = 0; i < usize(opaque.width) * opaque.height; ++i)
        opaque.pixels[i * 4 + 3] = 255;
    for (const Case& c : cases) {
        const Image& src = c.format == texture::BlockFormat::BC1 ? opaque : img;
        const Array<u8> blocks = texture::CompressBlocks(src, c.format);
        ZX_EXPECT_EQ(blocks.Size(), usize(18 * 12 * texture::BlockBytes(c.format)));
        const Image back = texture::DecompressBlocks(blocks.View(), src.width, src.height, c.format);
        const f64 psnr = Psnr(src, back, c.channels);
        if (psnr < c.minPsnr)
            test::ReportFailure(__FILE__, __LINE__, String::Format("format {} PSNR {:.2f} dB", u32(c.format), psnr));
    }
}

ZX_TEST(TextureTools, Bc1PunchThroughAlpha)
{
    Image img = Image::Create(4, 4, ImageFormat::RGBA8);
    for (u32 i = 0; i < 16; ++i) {
        img.pixels[i * 4 + 0] = 200;
        img.pixels[i * 4 + 3] = (i % 2) ? 255 : 0;
    }
    const Array<u8> blocks = texture::CompressBlocks(img, texture::BlockFormat::BC1);
    const Image back = texture::DecompressBlocks(blocks.View(), 4, 4, texture::BlockFormat::BC1);
    bool ok = true;
    for (u32 i = 0; i < 16; ++i)
        ok &= (back.pixels[i * 4 + 3] == 0) == (i % 2 == 0);
    ZX_EXPECT(ok);
}

ZX_TEST(TextureTools, MipChain)
{
    const Image img = MakeTestImage(64, 32);
    const Array<Image> mips = texture::GenerateMips(img);
    ZX_EXPECT_EQ(mips.Size(), 7u); // 64x32 -> 1x1
    ZX_EXPECT_EQ(mips.Back().width, 1u);
    ZX_EXPECT_EQ(mips.Back().height, 1u);
    ZX_EXPECT_EQ(mips[1].width, 32u);
    ZX_EXPECT_EQ(mips[1].height, 16u);
    // Average brightness (linear) is preserved approximately through the chain.
    f64 srcAvg = 0.0;
    for (usize i = 0; i < usize(img.width) * img.height; ++i)
        srcAvg += SRGB8ToLinear(img.pixels[i * 4]);
    srcAvg /= f64(usize(img.width) * img.height);
    ZX_EXPECT_NEAR(SRGB8ToLinear(mips.Back().pixels[0]), srcAvg, 0.03);

    // Alpha coverage preservation for alpha-tested textures.
    texture::MipOptions opt;
    opt.preserveAlphaCoverage = true;
    const Array<Image> covered = texture::GenerateMips(img, opt);
    auto coverage = [](const Image& m) {
        usize c = 0;
        for (usize i = 0; i < usize(m.width) * m.height; ++i)
            c += m.pixels[i * 4 + 3] >= 128 ? 1 : 0;
        return f64(c) / f64(usize(m.width) * m.height);
    };
    ZX_EXPECT_NEAR(coverage(covered[2]), coverage(img), 0.1);
}
