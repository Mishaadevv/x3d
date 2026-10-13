#include "Core/Compression/Compression.h"

// ZLZ block format (LZ4-like):
//   sequence := token [litLenExt*] literals [offset16 [matchLenExt*]]
//   token    := (literalLength:4 << 4) | (matchLength - MinMatch):4, value 15 = extended by bytes (255 = continue)
//   The last sequence carries only literals (no offset).

namespace zx::compression {

namespace {
constexpr u32 MinMatch = 4;
constexpr u32 HashBits = 16;
constexpr u32 MaxOffset = 65535;
constexpr usize LastLiterals = 5; // matches never start within the last bytes (keeps decoder simple)

ZX_FORCEINLINE u32 Read32(const u8* p)
{
    u32 v;
    MemCopy(&v, p, 4);
    return v;
}
ZX_FORCEINLINE u32 Hash4(u32 v) { return (v * 2654435761u) >> (32 - HashBits); }

void WriteLength(Array<u8>& out, usize len)
{
    while (len >= 255) {
        out.PushBack(255);
        len -= 255;
    }
    out.PushBack(u8(len));
}
} // namespace

usize ZlzMaxCompressedSize(usize n) { return n + n / 255 + 16; }

usize ZlzCompress(ByteSpan input, Array<u8>& out)
{
    const usize start = out.Size();
    const u8* data = input.Data();
    const usize n = input.Size();
    out.Reserve(start + ZlzMaxCompressedSize(n));

    Array<u32> table;
    table.Resize(usize(1) << HashBits, U32Max);

    usize anchor = 0;
    usize pos = 0;
    const usize matchLimit = n > LastLiterals + MinMatch ? n - LastLiterals - MinMatch : 0;

    auto emit = [&](usize literalEnd, usize matchLen, u32 offset) {
        const usize litLen = literalEnd - anchor;
        const usize ml = matchLen ? matchLen - MinMatch : 0;
        out.PushBack(u8((Min<usize>(litLen, 15) << 4) | (matchLen ? Min<usize>(ml, 15) : 0)));
        if (litLen >= 15)
            WriteLength(out, litLen - 15);
        out.Append(Span<const u8>(data + anchor, litLen));
        if (matchLen) {
            out.PushBack(u8(offset));
            out.PushBack(u8(offset >> 8));
            if (ml >= 15)
                WriteLength(out, ml - 15);
        }
    };

    while (pos < matchLimit) {
        const u32 v = Read32(data + pos);
        const u32 h = Hash4(v);
        const u32 cand = table[h];
        table[h] = u32(pos);
        if (cand != U32Max && pos - cand <= MaxOffset && Read32(data + cand) == v) {
            usize len = MinMatch;
            const usize maxLen = n - LastLiterals - pos;
            while (len < maxLen && data[cand + len] == data[pos + len])
                ++len;
            // Extend backwards over pending literals.
            usize mpos = pos, mcand = cand;
            while (mpos > anchor && mcand > 0 && data[mpos - 1] == data[mcand - 1]) {
                --mpos;
                --mcand;
                ++len;
            }
            emit(mpos, len, u32(mpos - mcand));
            pos = mpos + len;
            anchor = pos;
            // Seed the table inside the match for better future matches.
            if (pos - 2 < matchLimit)
                table[Hash4(Read32(data + pos - 2))] = u32(pos - 2);
            continue;
        }
        ++pos;
    }
    // Final literals.
    pos = n;
    emit(n, 0, 0);
    return out.Size() - start;
}

bool ZlzDecompress(ByteSpan input, u8* output, usize outputSize)
{
    const u8* ip = input.Data();
    const u8* const ipEnd = ip + input.Size();
    u8* op = output;
    u8* const opEnd = output + outputSize;

    auto readLength = [&](usize& len) -> bool {
        for (;;) {
            if (ip >= ipEnd)
                return false;
            const u8 b = *ip++;
            len += b;
            if (b != 255)
                return true;
        }
    };

    while (ip < ipEnd) {
        const u8 token = *ip++;
        usize litLen = token >> 4;
        if (litLen == 15 && !readLength(litLen))
            return false;
        if (litLen > usize(ipEnd - ip) || litLen > usize(opEnd - op))
            return false;
        MemCopy(op, ip, litLen);
        ip += litLen;
        op += litLen;
        if (ip == ipEnd)
            break; // last sequence
        if (ipEnd - ip < 2)
            return false;
        const usize offset = usize(ip[0]) | (usize(ip[1]) << 8);
        ip += 2;
        if (offset == 0 || offset > usize(op - output))
            return false;
        usize matchLen = token & 15;
        if (matchLen == 15 && !readLength(matchLen))
            return false;
        matchLen += MinMatch;
        if (matchLen > usize(opEnd - op))
            return false;
        const u8* src = op - offset;
        if (offset >= matchLen) {
            MemCopy(op, src, matchLen);
            op += matchLen;
        } else {
            for (usize i = 0; i < matchLen; ++i)
                *op++ = src[i];
        }
    }
    return op == opEnd;
}

} // namespace zx::compression
