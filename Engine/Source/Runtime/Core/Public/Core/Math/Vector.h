#pragma once

#include "Core/Hash/Hash.h"
#include "Core/Math/MathCommon.h"
#include "Core/String/Format.h"

namespace zx {

// ---------------------------------------------------------------------------------------------------------------------
// Vec2
// ---------------------------------------------------------------------------------------------------------------------
struct Vec2
{
    f32 x = 0.0f, y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(f32 x_, f32 y_) : x(x_), y(y_) {}
    constexpr explicit Vec2(f32 s) : x(s), y(s) {}

    static constexpr Vec2 Zero() { return {0.0f, 0.0f}; }
    static constexpr Vec2 One() { return {1.0f, 1.0f}; }
    static constexpr Vec2 UnitX() { return {1.0f, 0.0f}; }
    static constexpr Vec2 UnitY() { return {0.0f, 1.0f}; }

    constexpr f32& operator[](usize i) { return (&x)[i]; }
    constexpr f32 operator[](usize i) const { return (&x)[i]; }
    constexpr Vec2 operator-() const { return {-x, -y}; }
    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(Vec2 o) const { return {x * o.x, y * o.y}; }
    constexpr Vec2 operator/(Vec2 o) const { return {x / o.x, y / o.y}; }
    constexpr Vec2 operator*(f32 s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(f32 s) const { return {x / s, y / s}; }
    constexpr Vec2& operator+=(Vec2 o) { return *this = *this + o; }
    constexpr Vec2& operator-=(Vec2 o) { return *this = *this - o; }
    constexpr Vec2& operator*=(Vec2 o) { return *this = *this * o; }
    constexpr Vec2& operator*=(f32 s) { return *this = *this * s; }
    constexpr Vec2& operator/=(f32 s) { return *this = *this / s; }
    friend constexpr Vec2 operator*(f32 s, Vec2 v) { return v * s; }
    friend constexpr bool operator==(Vec2, Vec2) = default;
};

constexpr f32 Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
/// 2D cross product (z of the 3D cross).
constexpr f32 Cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline f32 Length(Vec2 v) { return Sqrt(Dot(v, v)); }
constexpr f32 LengthSq(Vec2 v) { return Dot(v, v); }
inline f32 Distance(Vec2 a, Vec2 b) { return Length(a - b); }
inline Vec2 Normalize(Vec2 v) { return v * (1.0f / Length(v)); }
inline Vec2 SafeNormalize(Vec2 v, Vec2 fallback = Vec2(0.0f, 0.0f))
{
    const f32 l2 = LengthSq(v);
    return l2 > SmallNumber ? v * InvSqrt(l2) : fallback;
}
constexpr Vec2 Min(Vec2 a, Vec2 b) { return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y}; }
constexpr Vec2 Max(Vec2 a, Vec2 b) { return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y}; }
constexpr Vec2 Lerp(Vec2 a, Vec2 b, f32 t) { return a + (b - a) * t; }
inline Vec2 Abs(Vec2 v) { return {Abs(v.x), Abs(v.y)}; }
inline Vec2 Floor(Vec2 v) { return {Floor(v.x), Floor(v.y)}; }
inline Vec2 Ceil(Vec2 v) { return {Ceil(v.x), Ceil(v.y)}; }
inline Vec2 Round(Vec2 v) { return {Round(v.x), Round(v.y)}; }
constexpr Vec2 Perpendicular(Vec2 v) { return {-v.y, v.x}; }

