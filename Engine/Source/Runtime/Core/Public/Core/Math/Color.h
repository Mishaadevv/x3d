#pragma once

#include "Core/Math/Vector.h"

namespace zx {

/// Linear-space RGBA color (HDR allowed). Used for lighting, materials and everything the GPU consumes.
struct LinearColor
{
    f32 r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;

    constexpr LinearColor() = default;
    constexpr LinearColor(f32 r_, f32 g_, f32 b_, f32 a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
    constexpr explicit LinearColor(Vec3 rgb, f32 a_ = 1.0f) : r(rgb.x), g(rgb.y), b(rgb.z), a(a_) {}
    constexpr explicit LinearColor(Vec4 v) : r(v.x), g(v.y), b(v.z), a(v.w) {}

    static constexpr LinearColor White() { return {1, 1, 1, 1}; }
    static constexpr LinearColor Black() { return {0, 0, 0, 1}; }
    static constexpr LinearColor Transparent() { return {0, 0, 0, 0}; }
    static constexpr LinearColor Red() { return {1, 0, 0, 1}; }
    static constexpr LinearColor Green() { return {0, 1, 0, 1}; }
    static constexpr LinearColor Blue() { return {0, 0, 1, 1}; }
    static constexpr LinearColor Yellow() { return {1, 1, 0, 1}; }

    /// From sRGB-encoded 8-bit components (e.g. color picker values).
    static LinearColor FromSRGB8(u8 r, u8 g, u8 b, u8 a = 255);
    /// From "#RRGGBB" / "#RRGGBBAA" / "RRGGBB" (sRGB). Returns magenta on parse error.
    static LinearColor FromHex(StringView hex);
    /// HSV in [0,1] (hue wraps) -> linear RGB (HSV is interpreted in sRGB space, as artists expect).
    static LinearColor FromHSV(f32 h, f32 s, f32 v, f32 a = 1.0f);
    /// Approximate blackbody color for a temperature in Kelvin (1000..40000), normalized, linear.
    static LinearColor FromTemperature(f32 kelvin);

    void ToHSV(f32& h, f32& s, f32& v) const;
    constexpr Vec3 Rgb() const { return {r, g, b}; }
    constexpr Vec4 ToVec4() const { return {r, g, b, a}; }
    /// Rec.709 relative luminance.
    constexpr f32 Luminance() const { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

    constexpr LinearColor operator+(const LinearColor& o) const { return {r + o.r, g + o.g, b + o.b, a + o.a}; }
    constexpr LinearColor operator-(const LinearColor& o) const { return {r - o.r, g - o.g, b - o.b, a - o.a}; }
    constexpr LinearColor operator*(const LinearColor& o) const { return {r * o.r, g * o.g, b * o.b, a * o.a}; }
    constexpr LinearColor operator*(f32 s) const { return {r * s, g * s, b * s, a * s}; }
    friend constexpr bool operator==(const LinearColor&, const LinearColor&) = default;
};

constexpr LinearColor Lerp(const LinearColor& a, const LinearColor& b, f32 t)
{
    return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), Lerp(a.a, b.a, t)};
}

/// 8-bit sRGB-encoded color (UI, vertex colors, textures).
struct Color
{
    u8 r = 0, g = 0, b = 0, a = 255;

    constexpr Color() = default;
    constexpr Color(u8 r_, u8 g_, u8 b_, u8 a_ = 255) : r(r_), g(g_), b(b_), a(a_) {}
    /// 0xRRGGBBAA
    static constexpr Color FromRGBA32(u32 v) { return {u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v)}; }
    static Color FromLinear(const LinearColor& c);
    LinearColor ToLinear() const { return LinearColor::FromSRGB8(r, g, b, a); }
    constexpr u32 ToRGBA32() const { return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | a; }
    /// Packed as bytes r,g,b,a in memory (DXGI R8G8B8A8).
    constexpr u32 ToPackedABGR() const { return (u32(a) << 24) | (u32(b) << 16) | (u32(g) << 8) | r; }
    friend constexpr bool operator==(const Color&, const Color&) = default;
};

f32 SRGBToLinear(f32 c);
f32 LinearToSRGB(f32 c);
/// Fast table lookup for 8-bit sRGB -> linear.
f32 SRGB8ToLinear(u8 c);

template <>
struct Formatter<LinearColor>
{
    static void Format(FormatSink& s, const LinearColor& c, const FormatSpec& spec)
    {
        Formatter<Vec4>::Format(s, c.ToVec4(), spec);
    }
};


// Reflection (defined in Core/Private/Reflection/CoreTypes.cpp).
class TypeInfo;
const TypeInfo& ZxTypeOf(LinearColor*);
const TypeInfo& ZxTypeOf(Color*);

} // namespace zx
