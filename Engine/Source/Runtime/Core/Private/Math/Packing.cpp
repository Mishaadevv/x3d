#include "Core/Math/Packing.h"

namespace zx {

u16 FloatToHalf(f32 value)
{
    const u32 f = std::bit_cast<u32>(value);
    const u32 sign = (f >> 16) & 0x8000u;
    const u32 absF = f & 0x7FFFFFFFu;

    if (absF >= 0x7F800000u) // inf or nan
        return u16(sign | 0x7C00u | (absF > 0x7F800000u ? 0x200u : 0u));
    if (absF >= 0x477FF000u) // overflow after rounding -> inf
        return u16(sign | 0x7C00u);
    if (absF < 0x38800000u) {
        // Denormal or zero in half precision.
        if (absF < 0x33000000u)
            return u16(sign); // underflow to zero
        const u32 shift = 126u - (absF >> 23); // 14..24 (for exponents -15..-25)
        const u32 mant = (absF & 0x7FFFFFu) | 0x800000u;
        u32 h = mant >> shift;
        // Round to nearest even.
        const u32 rem = mant & ((1u << shift) - 1u);
        const u32 halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (h & 1u)))
            ++h;
        return u16(sign | h);
    }
    // Normal: rebias exponent and round mantissa from 23 to 10 bits (nearest even).
    u32 h = ((absF - 0x38000000u) >> 13);
    const u32 rem = absF & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u)))
        ++h;
    return u16(sign | h);
}

f32 HalfToFloat(u16 half)
{
    const u32 sign = u32(half & 0x8000u) << 16;
    const u32 exp = (half >> 10) & 0x1Fu;
    u32 mant = half & 0x3FFu;
    u32 f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign;
        } else {
            // Denormal: normalize.
            u32 e = 113; // 127 - 15 + 1
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                --e;
            }
            mant &= 0x3FFu;
            f = sign | (e << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = sign | 0x7F800000u | (mant << 13);
    } else {
        f = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    return std::bit_cast<f32>(f);
}

Vec2 OctahedralEncode(Vec3 n)
{
    const f32 l1 = Abs(n.x) + Abs(n.y) + Abs(n.z);
    Vec2 p(n.x / l1, n.y / l1);
    if (n.z < 0.0f) {
        const Vec2 s(p.x >= 0.0f ? 1.0f : -1.0f, p.y >= 0.0f ? 1.0f : -1.0f);
        p = Vec2((1.0f - Abs(p.y)) * s.x, (1.0f - Abs(p.x)) * s.y);
    }
    return p;
}

Vec3 OctahedralDecode(Vec2 e)
{
    Vec3 n(e.x, e.y, 1.0f - Abs(e.x) - Abs(e.y));
    const f32 t = Saturate(-n.z);
    n.x += n.x >= 0.0f ? -t : t;
    n.y += n.y >= 0.0f ? -t : t;
    return Normalize(n);
}

u32 PackNormalOct16(Vec3 n)
{
    const Vec2 e = OctahedralEncode(n);
    return u32(u16(i16(PackSnorm16(e.x)))) | (u32(u16(i16(PackSnorm16(e.y)))) << 16);
}

Vec3 UnpackNormalOct16(u32 p)
{
    return OctahedralDecode(Vec2(UnpackSnorm16(i16(u16(p))), UnpackSnorm16(i16(u16(p >> 16)))));
}

namespace {
// Packs a positive float into an unsigned small float with `mantBits` mantissa bits and 5-bit exponent.
u32 PackSmallFloat(f32 v, u32 mantBits)
{
    if (!(v > 0.0f))
        return 0; // negatives / nan -> 0
    const u32 maxBits = (0x1Eu << mantBits) | ((1u << mantBits) - 1u); // largest finite
    const u32 f = std::bit_cast<u32>(v);
    if (f >= 0x7F800000u)
        return maxBits;
    const i32 exp = i32((f >> 23) & 0xFF) - 127 + 15;
    if (exp >= 31)
        return maxBits;
    if (exp <= 0) {
        // Denormal.
        const u32 mant = (f & 0x7FFFFFu) | 0x800000u;
        const i32 shift = 23 - i32(mantBits) + 1 - exp;
        return shift < 32 ? (mant >> shift) : 0;
    }
    u32 r = (u32(exp) << mantBits) | ((f & 0x7FFFFFu) >> (23 - mantBits));
    // Round half up on the dropped bit.
    if (f & (1u << (22 - mantBits)))
        ++r;
    return r > maxBits ? maxBits : r;
}

f32 UnpackSmallFloat(u32 bits, u32 mantBits)
{
    const u32 exp = bits >> mantBits;
    const u32 mant = bits & ((1u << mantBits) - 1u);
    if (exp == 0)
        return f32(mant) / f32(1u << mantBits) * Exp2(-14.0f);
    if (exp == 31)
        return Infinity;
    return (1.0f + f32(mant) / f32(1u << mantBits)) * Exp2(f32(i32(exp) - 15));
}
} // namespace

u32 PackR11G11B10F(Vec3 rgb)
{
    return (PackSmallFloat(rgb.x, 6) & 0x7FF) | ((PackSmallFloat(rgb.y, 6) & 0x7FF) << 11) |
           ((PackSmallFloat(rgb.z, 5) & 0x3FF) << 22);
}

Vec3 UnpackR11G11B10F(u32 p)
{
    return {UnpackSmallFloat(p & 0x7FF, 6), UnpackSmallFloat((p >> 11) & 0x7FF, 6), UnpackSmallFloat(p >> 22, 5)};
}

u32 PackRGB9E5(Vec3 rgb)
{
    constexpr f32 MaxValue = 65408.0f; // (2^9-1)/2^9 * 2^16
    const f32 r = Clamp(rgb.x, 0.0f, MaxValue);
    const f32 g = Clamp(rgb.y, 0.0f, MaxValue);
    const f32 b = Clamp(rgb.z, 0.0f, MaxValue);
    const f32 maxc = Max(r, Max(g, b));
    i32 expShared = Max(-16, FloorToInt(Log2(Max(maxc, 1e-30f)))) + 1 + 15;
    f32 denom = Exp2(f32(expShared - 15 - 9));
    if (i32(maxc / denom + 0.5f) == 512) {
        denom *= 2.0f;
        ++expShared;
    }
    const u32 rm = u32(r / denom + 0.5f), gm = u32(g / denom + 0.5f), bm = u32(b / denom + 0.5f);
    return (rm & 0x1FF) | ((gm & 0x1FF) << 9) | ((bm & 0x1FF) << 18) | (u32(expShared) << 27);
}

Vec3 UnpackRGB9E5(u32 p)
{
    const f32 scale = Exp2(f32(i32(p >> 27) - 15 - 9));
    return {f32(p & 0x1FF) * scale, f32((p >> 9) & 0x1FF) * scale, f32((p >> 18) & 0x1FF) * scale};
}

} // namespace zx
