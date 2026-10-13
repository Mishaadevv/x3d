#pragma once

#include "Core/Containers/Optional.h"
#include "Core/Hash/Hash.h"
#include "Core/String/Format.h"

namespace zx {

/// 128-bit globally unique identifier. Text form: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (lowercase hex).
struct Guid
{
    u64 high = 0;
    u64 low = 0;

    constexpr Guid() = default;
    constexpr Guid(u64 h, u64 l) : high(h), low(l) {}

    /// Random (version 4) GUID from the OS cryptographic RNG.
    static Guid New();
    /// Deterministic name-based (version 5, SHA-1) GUID: same namespace + name -> same GUID.
    static Guid FromName(const Guid& namespaceId, StringView name);
    static Optional<Guid> Parse(StringView text);

    constexpr bool IsValid() const { return high != 0 || low != 0; }
    /// Writes 36 chars + '\0' into out.
    void ToChars(char out[37]) const;

    friend constexpr bool operator==(const Guid&, const Guid&) = default;
    friend constexpr bool operator<(const Guid& a, const Guid& b)
    {
        return a.high != b.high ? a.high < b.high : a.low < b.low;
    }
};

template <>
struct Hasher<Guid>
{
    static u64 Hash(const Guid& g) { return HashCombine(g.high, g.low); }
};

template <>
struct Formatter<Guid>
{
    static void Format(FormatSink& s, const Guid& g, const FormatSpec& spec)
    {
        char buf[37];
        g.ToChars(buf);
        FormatString(s, StringView(buf, 36), spec);
    }
};

/// SHA-1 digest (20 bytes). Used for content hashes and GUID v5 - not for security.
struct Sha1Digest
{
    u8 bytes[20] = {};
    friend bool operator==(const Sha1Digest&, const Sha1Digest&) = default;
    /// 40 lowercase hex chars + '\0'.
    void ToHex(char out[41]) const;
};

class Sha1
{
public:
    Sha1() { Reset(); }
    void Reset();
    void Update(const void* data, usize size);
    void Update(StringView s) { Update(s.Data(), s.Size()); }
    Sha1Digest Finish();

    static Sha1Digest Hash(const void* data, usize size)
    {
        Sha1 s;
        s.Update(data, size);
        return s.Finish();
    }

private:
    void ProcessBlock(const u8* block);
    u32 m_state[5];
    u64 m_length;
    u8 m_buffer[64];
    u32 m_bufferSize;
};

} // namespace zx
