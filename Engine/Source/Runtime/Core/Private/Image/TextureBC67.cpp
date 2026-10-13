// BC6H (unsigned half-float RGB) and BC7 (RGBA) block encoders and decoders.
//
// BC6H: all 14 modes decode; the encoder searches the one-region modes 11-14 and, on the best candidate partitions,
// the two-region modes 1-10, fitting endpoints by PCA + least squares in the format's interpolation space (half-float
// bit patterns scaled to 16 bits, where BC6H interpolates linearly). Error is measured on the decoded half values.
// BC7: the encoder uses mode 6 (RGBA, 4-bit indices), mode 1 (two RGB subsets, 64 partitions) for opaque blocks and
// modes 4/5 (separate alpha, channel rotation); the decoder handles those modes.

#include "Core/Image/TextureTools.h"
#include "Core/Math/Packing.h"

#include <cmath>

namespace zx::texture {

namespace {

// --- Shared tables ------------------------------------------------------------------------------------------------------

// Two-subset partitions (bit i = subset of texel i, row-major) and the anchor texel of subset 1.
constexpr u16 Partitions2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
    0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};
constexpr u8 Anchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2, 8,
                            8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8,  2, 2,
                            2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15};

constexpr u8 Weights2[4] = {0, 21, 43, 64};
constexpr u8 Weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr u8 Weights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

const u8* WeightTable(u32 bits) { return bits == 2 ? Weights2 : bits == 3 ? Weights3 : Weights4; }
u32 Subset2(u32 partition, u32 texel) { return (Partitions2[partition] >> texel) & 1; }

/// Little-endian bit stream inside a 16-byte block.
struct BlockBits
{
    u8* data;
    u32 pos = 0;
    void Put(u32 value, u32 bits)
    {
        for (u32 i = 0; i < bits; ++i, ++pos)
            data[pos >> 3] |= u8(((value >> i) & 1) << (pos & 7));
    }
};

struct BlockReader
{
    const u8* data;
    u32 pos = 0;
    u32 Get(u32 bits)
    {
        u32 v = 0;
        for (u32 i = 0; i < bits; ++i, ++pos)
            v |= u32((data[pos >> 3] >> (pos & 7)) & 1) << i;
        return v;
    }
};

/// Principal axis of a point set (power iteration on the covariance). n = 3 or 4 dimensions.
template <u32 N>
void PrincipalAxis(const f32 (*points)[4], const u8* mask, u32 maskValue, u32 count, f32 mean[N], f32 axis[N], f32* residual = nullptr)
{
    f32 sum[N] = {};
    u32 n = 0;
    for (u32 i = 0; i < count; ++i)
        if (!mask || mask[i] == maskValue) {
            for (u32 c = 0; c < N; ++c)
                sum[c] += points[i][c];
            ++n;
        }
    for (u32 c = 0; c < N; ++c)
        mean[c] = n ? sum[c] / f32(n) : 0.0f;
    f32 cov[N][N] = {};
    for (u32 i = 0; i < count; ++i)
        if (!mask || mask[i] == maskValue) {
            f32 d[N];
            for (u32 c = 0; c < N; ++c)
                d[c] = points[i][c] - mean[c];
            for (u32 a = 0; a < N; ++a)
                for (u32 b = 0; b < N; ++b)
                    cov[a][b] += d[a] * d[b];
        }
    // Start from the largest-variance axis (avoids a start orthogonal to the answer).
    u32 best = 0;
    for (u32 c = 1; c < N; ++c)
        if (cov[c][c] > cov[best][best])
            best = c;
    for (u32 c = 0; c < N; ++c)
        axis[c] = c == best ? 1.0f : 0.0f;
    f32 lambda = 0.0f;
    for (u32 it = 0; it < 5; ++it) {
        f32 v[N] = {};
        for (u32 a = 0; a < N; ++a)
            for (u32 b = 0; b < N; ++b)
                v[a] += cov[a][b] * axis[b];
        f32 len = 0.0f;
        for (u32 c = 0; c < N; ++c)
            len += v[c] * v[c];
        len = std::sqrt(len);
        if (len < 1e-12f)
            break;
        lambda = len;
        for (u32 c = 0; c < N; ++c)
            axis[c] = v[c] / len;
    }
    if (residual) {
        f32 trace = 0.0f;
        for (u32 c = 0; c < N; ++c)
            trace += cov[c][c];
        *residual = Max(trace - lambda, 0.0f);
    }
}

