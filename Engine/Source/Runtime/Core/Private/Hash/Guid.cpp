#include "Core/Hash/Guid.h"
#include "Core/Assert.h"
#include "Core/Memory/Memory.h"

#include "Windows/WindowsInclude.h"

#include <bcrypt.h>
#include <bit>

#pragma comment(lib, "bcrypt.lib")

namespace zx {

// --- SHA-1 (FIPS 180-4) ---------------------------------------------------------------------------------------------

void Sha1::Reset()
{
    m_state[0] = 0x67452301u;
    m_state[1] = 0xEFCDAB89u;
    m_state[2] = 0x98BADCFEu;
    m_state[3] = 0x10325476u;
    m_state[4] = 0xC3D2E1F0u;
    m_length = 0;
    m_bufferSize = 0;
}

void Sha1::ProcessBlock(const u8* block)
{
    u32 w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (u32(block[i * 4]) << 24) | (u32(block[i * 4 + 1]) << 16) | (u32(block[i * 4 + 2]) << 8) |
               u32(block[i * 4 + 3]);
    for (int i = 16; i < 80; ++i)
        w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3], e = m_state[4];
    for (int i = 0; i < 80; ++i) {
        u32 f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const u32 temp = std::rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = std::rotl(b, 30);
        b = a;
        a = temp;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
}

void Sha1::Update(const void* data, usize size)
{
    const u8* p = static_cast<const u8*>(data);
    m_length += size;
    while (size > 0) {
        const usize take = Min<usize>(64 - m_bufferSize, size);
        MemCopy(m_buffer + m_bufferSize, p, take);
        m_bufferSize += u32(take);
        p += take;
        size -= take;
        if (m_bufferSize == 64) {
            ProcessBlock(m_buffer);
            m_bufferSize = 0;
        }
    }
}

Sha1Digest Sha1::Finish()
{
    const u64 bitLength = m_length * 8;
    const u8 pad = 0x80;
    Update(&pad, 1);
    const u8 zero = 0;
    while (m_bufferSize != 56)
        Update(&zero, 1);
    u8 lenBytes[8];
    for (int i = 0; i < 8; ++i)
        lenBytes[i] = u8(bitLength >> (56 - i * 8));
    Update(lenBytes, 8);
    Sha1Digest d;
    for (int i = 0; i < 5; ++i)
        for (int k = 0; k < 4; ++k)
            d.bytes[i * 4 + k] = u8(m_state[i] >> (24 - k * 8));
    Reset();
    return d;
}

void Sha1Digest::ToHex(char out[41]) const
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 20; ++i) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 15];
    }
    out[40] = '\0';
}

// --- Guid -----------------------------------------------------------------------------------------------------------

Guid Guid::New()
{
    u8 b[16];
    const NTSTATUS st = ::BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ZX_VERIFY(st == 0, "BCryptGenRandom failed");
    b[6] = u8((b[6] & 0x0F) | 0x40); // version 4
    b[8] = u8((b[8] & 0x3F) | 0x80); // RFC 4122 variant
    Guid g;
    for (int i = 0; i < 8; ++i) {
        g.high = (g.high << 8) | b[i];
        g.low = (g.low << 8) | b[i + 8];
    }
    return g;
}

Guid Guid::FromName(const Guid& ns, StringView name)
{
    u8 nsBytes[16];
    for (int i = 0; i < 8; ++i) {
        nsBytes[i] = u8(ns.high >> (56 - i * 8));
        nsBytes[i + 8] = u8(ns.low >> (56 - i * 8));
    }
    Sha1 sha;
    sha.Update(nsBytes, 16);
    sha.Update(name);
    Sha1Digest d = sha.Finish();
    d.bytes[6] = u8((d.bytes[6] & 0x0F) | 0x50); // version 5
    d.bytes[8] = u8((d.bytes[8] & 0x3F) | 0x80);
    Guid g;
    for (int i = 0; i < 8; ++i) {
        g.high = (g.high << 8) | d.bytes[i];
        g.low = (g.low << 8) | d.bytes[i + 8];
    }
    return g;
}

void Guid::ToChars(char out[37]) const
{
    static const char hex[] = "0123456789abcdef";
    usize o = 0;
    for (int i = 0; i < 16; ++i) {
        const u8 byte = i < 8 ? u8(high >> (56 - i * 8)) : u8(low >> (56 - (i - 8) * 8));
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[o++] = '-';
        out[o++] = hex[byte >> 4];
        out[o++] = hex[byte & 15];
    }
    out[36] = '\0';
}

Optional<Guid> Guid::Parse(StringView text)
{
    text = text.Trim();
    if (text.StartsWith('{') && text.EndsWith('}'))
        text = text.SubStr(1, text.Size() - 2);
    u8 b[16];
    usize n = 0;
    u32 nibbleCount = 0;
    u8 current = 0;
    for (char c : text) {
        if (c == '-')
            continue;
        u8 v;
        if (c >= '0' && c <= '9')
            v = u8(c - '0');
        else if (c >= 'a' && c <= 'f')
            v = u8(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v = u8(c - 'A' + 10);
        else
            return NullOpt;
        current = u8((current << 4) | v);
        if (++nibbleCount % 2 == 0) {
            if (n >= 16)
                return NullOpt;
            b[n++] = current;
            current = 0;
        }
    }
    if (n != 16 || nibbleCount != 32)
        return NullOpt;
    Guid g;
    for (int i = 0; i < 8; ++i) {
        g.high = (g.high << 8) | b[i];
        g.low = (g.low << 8) | b[i + 8];
    }
    return g;
}

} // namespace zx
