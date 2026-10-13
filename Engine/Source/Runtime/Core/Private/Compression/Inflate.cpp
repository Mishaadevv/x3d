#include "Core/Compression/Compression.h"
#include "Core/Hash/Hash.h"

namespace zx::compression {

namespace {

constexpr u32 FastBits = 10;
constexpr u32 FastMask = (1u << FastBits) - 1;

constexpr u16 LengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u8 LengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 DistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                              193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr u8 DistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr u8 CodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

constexpr u32 ReverseBits(u32 v, u32 bits)
{
    u32 r = 0;
    for (u32 i = 0; i < bits; ++i) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

/// Canonical Huffman decoder: a FastBits lookup table plus a per-length slow path.
struct Huffman
{
    u16 fast[1u << FastBits]; // (length << 9) | symbol, 0 = not in fast table
    u16 firstCode[17];
    u16 firstSymbol[17];
    u32 maxCode[18]; // exclusive upper bound of codes of length l, left-aligned to 16 bits
    u8 lengths[288];
    u16 symbols[288]; // symbols sorted by (length, value)

    bool Build(const u8* codeLengths, u32 count)
    {
        u32 lengthCount[17] = {};
        MemZero(fast, sizeof(fast));
        for (u32 i = 0; i < count; ++i)
            ++lengthCount[codeLengths[i]];
        lengthCount[0] = 0;
        // Over-subscription check.
        i32 left = 1;
        for (u32 l = 1; l <= 16; ++l) {
            left <<= 1;
            left -= i32(lengthCount[l]);
            if (left < 0)
                return false;
        }
        u32 code = 0;
        u32 symbolIndex = 0;
        u32 nextCode[17];
        for (u32 l = 1; l <= 16; ++l) {
            nextCode[l] = code;
            firstCode[l] = u16(code);
            firstSymbol[l] = u16(symbolIndex);
            code += lengthCount[l];
            if (lengthCount[l] && code - 1 >= (1u << l))
                return false;
            maxCode[l] = code << (16 - l);
            code <<= 1;
            symbolIndex += lengthCount[l];
        }
        maxCode[17] = 0x10000;
        for (u32 i = 0; i < count; ++i) {
            const u32 len = codeLengths[i];
            lengths[i] = u8(len);
            if (!len)
                continue;
            const u32 c = nextCode[len]++;
            symbols[firstSymbol[len] + (c - firstCode[len])] = u16(i);
            if (len <= FastBits) {
                // Fill every table slot whose low `len` bits are the reversed code.
                const u32 rev = ReverseBits(c, len);
                for (u32 j = rev; j < (1u << FastBits); j += (1u << len))
                    fast[j] = u16((len << 9) | i);
            }
        }
        return true;
    }
};

class BitReader
{
public:
    BitReader(const u8* data, usize size) : m_data(data), m_end(data + size) {}

    ZX_FORCEINLINE void Refill()
    {
        while (m_count <= 56) {
            if (m_data < m_end) {
                m_bits |= u64(*m_data++) << m_count;
            } else {
                ++m_overrun; // pretend zeros past the end; detected later
            }
            m_count += 8;
        }
    }
    ZX_FORCEINLINE u32 Peek(u32 n)
    {
        if (m_count < n)
            Refill();
        return u32(m_bits & ((u64(1) << n) - 1));
    }
    ZX_FORCEINLINE void Consume(u32 n)
    {
        m_bits >>= n;
        m_count -= n;
    }
    ZX_FORCEINLINE u32 Read(u32 n)
    {
        if (n == 0)
            return 0;
        const u32 v = Peek(n);
        Consume(n);
        return v;
    }
    void AlignToByte() { Consume(m_count & 7); }
    /// Bytes past the real end that were consumed (corruption indicator).
    bool Overrun() const { return m_overrun > m_count / 8; }
    // Returns pointer to the next unconsumed byte after aligning (for stored blocks). Padding bytes past the end
    // that are still buffered were never read from m_data, so they are excluded.
    const u8* BytePointer() const { return m_data - (m_count / 8) + m_overrun; }
    void SkipToPointer(const u8* p)
    {
        m_data = p;
        m_bits = 0;
        m_count = 0;
        m_overrun = 0;
    }
    const u8* End() const { return m_end; }

private:
    const u8* m_data;
    const u8* m_end;
    u64 m_bits = 0;
    u32 m_count = 0;
    u32 m_overrun = 0;
};

i32 DecodeSymbol(BitReader& br, const Huffman& h)
{
    const u32 bits = br.Peek(16);
    const u16 f = h.fast[bits & FastMask];
    if (f) {
        br.Consume(f >> 9);
        return f & 511;
    }
    // Slow path: codes longer than FastBits. Reverse 16 bits to compare MSB-first.
    const u32 k = ReverseBits(bits, 16);
    u32 len;
    for (len = FastBits + 1; len <= 16; ++len)
        if (k < h.maxCode[len])
            break;
    if (len > 16)
        return -1;
    const u32 index = (k >> (16 - len)) - h.firstCode[len] + h.firstSymbol[len];
    if (index >= 288)
        return -1;
    br.Consume(len);
    return h.symbols[index];
}

struct FixedTables
{
    Huffman literal;
    Huffman distance;
    FixedTables()
    {
        u8 lengths[288];
        for (u32 i = 0; i < 144; ++i)
            lengths[i] = 8;
        for (u32 i = 144; i < 256; ++i)
            lengths[i] = 9;
        for (u32 i = 256; i < 280; ++i)
            lengths[i] = 7;
        for (u32 i = 280; i < 288; ++i)
            lengths[i] = 8;
        literal.Build(lengths, 288);
        u8 dl[30];
        for (u32 i = 0; i < 30; ++i)
            dl[i] = 5;
        distance.Build(dl, 30);
    }
};

const FixedTables& GetFixedTables()
{
    static const FixedTables tables;
    return tables;
}

} // namespace

bool Inflate(ByteSpan input, Array<u8>& out, usize expectedSize, usize maxOutput)
{
    if (expectedSize)
        out.Reserve(out.Size() + expectedSize);
    const usize outStart = out.Size();
    BitReader br(input.Data(), input.Size());
    Huffman dynLiteral, dynDistance;

    bool last = false;
    while (!last) {
        last = br.Read(1) != 0;
        const u32 type = br.Read(2);
        if (type == 0) {
            br.AlignToByte();
            const u8* p = br.BytePointer();
            if (p + 4 > br.End())
                return false;
            const u32 len = u32(p[0]) | (u32(p[1]) << 8);
            const u32 nlen = u32(p[2]) | (u32(p[3]) << 8);
            if ((len ^ 0xFFFF) != nlen || p + 4 + len > br.End())
                return false;
            if (out.Size() - outStart + len > maxOutput)
                return false;
            out.Append(Span<const u8>(p + 4, len));
            br.SkipToPointer(p + 4 + len);
            continue;
        }
        const Huffman* lit;
        const Huffman* dist;
        if (type == 1) {
            lit = &GetFixedTables().literal;
            dist = &GetFixedTables().distance;
        } else if (type == 2) {
            const u32 hlit = br.Read(5) + 257;
            const u32 hdist = br.Read(5) + 1;
            const u32 hclen = br.Read(4) + 4;
            u8 clLengths[19] = {};
            for (u32 i = 0; i < hclen; ++i)
                clLengths[CodeLengthOrder[i]] = u8(br.Read(3));
            Huffman clHuff;
            if (!clHuff.Build(clLengths, 19))
                return false;
            u8 lengths[288 + 32] = {};
            u32 n = 0;
            while (n < hlit + hdist) {
                const i32 sym = DecodeSymbol(br, clHuff);
                if (sym < 0 || br.Overrun())
                    return false;
                if (sym < 16) {
                    lengths[n++] = u8(sym);
                } else {
                    u32 repeat;
                    u8 value = 0;
                    if (sym == 16) {
                        if (n == 0)
                            return false;
                        value = lengths[n - 1];
                        repeat = 3 + br.Read(2);
                    } else if (sym == 17) {
                        repeat = 3 + br.Read(3);
                    } else {
                        repeat = 11 + br.Read(7);
                    }
                    if (n + repeat > hlit + hdist)
                        return false;
                    while (repeat--)
                        lengths[n++] = value;
                }
            }
            if (lengths[256] == 0)
                return false; // no end-of-block code
            if (!dynLiteral.Build(lengths, hlit) || !dynDistance.Build(lengths + hlit, hdist))
                return false;
            lit = &dynLiteral;
            dist = &dynDistance;
        } else {
            return false;
        }

        for (;;) {
            const i32 sym = DecodeSymbol(br, *lit);
            // Corrupt streams can decode the zero padding past the end as literals forever: stop at the overrun.
            if (sym < 0 || br.Overrun())
                return false;
            if (sym < 256) {
                if (out.Size() - outStart >= maxOutput)
                    return false;
                out.PushBack(u8(sym));
                continue;
            }
            if (sym == 256)
                break;
            const u32 li = u32(sym - 257);
            if (li >= 29)
                return false;
            const u32 length = LengthBase[li] + br.Read(LengthExtra[li]);
            const i32 ds = DecodeSymbol(br, *dist);
            if (ds < 0 || ds >= 30)
                return false;
            const u32 distance = DistBase[ds] + br.Read(DistExtra[ds]);
            const usize produced = out.Size() - outStart;
            if (distance > produced || produced + length > maxOutput)
                return false;
            const usize start = out.Size();
            out.ResizeUninitialized(start + length);
            u8* dst = out.Data() + start;
            const u8* src = dst - distance;
            if (distance >= length) {
                MemCopy(dst, src, length);
            } else {
                for (u32 i = 0; i < length; ++i)
                    dst[i] = src[i]; // overlapping copy (run-length)
            }
        }
        if (br.Overrun())
            return false;
    }
    return true;
}

bool ZlibDecompress(ByteSpan input, Array<u8>& output, usize expectedSize)
{
    if (input.Size() < 6)
        return false;
    const u8 cmf = input[0], flg = input[1];
    if ((cmf & 0x0F) != 8 || ((u32(cmf) << 8) | flg) % 31 != 0 || (flg & 0x20))
        return false; // not deflate / bad check / preset dictionary unsupported
    const usize start = output.Size();
    if (!Inflate(input.SubSpan(2, input.Size() - 6), output, expectedSize))
        return false;
    const u8* t = input.Data() + input.Size() - 4;
    const u32 expected = (u32(t[0]) << 24) | (u32(t[1]) << 16) | (u32(t[2]) << 8) | t[3];
    return Adler32(output.Data() + start, output.Size() - start) == expected;
}

} // namespace zx::compression