/// Endpoints along the principal axis, refined by least squares for `indexBits`-bit interpolation weights.
template <u32 N>
void FitEndpoints(const f32 (*points)[4], const u8* mask, u32 maskValue, u32 count, u32 indexBits, f32 lo, f32 hi, f32 a[N], f32 b[N])
{
    f32 mean[N], axis[N];
    PrincipalAxis<N>(points, mask, maskValue, count, mean, axis);
    f32 tMin = 1e30f, tMax = -1e30f;
    for (u32 i = 0; i < count; ++i)
        if (!mask || mask[i] == maskValue) {
            f32 t = 0.0f;
            for (u32 c = 0; c < N; ++c)
                t += (points[i][c] - mean[c]) * axis[c];
            tMin = Min(tMin, t);
            tMax = Max(tMax, t);
        }
    if (tMin > tMax)
        tMin = tMax = 0.0f;
    for (u32 c = 0; c < N; ++c) {
        a[c] = Clamp(mean[c] + axis[c] * tMin, lo, hi);
        b[c] = Clamp(mean[c] + axis[c] * tMax, lo, hi);
    }
    const u8* weights = WeightTable(indexBits);
    const u32 levels = 1u << indexBits;
    for (u32 iter = 0; iter < 2; ++iter) {
        f32 aa = 0, ab = 0, bb = 0, xa[N] = {}, xb[N] = {};
        f32 dir[N];
        f32 dirLen = 0.0f;
        for (u32 c = 0; c < N; ++c) {
            dir[c] = b[c] - a[c];
            dirLen += dir[c] * dir[c];
        }
        if (dirLen < 1e-8f)
            return;
        for (u32 i = 0; i < count; ++i) {
            if (mask && mask[i] != maskValue)
                continue;
            f32 t = 0.0f;
            for (u32 c = 0; c < N; ++c)
                t += (points[i][c] - a[c]) * dir[c];
            t /= dirLen;
            // Nearest weight level.
            u32 best = 0;
            f32 bestD = 1e30f;
            for (u32 l = 0; l < levels; ++l) {
                const f32 d = Abs(f32(weights[l]) / 64.0f - t);
                if (d < bestD) {
                    bestD = d;
                    best = l;
                }
            }
            const f32 w = f32(weights[best]) / 64.0f, iw = 1.0f - w;
            aa += iw * iw;
            ab += iw * w;
            bb += w * w;
            for (u32 c = 0; c < N; ++c) {
                xa[c] += iw * points[i][c];
                xb[c] += w * points[i][c];
            }
        }
        const f32 det = aa * bb - ab * ab;
        if (Abs(det) < 1e-8f)
            return;
        for (u32 c = 0; c < N; ++c) {
            a[c] = Clamp((bb * xa[c] - ab * xb[c]) / det, lo, hi);
            b[c] = Clamp((aa * xb[c] - ab * xa[c]) / det, lo, hi);
        }
    }
}

// --- BC6H ----------------------------------------------------------------------------------------------------------------

struct Bc6Mode
{
    u8 modeBits;
    u8 modeValue;
    bool twoRegions;
    bool transformed;
    u8 epb;      // endpoint precision
    u8 delta[3]; // bits of the stored x/y/z endpoints per channel
    const char* layout; // bit order after the mode bits: <channel><endpoint>:<bit>|<from>-<to>
};

// Endpoints 0/1 = region 0 (w, x), 2/3 = region 1 (y, z). Layouts follow the D3D11 BC6H block format tables.
const Bc6Mode Bc6Modes[14] = {
    {2, 0x00, true, true, 10, {5, 5, 5},
     "g2:4 b2:4 b3:4 r0:0-9 g0:0-9 b0:0-9 r1:0-4 g3:4 g2:0-3 g1:0-4 b3:0 g3:0-3 b1:0-4 b3:1 b2:0-3 r2:0-4 b3:2 r3:0-4 b3:3"},
    {2, 0x01, true, true, 7, {6, 6, 6},
     "g2:5 g3:4 g3:5 r0:0-6 b3:0 b3:1 b2:4 g0:0-6 b2:5 b3:2 g2:4 b0:0-6 b3:3 b3:5 b3:4 r1:0-5 g2:0-3 g1:0-5 g3:0-3 b1:0-5 b2:0-3 "
     "r2:0-5 r3:0-5"},
    {5, 0x02, true, true, 11, {5, 4, 4},
     "r0:0-9 g0:0-9 b0:0-9 r1:0-4 r0:10 g2:0-3 g1:0-3 g0:10 b3:0 g3:0-3 b1:0-3 b0:10 b3:1 b2:0-3 r2:0-4 b3:2 r3:0-4 b3:3"},
    {5, 0x06, true, true, 11, {4, 5, 4},
     "r0:0-9 g0:0-9 b0:0-9 r1:0-3 r0:10 g3:4 g2:0-3 g1:0-4 g0:10 g3:0-3 b1:0-3 b0:10 b3:1 b2:0-3 r2:0-3 b3:0 b3:2 r3:0-3 g2:4 b3:3"},
    {5, 0x0A, true, true, 11, {4, 4, 5},
     "r0:0-9 g0:0-9 b0:0-9 r1:0-3 r0:10 b2:4 g2:0-3 g1:0-3 g0:10 b3:0 g3:0-3 b1:0-4 b0:10 b2:0-3 r2:0-3 b3:1 b3:2 r3:0-3 b3:4 b3:3"},
    {5, 0x0E, true, true, 9, {5, 5, 5},
     "r0:0-8 b2:4 g0:0-8 g2:4 b0:0-8 b3:4 r1:0-4 g3:4 g2:0-3 g1:0-4 b3:0 g3:0-3 b1:0-4 b3:1 b2:0-3 r2:0-4 b3:2 r3:0-4 b3:3"},
    {5, 0x12, true, true, 8, {6, 5, 5},
     "r0:0-7 g3:4 b2:4 g0:0-7 b3:2 g2:4 b0:0-7 b3:3 b3:4 r1:0-5 g2:0-3 g1:0-4 b3:0 g3:0-3 b1:0-4 b3:1 b2:0-3 r2:0-5 r3:0-5"},
    {5, 0x16, true, true, 8, {5, 6, 5},
     "r0:0-7 b3:0 b2:4 g0:0-7 g2:5 g2:4 b0:0-7 g3:5 b3:4 r1:0-4 g3:4 g2:0-3 g1:0-5 g3:0-3 b1:0-4 b3:1 b2:0-3 r2:0-4 b3:2 r3:0-4 b3:3"},
    {5, 0x1A, true, true, 8, {5, 5, 6},
     "r0:0-7 b3:1 b2:4 g0:0-7 b2:5 g2:4 b0:0-7 b3:5 b3:4 r1:0-4 g3:4 g2:0-3 g1:0-4 b3:0 g3:0-3 b1:0-5 b2:0-3 r2:0-4 b3:2 r3:0-4 b3:3"},
    {5, 0x1E, true, false, 6, {6, 6, 6},
     "r0:0-5 g3:4 b3:0 b3:1 b2:4 g0:0-5 g2:5 b2:5 b3:2 g2:4 b0:0-5 g3:5 b3:3 b3:5 b3:4 r1:0-5 g2:0-3 g1:0-5 g3:0-3 b1:0-5 b2:0-3 "
     "r2:0-5 r3:0-5"},
    {5, 0x03, false, false, 10, {10, 10, 10}, "r0:0-9 g0:0-9 b0:0-9 r1:0-9 g1:0-9 b1:0-9"},
    {5, 0x07, false, true, 11, {9, 9, 9}, "r0:0-9 g0:0-9 b0:0-9 r1:0-8 r0:10 g1:0-8 g0:10 b1:0-8 b0:10"},
    {5, 0x0B, false, true, 12, {8, 8, 8}, "r0:0-9 g0:0-9 b0:0-9 r1:0-7 r0:11-10 g1:0-7 g0:11-10 b1:0-7 b0:11-10"},
    {5, 0x0F, false, true, 16, {4, 4, 4}, "r0:0-9 g0:0-9 b0:0-9 r1:0-3 r0:15-10 g1:0-3 g0:15-10 b1:0-3 b0:15-10"},
};

