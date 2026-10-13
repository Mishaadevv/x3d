#pragma once

#include "Core/String/StringView.h"

#include <bit>

namespace zx {

/// xxHash64-compatible hash (own implementation of the public algorithm).
u64 HashXX64(const void* data, usize size, u64 seed = 0);
inline u64 HashXX64(StringView s, u64 seed = 0) { return HashXX64(s.Data(), s.Size(), seed); }

/// Incremental xxHash64.
class XXHash64Stream
{
public:
    explicit XXHash64Stream(u64 seed = 0) { Reset(seed); }
    void Reset(u64 seed = 0);
    void Update(const void* data, usize size);
    void Update(StringView s) { Update(s.Data(), s.Size()); }
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void UpdateValue(const T& v)
    {
        Update(&v, sizeof(T));
    }
    u64 Digest() const;

private:
    u64 m_v[4];
    u64 m_seed;
    u64 m_totalLen;
    u8 m_buffer[32];
    u32 m_bufferSize;
};

/// CRC-32 (IEEE 802.3, as used by PNG/zlib/gzip). Pass the previous value to continue a running CRC.
u32 Crc32(const void* data, usize size, u32 previous = 0);

/// Adler-32 (zlib). Pass the previous value (initially 1) to continue.
u32 Adler32(const void* data, usize size, u32 previous = 1);

/// Compile-time FNV-1a 64-bit (for string literals / switch on hashed names).
constexpr u64 HashFnv1a64(StringView s)
{
    u64 h = 0xcbf29ce484222325ull;
    for (char c : s) {
        h ^= u8(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

constexpr u32 HashFnv1a32(StringView s)
{
    u32 h = 0x811c9dc5u;
    for (char c : s) {
        h ^= u8(c);
        h *= 0x01000193u;
    }
    return h;
}

/// 64-bit finalizer (splitmix64) - good avalanche for integer keys.
constexpr u64 HashMix64(u64 x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

constexpr u64 HashCombine(u64 seed, u64 value)
{
    return HashMix64(seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2)));
}

/// Hasher<T>: specialize for custom key types (static u64 Hash(const T&)).
template <typename T, typename = void>
struct Hasher;

template <typename T>
struct Hasher<T, std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>>>
{
    static constexpr u64 Hash(T v) { return HashMix64(u64(v)); }
};

template <typename T>
struct Hasher<T*>
{
    static u64 Hash(const T* p) { return HashMix64(u64(reinterpret_cast<uptr>(p))); }
};

template <>
struct Hasher<f32>
{
    static u64 Hash(f32 v) { return HashMix64(v == 0.0f ? 0u : std::bit_cast<u32>(v)); }
};

template <>
struct Hasher<f64>
{
    static u64 Hash(f64 v) { return HashMix64(v == 0.0 ? 0u : std::bit_cast<u64>(v)); }
};

template <>
struct Hasher<StringView>
{
    static u64 Hash(StringView s) { return HashXX64(s); }
};

template <>
struct Hasher<const char*>
{
    static u64 Hash(const char* s) { return HashXX64(StringView(s)); }
};

// String literals passed as heterogeneous keys decay to char* - hash them as text, not as pointers.
template <>
struct Hasher<char*>
{
    static u64 Hash(const char* s) { return HashXX64(StringView(s)); }
};

template <typename A, typename B>
struct Hasher<Pair<A, B>>
{
    static u64 Hash(const Pair<A, B>& p) { return HashCombine(Hasher<A>::Hash(p.first), Hasher<B>::Hash(p.second)); }
};

template <typename T>
concept Hashable = requires(const T& v) {
    { Hasher<T>::Hash(v) } -> std::convertible_to<u64>;
};

template <typename T>
u64 GetHash(const T& v)
{
    return Hasher<T>::Hash(v);
}

} // namespace zx
