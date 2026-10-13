// OpenEXR decoder tests against files written and decoded by the OpenEXR library (Tools/gen_exr_testdata.py).

#include "Core/CoreInit.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Image/Exr.h"
#include "Core/Math/Packing.h"
#include "Core/Math/Random.h"
#include "Core/Image/TextureTools.h"
#include "Core/Testing/Test.h"
#include "Core/Time/Time.h"

#include <cstdio>
#include <cstdlib>

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

void CheckReference(StringView name, u32 width, u32 height)
{
    Array<u8> data, ref;
    ZX_REQUIRE(ReadAll(DataPath(String::Format("exr_{}.exr", name)), data));
    ZX_REQUIRE(ReadAll(DataPath(String::Format("exr_{}.rgbaf", name)), ref));
    ZX_EXPECT(DetectImageType(data.View()) == ImageFileType::Exr);
    Result<Image> img = DecodeImage(data.View(), "x.exr");
    if (!img) {
        test::ReportFailure(__FILE__, __LINE__, String::Format("{}: {}", name, img.GetError().message));
        return;
    }
    ZX_EXPECT(img->format == ImageFormat::RGBA32F);
    ZX_EXPECT_EQ(img->width, width);
    ZX_EXPECT_EQ(img->height, height);
    if (img->pixels.Size() != ref.Size()) {
        test::ReportFailure(__FILE__, __LINE__, String::Format("{}: {} bytes, reference {}", name, img->pixels.Size(), ref.Size()));
        return;
    }
    usize mismatches = 0, first = 0;
    for (usize i = 0; i < ref.Size(); i += 4)
        if (MemCompare(img->pixels.Data() + i, ref.Data() + i, 4) != 0 && mismatches++ == 0)
            first = i / 4;
    if (mismatches)
        test::ReportFailure(__FILE__, __LINE__,
                            String::Format("{}: {} values differ (first: pixel {}, channel {}: {} vs {})", name, mismatches, first / 4, first % 4,
                                           reinterpret_cast<const f32*>(img->pixels.Data())[first],
                                           reinterpret_cast<const f32*>(ref.Data())[first]));
}

} // namespace

ZX_TEST(Exr, ScanlineCompressions)
{
    for (StringView c : {"none", "rle", "zips", "zip", "piz", "pxr24"})
        CheckReference(String::Format("{}_half", c), 37, 23);
}

ZX_TEST(Exr, FloatAndMixedChannels)
{
    for (StringView n : {"zip_float", "piz_float", "pxr24_float", "piz_mixed", "zip_mixed", "zip_y"})
        CheckReference(n, 37, 23);
}

ZX_TEST(Exr, LargeWindowAndTiles)
{
    CheckReference("piz_large", 200, 70);
    CheckReference("zip_large", 200, 70);
    CheckReference("zip_window", 37, 23);
    CheckReference("tiled_zip", 37, 23);
    CheckReference("tiled_piz", 37, 23);
}

ZX_TEST(Exr, WriterRoundTrip)
{
    Random rng(7);
    Image img = Image::Create(53, 35, ImageFormat::RGBA32F);
    f32* p = reinterpret_cast<f32*>(img.pixels.Data());
    for (usize i = 0; i < usize(img.width) * img.height; ++i) {
        for (u32 c = 0; c < 3; ++c)
            p[i * 4 + c] = rng.NextFloat() * (i % 7 == 0 ? 5000.0f : 2.0f);
        p[i * 4 + 3] = (i % 3) ? 1.0f : rng.NextFloat();
    }
    for (bool compress : {true, false}) {
        const Array<u8> file = exr::Encode(img, compress);
        Result<Image> back = DecodeExr(file.View());
        ZX_REQUIRE(back.IsOk());
        ZX_EXPECT(back->width == img.width && back->height == img.height);
        const f32* q = reinterpret_cast<const f32*>(back->pixels.Data());
        bool same = true;
        for (usize i = 0; i < usize(img.width) * img.height * 4; ++i)
            same &= q[i] == HalfToFloat(FloatToHalf(p[i]));
        ZX_EXPECT(same);
    }
}

ZX_TEST(Exr, CorruptDataFailsCleanly)
{
    for (StringView n : {"exr_piz_large.exr", "exr_zip_half.exr", "exr_pxr24_float.exr", "exr_rle_half.exr", "exr_tiled_piz.exr"}) {
        Array<u8> data;
        ZX_REQUIRE(ReadAll(DataPath(n), data));
        // Truncations never crash and fail.
        for (usize len : {usize(0), usize(4), usize(100), data.Size() / 2, data.Size() - 1})
            ZX_EXPECT(!DecodeExr(ByteSpan(data.Data(), len)).IsOk());
        // Random corruption either decodes or fails; never crashes.
        Random rng(99);
        for (u32 round = 0; round < 40; ++round) {
            Array<u8> bad = data;
            for (u32 k = 0; k < 8; ++k)
                bad[usize(rng.NextU32()) % bad.Size()] ^= u8(1u << (rng.NextU32() & 7));
            (void)DecodeExr(bad.View());
        }
    }
}

// Manual: decode + mips + BC6H timing of a real panorama: ZX_EXR_FILE=<path> ZXTests ManualExr.*
ZX_TEST(ManualExr, DecodeAndCompressFile)
{
    const char* path = std::getenv("ZX_EXR_FILE");
    if (!path)
        return;
    Array<u8> data;
    ZX_REQUIRE(ReadAll(path, data));
    Stopwatch sw;
    Result<Image> img = DecodeImage(data.View(), path);
    if (!img)
        std::printf("decode failed: %s\n", String(img.GetError().message).CStr());
    ZX_REQUIRE(img.IsOk());
    const f64 decodeMs = sw.ElapsedMilliseconds();
    std::printf("decoded %ux%u in %.1f ms\n", img->width, img->height, decodeMs);
    std::fflush(stdout);
    sw.Restart();
    texture::MipOptions mo;
    mo.filter = texture::MipFilter::Box;
    const Array<Image> mips = texture::GenerateMips(*img, mo);
    const f64 mipMs = sw.ElapsedMilliseconds();
    sw.Restart();
    usize bytes = 0;
    for (const Image& m : mips)
        if (m.width % 4 == 0 && m.height % 4 == 0)
            bytes += texture::CompressBlocks(m, texture::BlockFormat::BC6H).Size();
    std::printf("%ux%u: decode %.1f ms, mips %.1f ms, BC6H %.1f ms (%zu KB)\n", img->width, img->height, decodeMs, mipMs,
                sw.ElapsedMilliseconds(), bytes / 1024);
}