struct Bc6Bit
{
    u8 field; // endpoint * 3 + channel (0..11), 12 = partition
    u8 bit;
};

struct Bc6Layouts
{
    Bc6Bit bits[14][82];
    u8 count[14];

    Bc6Layouts()
    {
        for (u32 m = 0; m < 14; ++m) {
            u32 n = 0;
            const char* s = Bc6Modes[m].layout;
            while (*s) {
                while (*s == ' ')
                    ++s;
                if (!*s)
                    break;
                const u32 channel = s[0] == 'r' ? 0 : s[0] == 'g' ? 1 : 2;
                const u32 endpoint = u32(s[1] - '0');
                s += 3; // "<c><e>:"
                auto number = [&s]() {
                    u32 v = 0;
                    while (*s >= '0' && *s <= '9')
                        v = v * 10 + u32(*s++ - '0');
                    return v;
                };
                const u32 from = number();
                u32 to = from;
                if (*s == '-') {
                    ++s;
                    to = number();
                }
                for (i32 b = i32(from);; b += from <= to ? 1 : -1) {
                    bits[m][n++] = Bc6Bit{u8(endpoint * 3 + channel), u8(b)};
                    if (b == i32(to))
                        break;
                }
            }
            if (Bc6Modes[m].twoRegions)
                for (u32 b = 0; b < 5; ++b)
                    bits[m][n++] = Bc6Bit{12, u8(b)};
            count[m] = u8(n);
            ZX_ASSERT(n + Bc6Modes[m].modeBits == (Bc6Modes[m].twoRegions ? 82u : 65u), "BC6H layout size");
        }
    }
};

const Bc6Layouts& GetBc6Layouts()
{
    static const Bc6Layouts layouts;
    return layouts;
}

i32 SignExtend(u32 v, u32 bits) { return i32(v << (32 - bits)) >> (32 - bits); }

u32 Bc6Unquantize(u32 comp, u32 epb)
{
    if (epb >= 15)
        return comp;
    if (comp == 0)
        return 0;
    if (comp == (1u << epb) - 1)
        return 0xFFFF;
    return ((comp << 16) + 0x8000) >> epb;
}

/// Quantized endpoint value whose unquantized value is closest to u (0..65535).
u32 Bc6Quantize(f32 u, u32 epb)
{
    const u32 maxC = (1u << epb) - 1;
    const i32 c0 = Clamp(i32(u * f32(1u << epb) / 65536.0f), 0, i32(maxC));
    u32 best = u32(c0);
    f32 bestD = 1e30f;
    for (i32 c = Max(c0 - 1, 0); c <= Min(c0 + 1, i32(maxC)); ++c) {
        const f32 d = Abs(f32(Bc6Unquantize(u32(c), epb)) - u);
        if (d < bestD) {
            bestD = d;
            best = u32(c);
        }
    }
    return best;
}

u16 Bc6Interpolate(u32 a, u32 b, u32 w) { return u16(((((64 - w) * a + w * b + 32) >> 6) * 31) >> 6); }

void PackBc6(u32 mode, const u32 stored[4][3], u32 partition, const u8 indices[16], u8* out)
{
    MemZero(out, 16);
    const Bc6Mode& m = Bc6Modes[mode];
    BlockBits w{out};
    w.Put(m.modeValue, m.modeBits);
    const Bc6Layouts& layouts = GetBc6Layouts();
    for (u32 i = 0; i < layouts.count[mode]; ++i) {
        const Bc6Bit& b = layouts.bits[mode][i];
        const u32 v = b.field == 12 ? partition : stored[b.field / 3][b.field % 3];
        w.Put((v >> b.bit) & 1, 1);
    }
    const u32 ib = m.twoRegions ? 3 : 4;
    const u32 anchor2 = m.twoRegions ? Anchor2[partition] : 0xFF;
    for (u32 t = 0; t < 16; ++t)
        w.Put(indices[t], ib - ((t == 0 || t == anchor2) ? 1 : 0));
}

/// Candidate encoding of a block in one mode; returns the error (sum of squared half-bit differences).
struct Bc6Texels
{
    f32 u[16][4];   // interpolation space: half bits * 64 / 31
    u16 half[16][3];
};

