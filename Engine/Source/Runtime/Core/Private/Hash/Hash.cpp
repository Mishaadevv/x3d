#include "Core/Hash/Hash.h"

#include <cstring>

namespace zx {

namespace {

constexpr u64 P1 = 0x9E3779B185EBCA87ull;
constexpr u64 P2 = 0xC2B2AE3D27D4EB4Full;
constexpr u64 P3 = 0x165667B19E3779F9ull;
constexpr u64 P4 = 0x85EBCA77C2B2AE63ull;
constexpr u64 P5 = 0x27D4EB2F165667C5ull;

ZX_FORCEINLINE u64 Read64(const u8* p)
{
    u64 v;
    std::memcpy(&v, p, 8);
    return v;
}
ZX_FORCEINLINE u32 Read32(const u8* p)
{
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}
ZX_FORCEINLINE u64 Round(u64 acc, u64 input)
{
    acc += input * P2;
    acc = std::rotl(acc, 31);
    return acc * P1;
}
ZX_FORCEINLINE u64 MergeRound(u64 acc, u64 val)
{
    acc ^= Round(0, val);
    return acc * P1 + P4;
}
ZX_FORCEINLINE u64 Avalanche(u64 h)
{
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

u64 Finalize(u64 h, const u8* p, usize len)
{
    while (len >= 8) {
        h ^= Round(0, Read64(p));
        h = std::rotl(h, 27) * P1 + P4;
        p += 8;
        len -= 8;
    }
    if (len >= 4) {
        h ^= u64(Read32(p)) * P1;
        h = std::rotl(h, 23) * P2 + P3;
        p += 4;
        len -= 4;
    }
    while (len > 0) {
        h ^= u64(*p) * P5;
        h = std::rotl(h, 11) * P1;
        ++p;
        --len;
    }
    return Avalanche(h);
}

u64 MergeAccumulators(const u64 v[4])
{
    u64 h = std::rotl(v[0], 1) + std::rotl(v[1], 7) + std::rotl(v[2], 12) + std::rotl(v[3], 18);
    h = MergeRound(h, v[0]);
    h = MergeRound(h, v[1]);
    h = MergeRound(h, v[2]);
    h = MergeRound(h, v[3]);
    return h;
}

struct Crc32Table
{
    u32 t[8][256];
    constexpr Crc32Table() : t()
    {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[0][i] = c;
        }
        for (u32 i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s)
                t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};
constexpr Crc32Table g_crcTable;

} // namespace

u64 HashXX64(const void* data, usize size, u64 seed)
{
    const u8* p = static_cast<const u8*>(data);
    const usize len = size;
    u64 h;
    if (size >= 32) {
        u64 v[4] = {seed + P1 + P2, seed + P2, seed, seed - P1};
        const u8* limit = p + size - 32;
        do {
            v[0] = Round(v[0], Read64(p));
            v[1] = Round(v[1], Read64(p + 8));
            v[2] = Round(v[2], Read64(p + 16));
            v[3] = Round(v[3], Read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = MergeAccumulators(v);
        size = usize(static_cast<const u8*>(data) + len - p);
    } else {
        h = seed + P5;
    }
    h += u64(len);
    return Finalize(h, p, size);
}

void XXHash64Stream::Reset(u64 seed)
{
    m_seed = seed;
    m_v[0] = seed + P1 + P2;
    m_v[1] = seed + P2;
    m_v[2] = seed;
    m_v[3] = seed - P1;
    m_totalLen = 0;
    m_bufferSize = 0;
}

void XXHash64Stream::Update(const void* data, usize size)
{
    const u8* p = static_cast<const u8*>(data);
    m_totalLen += size;
    if (m_bufferSize + size < 32) {
        std::memcpy(m_buffer + m_bufferSize, p, size);
        m_bufferSize += u32(size);
        return;
    }
    if (m_bufferSize > 0) {
        const usize fill = 32 - m_bufferSize;
        std::memcpy(m_buffer + m_bufferSize, p, fill);
        m_v[0] = Round(m_v[0], Read64(m_buffer));
        m_v[1] = Round(m_v[1], Read64(m_buffer + 8));
        m_v[2] = Round(m_v[2], Read64(m_buffer + 16));
        m_v[3] = Round(m_v[3], Read64(m_buffer + 24));
        p += fill;
        size -= fill;
        m_bufferSize = 0;
    }
    while (size >= 32) {
        m_v[0] = Round(m_v[0], Read64(p));
        m_v[1] = Round(m_v[1], Read64(p + 8));
        m_v[2] = Round(m_v[2], Read64(p + 16));
        m_v[3] = Round(m_v[3], Read64(p + 24));
        p += 32;
        size -= 32;
    }
    if (size > 0) {
        std::memcpy(m_buffer, p, size);
        m_bufferSize = u32(size);
    }
}

u64 XXHash64Stream::Digest() const
{
    u64 h = m_totalLen >= 32 ? MergeAccumulators(m_v) : m_seed + P5;
    h += m_totalLen;
    return Finalize(h, m_buffer, m_bufferSize);
}

u32 Crc32(const void* data, usize size, u32 previous)
{
    const u8* p = static_cast<const u8*>(data);
    u32 c = ~previous;
    // Slicing-by-8.
    while (size >= 8) {
        u32 lo;
        u32 hi;
        std::memcpy(&lo, p, 4);
        std::memcpy(&hi, p + 4, 4);
        lo ^= c;
        c = g_crcTable.t[7][lo & 0xFF] ^ g_crcTable.t[6][(lo >> 8) & 0xFF] ^ g_crcTable.t[5][(lo >> 16) & 0xFF] ^
            g_crcTable.t[4][lo >> 24] ^ g_crcTable.t[3][hi & 0xFF] ^ g_crcTable.t[2][(hi >> 8) & 0xFF] ^
            g_crcTable.t[1][(hi >> 16) & 0xFF] ^ g_crcTable.t[0][hi >> 24];
        p += 8;
        size -= 8;
    }
    while (size-- > 0)
        c = g_crcTable.t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

u32 Adler32(const void* data, usize size, u32 previous)
{
    constexpr u32 Mod = 65521;
    const u8* p = static_cast<const u8*>(data);
    u32 a = previous & 0xFFFF;
    u32 b = previous >> 16;
    while (size > 0) {
        // 5552 is the largest n such that 255n(n+1)/2 + (n+1)(Mod-1) fits in 32 bits.
        usize block = size < 5552 ? size : 5552;
        size -= block;
        while (block-- > 0) {
            a += *p++;
            b += a;
        }
        a %= Mod;
        b %= Mod;
    }
    return (b << 16) | a;
}

} // namespace zx