// ---------------------------------------------------------------------------------------------------------------------
// Vec3
// ---------------------------------------------------------------------------------------------------------------------
struct Vec3
{
    f32 x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}
    constexpr explicit Vec3(f32 s) : x(s), y(s), z(s) {}
    constexpr Vec3(Vec2 xy, f32 z_) : x(xy.x), y(xy.y), z(z_) {}

    static constexpr Vec3 Zero() { return {0.0f, 0.0f, 0.0f}; }
    static constexpr Vec3 One() { return {1.0f, 1.0f, 1.0f}; }
    static constexpr Vec3 UnitX() { return {1.0f, 0.0f, 0.0f}; }
    static constexpr Vec3 UnitY() { return {0.0f, 1.0f, 0.0f}; }
    static constexpr Vec3 UnitZ() { return {0.0f, 0.0f, 1.0f}; }
    /// Engine basis: +X right, +Y up, +Z forward.
    static constexpr Vec3 Right() { return {1.0f, 0.0f, 0.0f}; }
    static constexpr Vec3 Up() { return {0.0f, 1.0f, 0.0f}; }
    static constexpr Vec3 Forward() { return {0.0f, 0.0f, 1.0f}; }

    constexpr Vec2 XY() const { return {x, y}; }
    constexpr Vec2 XZ() const { return {x, z}; }

    constexpr f32& operator[](usize i) { return (&x)[i]; }
    constexpr f32 operator[](usize i) const { return (&x)[i]; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(Vec3 o) const { return {x * o.x, y * o.y, z * o.z}; }
    constexpr Vec3 operator/(Vec3 o) const { return {x / o.x, y / o.y, z / o.z}; }
    constexpr Vec3 operator*(f32 s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(f32 s) const
    {
        const f32 inv = 1.0f / s;
        return {x * inv, y * inv, z * inv};
    }
    constexpr Vec3& operator+=(Vec3 o) { return *this = *this + o; }
    constexpr Vec3& operator-=(Vec3 o) { return *this = *this - o; }
    constexpr Vec3& operator*=(Vec3 o) { return *this = *this * o; }
    constexpr Vec3& operator*=(f32 s) { return *this = *this * s; }
    constexpr Vec3& operator/=(f32 s) { return *this = *this / s; }
    friend constexpr Vec3 operator*(f32 s, Vec3 v) { return v * s; }
    friend constexpr bool operator==(Vec3, Vec3) = default;
};

constexpr f32 Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline f32 Length(Vec3 v) { return Sqrt(Dot(v, v)); }
constexpr f32 LengthSq(Vec3 v) { return Dot(v, v); }
inline f32 Distance(Vec3 a, Vec3 b) { return Length(a - b); }
constexpr f32 DistanceSq(Vec3 a, Vec3 b) { return LengthSq(a - b); }
inline Vec3 Normalize(Vec3 v) { return v * (1.0f / Length(v)); }
inline Vec3 SafeNormalize(Vec3 v, Vec3 fallback = Vec3(0.0f, 0.0f, 0.0f))
{
    const f32 l2 = LengthSq(v);
    return l2 > SmallNumber ? v * InvSqrt(l2) : fallback;
}
inline bool IsNormalized(Vec3 v, f32 tolerance = 1e-3f) { return Abs(LengthSq(v) - 1.0f) <= tolerance; }
constexpr Vec3 Min(Vec3 a, Vec3 b) { return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z}; }
constexpr Vec3 Max(Vec3 a, Vec3 b) { return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z}; }
constexpr Vec3 Clamp(Vec3 v, Vec3 lo, Vec3 hi) { return Min(Max(v, lo), hi); }
constexpr Vec3 Saturate(Vec3 v) { return {Saturate(v.x), Saturate(v.y), Saturate(v.z)}; }
constexpr Vec3 Lerp(Vec3 a, Vec3 b, f32 t) { return a + (b - a) * t; }
inline Vec3 Abs(Vec3 v) { return {Abs(v.x), Abs(v.y), Abs(v.z)}; }
inline Vec3 Floor(Vec3 v) { return {Floor(v.x), Floor(v.y), Floor(v.z)}; }
constexpr f32 MinComponent(Vec3 v) { return v.x < v.y ? (v.x < v.z ? v.x : v.z) : (v.y < v.z ? v.y : v.z); }
constexpr f32 MaxComponent(Vec3 v) { return v.x > v.y ? (v.x > v.z ? v.x : v.z) : (v.y > v.z ? v.y : v.z); }
/// Reflects incident direction i about normal n (n normalized).
constexpr Vec3 Reflect(Vec3 i, Vec3 n) { return i - n * (2.0f * Dot(i, n)); }
/// Projects a onto b.
constexpr Vec3 Project(Vec3 a, Vec3 b) { return b * (Dot(a, b) / Dot(b, b)); }
constexpr Vec3 ProjectOnPlane(Vec3 v, Vec3 planeNormal) { return v - planeNormal * Dot(v, planeNormal); }
inline bool IsNearlyEqual(Vec3 a, Vec3 b, f32 tolerance = KindaSmallNumber)
{
    return Abs(a.x - b.x) <= tolerance && Abs(a.y - b.y) <= tolerance && Abs(a.z - b.z) <= tolerance;
}
/// Unsigned angle between vectors in radians.
inline f32 AngleBetween(Vec3 a, Vec3 b)
{
    const f32 d = Sqrt(LengthSq(a) * LengthSq(b));
    return d > SmallNumber ? Acos(Dot(a, b) / d) : 0.0f;
}
/// Builds an orthonormal basis (t, b) for unit normal n (Duff et al. 2017).
inline void OrthonormalBasis(Vec3 n, Vec3& t, Vec3& b)
{
    const f32 sign = n.z >= 0.0f ? 1.0f : -1.0f;
    const f32 a = -1.0f / (sign + n.z);
    const f32 c = n.x * n.y * a;
    t = Vec3(1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x);
    b = Vec3(c, sign + n.y * n.y * a, -n.y);
}