f32 EncodeBc6Mode(const Bc6Texels& tx, u32 mode, u32 partition, const f32 fit[4][3], u8* out)
{
    const Bc6Mode& m = Bc6Modes[mode];
    const u32 regions = m.twoRegions ? 2 : 1;
    const u32 ib = m.twoRegions ? 3 : 4;
    const u32 maxC = (1u << m.epb) - 1;
    u8 subset[16];
    for (u32 t = 0; t < 16; ++t)
        subset[t] = u8(m.twoRegions ? Subset2(partition, t) : 0);
    // Orientation: the anchor texel of each region must land in the lower half of the indices.
    f32 ends[4][3];
    for (u32 r = 0; r < regions; ++r) {
        const u32 anchor = r == 0 ? 0 : Anchor2[partition];
        f32 dir[3], len = 0.0f, t = 0.0f;
        for (u32 c = 0; c < 3; ++c) {
            dir[c] = fit[r * 2 + 1][c] - fit[r * 2][c];
            len += dir[c] * dir[c];
            t += (tx.u[anchor][c] - fit[r * 2][c]) * dir[c];
        }
        const bool swapEnds = len > 0.0f && t / len > 0.5f;
        for (u32 c = 0; c < 3; ++c) {
            ends[r * 2][c] = fit[r * 2 + (swapEnds ? 1 : 0)][c];
            ends[r * 2 + 1][c] = fit[r * 2 + (swapEnds ? 0 : 1)][c];
        }
    }
    // Quantize; transformed modes store x/y/z as deltas from w (clamped to the delta range).
    i32 q[4][3];
    u32 stored[4][3] = {};
    for (u32 e = 0; e < regions * 2; ++e)
        for (u32 c = 0; c < 3; ++c)
            q[e][c] = i32(Bc6Quantize(ends[e][c], m.epb));
    for (u32 c = 0; c < 3; ++c) {
        stored[0][c] = u32(q[0][c]);
        for (u32 e = 1; e < regions * 2; ++e) {
            if (m.transformed) {
                const i32 lim = 1 << (m.delta[c] - 1);
                const i32 d = Clamp(q[e][c] - q[0][c], -lim, lim - 1);
                q[e][c] = q[0][c] + d;
                stored[e][c] = u32(d) & ((1u << m.delta[c]) - 1);
            } else {
                stored[e][c] = u32(q[e][c]);
            }
        }
    }
    // Palettes (decoded half values) and indices.
    const u8* weights = WeightTable(ib);
    const u32 levels = 1u << ib;
    u16 palette[2][16][3];
    for (u32 r = 0; r < regions; ++r) {
        u32 ua[3], ub[3];
        for (u32 c = 0; c < 3; ++c) {
            ua[c] = Bc6Unquantize(u32(q[r * 2][c]) & maxC, m.epb);
            ub[c] = Bc6Unquantize(u32(q[r * 2 + 1][c]) & maxC, m.epb);
        }
        for (u32 l = 0; l < levels; ++l)
            for (u32 c = 0; c < 3; ++c)
                palette[r][l][c] = Bc6Interpolate(ua[c], ub[c], weights[l]);
    }
    u8 indices[16];
    f32 error = 0.0f;
    const u32 anchor2 = m.twoRegions ? Anchor2[partition] : 0xFF;
    for (u32 t = 0; t < 16; ++t) {
        const u32 r = subset[t];
        const u32 maxIndex = (t == 0 || t == anchor2) ? levels / 2 : levels;
        f32 bestD = 1e30f;
        u32 best = 0;
        for (u32 l = 0; l < maxIndex; ++l) {
            f32 d = 0.0f;
            for (u32 c = 0; c < 3; ++c) {
                const f32 diff = f32(palette[r][l][c]) - f32(tx.half[t][c]);
                d += diff * diff;
            }
            if (d < bestD) {
                bestD = d;
                best = l;
            }
        }
        indices[t] = u8(best);
        error += bestD;
    }
    PackBc6(mode, stored, partition, indices, out);
    return error;
}

} // namespace

void EncodeBC6HBlock(const f32 rgb[16][3], u8* out, i32 forceMode)
{
    Bc6Texels tx;
    for (u32 t = 0; t < 16; ++t)
        for (u32 c = 0; c < 3; ++c) {
            f32 v = rgb[t][c];
            v = (v == v) ? Clamp(v, 0.0f, 65504.0f) : 0.0f; // NaN -> 0; unsigned format
            tx.half[t][c] = FloatToHalf(v);
            if (tx.half[t][c] > 0x7BFF)
                tx.half[t][c] = 0x7BFF;
            tx.u[t][c] = f32(tx.half[t][c]) * (64.0f / 31.0f);
        }
    u8 candidate[16];
    f32 bestError = 1e30f;
    auto consider = [&](u32 mode, u32 partition, const f32 fit[4][3]) {
        const f32 e = EncodeBc6Mode(tx, mode, partition, fit, candidate);
        if (e < bestError) {
            bestError = e;
            MemCopy(out, candidate, 16);
        }
    };
    // One region.
    f32 fit1[4][3] = {};
    FitEndpoints<3>(tx.u, nullptr, 0, 16, 4, 0.0f, 65535.0f, fit1[0], fit1[1]);
    for (u32 mode = 10; mode < 14; ++mode)
        if (forceMode < 0 || forceMode == i32(mode))
            consider(mode, 0, fit1);
    if (forceMode >= 10 || (forceMode < 0 && bestError <= 192.0f))
        return; // exact enough (flat or smooth block)
    // Two regions: rank the 32 partitions by the residual off each subset's principal axis.
    struct Ranked
    {
        f32 residual;
        u32 partition;
    } ranked[32];
    for (u32 p = 0; p < 32; ++p) {
        u8 mask[16];
        for (u32 t = 0; t < 16; ++t)
            mask[t] = u8(Subset2(p, t));
        f32 mean[3], axis[3], r0 = 0.0f, r1 = 0.0f;
        PrincipalAxis<3>(tx.u, mask, 0, 16, mean, axis, &r0);
        PrincipalAxis<3>(tx.u, mask, 1, 16, mean, axis, &r1);
        ranked[p] = Ranked{r0 + r1, p};
    }
    for (u32 i = 0; i < 2; ++i)
        for (u32 j = i + 1; j < 32; ++j)
            if (ranked[j].residual < ranked[i].residual)
                swap(ranked[i], ranked[j]);
    for (u32 k = 0; k < 2; ++k) {
        const u32 p = ranked[k].partition;
        u8 mask[16];
        for (u32 t = 0; t < 16; ++t)
            mask[t] = u8(Subset2(p, t));
        f32 fit2[4][3];
        FitEndpoints<3>(tx.u, mask, 0, 16, 3, 0.0f, 65535.0f, fit2[0], fit2[1]);
        FitEndpoints<3>(tx.u, mask, 1, 16, 3, 0.0f, 65535.0f, fit2[2], fit2[3]);
        for (u32 mode = 0; mode < 10; ++mode)
            if (forceMode < 0 || forceMode == i32(mode))
                consider(mode, p, fit2);
    }
}

