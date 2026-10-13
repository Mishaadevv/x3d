#pragma once

#include "Core/Build.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace zx {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;
using isize = std::ptrdiff_t;
using uptr = std::uintptr_t;

inline constexpr u8 U8Max = 0xFFu;
inline constexpr u16 U16Max = 0xFFFFu;
inline constexpr u32 U32Max = 0xFFFFFFFFu;
inline constexpr u64 U64Max = 0xFFFFFFFFFFFFFFFFull;
inline constexpr i32 I32Max = 0x7FFFFFFF;
inline constexpr i32 I32Min = -I32Max - 1;
inline constexpr i64 I64Max = 0x7FFFFFFFFFFFFFFFll;
inline constexpr i64 I64Min = -I64Max - 1;
inline constexpr f32 F32Max = 3.402823466e+38f;
inline constexpr f32 F32Min = 1.175494351e-38f; // smallest positive normal
inline constexpr f32 F32Epsilon = 1.192092896e-07f;

/// Sentinel "no index" value for u32 indices.
inline constexpr u32 InvalidIndex = U32Max;

constexpr u64 operator""_KB(unsigned long long v) { return v * 1024ull; }
constexpr u64 operator""_MB(unsigned long long v) { return v * 1024ull * 1024ull; }
constexpr u64 operator""_GB(unsigned long long v) { return v * 1024ull * 1024ull * 1024ull; }

using std::forward;
using std::move;
using std::swap;

template <typename T>
constexpr T Min(T a, T b) { return b < a ? b : a; }
template <typename T>
constexpr T Max(T a, T b) { return a < b ? b : a; }
template <typename T>
constexpr T Clamp(T v, T lo, T hi) { return v < lo ? lo : (hi < v ? hi : v); }

template <typename T>
constexpr bool IsPowerOfTwo(T v) { return v != 0 && (v & (v - 1)) == 0; }

template <typename T>
constexpr T AlignUp(T v, T align) { return (v + (align - 1)) & ~(align - 1); }
template <typename T>
constexpr T AlignDown(T v, T align) { return v & ~(align - 1); }

inline void* AlignPtr(void* p, usize align) { return reinterpret_cast<void*>(AlignUp(reinterpret_cast<uptr>(p), align)); }

template <typename T>
concept TriviallyRelocatable = std::is_trivially_copyable_v<T>;

/// Pair of values (std::pair-like, but trivially usable in our containers).
template <typename A, typename B>
struct Pair
{
    A first;
    B second;

    friend constexpr bool operator==(const Pair&, const Pair&) = default;
};

} // namespace zx