// ---------------------------------------------------------------------------------------------------------------------
// Vec4
// ---------------------------------------------------------------------------------------------------------------------
struct alignas(16) Vec4
{
    f32 x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

    constexpr Vec4() = default;
    constexpr Vec4(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr explicit Vec4(f32 s) : x(s), y(s), z(s), w(s) {}
    constexpr Vec4(Vec3 v, f32 w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    constexpr Vec4(Vec2 a, Vec2 b) : x(a.x), y(a.y), z(b.x), w(b.y) {}

    static constexpr Vec4 Zero() { return {0.0f, 0.0f, 0.0f, 0.0f}; }
    static constexpr Vec4 One() { return {1.0f, 1.0f, 1.0f, 1.0f}; }

    constexpr Vec3 XYZ() const { return {x, y, z}; }
    constexpr Vec2 XY() const { return {x, y}; }
    constexpr Vec2 ZW() const { return {z, w}; }

    constexpr f32& operator[](usize i) { return (&x)[i]; }
    constexpr f32 operator[](usize i) const { return (&x)[i]; }
    constexpr Vec4 operator-() const { return {-x, -y, -z, -w}; }
    constexpr Vec4 operator+(Vec4 o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    constexpr Vec4 operator-(Vec4 o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    constexpr Vec4 operator*(Vec4 o) const { return {x * o.x, y * o.y, z * o.z, w * o.w}; }
    constexpr Vec4 operator/(Vec4 o) const { return {x / o.x, y / o.y, z / o.z, w / o.w}; }
    constexpr Vec4 operator*(f32 s) const { return {x * s, y * s, z * s, w * s}; }
    constexpr Vec4 operator/(f32 s) const { return *this * (1.0f / s); }
    constexpr Vec4& operator+=(Vec4 o) { return *this = *this + o; }
    constexpr Vec4& operator-=(Vec4 o) { return *this = *this - o; }
    constexpr Vec4& operator*=(Vec4 o) { return *this = *this * o; }
    constexpr Vec4& operator*=(f32 s) { return *this = *this * s; }
    friend constexpr Vec4 operator*(f32 s, Vec4 v) { return v * s; }
    friend constexpr bool operator==(Vec4, Vec4) = default;
};

constexpr f32 Dot(Vec4 a, Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline f32 Length(Vec4 v) { return Sqrt(Dot(v, v)); }
constexpr f32 LengthSq(Vec4 v) { return Dot(v, v); }
inline Vec4 Normalize(Vec4 v) { return v * (1.0f / Length(v)); }
constexpr Vec4 Min(Vec4 a, Vec4 b)
{
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w};
}
constexpr Vec4 Max(Vec4 a, Vec4 b)
{
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w};
}
constexpr Vec4 Lerp(Vec4 a, Vec4 b, f32 t) { return a + (b - a) * t; }
inline bool IsNearlyEqual(Vec4 a, Vec4 b, f32 tolerance = KindaSmallNumber)
{
    return Abs(a.x - b.x) <= tolerance && Abs(a.y - b.y) <= tolerance && Abs(a.z - b.z) <= tolerance &&
           Abs(a.w - b.w) <= tolerance;
}

// ---------------------------------------------------------------------------------------------------------------------
// Integer vectors
// ---------------------------------------------------------------------------------------------------------------------
template <typename T>
struct TVec2
{
    T x = 0, y = 0;
    constexpr TVec2() = default;
    constexpr TVec2(T x_, T y_) : x(x_), y(y_) {}
    constexpr explicit TVec2(T s) : x(s), y(s) {}
    constexpr T& operator[](usize i) { return (&x)[i]; }
    constexpr T operator[](usize i) const { return (&x)[i]; }
    constexpr TVec2 operator+(TVec2 o) const { return {T(x + o.x), T(y + o.y)}; }
    constexpr TVec2 operator-(TVec2 o) const { return {T(x - o.x), T(y - o.y)}; }
    constexpr TVec2 operator*(TVec2 o) const { return {T(x * o.x), T(y * o.y)}; }
    constexpr TVec2 operator/(TVec2 o) const { return {T(x / o.x), T(y / o.y)}; }
    constexpr TVec2 operator*(T s) const { return {T(x * s), T(y * s)}; }
    constexpr TVec2 operator/(T s) const { return {T(x / s), T(y / s)}; }
    constexpr TVec2& operator+=(TVec2 o) { return *this = *this + o; }
    constexpr TVec2& operator-=(TVec2 o) { return *this = *this - o; }
    constexpr explicit operator Vec2() const { return {f32(x), f32(y)}; }
    friend constexpr bool operator==(TVec2, TVec2) = default;
};

template <typename T>
struct TVec3
{
    T x = 0, y = 0, z = 0;
    constexpr TVec3() = default;
    constexpr TVec3(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}
    constexpr explicit TVec3(T s) : x(s), y(s), z(s) {}
    constexpr T& operator[](usize i) { return (&x)[i]; }
    constexpr T operator[](usize i) const { return (&x)[i]; }
    constexpr TVec3 operator+(TVec3 o) const { return {T(x + o.x), T(y + o.y), T(z + o.z)}; }
    constexpr TVec3 operator-(TVec3 o) const { return {T(x - o.x), T(y - o.y), T(z - o.z)}; }
    constexpr TVec3 operator*(TVec3 o) const { return {T(x * o.x), T(y * o.y), T(z * o.z)}; }
    constexpr TVec3 operator*(T s) const { return {T(x * s), T(y * s), T(z * s)}; }
    constexpr TVec3 operator/(T s) const { return {T(x / s), T(y / s), T(z / s)}; }
    constexpr TVec3& operator+=(TVec3 o) { return *this = *this + o; }
    constexpr explicit operator Vec3() const { return {f32(x), f32(y), f32(z)}; }
    friend constexpr bool operator==(TVec3, TVec3) = default;
};

template <typename T>
struct TVec4
{
    T x = 0, y = 0, z = 0, w = 0;
    constexpr TVec4() = default;
    constexpr TVec4(T x_, T y_, T z_, T w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr explicit TVec4(T s) : x(s), y(s), z(s), w(s) {}
    constexpr T& operator[](usize i) { return (&x)[i]; }
    constexpr T operator[](usize i) const { return (&x)[i]; }
    constexpr TVec4 operator+(TVec4 o) const { return {T(x + o.x), T(y + o.y), T(z + o.z), T(w + o.w)}; }
    constexpr TVec4 operator-(TVec4 o) const { return {T(x - o.x), T(y - o.y), T(z - o.z), T(w - o.w)}; }
    friend constexpr bool operator==(TVec4, TVec4) = default;
};

using IVec2 = TVec2<i32>;
using IVec3 = TVec3<i32>;
using IVec4 = TVec4<i32>;
using UVec2 = TVec2<u32>;
using UVec3 = TVec3<u32>;
using UVec4 = TVec4<u32>;

inline IVec2 FloorToInt(Vec2 v) { return {FloorToInt(v.x), FloorToInt(v.y)}; }
inline IVec3 FloorToInt(Vec3 v) { return {FloorToInt(v.x), FloorToInt(v.y), FloorToInt(v.z)}; }

// ---------------------------------------------------------------------------------------------------------------------
// Hashing / formatting
// ---------------------------------------------------------------------------------------------------------------------
template <typename T>
struct Hasher<TVec2<T>>
{
    static u64 Hash(TVec2<T> v) { return HashCombine(Hasher<T>::Hash(v.x), Hasher<T>::Hash(v.y)); }
};
template <typename T>
struct Hasher<TVec3<T>>
{
    static u64 Hash(TVec3<T> v)
    {
        return HashCombine(HashCombine(Hasher<T>::Hash(v.x), Hasher<T>::Hash(v.y)), Hasher<T>::Hash(v.z));
    }
};
template <>
struct Hasher<Vec3>
{
    static u64 Hash(Vec3 v)
    {
        return HashCombine(HashCombine(Hasher<f32>::Hash(v.x), Hasher<f32>::Hash(v.y)), Hasher<f32>::Hash(v.z));
    }
};

template <>
struct Formatter<Vec2>
{
    static void Format(FormatSink& s, Vec2 v, const FormatSpec& spec)
    {
        s.Append("(");
        FormatFloat(s, v.x, spec, true);
        s.Append(", ");
        FormatFloat(s, v.y, spec, true);
        s.Append(")");
    }
};
template <>
struct Formatter<Vec3>
{
    static void Format(FormatSink& s, Vec3 v, const FormatSpec& spec)
    {
        s.Append("(");
        FormatFloat(s, v.x, spec, true);
        s.Append(", ");
        FormatFloat(s, v.y, spec, true);
        s.Append(", ");
        FormatFloat(s, v.z, spec, true);
        s.Append(")");
    }
};
template <>
struct Formatter<Vec4>
{
    static void Format(FormatSink& s, Vec4 v, const FormatSpec& spec)
    {
        s.Append("(");
        for (int i = 0; i < 4; ++i) {
            if (i)
                s.Append(", ");
            FormatFloat(s, v[usize(i)], spec, true);
        }
        s.Append(")");
    }
};
template <typename T>
struct Formatter<TVec2<T>>
{
    static void Format(FormatSink& s, TVec2<T> v, const FormatSpec&) { FormatTo(s, "({}, {})", v.x, v.y); }
};
template <typename T>
struct Formatter<TVec3<T>>
{
    static void Format(FormatSink& s, TVec3<T> v, const FormatSpec&) { FormatTo(s, "({}, {}, {})", v.x, v.y, v.z); }
};


// Reflection (defined in Core/Private/Reflection/CoreTypes.cpp).
class TypeInfo;
const TypeInfo& ZxTypeOf(Vec2*);
const TypeInfo& ZxTypeOf(Vec3*);
const TypeInfo& ZxTypeOf(Vec4*);
const TypeInfo& ZxTypeOf(IVec2*);
const TypeInfo& ZxTypeOf(IVec3*);
const TypeInfo& ZxTypeOf(IVec4*);
const TypeInfo& ZxTypeOf(UVec2*);
const TypeInfo& ZxTypeOf(UVec3*);
const TypeInfo& ZxTypeOf(UVec4*);

} // namespace zx
