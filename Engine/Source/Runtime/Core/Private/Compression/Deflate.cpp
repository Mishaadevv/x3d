#include "Core/Compression/Compression.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Hash/Hash.h"

namespace zx::compression {

namespace {

constexpr u32 WindowSize = 32768;
constexpr u32 WindowMask = WindowSize - 1;
constexpr u32 MinMatch = 3;
constexpr u32 MaxMatch = 258;
constexpr u32 HashBits = 15;
constexpr u32 HashSize = 1u << HashBits;
constexpr u32 MaxBlockSymbols = 16384;

constexpr u16 LengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u8 LengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 DistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                              193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr u8 DistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr u8 CodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct CodeTables
{
    u8 lengthCode[MaxMatch + 1]; // length -> index into LengthBase
    u8 distCode[512];            // dist-1 < 256 -> code; else 256 + ((dist-1) >> 7)
    CodeTables()
    {
        for (u32 c = 0; c < 29; ++c) {
            const u32 end = c + 1 < 29 ? LengthBase[c + 1] : MaxMatch + 1;
            for (u32 l = LengthBase[c]; l < end && l <= MaxMatch; ++l)
                lengthCode[l] = u8(c);
        }
        lengthCode[MaxMatch] = 28;
        for (u32 c = 0; c < 30; ++c) {
            const u32 end = c + 1 < 30 ? DistBase[c + 1] : 32769;
            for (u32 d = DistBase[c]; d < end; ++d) {
                if (d - 1 < 256)
                    distCode[d - 1] = u8(c);
                else
                    distCode[256 + ((d - 1) >> 7)] = u8(c);
            }
        }
    }
    u32 DistanceCode(u32 dist) const { return dist - 1 < 256 ? distCode[dist - 1] : distCode[256 + ((dist - 1) >> 7)]; }
};

const CodeTables& Tables()
{
    static const CodeTables t;
    return t;
}

class BitWriter
{
public:
    explicit BitWriter(Array<u8>& out) : m_out(out) {}
    void Write(u32 value, u32 count)
    {
        m_bits |= u64(value) << m_count;
        m_count += count;
        while (m_count >= 8) {
            m_out.PushBack(u8(m_bits));
            m_bits >>= 8;
            m_count -= 8;
        }
    }
    void AlignToByte()
    {
        if (m_count > 0)
            Write(0, 8 - m_count);
    }

private:
    Array<u8>& m_out;
    u64 m_bits = 0;
    u32 m_count = 0;
};

constexpr u32 ReverseBits(u32 v, u32 bits)
{
    u32 r = 0;
    for (u32 i = 0; i < bits; ++i) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

/// Computes Huffman code lengths (<= maxLength) for frequencies. Unused symbols get length 0.
void BuildLengths(const u32* freqIn, u32 count, u32 maxLength, u8* lengths)
{
    MemZero(lengths, count);
    Array<u32> freq;
    freq.Resize(count);
    for (u32 i = 0; i < count; ++i)
        freq[i] = freqIn[i];

    for (;;) {
        // Collect used symbols.
        struct Node
        {
            u32 freq;
            i32 left, right; // children (-1 for leaves)
            u32 symbol;
        };
        Array<Node> nodes;
        for (u32 i = 0; i < count; ++i)
            if (freq[i])
                nodes.PushBack(Node{freq[i], -1, -1, i});
        const usize leafCount = nodes.Size();
        if (leafCount == 0)
            return;
        if (leafCount == 1) {
            lengths[nodes[0].symbol] = 1;
            return;
        }
        // Two-queue Huffman construction over leaves sorted by frequency.
        Sort(nodes, [](const Node& a, const Node& b) { return a.freq < b.freq || (a.freq == b.freq && a.symbol < b.symbol); });
        usize leafHead = 0;
        const usize firstInternal = nodes.Size();
        usize internalHead = firstInternal;
        auto popMin = [&]() -> i32 {
            const bool haveLeaf = leafHead < leafCount;
            const bool haveInternal = internalHead < nodes.Size();
            if (haveLeaf && (!haveInternal || nodes[leafHead].freq <= nodes[internalHead].freq))
                return i32(leafHead++);
            return i32(internalHead++);
        };
        for (usize k = 0; k + 1 < leafCount; ++k) {
            const i32 a = popMin();
            const i32 b = popMin();
            nodes.PushBack(Node{nodes[usize(a)].freq + nodes[usize(b)].freq, a, b, 0});
        }
        // Depth-first depth assignment from the root (last node).
        Array<u32> depth;
        depth.ResizeZeroed(nodes.Size());
        u32 maxDepth = 0;
        for (usize n = nodes.Size(); n > 0; --n) {
            const Node& node = nodes[n - 1];
            if (node.left >= 0) {
                depth[usize(node.left)] = depth[n - 1] + 1;
                depth[usize(node.right)] = depth[n - 1] + 1;
            } else {
                maxDepth = Max(maxDepth, depth[n - 1]);
            }
        }
        if (maxDepth <= maxLength) {
            for (usize i = 0; i < leafCount; ++i)
                lengths[nodes[i].symbol] = u8(depth[i]);
            return;
        }
        // Too deep: flatten the distribution and retry.
        for (u32 i = 0; i < count; ++i)
            if (freq[i])
                freq[i] = (freq[i] >> 1) | 1;
    }
}

/// Canonical codes (already bit-reversed for LSB-first writing).
void BuildCodes(const u8* lengths, u32 count, u16* codes)
{
    u32 lengthCount[16] = {};
    for (u32 i = 0; i < count; ++i)
        ++lengthCount[lengths[i]];
    lengthCount[0] = 0;
    u32 nextCode[16] = {};
    u32 code = 0;
    for (u32 l = 1; l < 16; ++l) {
        code = (code + lengthCount[l - 1]) << 1;
        nextCode[l] = code;
    }
    for (u32 i = 0; i < count; ++i)
        codes[i] = lengths[i] ? u16(ReverseBits(nextCode[lengths[i]]++, lengths[i])) : 0;
}

struct Symbol
{
    u16 litLen; // literal byte, or match length when dist > 0
    u16 dist;   // 0 = literal
};

void WriteStored(BitWriter& bw, Array<u8>& out, const u8* data, usize size, bool last)
{
    do {
        const u32 chunk = u32(Min<usize>(size, 65535));
        const bool final = last && chunk == size;
        bw.Write(final ? 1 : 0, 1);
        bw.Write(0, 2);
        bw.AlignToByte();
        out.PushBack(u8(chunk));
        out.PushBack(u8(chunk >> 8));
        out.PushBack(u8(~chunk));
        out.PushBack(u8(~chunk >> 8));
        out.Append(Span<const u8>(data, chunk));
        data += chunk;
        size -= chunk;
    } while (size > 0);
}

void WriteBlock(BitWriter& bw, Array<u8>& out, const Symbol* symbols, usize symbolCount, const u8* raw, usize rawSize,
                bool last)
{
    const CodeTables& t = Tables();
    u32 litFreq[286] = {};
    u32 distFreq[30] = {};
    for (usize i = 0; i < symbolCount; ++i) {
        const Symbol& s = symbols[i];
        if (s.dist == 0) {
            ++litFreq[s.litLen];
        } else {
            ++litFreq[257 + t.lengthCode[s.litLen]];
            ++distFreq[t.DistanceCode(s.dist)];
        }
    }
    litFreq[256] = 1;
    u8 litLen[286], distLen[30];
    BuildLengths(litFreq, 286, 15, litLen);
    BuildLengths(distFreq, 30, 15, distLen);
    bool anyDist = false;
    for (u8 l : distLen)
        anyDist |= l != 0;
    if (!anyDist)
        distLen[0] = 1; // at least one distance code must be present

    u32 hlit = 286;
    while (hlit > 257 && litLen[hlit - 1] == 0)
        --hlit;
    u32 hdist = 30;
    while (hdist > 1 && distLen[hdist - 1] == 0)
        --hdist;

    // Run-length encode the concatenated code lengths.
    u8 all[286 + 30];
    MemCopy(all, litLen, hlit);
    MemCopy(all + hlit, distLen, hdist);
    const u32 total = hlit + hdist;
    struct Rle
    {
        u8 sym, extra;
    };
    Array<Rle> rle;
    u32 clFreq[19] = {};
    for (u32 i = 0; i < total;) {
        const u8 v = all[i];
        u32 run = 1;
        while (i + run < total && all[i + run] == v)
            ++run;
        if (v == 0 && run >= 3) {
            const u32 n = Min<u32>(run, 138);
            if (n >= 11)
                rle.PushBack(Rle{18, u8(n - 11)});
            else
                rle.PushBack(Rle{17, u8(n - 3)});
            ++clFreq[rle.Back().sym];
            i += n;
        } else if (v != 0 && run >= 4) {
            rle.PushBack(Rle{v, 0});
            ++clFreq[v];
            const u32 n = Min<u32>(run - 1, 6);
            rle.PushBack(Rle{16, u8(n - 3)});
            ++clFreq[16];
            i += 1 + n;
        } else {
            rle.PushBack(Rle{v, 0});
            ++clFreq[v];
            ++i;
        }
    }
    u8 clLen[19];
    BuildLengths(clFreq, 19, 7, clLen);
    u32 hclen = 19;
    while (hclen > 4 && clLen[CodeLengthOrder[hclen - 1]] == 0)
        --hclen;

    u16 litCodes[286], distCodes[30], clCodes[19];
    BuildCodes(litLen, 286, litCodes);
    BuildCodes(distLen, 30, distCodes);
    BuildCodes(clLen, 19, clCodes);

    // Estimate dynamic size vs stored.
    u64 bits = 3 + 14 + hclen * 3;
    for (const Rle& r : rle)
        bits += clLen[r.sym] + (r.sym == 16 ? 2 : r.sym == 17 ? 3 : r.sym == 18 ? 7 : 0);
    for (u32 i = 0; i < 286; ++i)
        bits += u64(litFreq[i]) * litLen[i];
    for (u32 i = 0; i < 29; ++i)
        bits += u64(litFreq[257 + i]) * LengthExtra[i];
    for (u32 i = 0; i < 30; ++i)
        bits += u64(distFreq[i]) * (distLen[i] + DistExtra[i]);
    const u64 storedBits = (rawSize + 5 * (rawSize / 65535 + 1)) * 8 + 8;
    if (bits >= storedBits) {
        WriteStored(bw, out, raw, rawSize, last);
        return;
    }

    bw.Write(last ? 1 : 0, 1);
    bw.Write(2, 2);
    bw.Write(hlit - 257, 5);
    bw.Write(hdist - 1, 5);
    bw.Write(hclen - 4, 4);
    for (u32 i = 0; i < hclen; ++i)
        bw.Write(clLen[CodeLengthOrder[i]], 3);
    for (const Rle& r : rle) {
        bw.Write(clCodes[r.sym], clLen[r.sym]);
        if (r.sym == 16)
            bw.Write(r.extra, 2);
        else if (r.sym == 17)
            bw.Write(r.extra, 3);
        else if (r.sym == 18)
            bw.Write(r.extra, 7);
    }
    for (usize i = 0; i < symbolCount; ++i) {
        const Symbol& s = symbols[i];
        if (s.dist == 0) {
            bw.Write(litCodes[s.litLen], litLen[s.litLen]);
        } else {
            const u32 lc = t.lengthCode[s.litLen];
            bw.Write(litCodes[257 + lc], litLen[257 + lc]);
            bw.Write(s.litLen - LengthBase[lc], LengthExtra[lc]);
            const u32 dc = t.DistanceCode(s.dist);
            bw.Write(distCodes[dc], distLen[dc]);
            bw.Write(s.dist - DistBase[dc], DistExtra[dc]);
        }
    }
    bw.Write(litCodes[256], litLen[256]);
}

ZX_FORCEINLINE u32 Hash3(const u8* p) { return ((u32(p[0]) << 16 | u32(p[1]) << 8 | p[2]) * 2654435761u) >> (32 - HashBits); }

} // namespace

Array<u8> Deflate(ByteSpan input, i32 level)
{
    Array<u8> out;
    out.Reserve(input.Size() / 2 + 64);
    BitWriter bw(out);
    const u8* data = input.Data();
    const usize n = input.Size();

    if (level <= 0 || n < 16) {
        if (n == 0) {
            // Single empty fixed block: BFINAL=1, BTYPE=01, EOB (7 zero bits).
            bw.Write(1, 1);
            bw.Write(1, 2);
            bw.Write(0, 7);
        } else {
            WriteStored(bw, out, data, n, true);
        }
        bw.AlignToByte();
        return out;
    }
    level = Min(level, 9);
    const u32 maxChain = level <= 1 ? 4 : level <= 3 ? 16 : level <= 6 ? 64 : level <= 8 ? 256 : 1024;
    const bool lazy = level >= 4;
    const u32 niceLength = level >= 8 ? MaxMatch : 64 + u32(level) * 16;

    Array<i32> head;
    head.Resize(HashSize, -1);
    Array<i32> prev;
    prev.Resize(WindowSize, -1);
    Array<Symbol> symbols;
    symbols.Reserve(MaxBlockSymbols + 2);

    auto insert = [&](usize pos) {
        if (pos + MinMatch > n)
            return;
        const u32 h = Hash3(data + pos);
        prev[pos & WindowMask] = head[h];
        head[h] = i32(pos);
    };
    auto findMatch = [&](usize pos, u32& bestDist) -> u32 {
        if (pos + MinMatch > n)
            return 0;
        const u32 maxLen = u32(Min<usize>(MaxMatch, n - pos));
        u32 bestLen = 0;
        i32 cand = head[Hash3(data + pos)];
        u32 chain = maxChain;
        while (cand >= 0 && chain-- > 0) {
            const usize c = usize(cand);
            if (pos - c > WindowSize - 1 || c >= pos)
                break;
            if (data[c + bestLen] == data[pos + bestLen] && data[c] == data[pos]) {
                u32 len = 0;
                while (len < maxLen && data[c + len] == data[pos + len])
                    ++len;
                if (len > bestLen) {
                    bestLen = len;
                    bestDist = u32(pos - c);
                    if (len >= niceLength || len == maxLen)
                        break; // also prevents reading data[pos + maxLen] past the end
                }
            }
            const i32 next = prev[c & WindowMask];
            if (next >= cand)
                break; // stale chain entry
            cand = next;
        }
        return bestLen >= MinMatch ? bestLen : 0;
    };

    usize blockStart = 0;
    usize pos = 0;
    auto flush = [&](bool last) {
        WriteBlock(bw, out, symbols.Data(), symbols.Size(), data + blockStart, pos - blockStart, last);
        symbols.Clear();
        blockStart = pos;
    };

    while (pos < n) {
        u32 dist = 0;
        u32 len = findMatch(pos, dist);
        bool posInserted = false;
        if (len && lazy && pos + 1 < n && len < niceLength) {
            // Lazy matching: if the next position has a longer match, emit a literal here instead.
            insert(pos);
            u32 dist2 = 0;
            const u32 len2 = findMatch(pos + 1, dist2);
            if (len2 > len) {
                symbols.PushBack(Symbol{data[pos], 0});
                ++pos;
                len = len2;
                dist = dist2;
            } else {
                posInserted = true;
            }
        }
        if (len) {
            symbols.PushBack(Symbol{u16(len), u16(dist)});
            for (u32 k = posInserted ? 1 : 0; k < len; ++k)
                insert(pos + k);
            pos += len;
        } else {
            symbols.PushBack(Symbol{data[pos], 0});
            insert(pos);
            ++pos;
        }
        if (symbols.Size() >= MaxBlockSymbols)
            flush(pos >= n);
    }
    if (!symbols.Empty() || blockStart < n)
        flush(true);
    bw.AlignToByte();
    return out;
}

Array<u8> ZlibCompress(ByteSpan input, i32 level)
{
    Array<u8> out;
    out.PushBack(0x78);
    const u8 levelBits = level <= 1 ? 0 : level <= 5 ? 1 : level == 6 ? 2 : 3;
    u8 flg = u8(levelBits << 6);
    flg = u8(flg + (31 - ((0x78u << 8) | flg) % 31) % 31);
    out.PushBack(flg);
    const Array<u8> body = Deflate(input, level);
    out.Append(body.View());
    const u32 adler = Adler32(input.Data(), input.Size());
    out.PushBack(u8(adler >> 24));
    out.PushBack(u8(adler >> 16));
    out.PushBack(u8(adler >> 8));
    out.PushBack(u8(adler));
    return out;
}

} // namespace zx::compression
