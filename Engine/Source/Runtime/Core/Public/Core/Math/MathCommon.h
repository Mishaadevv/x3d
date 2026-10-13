#pragma once

// Scalar math helpers and constants.
// Engine conventions (see TZ ADR-002/003): left-handed, Y-up, +Z forward, +X right, meters, radians in code
// (degrees only in UI/serialized editor data), column vectors (v' = M * v), column-major storage, reversed-Z depth.

#include "Core/Types.h"

#include <bit>
#include <cmath>

namespace zx {

inline constexpr f32 Pi = 3.14159265358979323846f;
inline constexpr f32 TwoPi = 6.28318530717958647692f;
inline constexpr f32 HalfPi = 1.57079632679489661923f;
inline constexpr f32 InvPi = 0.31830988618379067154f;
inline constexpr f32 DegToRadFactor = Pi / 180.0f;
inline constexpr f32 RadToDegFactor = 180.0f / Pi;
inline constexpr f32 SmallNumber = 1e-8f;
inline constexpr f32 KindaSmallNumber = 1e-4f;
inline constexpr f32 Infinity = __builtin_huge_valf();

constexpr f32 DegToRad(f32 deg) { return deg * DegToRadFactor; }
constexpr f32 RadToDeg(f32 rad) { return rad * RadToDegFactor; }

inline f32 Abs(f32 v) { return std::fabs(v); }
inline f64 Abs(f64 v) { return std::fabs(v); }
inline i32 Abs(i32 v) { return v < 0 ? -v : v; }
inline f32 Sqrt(f32 v) { return std::sqrt(v); }
inline f32 InvSqrt(f32 v) { return 1.0f / std::sqrt(v); }
inline f32 Sin(f32 v) { return std::sin(v); }
inline f32 Cos(f32 v) { return std::cos(v); }
inline f32 Tan(f32 v) { return std::tan(v); }
inline f32 Asin(f32 v) { return std::asin(v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v)); }
inline f32 Acos(f32 v) { return std::acos(v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v)); }
inline f32 Atan(f32 v) { return std::atan(v); }
inline f32 Atan2(f32 y, f32 x) { return std::atan2(y, x); }
inline f32 Pow(f32 b, f32 e) { return std::pow(b, e); }
inline f32 Exp(f32 v) { return std::exp(v); }
inline f32 Exp2(f32 v) { return std::exp2(v); }
/// Natural logarithm (named Ln to avoid clashing with the zx::Log logging namespace).
inline f32 Ln(f32 v) { return std::log(v); }
inline f32 Log2(f32 v) { return std::log2(v); }
inline f32 Floor(f32 v) { return std::floor(v); }
inline f32 Ceil(f32 v) { return std::ceil(v); }
inline f32 Round(f32 v) { return std::round(v); }
inline f32 Trunc(f32 v) { return std::trunc(v); }
inline f32 Frac(f32 v) { return v - std::floor(v); }
inline f32 Fmod(f32 a, f32 b) { return std::fmod(a, b); }
inline i32 FloorToInt(f32 v) { return i32(std::floor(v)); }
inline i32 CeilToInt(f32 v) { return i32(std::ceil(v)); }
inline i32 RoundToInt(f32 v) { return i32(std::lround(v)); }
inline bool IsNaN(f32 v) { return std::isnan(v); }
inline bool IsFinite(f32 v) { return std::isfinite(v); }
inline void SinCos(f32 angle, f32& s, f32& c)
{
    s = std::sin(angle);
    c = std::cos(angle);
}

constexpr f32 Sign(f32 v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }
constexpr f32 Saturate(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
constexpr f32 Lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
constexpr f64 Lerp(f64 a, f64 b, f64 t) { return a + (b - a) * t; }
constexpr f32 InverseLerp(f32 a, f32 b, f32 v) { return (b - a) != 0.0f ? (v - a) / (b - a) : 0.0f; }
constexpr f32 Remap(f32 v, f32 inMin, f32 inMax, f32 outMin, f32 outMax)
{
    return Lerp(outMin, outMax, InverseLerp(inMin, inMax, v));
}
constexpr f32 SmoothStep(f32 edge0, f32 edge1, f32 x)
{
    const f32 t = Saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}
constexpr f32 SmootherStep(f32 edge0, f32 edge1, f32 x)
{
    const f32 t = Saturate((x - edge0) / (edge1 - edge0));
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}
constexpr f32 Square(f32 v) { return v * v; }

inline bool IsNearlyEqual(f32 a, f32 b, f32 tolerance = KindaSmallNumber) { return Abs(a - b) <= tolerance; }
inline bool IsNearlyZero(f32 a, f32 tolerance = KindaSmallNumber) { return Abs(a) <= tolerance; }

/// Wraps angle (radians) to [-Pi, Pi].
inline f32 WrapAngle(f32 a)
{
    a = std::fmod(a + Pi, TwoPi);
    if (a < 0.0f)
        a += TwoPi;
    return a - Pi;
}

/// Shortest signed difference b - a between angles (radians).
inline f32 AngleDelta(f32 a, f32 b) { return WrapAngle(b - a); }

/// Framerate-independent exponential smoothing: moves current toward target with given half-life.
inline f32 Damp(f32 current, f32 target, f32 halfLife, f32 dt)
{
    return Lerp(target, current, Exp2(-dt / (halfLife > 1e-6f ? halfLife : 1e-6f)));
}

/// Critically damped spring (Game Programming Gems 4). velocity is in/out.
inline f32 SmoothDamp(f32 current, f32 target, f32& velocity, f32 smoothTime, f32 dt)
{
    smoothTime = smoothTime > 1e-4f ? smoothTime : 1e-4f;
    const f32 omega = 2.0f / smoothTime;
    const f32 x = omega * dt;
    const f32 e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const f32 change = current - target;
    const f32 temp = (velocity + omega * change) * dt;
    velocity = (velocity - omega * temp) * e;
    return target + (change + temp) * e;
}

constexpr u32 NextPowerOfTwo(u32 v)
{
    if (v <= 1)
        return 1;
    return 1u << (32 - std::countl_zero(v - 1));
}
constexpr u32 FloorLog2(u32 v) { return v ? 31u - u32(std::countl_zero(v)) : 0; }
constexpr u32 CeilLog2(u32 v) { return v > 1 ? 32u - u32(std::countl_zero(v - 1)) : 0; }
constexpr u32 DivideRoundUp(u32 a, u32 b) { return (a + b - 1) / b; }

/// Number of mips for a 2D texture.
constexpr u32 MipCount(u32 width, u32 height) { return FloorLog2(width > height ? width : height) + 1; }

} // namespace zx
