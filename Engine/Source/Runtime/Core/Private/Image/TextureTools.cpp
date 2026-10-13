#include "Core/Image/TextureTools.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Math/Color.h"

#include <cmath>

namespace zx::texture {

// --- Mips -----------------------------------------------------------------------------------------------------------

namespace {

/// Planar float image for filtering.
struct FloatImage
{
    u32 w = 0, h = 0;
    Array<f32> data; // RGBA
    f32* At(u32 x, u32 y) { return &data[(usize(y) * w + x) * 4]; }
    const f32* At(u32 x, u32 y) const { return &data[(usize(y) * w + x) * 4]; }
};

FloatImage ToFloat(const Image& img, bool srgb)
{
    FloatImage f;
    f.w = img.width;
    f.h = img.height;
    f.data.ResizeUninitialized(usize(f.w) * f.h * 4);
    for (usize i = 0; i < usize(f.w) * f.h; ++i)
        for (u32 c = 0; c < 4; ++c) {
            const u8 v = img.pixels[i * 4 + c];
            f.data[i * 4 + c] = (srgb && c < 3) ? SRGB8ToLinear(v) : f32(v) / 255.0f;
        }
    return f;
}

Image ToImage(const FloatImage& f, bool srgb)
{
    Image img = Image::Create(f.w, f.h, ImageFormat::RGBA8);
    for (usize i = 0; i < usize(f.w) * f.h; ++i)
        for (u32 c = 0; c < 4; ++c) {
            f32 v = f.data[i * 4 + c];
            if (srgb && c < 3)
                v = LinearToSRGB(v);
            img.pixels[i * 4 + c] = u8(Saturate(v) * 255.0f + 0.5f);
        }
    return img;
}

f32 KaiserWeight(f32 x)
{
    // Kaiser-windowed sinc, width 2 (in destination texels), alpha 4.
    auto bessel0 = [](f32 v) {
        f32 sum = 1.0f, term = 1.0f;
        for (int k = 1; k < 16; ++k) {
            term *= (v / (2.0f * f32(k))) * (v / (2.0f * f32(k)));
            sum += term;
        }
        return sum;
    };
    const f32 width = 2.0f, alpha = 4.0f;
    if (Abs(x) >= width)
        return 0.0f;
    const f32 t = x / width;
    const f32 window = bessel0(alpha * Sqrt(1.0f - t * t)) / bessel0(alpha);
    const f32 sinc = x == 0.0f ? 1.0f : Sin(Pi * x) / (Pi * x);
    return sinc * window;
}

FloatImage Downsample(const FloatImage& src, MipFilter filter)
{
    FloatImage dst;
    dst.w = Max(1u, src.w / 2);
    dst.h = Max(1u, src.h / 2);
    dst.data.ResizeZeroed(usize(dst.w) * dst.h * 4);
    if (filter == MipFilter::Box) {
        for (u32 y = 0; y < dst.h; ++y)
            for (u32 x = 0; x < dst.w; ++x) {
                f32* o = dst.At(x, y);
                for (u32 dy = 0; dy < 2; ++dy)
                    for (u32 dx = 0; dx < 2; ++dx) {
                        const f32* s = src.At(Min(x * 2 + dx, src.w - 1), Min(y * 2 + dy, src.h - 1));
                        for (u32 c = 0; c < 4; ++c)
                            o[c] += s[c] * 0.25f;
                    }
            }
        return dst;
    }
    // Separable Kaiser: source taps at offsets -3..+4 around 2x (half-texel centered), weights in dst units.
    f32 weights[8];
    f32 sum = 0.0f;
    for (int t = 0; t < 8; ++t) {
        const f32 srcOffset = f32(t - 3) - 0.5f; // relative to the dst texel center in source texels
        weights[t] = KaiserWeight(srcOffset * 0.5f);
        sum += weights[t];
    }
    for (f32& w : weights)
        w /= sum;
    FloatImage tmp;
    tmp.w = dst.w;
    tmp.h = src.h;
    tmp.data.ResizeZeroed(usize(tmp.w) * tmp.h * 4);
    for (u32 y = 0; y < src.h; ++y)
        for (u32 x = 0; x < tmp.w; ++x) {
            f32* o = tmp.At(x, y);
            for (int t = 0; t < 8; ++t) {
                const i32 sx = Clamp(i32(x * 2) + t - 3, 0, i32(src.w) - 1);
                const f32* s = src.At(u32(sx), y);
                for (u32 c = 0; c < 4; ++c)
                    o[c] += s[c] * weights[t];
            }
        }
    for (u32 y = 0; y < dst.h; ++y)
        for (u32 x = 0; x < dst.w; ++x) {
            f32* o = dst.At(x, y);
            for (int t = 0; t < 8; ++t) {
                const i32 sy = Clamp(i32(y * 2) + t - 3, 0, i32(tmp.h) - 1);
                const f32* s = tmp.At(x, u32(sy));
                for (u32 c = 0; c < 4; ++c)
                    o[c] += s[c] * weights[t];
            }
            for (u32 c = 0; c < 4; ++c)
                o[c] = Max(o[c], 0.0f); // sinc ringing can go negative
        }
    return dst;
}

f32 AlphaCoverage(const FloatImage& img, f32 cutoff, f32 scale)
{
    usize covered = 0;
    for (usize i = 0; i < usize(img.w) * img.h; ++i)
        covered += img.data[i * 4 + 3] * scale >= cutoff ? 1 : 0;
    return f32(covered) / f32(usize(img.w) * img.h);
}

void ScaleToCoverage(FloatImage& img, f32 cutoff, f32 target)
{
    // Binary search an alpha scale that reproduces the target coverage.
    f32 lo = 0.0f, hi = 4.0f, best = 1.0f;
    for (int i = 0; i < 12; ++i) {
        const f32 mid = (lo + hi) * 0.5f;
        if (AlphaCoverage(img, cutoff, mid) < target)
            lo = mid;
        else
            hi = mid;
        best = mid;
    }
    for (usize i = 0; i < usize(img.w) * img.h; ++i)
        img.data[i * 4 + 3] = Saturate(img.data[i * 4 + 3] * best);
}

void RenormalizeNormals(FloatImage& img)
{
    for (usize i = 0; i < usize(img.w) * img.h; ++i) {
        f32* p = &img.data[i * 4];
        Vec3 n(p[0] * 2.0f - 1.0f, p[1] * 2.0f - 1.0f, p[2] * 2.0f - 1.0f);
        n = SafeNormalize(n, Vec3(0, 0, 1));
        p[0] = n.x * 0.5f + 0.5f;
        p[1] = n.y * 0.5f + 0.5f;
        p[2] = n.z * 0.5f + 0.5f;
    }
}

} // namespace

Array<Image> GenerateMips(const Image& source, const MipOptions& options)
{
    ZX_ASSERT(source.format == ImageFormat::RGBA8, "GenerateMips expects RGBA8");
    Array<Image> mips;
    mips.PushBack(source);
    const bool srgb = options.srgb && !options.normalMap;
    FloatImage current = ToFloat(source, srgb);
    const f32 coverage = options.preserveAlphaCoverage ? AlphaCoverage(current, options.alphaCutoff, 1.0f) : 0.0f;
    while (current.w > 1 || current.h > 1) {
        FloatImage next = Downsample(current, options.filter);
        if (options.normalMap)
            RenormalizeNormals(next);
        FloatImage stored = next;
        if (options.preserveAlphaCoverage)
            ScaleToCoverage(stored, options.alphaCutoff, coverage);
        mips.PushBack(ToImage(stored, srgb));
        current = move(next); // keep filtering from unscaled data
    }
    return mips;
}

// --- Block compression ----------------------------------------------------------------------------------------------

u32 BlockBytes(BlockFormat f) { return (f == BlockFormat::BC1 || f == BlockFormat::BC4) ? 8 : 16; }

namespace {

u16 To565(Vec3 c)
{
    const u32 r = u32(Saturate(c.x) * 31.0f + 0.5f);
    const u32 g = u32(Saturate(c.y) * 63.0f + 0.5f);
    const u32 b = u32(Saturate(c.z) * 31.0f + 0.5f);
    return u16((r << 11) | (g << 5) | b);
}

Vec3 From565(u16 v)
{
    return Vec3(f32((v >> 11) & 31) / 31.0f, f32((v >> 5) & 63) / 63.0f, f32(v & 31) / 31.0f);
}

/// Color block (BC1 color part, also used by BC3). texels: 16 RGB in [0,1], mask of used texels.
void EncodeColorBlock(const Vec3 texels[16], const bool transparent[16], bool allowAlphaMode, u8* out)
{
    // Principal axis through the mean (power iteration on the covariance matrix), then endpoints = extremes.
    Vec3 mean;
    u32 count = 0;
    bool anyTransparent = false;
    for (u32 i = 0; i < 16; ++i) {
        if (transparent[i]) {
            anyTransparent = true;
            continue;
        }
        mean += texels[i];
        ++count;
    }
    const bool alphaMode = allowAlphaMode && anyTransparent;
    if (count == 0) {
        // Fully transparent BC1 block: 3-color mode with all indices = 3.
        out[0] = out[1] = out[2] = out[3] = 0;
        out[4] = out[5] = out[6] = out[7] = 0xFF;
        return;
    }
    mean = mean / f32(count);
    f32 cov[6] = {};
    for (u32 i = 0; i < 16; ++i) {
        if (transparent[i])
            continue;
        const Vec3 d = texels[i] - mean;
        cov[0] += d.x * d.x;
        cov[1] += d.x * d.y;
        cov[2] += d.x * d.z;
        cov[3] += d.y * d.y;
        cov[4] += d.y * d.z;
        cov[5] += d.z * d.z;
    }
    Vec3 axis(0.577f, 0.577f, 0.577f);
    for (int it = 0; it < 8; ++it) {
        const Vec3 n(cov[0] * axis.x + cov[1] * axis.y + cov[2] * axis.z, cov[1] * axis.x + cov[3] * axis.y + cov[4] * axis.z,
                     cov[2] * axis.x + cov[4] * axis.y + cov[5] * axis.z);
        const f32 len = Length(n);
        if (len < 1e-9f)
            break;
        axis = n / len;
    }
    f32 minT = Infinity, maxT = -Infinity;
    for (u32 i = 0; i < 16; ++i) {
        if (transparent[i])
            continue;
        const f32 t = Dot(texels[i] - mean, axis);
        minT = Min(minT, t);
        maxT = Max(maxT, t);
    }
    Vec3 e0 = mean + axis * maxT;
    Vec3 e1 = mean + axis * minT;
    u16 c0 = To565(e0), c1 = To565(e1);

    // Palette; 4-color mode requires c0 > c1, 3-color (+transparent) mode requires c0 <= c1.
    if (alphaMode) {
        if (c0 > c1)
            swap(c0, c1);
    } else {
        if (c0 < c1)
            swap(c0, c1);
    }
    const Vec3 p0 = From565(c0), p1 = From565(c1);
    Vec3 palette[4];
    u32 paletteCount;
    palette[0] = p0;
    palette[1] = p1;
    if (c0 > c1) {
        palette[2] = (p0 * 2.0f + p1) / 3.0f;
        palette[3] = (p0 + p1 * 2.0f) / 3.0f;
        paletteCount = 4;
    } else {
        palette[2] = (p0 + p1) * 0.5f;
        palette[3] = Vec3();
        paletteCount = 3; // index 3 = transparent black
    }
    u32 indices = 0;
    for (u32 i = 0; i < 16; ++i) {
        u32 best = 0;
        if (alphaMode && transparent[i]) {
            best = 3;
        } else {
            f32 bestDist = Infinity;
            for (u32 p = 0; p < paletteCount; ++p) {
                const f32 dist = DistanceSq(texels[i], palette[p]);
                if (dist < bestDist) {
                    bestDist = dist;
                    best = p;
                }
            }
        }
        indices |= best << (i * 2);
    }
    out[0] = u8(c0);
    out[1] = u8(c0 >> 8);
    out[2] = u8(c1);
    out[3] = u8(c1 >> 8);
    out[4] = u8(indices);
    out[5] = u8(indices >> 8);
    out[6] = u8(indices >> 16);
    out[7] = u8(indices >> 24);
}

/// BC4-style single channel block (also BC3 alpha / BC5 channels).
void EncodeChannelBlock(const u8 values[16], u8* out)
{
    u8 lo = 255, hi = 0;
    for (u32 i = 0; i < 16; ++i) {
        lo = Min(lo, values[i]);
        hi = Max(hi, values[i]);
    }
    // 8-value mode (e0 > e1) with interpolated palette.
    u8 e0 = hi, e1 = lo;
    f32 palette[8];
    if (e0 == e1) {
        for (f32& p : palette)
            p = e0;
    } else {
        palette[0] = e0;
        palette[1] = e1;
        for (u32 i = 1; i < 7; ++i)
            palette[i + 1] = (f32(7 - i) * e0 + f32(i) * e1) / 7.0f;
    }
    u64 bits = 0;
    for (u32 i = 0; i < 16; ++i) {
        u32 best = 0;
        f32 bestDist = Infinity;
        for (u32 p = 0; p < 8; ++p) {
            const f32 d = Abs(palette[p] - f32(values[i]));
            if (d < bestDist) {
                bestDist = d;
                best = p;
            }
        }
        bits |= u64(best) << (i * 3);
    }
    out[0] = e0;
    out[1] = e1;
    for (u32 i = 0; i < 6; ++i)
        out[2 + i] = u8(bits >> (i * 8));
}

void DecodeChannelBlock(const u8* in, u8 out[16])
{
    const u8 e0 = in[0], e1 = in[1];
    f32 palette[8];
    palette[0] = e0;
    palette[1] = e1;
    if (e0 > e1) {
        for (u32 i = 1; i < 7; ++i)
            palette[i + 1] = (f32(7 - i) * e0 + f32(i) * e1) / 7.0f;
    } else {
        for (u32 i = 1; i < 5; ++i)
            palette[i + 1] = (f32(5 - i) * e0 + f32(i) * e1) / 5.0f;
        palette[6] = 0.0f;
        palette[7] = 255.0f;
    }
    u64 bits = 0;
    for (u32 i = 0; i < 6; ++i)
        bits |= u64(in[2 + i]) << (i * 8);
    for (u32 i = 0; i < 16; ++i)
        out[i] = u8(palette[(bits >> (i * 3)) & 7] + 0.5f);
}

void DecodeColorBlock(const u8* in, u8 out[16][4], bool bc1)
{
    const u16 c0 = u16(in[0] | (in[1] << 8)), c1 = u16(in[2] | (in[3] << 8));
    const Vec3 p0 = From565(c0), p1 = From565(c1);
    Vec3 palette[4] = {p0, p1};
    bool transparent3 = false;
    if (c0 > c1 || !bc1) {
        palette[2] = (p0 * 2.0f + p1) / 3.0f;
        palette[3] = (p0 + p1 * 2.0f) / 3.0f;
    } else {
        palette[2] = (p0 + p1) * 0.5f;
        palette[3] = Vec3();
        transparent3 = true;
    }
    const u32 indices = u32(in[4]) | (u32(in[5]) << 8) | (u32(in[6]) << 16) | (u32(in[7]) << 24);
    for (u32 i = 0; i < 16; ++i) {
        const u32 idx = (indices >> (i * 2)) & 3;
        out[i][0] = u8(palette[idx].x * 255.0f + 0.5f);
        out[i][1] = u8(palette[idx].y * 255.0f + 0.5f);
        out[i][2] = u8(palette[idx].z * 255.0f + 0.5f);
        out[i][3] = (transparent3 && idx == 3) ? 0 : 255;
    }
}

} // namespace

Array<u8> CompressBlocks(const Image& image, BlockFormat format)
{
    ZX_ASSERT(image.format == ImageFormat::RGBA8, "CompressBlocks expects RGBA8");
    const u32 bw = (image.width + 3) / 4, bh = (image.height + 3) / 4;
    const u32 blockBytes = BlockBytes(format);
    Array<u8> out;
    out.ResizeZeroed(usize(bw) * bh * blockBytes);
    auto encodeRow = [&](u32 by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            u8 rgba[16][4];
            for (u32 i = 0; i < 16; ++i) {
                const u32 x = Min(bx * 4 + (i & 3), image.width - 1);
                const u32 y = Min(by * 4 + (i >> 2), image.height - 1);
                MemCopy(rgba[i], image.Row(y) + usize(x) * 4, 4);
            }
            u8* dst = &out[(usize(by) * bw + bx) * blockBytes];
            switch (format) {
            case BlockFormat::BC1:
            case BlockFormat::BC3: {
                Vec3 texels[16];
                bool transparent[16];
                for (u32 i = 0; i < 16; ++i) {
                    texels[i] = Vec3(rgba[i][0], rgba[i][1], rgba[i][2]) / 255.0f;
                    transparent[i] = format == BlockFormat::BC1 && rgba[i][3] < 128;
                }
                if (format == BlockFormat::BC3) {
                    u8 alpha[16];
                    for (u32 i = 0; i < 16; ++i)
                        alpha[i] = rgba[i][3];
                    EncodeChannelBlock(alpha, dst);
                    EncodeColorBlock(texels, transparent, false, dst + 8);
                } else {
                    EncodeColorBlock(texels, transparent, true, dst);
                }
                break;
            }
            case BlockFormat::BC4:
            case BlockFormat::BC5: {
                u8 channel[16];
                for (u32 i = 0; i < 16; ++i)
                    channel[i] = rgba[i][0];
                EncodeChannelBlock(channel, dst);
                if (format == BlockFormat::BC5) {
                    for (u32 i = 0; i < 16; ++i)
                        channel[i] = rgba[i][1];
                    EncodeChannelBlock(channel, dst + 8);
                }
                break;
            }
            }
        }
    };
    jobs::ParallelFor(bh, 8, [&](u32 begin, u32 end) {
        for (u32 by = begin; by < end; ++by)
            encodeRow(by);
    });
    return out;
}