void DecodeBC6HBlock(const u8* in, u16 out[16][3])
{
    BlockReader r{in};
    u32 modeValue = r.Get(2);
    u32 bits = 2;
    if (modeValue >= 2) {
        modeValue |= r.Get(3) << 2;
        bits = 5;
    }
    i32 mode = -1;
    for (u32 m = 0; m < 14; ++m)
        if (Bc6Modes[m].modeBits == bits && Bc6Modes[m].modeValue == modeValue)
            mode = i32(m);
    if (mode < 0) {
        MemZero(out, sizeof(u16) * 48); // reserved mode: black
        return;
    }
    const Bc6Mode& m = Bc6Modes[mode];
    const Bc6Layouts& layouts = GetBc6Layouts();
    u32 ep[4][3] = {};
    u32 partition = 0;
    for (u32 i = 0; i < layouts.count[mode]; ++i) {
        const Bc6Bit& b = layouts.bits[mode][i];
        const u32 bit = r.Get(1);
        if (b.field == 12)
            partition |= bit << b.bit;
        else
            ep[b.field / 3][b.field % 3] |= bit << b.bit;
    }
    const u32 regions = m.twoRegions ? 2 : 1;
    const u32 mask = (1u << m.epb) - 1;
    if (m.transformed)
        for (u32 e = 1; e < regions * 2; ++e)
            for (u32 c = 0; c < 3; ++c)
                ep[e][c] = u32(i32(ep[0][c]) + SignExtend(ep[e][c], m.delta[c])) & mask;
    u32 unq[4][3];
    for (u32 e = 0; e < regions * 2; ++e)
        for (u32 c = 0; c < 3; ++c)
            unq[e][c] = Bc6Unquantize(ep[e][c], m.epb);
    const u32 ib = m.twoRegions ? 3 : 4;
    const u8* weights = WeightTable(ib);
    const u32 anchor2 = m.twoRegions ? Anchor2[partition] : 0xFF;
    for (u32 t = 0; t < 16; ++t) {
        const u32 idx = r.Get(ib - ((t == 0 || t == anchor2) ? 1 : 0));
        const u32 s = m.twoRegions ? Subset2(partition, t) : 0;
        for (u32 c = 0; c < 3; ++c)
            out[t][c] = Bc6Interpolate(unq[s * 2][c], unq[s * 2 + 1][c], weights[idx]);
    }
}

// --- BC7 -----------------------------------------------------------------------------------------------------------------

