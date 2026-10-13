#pragma once

// Bit-packing helpers shared with shaders (keep in sync with Engine/Shaders/Common/Packing.hlsli).

#include "Core/Math/Vector.h"

namespace zx {

/// IEEE 754 binary16 conversion (round-to-nearest-even, handles inf/nan/denormals).
u16 FloatToHalf(f32 value);
f32 HalfToFloat(u16 half);

struct Half
{
    u16 bits = 0;
    Half() = default;
    Half(f32 v) : bits(FloatToHalf(v)) {}
    operator f32() const { return HalfToFloat(bits); }
};

constexpr u32 PackUnorm8(f32 v) { return u32(Saturate(v) * 255.0f + 0.5f); }
constexpr u32 PackUnorm16(f32 v) { return u32(Saturate(v) * 65535.0f + 0.5f); }
constexpr f32 UnpackUnorm8(u32 v) { return f32(v & 0xFF) / 255.0f; }
constexpr i32 PackSnorm8(f32 v)
{
    const f32 c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    return i32(c * 127.0f + (c >= 0.0f ? 0.5f : -0.5f));
}
constexpr i32 PackSnorm16(f32 v)
{
    const f32 c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    return i32(c * 32767.0f + (c >= 0.0f ? 0.5f : -0.5f));
}
constexpr f32 UnpackSnorm16(i16 v) { return v < -32767 ? -1.0f : f32(v) / 32767.0f; }

/// RGBA8 unorm packed with r in the lowest byte.
constexpr u32 PackUnorm4x8(Vec4 v)
{
    return PackUnorm8(v.x) | (PackUnorm8(v.y) << 8) | (PackUnorm8(v.z) << 16) | (PackUnorm8(v.w) << 24);
}
constexpr Vec4 UnpackUnorm4x8(u32 p) { return {UnpackUnorm8(p), UnpackUnorm8(p >> 8), UnpackUnorm8(p >> 16), UnpackUnorm8(p >> 24)}; }

/// Octahedral encoding of a unit vector into [-1, 1]^2.
Vec2 OctahedralEncode(Vec3 n);
Vec3 OctahedralDecode(Vec2 e);
/// Octahedral normal packed into 2x16-bit snorm (u32).
u32 PackNormalOct16(Vec3 n);
Vec3 UnpackNormalOct16(u32 packed);

/// DXGI_FORMAT_R11G11B10_FLOAT packing (positive values only).
u32 PackR11G11B10F(Vec3 rgb);
Vec3 UnpackR11G11B10F(u32 packed);

/// DXGI_FORMAT_R9G9B9E5_SHAREDEXP packing.
u32 PackRGB9E5(Vec3 rgb);
Vec3 UnpackRGB9E5(u32 packed);

/// Two halves in a u32 (x low, y high).
inline u32 PackHalf2(Vec2 v) { return u32(FloatToHalf(v.x)) | (u32(FloatToHalf(v.y)) << 16); }
inline Vec2 UnpackHalf2(u32 p) { return {HalfToFloat(u16(p)), HalfToFloat(u16(p >> 16))}; }

} // namespace zx
