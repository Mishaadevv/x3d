#include "Core/Math/Noise.h"
#include "Core/Math/Random.h"
#include "Core/Time/Time.h"
#include "Core/Threading/Thread.h"

namespace zx {

Random& ThreadRandom()
{
    thread_local Random rng(Clock::NowTicks() ^ (u64(Thread::CurrentId()) << 32), Thread::CurrentId());
    return rng;
}

namespace noise {

namespace {

ZX_FORCEINLINE u32 Hash(i32 x, i32 y, u32 seed)
{
    u32 h = u32(x) * 0x8da6b343u ^ u32(y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

ZX_FORCEINLINE u32 Hash(i32 x, i32 y, i32 z, u32 seed)
{
    u32 h = u32(x) * 0x8da6b343u ^ u32(y) * 0xd8163841u ^ u32(z) * 0xcb1ab31fu ^ seed * 0x165667b1u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

ZX_FORCEINLINE f32 Fade(f32 t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

ZX_FORCEINLINE f32 Grad2(u32 h, f32 x, f32 y)
{
    // 8 directions.
    switch (h & 7) {
    case 0:
        return x + y;
    case 1:
        return -x + y;
    case 2:
        return x - y;
    case 3:
        return -x - y;
    case 4:
        return x;
    case 5:
        return -x;
    case 6:
        return y;
    default:
        return -y;
    }
}

ZX_FORCEINLINE f32 Grad3(u32 h, f32 x, f32 y, f32 z)
{
    // Ken Perlin's 12 edge gradients (+4 repeats).
    const u32 g = h & 15;
    const f32 u = g < 8 ? x : y;
    const f32 v = g < 4 ? y : (g == 12 || g == 14 ? x : z);
    return ((g & 1) ? -u : u) + ((g & 2) ? -v : v);
}

} // namespace

f32 Perlin(Vec2 p, u32 seed)
{
    const i32 xi = FloorToInt(p.x), yi = FloorToInt(p.y);
    const f32 xf = p.x - f32(xi), yf = p.y - f32(yi);
    const f32 u = Fade(xf), v = Fade(yf);
    const f32 n00 = Grad2(Hash(xi, yi, seed), xf, yf);
    const f32 n10 = Grad2(Hash(xi + 1, yi, seed), xf - 1.0f, yf);
    const f32 n01 = Grad2(Hash(xi, yi + 1, seed), xf, yf - 1.0f);
    const f32 n11 = Grad2(Hash(xi + 1, yi + 1, seed), xf - 1.0f, yf - 1.0f);
    return Lerp(Lerp(n00, n10, u), Lerp(n01, n11, u), v) * 0.7071f;
}

f32 Perlin(Vec3 p, u32 seed)
{
    const i32 xi = FloorToInt(p.x), yi = FloorToInt(p.y), zi = FloorToInt(p.z);
    const f32 xf = p.x - f32(xi), yf = p.y - f32(yi), zf = p.z - f32(zi);
    const f32 u = Fade(xf), v = Fade(yf), w = Fade(zf);
    auto g = [&](i32 dx, i32 dy, i32 dz) {
        return Grad3(Hash(xi + dx, yi + dy, zi + dz, seed), xf - f32(dx), yf - f32(dy), zf - f32(dz));
    };
    const f32 x00 = Lerp(g(0, 0, 0), g(1, 0, 0), u);
    const f32 x10 = Lerp(g(0, 1, 0), g(1, 1, 0), u);
    const f32 x01 = Lerp(g(0, 0, 1), g(1, 0, 1), u);
    const f32 x11 = Lerp(g(0, 1, 1), g(1, 1, 1), u);
    return Lerp(Lerp(x00, x10, v), Lerp(x01, x11, v), w);
}

f32 Simplex(Vec2 p, u32 seed)
{
    constexpr f32 F2 = 0.36602540378f; // (sqrt(3) - 1) / 2
    constexpr f32 G2 = 0.21132486540f; // (3 - sqrt(3)) / 6
    const f32 s = (p.x + p.y) * F2;
    const i32 i = FloorToInt(p.x + s), j = FloorToInt(p.y + s);
    const f32 t = f32(i + j) * G2;
    const f32 x0 = p.x - (f32(i) - t), y0 = p.y - (f32(j) - t);
    const i32 i1 = x0 > y0 ? 1 : 0, j1 = x0 > y0 ? 0 : 1;
    const f32 x1 = x0 - f32(i1) + G2, y1 = y0 - f32(j1) + G2;
    const f32 x2 = x0 - 1.0f + 2.0f * G2, y2 = y0 - 1.0f + 2.0f * G2;
    auto corner = [&](f32 x, f32 y, i32 ci, i32 cj) {
        f32 tt = 0.5f - x * x - y * y;
        if (tt < 0.0f)
            return 0.0f;
        tt *= tt;
        return tt * tt * Grad2(Hash(ci, cj, seed), x, y);
    };
    return 70.0f * (corner(x0, y0, i, j) + corner(x1, y1, i + i1, j + j1) + corner(x2, y2, i + 1, j + 1)) * 0.5f;
}

f32 Simplex(Vec3 p, u32 seed)
{
    constexpr f32 F3 = 1.0f / 3.0f;
    constexpr f32 G3 = 1.0f / 6.0f;
    const f32 s = (p.x + p.y + p.z) * F3;
    const i32 i = FloorToInt(p.x + s), j = FloorToInt(p.y + s), k = FloorToInt(p.z + s);
    const f32 t = f32(i + j + k) * G3;
    const f32 x0 = p.x - (f32(i) - t), y0 = p.y - (f32(j) - t), z0 = p.z - (f32(k) - t);
    i32 i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0) {
            i1 = 1, j1 = 0, k1 = 0, i2 = 1, j2 = 1, k2 = 0;
        } else if (x0 >= z0) {
            i1 = 1, j1 = 0, k1 = 0, i2 = 1, j2 = 0, k2 = 1;
        } else {
            i1 = 0, j1 = 0, k1 = 1, i2 = 1, j2 = 0, k2 = 1;
        }
    } else {
        if (y0 < z0) {
            i1 = 0, j1 = 0, k1 = 1, i2 = 0, j2 = 1, k2 = 1;
        } else if (x0 < z0) {
            i1 = 0, j1 = 1, k1 = 0, i2 = 0, j2 = 1, k2 = 1;
        } else {
            i1 = 0, j1 = 1, k1 = 0, i2 = 1, j2 = 1, k2 = 0;
        }
    }
    auto corner = [&](f32 x, f32 y, f32 z, i32 ci, i32 cj, i32 ck) {
        f32 tt = 0.6f - x * x - y * y - z * z;
        if (tt < 0.0f)
            return 0.0f;
        tt *= tt;
        return tt * tt * Grad3(Hash(ci, cj, ck, seed), x, y, z);
    };
    const f32 n0 = corner(x0, y0, z0, i, j, k);
    const f32 n1 = corner(x0 - f32(i1) + G3, y0 - f32(j1) + G3, z0 - f32(k1) + G3, i + i1, j + j1, k + k1);
    const f32 n2 =
        corner(x0 - f32(i2) + 2.0f * G3, y0 - f32(j2) + 2.0f * G3, z0 - f32(k2) + 2.0f * G3, i + i2, j + j2, k + k2);
    const f32 n3 = corner(x0 - 1.0f + 3.0f * G3, y0 - 1.0f + 3.0f * G3, z0 - 1.0f + 3.0f * G3, i + 1, j + 1, k + 1);
    return 32.0f * (n0 + n1 + n2 + n3);
}

f32 Worley(Vec2 p, u32 seed)
{
    const i32 xi = FloorToInt(p.x), yi = FloorToInt(p.y);
    f32 best = 1e9f;
    for (i32 dy = -1; dy <= 1; ++dy)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const u32 h = Hash(xi + dx, yi + dy, seed);
            const Vec2 feature(f32(xi + dx) + HashToFloat(h), f32(yi + dy) + HashToFloat(h ^ 0x9e3779b9u));
            const f32 d = LengthSq(feature - p);
            best = d < best ? d : best;
        }
    return Sqrt(best);
}

f32 Worley(Vec3 p, u32 seed)
{
    const i32 xi = FloorToInt(p.x), yi = FloorToInt(p.y), zi = FloorToInt(p.z);
    f32 best = 1e9f;
    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dy = -1; dy <= 1; ++dy)
            for (i32 dx = -1; dx <= 1; ++dx) {
                const u32 h = Hash(xi + dx, yi + dy, zi + dz, seed);
                const Vec3 feature(f32(xi + dx) + HashToFloat(h), f32(yi + dy) + HashToFloat(h ^ 0x9e3779b9u),
                                   f32(zi + dz) + HashToFloat(h ^ 0x7f4a7c15u));
                const f32 d = LengthSq(feature - p);
                best = d < best ? d : best;
            }
    return Sqrt(best);
}

f32 Fbm(Vec2 p, u32 octaves, f32 lacunarity, f32 gain, u32 seed)
{
    f32 sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (u32 o = 0; o < octaves; ++o) {
        sum += Simplex(p, seed + o) * amp;
        norm += amp;
        amp *= gain;
        p = p * lacunarity;
    }
    return sum / norm;
}

f32 Fbm(Vec3 p, u32 octaves, f32 lacunarity, f32 gain, u32 seed)
{
    f32 sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (u32 o = 0; o < octaves; ++o) {
        sum += Simplex(p, seed + o) * amp;
        norm += amp;
        amp *= gain;
        p = p * lacunarity;
    }
    return sum / norm;
}

f32 Ridged(Vec2 p, u32 octaves, f32 lacunarity, f32 gain, u32 seed)
{
    f32 sum = 0.0f, amp = 1.0f, norm = 0.0f, weight = 1.0f;
    for (u32 o = 0; o < octaves; ++o) {
        f32 n = 1.0f - Abs(Simplex(p, seed + o));
        n *= n * weight;
        weight = Saturate(n * 2.0f);
        sum += n * amp;
        norm += amp;
        amp *= gain;
        p = p * lacunarity;
    }
    return sum / norm;
}

} // namespace noise
} // namespace zx