namespace {

struct Bc7Texels
{
    f32 v[16][4];
    u8 rgba[16][4];
};

u8 Interp8(u32 a, u32 b, u32 w) { return u8(((64 - w) * a + w * b + 32) >> 6); }

/// Mode 6: one subset, RGBA endpoints 7 bits + a p-bit each, 4-bit indices.
f32 EncodeBc7Mode6(const Bc7Texels& tx, u8* out)
{
    f32 a[4], b[4];
    FitEndpoints<4>(tx.v, nullptr, 0, 16, 4, 0.0f, 255.0f, a, b);
    f32 bestError = 1e30f;
    u32 bestEnds[2][4] = {}, bestP[2] = {};
    u8 bestIdx[16] = {};
    for (u32 pc = 0; pc < 4; ++pc) {
        const u32 p[2] = {pc & 1, pc >> 1};
        u32 q[2][4], e8[2][4];
        for (u32 c = 0; c < 4; ++c) {
            q[0][c] = u32(Clamp(i32(std::floor((a[c] - f32(p[0])) * 0.5f + 0.5f)), 0, 127));
            q[1][c] = u32(Clamp(i32(std::floor((b[c] - f32(p[1])) * 0.5f + 0.5f)), 0, 127));
            e8[0][c] = (q[0][c] << 1) | p[0];
            e8[1][c] = (q[1][c] << 1) | p[1];
        }
        u8 palette[16][4];
        for (u32 l = 0; l < 16; ++l)
            for (u32 c = 0; c < 4; ++c)
                palette[l][c] = Interp8(e8[0][c], e8[1][c], Weights4[l]);
        f32 error = 0.0f;
        u8 idx[16];
        for (u32 t = 0; t < 16; ++t) {
            f32 bestD = 1e30f;
            for (u32 l = 0; l < 16; ++l) {
                f32 d = 0.0f;
                for (u32 c = 0; c < 4; ++c) {
                    const f32 diff = f32(palette[l][c]) - f32(tx.rgba[t][c]);
                    d += diff * diff;
                }
                if (d < bestD) {
                    bestD = d;
                    idx[t] = u8(l);
                }
            }
            error += bestD;
        }
        if (error < bestError) {
            bestError = error;
            MemCopy(bestEnds, q, sizeof(q));
            bestP[0] = p[0];
            bestP[1] = p[1];
            MemCopy(bestIdx, idx, 16);
        }
    }
    if (bestIdx[0] >= 8) { // anchor: swap endpoints (with their p-bits) and invert the indices
        for (u32 c = 0; c < 4; ++c)
            swap(bestEnds[0][c], bestEnds[1][c]);
        swap(bestP[0], bestP[1]);
        for (u8& i : bestIdx)
            i = u8(15 - i);
    }
    MemZero(out, 16);
    BlockBits w{out};
    w.Put(1u << 6, 7);
    for (u32 c = 0; c < 4; ++c) {
        w.Put(bestEnds[0][c], 7);
        w.Put(bestEnds[1][c], 7);
    }
    w.Put(bestP[0], 1);
    w.Put(bestP[1], 1);
    for (u32 t = 0; t < 16; ++t)
        w.Put(bestIdx[t], t == 0 ? 3 : 4);
    return bestError;
}

/// Mode 1: two RGB subsets (partition), endpoints 6 bits + a shared p-bit per subset, 3-bit indices; alpha = 255.
f32 EncodeBc7Mode1(const Bc7Texels& tx, u32 partition, u8* out)
{
    u8 mask[16];
    for (u32 t = 0; t < 16; ++t)
        mask[t] = u8(Subset2(partition, t));
    u32 q[2][2][3] = {}, pbit[2] = {};
    u8 idx[16] = {};
    f32 total = 0.0f;
    for (u32 s = 0; s < 2; ++s) {
        f32 a[3], b[3];
        FitEndpoints<3>(tx.v, mask, s, 16, 3, 0.0f, 255.0f, a, b);
        f32 bestError = 1e30f;
        for (u32 p = 0; p < 2; ++p) {
            u32 qq[2][3], e8[2][3];
            for (u32 c = 0; c < 3; ++c) {
                const f32 ends[2] = {a[c], b[c]};
                for (u32 e = 0; e < 2; ++e) {
                    // 8-bit target -> 7-bit (6 bits + p) -> expanded back to 8 bits.
                    const f32 v7 = ends[e] * (127.0f / 255.0f);
                    qq[e][c] = u32(Clamp(i32(std::floor((v7 - f32(p)) * 0.5f + 0.5f)), 0, 63));
                    const u32 c7 = (qq[e][c] << 1) | p;
                    e8[e][c] = (c7 << 1) | (c7 >> 6);
                }
            }
            u8 palette[8][3];
            for (u32 l = 0; l < 8; ++l)
                for (u32 c = 0; c < 3; ++c)
                    palette[l][c] = Interp8(e8[0][c], e8[1][c], Weights3[l]);
            f32 error = 0.0f;
            u8 sidx[16] = {};
            for (u32 t = 0; t < 16; ++t) {
                if (mask[t] != s)
                    continue;
                f32 bestD = 1e30f;
                for (u32 l = 0; l < 8; ++l) {
                    f32 d = 0.0f;
                    for (u32 c = 0; c < 3; ++c) {
                        const f32 diff = f32(palette[l][c]) - f32(tx.rgba[t][c]);
                        d += diff * diff;
                    }
                    if (d < bestD) {
                        bestD = d;
                        sidx[t] = u8(l);
                    }
                }
                const f32 da = 255.0f - f32(tx.rgba[t][3]);
                error += bestD + da * da;
            }
            if (error < bestError) {
                bestError = error;
                MemCopy(q[s], qq, sizeof(qq));
                pbit[s] = p;
                for (u32 t = 0; t < 16; ++t)
                    if (mask[t] == s)
                        idx[t] = sidx[t];
            }
        }
        total += bestError;
        const u32 anchor = s == 0 ? 0 : Anchor2[partition];
        if (idx[anchor] >= 4) {
            for (u32 c = 0; c < 3; ++c)
                swap(q[s][0][c], q[s][1][c]);
            for (u32 t = 0; t < 16; ++t)
                if (mask[t] == s)
                    idx[t] = u8(7 - idx[t]);
        }
    }
    MemZero(out, 16);
    BlockBits w{out};
    w.Put(1u << 1, 2);
    w.Put(partition, 6);
    for (u32 c = 0; c < 3; ++c)
        for (u32 s = 0; s < 2; ++s) {
            w.Put(q[s][0][c], 6);
            w.Put(q[s][1][c], 6);
        }
    w.Put(pbit[0], 1);
    w.Put(pbit[1], 1);
    const u32 anchor2 = Anchor2[partition];
    for (u32 t = 0; t < 16; ++t)
        w.Put(idx[t], (t == 0 || t == anchor2) ? 2 : 3);
    return total;
}

/// Modes 4 and 5: one subset with separate color and alpha endpoints/indices. `rotation` swaps alpha with R/G/B
/// (1/2/3) so an uncorrelated color channel gets its own indices. Mode 5: color 7 bits, alpha 8 bits, 2-bit indices for
/// both. Mode 4: color 5 bits, alpha 6 bits, one 2-bit and one 3-bit index set (idxMode 1 = color uses the 3-bit set).
f32 EncodeBc7SeparateAlpha(const Bc7Texels& src, u32 mode, u32 rotation, u32 idxMode, u8* out)
{
    Bc7Texels tx = src;
    if (rotation)
        for (u32 t = 0; t < 16; ++t) {
            swap(tx.rgba[t][3], tx.rgba[t][rotation - 1]);
            swap(tx.v[t][3], tx.v[t][rotation - 1]);
        }
    const u32 colorBits = mode == 5 ? 7 : 5, alphaBits = mode == 5 ? 8 : 6;
    const u32 colorIndexBits = mode == 5 ? 2 : (idxMode ? 3 : 2), alphaIndexBits = mode == 5 ? 2 : (idxMode ? 2 : 3);
    auto expand = [](u32 v, u32 bits) { return bits == 8 ? v : (v << (8 - bits)) | (v >> (2 * bits - 8)); };
    auto quantize = [](f32 v, u32 bits) { return u32(Clamp(i32(v * f32((1u << bits) - 1) / 255.0f + 0.5f), 0, i32((1u << bits) - 1))); };
    f32 a[3], b[3];
    FitEndpoints<3>(tx.v, nullptr, 0, 16, colorIndexBits, 0.0f, 255.0f, a, b);
    u32 q[2][3], e8[2][3];
    for (u32 c = 0; c < 3; ++c) {
        q[0][c] = quantize(a[c], colorBits);
        q[1][c] = quantize(b[c], colorBits);
        e8[0][c] = expand(q[0][c], colorBits);
        e8[1][c] = expand(q[1][c], colorBits);
    }
    f32 alpha[16][4];
    for (u32 t = 0; t < 16; ++t)
        alpha[t][0] = tx.v[t][3];
    f32 aa[1], ab[1];
    FitEndpoints<1>(alpha, nullptr, 0, 16, alphaIndexBits, 0.0f, 255.0f, aa, ab);
    u32 qa[2] = {quantize(aa[0], alphaBits), quantize(ab[0], alphaBits)};
    const u32 ea[2] = {expand(qa[0], alphaBits), expand(qa[1], alphaBits)};
    const u8* cw = WeightTable(colorIndexBits);
    const u8* aw = WeightTable(alphaIndexBits);
    const u32 cl = 1u << colorIndexBits, al = 1u << alphaIndexBits;
    u8 cidx[16], aidx[16];
    f32 error = 0.0f;
    for (u32 t = 0; t < 16; ++t) {
        f32 bestD = 1e30f;
        for (u32 l = 0; l < cl; ++l) {
            f32 d = 0.0f;
            for (u32 c = 0; c < 3; ++c) {
                const f32 diff = f32(Interp8(e8[0][c], e8[1][c], cw[l])) - f32(tx.rgba[t][c]);
                d += diff * diff;
            }
            if (d < bestD) {
                bestD = d;
                cidx[t] = u8(l);
            }
        }
        f32 bestA = 1e30f;
        for (u32 l = 0; l < al; ++l) {
            const f32 diff = f32(Interp8(ea[0], ea[1], aw[l])) - f32(tx.rgba[t][3]);
            if (diff * diff < bestA) {
                bestA = diff * diff;
                aidx[t] = u8(l);
            }
        }
        error += bestD + bestA;
    }
    if (cidx[0] >= cl / 2) {
        for (u32 c = 0; c < 3; ++c)
            swap(q[0][c], q[1][c]);
        for (u8& i : cidx)
            i = u8(cl - 1 - i);
    }
    if (aidx[0] >= al / 2) {
        swap(qa[0], qa[1]);
        for (u8& i : aidx)
            i = u8(al - 1 - i);
    }
    MemZero(out, 16);
    BlockBits w{out};
    w.Put(1u << mode, mode + 1);
    w.Put(rotation, 2);
    if (mode == 4)
        w.Put(idxMode, 1);
    for (u32 c = 0; c < 3; ++c) {
        w.Put(q[0][c], colorBits);
        w.Put(q[1][c], colorBits);
    }
    w.Put(qa[0], alphaBits);
    w.Put(qa[1], alphaBits);
    // Index sets in block order: mode 5 color then alpha; mode 4 the 2-bit set then the 3-bit set.
    const bool colorFirst = mode == 5 || idxMode == 0;
    const u8* first = colorFirst ? cidx : aidx;
    const u8* second = colorFirst ? aidx : cidx;
    const u32 firstBits = colorFirst ? colorIndexBits : alphaIndexBits, secondBits = colorFirst ? alphaIndexBits : colorIndexBits;
    for (u32 t = 0; t < 16; ++t)
        w.Put(first[t], firstBits - (t == 0 ? 1 : 0));
    for (u32 t = 0; t < 16; ++t)
        w.Put(second[t], secondBits - (t == 0 ? 1 : 0));
    return error;
}

} // namespace