Image DecompressBlocks(ByteSpan blocks, u32 width, u32 height, BlockFormat format)
{
    Image img = Image::Create(width, height, ImageFormat::RGBA8);
    const u32 bw = (width + 3) / 4, bh = (height + 3) / 4;
    const u32 blockBytes = BlockBytes(format);
    for (u32 by = 0; by < bh; ++by)
        for (u32 bx = 0; bx < bw; ++bx) {
            const u8* src = blocks.Data() + (usize(by) * bw + bx) * blockBytes;
            u8 rgba[16][4] = {};
            switch (format) {
            case BlockFormat::BC1: DecodeColorBlock(src, rgba, true); break;
            case BlockFormat::BC3: {
                DecodeColorBlock(src + 8, rgba, false);
                u8 a[16];
                DecodeChannelBlock(src, a);
                for (u32 i = 0; i < 16; ++i)
                    rgba[i][3] = a[i];
                break;
            }
            case BlockFormat::BC4:
            case BlockFormat::BC5: {
                u8 r[16], g[16] = {};
                DecodeChannelBlock(src, r);
                if (format == BlockFormat::BC5)
                    DecodeChannelBlock(src + 8, g);
                for (u32 i = 0; i < 16; ++i) {
                    rgba[i][0] = r[i];
                    rgba[i][1] = g[i];
                    rgba[i][2] = 0;
                    rgba[i][3] = 255;
                }
                break;
            }
            }
            for (u32 i = 0; i < 16; ++i) {
                const u32 x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < width && y < height)
                    MemCopy(img.Row(y) + usize(x) * 4, rgba[i], 4);
            }
        }
    return img;
}

} // namespace zx::texture