void EncodeBC7Block(const u8 rgba[16][4], u8* out, i32 forceMode)
{
    Bc7Texels tx;
    bool opaque = true;
    for (u32 t = 0; t < 16; ++t) {
        for (u32 c = 0; c < 4; ++c) {
            tx.rgba[t][c] = rgba[t][c];
            tx.v[t][c] = f32(rgba[t][c]);
        }
        opaque &= rgba[t][3] == 255;
    }
    u8 candidate[16];
    f32 bestError = 1e30f;
    auto keep = [&](f32 e) {
        if (e < bestError) {
            bestError = e;
            MemCopy(out, candidate, 16);
        }
    };
    if (forceMode < 0 || forceMode == 6)
        keep(EncodeBc7Mode6(tx, candidate));
    // Separate alpha (modes 4/5), every rotation: for alpha that doesn't follow the color, or one odd color channel.
    if (forceMode == 4 || forceMode == 5 || forceMode < 0)
        for (u32 rotation = 0; rotation < 4; ++rotation) {
            if (opaque && rotation == 0 && forceMode < 0)
                continue; // nothing to separate
            if (forceMode != 4)
                keep(EncodeBc7SeparateAlpha(tx, 5, rotation, 0, candidate));
            if (forceMode != 5)
                for (u32 idxMode = 0; idxMode < 2; ++idxMode)
                    keep(EncodeBc7SeparateAlpha(tx, 4, rotation, idxMode, candidate));
        }
    if ((forceMode == 1 || (forceMode < 0 && opaque)) && bestError > 0.0f) {
        struct Ranked
        {
            f32 residual;
            u32 partition;
        } ranked[64];
        for (u32 p = 0; p < 64; ++p) {
            u8 mask[16];
            for (u32 t = 0; t < 16; ++t)
                mask[t] = u8(Subset2(p, t));
            f32 mean[3], axis[3], r0 = 0.0f, r1 = 0.0f;
            PrincipalAxis<3>(tx.v, mask, 0, 16, mean, axis, &r0);
            PrincipalAxis<3>(tx.v, mask, 1, 16, mean, axis, &r1);
            ranked[p] = Ranked{r0 + r1, p};
        }
        for (u32 i = 0; i < 4; ++i)
            for (u32 j = i + 1; j < 64; ++j)
                if (ranked[j].residual < ranked[i].residual)
                    swap(ranked[i], ranked[j]);
        for (u32 k = 0; k < 4; ++k)
            keep(EncodeBc7Mode1(tx, ranked[k].partition, candidate));
    }
}

bool DecodeBC7Block(const u8* in, u8 out[16][4])
{
    BlockReader r{in};
    u32 mode = 0;
    while (mode < 8 && r.Get(1) == 0)
        ++mode;
    auto fail = [&] {
        for (u32 t = 0; t < 16; ++t) {
            out[t][0] = 255;
            out[t][1] = 0;
            out[t][2] = 255;
            out[t][3] = 255;
        }
        return false;
    };
    if (mode == 6) {
        u32 e[2][4];
        for (u32 c = 0; c < 4; ++c) {
            e[0][c] = r.Get(7);
            e[1][c] = r.Get(7);
        }
        const u32 p0 = r.Get(1), p1 = r.Get(1);
        for (u32 c = 0; c < 4; ++c) {
            e[0][c] = (e[0][c] << 1) | p0;
            e[1][c] = (e[1][c] << 1) | p1;
        }
        for (u32 t = 0; t < 16; ++t) {
            const u32 idx = r.Get(t == 0 ? 3 : 4);
            for (u32 c = 0; c < 4; ++c)
                out[t][c] = Interp8(e[0][c], e[1][c], Weights4[idx]);
        }
        return true;
    }
    if (mode == 1) {
        const u32 partition = r.Get(6);
        u32 e[4][3];
        for (u32 c = 0; c < 3; ++c)
            for (u32 k = 0; k < 4; ++k)
                e[k][c] = r.Get(6);
        const u32 p[2] = {r.Get(1), r.Get(1)};
        for (u32 k = 0; k < 4; ++k)
            for (u32 c = 0; c < 3; ++c) {
                const u32 c7 = (e[k][c] << 1) | p[k / 2];
                e[k][c] = (c7 << 1) | (c7 >> 6);
            }
        const u32 anchor2 = Anchor2[partition];
        for (u32 t = 0; t < 16; ++t) {
            const u32 idx = r.Get((t == 0 || t == anchor2) ? 2 : 3);
            const u32 s = Subset2(partition, t);
            for (u32 c = 0; c < 3; ++c)
                out[t][c] = Interp8(e[s * 2][c], e[s * 2 + 1][c], Weights3[idx]);
            out[t][3] = 255;
        }
        return true;
    }
    if (mode == 4 || mode == 5) {
        const u32 rotation = r.Get(2);
        const u32 idxMode = mode == 4 ? r.Get(1) : 0;
        const u32 colorBits = mode == 5 ? 7 : 5, alphaBits = mode == 5 ? 8 : 6;
        auto expand = [](u32 v, u32 bits) { return bits == 8 ? v : (v << (8 - bits)) | (v >> (2 * bits - 8)); };
        u32 e[2][4];
        for (u32 c = 0; c < 3; ++c) {
            e[0][c] = expand(r.Get(colorBits), colorBits);
            e[1][c] = expand(r.Get(colorBits), colorBits);
        }
        e[0][3] = expand(r.Get(alphaBits), alphaBits);
        e[1][3] = expand(r.Get(alphaBits), alphaBits);
        const u32 firstBits = 2, secondBits = mode == 5 ? 2 : 3;
        u32 first[16], second[16];
        for (u32 t = 0; t < 16; ++t)
            first[t] = r.Get(firstBits - (t == 0 ? 1 : 0));
        for (u32 t = 0; t < 16; ++t)
            second[t] = r.Get(secondBits - (t == 0 ? 1 : 0));
        const bool colorFirst = idxMode == 0;
        const u8* cw = WeightTable(colorFirst ? firstBits : secondBits);
        const u8* aw = WeightTable(colorFirst ? secondBits : firstBits);
        for (u32 t = 0; t < 16; ++t) {
            const u32 ci = colorFirst ? first[t] : second[t], ai = colorFirst ? second[t] : first[t];
            for (u32 c = 0; c < 3; ++c)
                out[t][c] = Interp8(e[0][c], e[1][c], cw[ci]);
            out[t][3] = Interp8(e[0][3], e[1][3], aw[ai]);
            if (rotation)
                swap(out[t][3], out[t][rotation - 1]);
        }
        return true;
    }
    return fail();
}

} // namespace zx::texture
