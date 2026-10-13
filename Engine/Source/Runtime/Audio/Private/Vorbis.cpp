// Ogg Vorbis decoder (Vorbis I specification): Ogg pages/packets, identification and setup headers (codebooks with
// Huffman + VQ lookups, floor 1, residues 0/1/2, mappings with channel coupling, modes), audio packets, IMDCT via a
// complex FFT, windowed overlap-add and end trimming by granule position. Floor 0 (unused by encoders since 2000) is
// not supported.

#include "Audio/AudioClip.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Math/MathCommon.h"
#include "Core/Memory/Memory.h"

#include <cmath>

namespace zx::audio {

namespace {

constexpr f64 PiD = 3.14159265358979323846;

u32 ILog(u32 x)
{
    u32 n = 0;
    while (x) {
        ++n;
        x >>= 1;
    }
    return n;
}

f32 Float32Unpack(u32 x)
{
    const i32 mantissa = i32(x & 0x1FFFFF);
    const u32 exponent = (x & 0x7FE00000) >> 21;
    const f64 m = (x & 0x80000000u) ? -f64(mantissa) : f64(mantissa);
    return f32(std::ldexp(m, i32(exponent) - 788));
}

u32 Lookup1Values(u32 entries, u32 dims)
{
    u32 r = u32(std::floor(std::pow(f64(entries), 1.0 / f64(dims))));
    // Fix rounding either way.
    auto pow = [&](u32 v) {
        u64 p = 1;
        for (u32 i = 0; i < dims; ++i) {
            p *= v;
            if (p > entries)
                return u64(entries) + 1;
        }
        return p;
    };
    while (r > 0 && pow(r) > entries)
        --r;
    while (pow(r + 1) <= entries)
        ++r;
    return r;
}

u32 BitReverse(u32 n)
{
    n = ((n & 0xAAAAAAAA) >> 1) | ((n & 0x55555555) << 1);
    n = ((n & 0xCCCCCCCC) >> 2) | ((n & 0x33333333) << 2);
    n = ((n & 0xF0F0F0F0) >> 4) | ((n & 0x0F0F0F0F) << 4);
    n = ((n & 0xFF00FF00) >> 8) | ((n & 0x00FF00FF) << 8);
    return (n >> 16) | (n << 16);
}

// --- Ogg --------------------------------------------------------------------------------------------------------------

/// Reassembles the packets of the first logical stream in an Ogg file held in memory.
class OggReader
{
public:
    void Init(Span<const u8> data)
    {
        m_data = data;
        m_pos = 0;
        m_segment = 0;
        m_segments = 0;
        m_serial = 0;
        m_haveSerial = false;
        m_pageEos = false;
        m_lastPage = false;
    }
    /// Next complete packet. `granule` is set when the packet ends a page (else -1); `last` when it ends the stream.
    bool NextPacket(Array<u8>& packet, i64& granule, bool& last)
    {
        packet.Clear();
        granule = -1;
        last = false;
        for (;;) {
            if (m_segment >= m_segments) {
                if (!NextPage())
                    return false;
                continue;
            }
            const u8 lacing = m_lacing[m_segment++];
            if (m_segmentPos + lacing > m_data.Size())
                return false;
            packet.Append(m_data.SubSpan(m_segmentPos, lacing));
            m_segmentPos += lacing;
            if (lacing < 255) {
                // A packet ends here; it carries the page granule if it is the last one ending on this page.
                bool laterEnd = false;
                for (u32 s = m_segment; s < m_segments; ++s)
                    if (m_lacing[s] < 255)
                        laterEnd = true;
                if (!laterEnd) {
                    granule = m_granule;
                    last = m_pageEos;
                }
                return true;
            }
        }
    }

private:
    bool NextPage()
    {
        while (m_pos + 27 <= m_data.Size()) {
            const u8* p = m_data.Data() + m_pos;
            if (MemCompare(p, "OggS", 4) != 0) {
                ++m_pos; // resync
                continue;
            }
            const u8 headerType = p[5];
            i64 granule;
            MemCopy(&granule, p + 6, 8);
            u32 serial;
            MemCopy(&serial, p + 14, 4);
            const u32 segments = p[26];
            if (m_pos + 27 + segments > m_data.Size())
                return false;
            usize bodySize = 0;
            for (u32 i = 0; i < segments; ++i)
                bodySize += p[27 + i];
            const usize next = m_pos + 27 + segments + bodySize;
            if (!m_haveSerial) {
                m_serial = serial;
                m_haveSerial = true;
            }
            if (serial != m_serial) { // another logical stream (e.g. a muxed video): skip
                m_pos = next;
                continue;
            }
            MemCopy(m_lacing, p + 27, segments);
            m_segments = segments;
            m_segment = 0;
            m_segmentPos = m_pos + 27 + segments;
            m_granule = granule;
            m_pageEos = (headerType & 4) != 0;
            m_pos = next;
            return true;
        }
        return false;
    }

    Span<const u8> m_data;
    usize m_pos = 0, m_segmentPos = 0;
    u8 m_lacing[255];
    u32 m_segment = 0, m_segments = 0;
    u32 m_serial = 0;
    bool m_haveSerial = false, m_pageEos = false, m_lastPage = false;
    i64 m_granule = -1;
};

// --- Bits -------------------------------------------------------------------------------------------------------------

class BitReader
{
public:
    BitReader(const u8* data, usize size) : m_data(data), m_size(size) {}
    u32 Read(u32 bits)
    {
        u32 v = 0;
        for (u32 i = 0; i < bits; ++i) {
            if (m_bytePos >= m_size) {
                m_eop = true;
                return 0;
            }
            v |= u32((m_data[m_bytePos] >> m_bitPos) & 1) << i;
            if (++m_bitPos == 8) {
                m_bitPos = 0;
                ++m_bytePos;
            }
        }
        return v;
    }
    /// Up to 32 bits without consuming (missing bits read as 0).
    u32 Peek(u32 bits) const
    {
        u32 v = 0;
        usize byte = m_bytePos;
        u32 bit = m_bitPos;
        for (u32 i = 0; i < bits && byte < m_size; ++i) {
            v |= u32((m_data[byte] >> bit) & 1) << i;
            if (++bit == 8) {
                bit = 0;
                ++byte;
            }
        }
        return v;
    }
    void Skip(u32 bits)
    {
        const u64 total = u64(m_bytePos) * 8 + m_bitPos + bits;
        if (total > u64(m_size) * 8) {
            m_eop = true;
            m_bytePos = m_size;
            m_bitPos = 0;
            return;
        }
        m_bytePos = usize(total / 8);
        m_bitPos = u32(total % 8);
    }
    u64 BitsLeft() const { return m_bytePos >= m_size ? 0 : u64(m_size - m_bytePos) * 8 - m_bitPos; }
    bool Eop() const { return m_eop; }

private:
    const u8* m_data;
    usize m_size;
    usize m_bytePos = 0;
    u32 m_bitPos = 0;
    bool m_eop = false;
};

// --- Codebooks ----------------------------------------------------------------------------------------------------------

constexpr u32 FastBits = 10;

struct Codebook
{
    u32 dimensions = 0;
    u32 entries = 0;
    Array<u8> lengths;             // 0 = unused entry
    Array<f32> vectors;            // entries x dimensions (lookup types 1/2), empty otherwise
    // Huffman decoding.
    Array<i16> fast;               // 2^FastBits -> entry (or -1)
    Array<u32> sortedCodewords;    // bit-reversed (LSB-first) codewords of entries longer than FastBits, sorted
    Array<u32> sortedEntries;
    Array<u8> sortedLengths;
    i32 singleEntry = -1;          // a codebook with exactly one used entry

    bool Build()
    {
        // Assign codewords in entry order (spec 3.2.1): the lowest available codeword of each length.
        u32 available[33] = {};
        Array<u32> codewords;
        codewords.Resize(entries);
        u32 used = 0;
        i32 lastUsed = -1;
        for (u32 i = 0; i < entries; ++i)
            if (lengths[i]) {
                ++used;
                lastUsed = i32(i);
            }
        if (used == 0)
            return true;
        if (used == 1) {
            singleEntry = lastUsed;
            return true;
        }
        bool first = true;
        for (u32 i = 0; i < entries; ++i) {
            const u32 len = lengths[i];
            if (!len)
                continue;
            if (first) {
                codewords[i] = 0;
                for (u32 l = 1; l <= len; ++l)
                    available[l] = 1u << (32 - l);
                first = false;
                continue;
            }
            u32 z = len;
            while (z > 0 && !available[z])
                --z;
            if (z == 0)
                return false; // over-specified tree
            const u32 res = available[z];
            available[z] = 0;
            codewords[i] = BitReverse(res); // LSB-first codeword
            if (z != len)
                for (u32 l = len; l > z; --l)
                    available[l] = res + (1u << (32 - l));
        }
        fast.Resize(1u << FastBits);
        for (i16& f : fast)
            f = -1;
        for (u32 i = 0; i < entries; ++i) {
            const u32 len = lengths[i];
            if (!len)
                continue;
            if (len <= FastBits) {
                for (u32 k = codewords[i]; k < (1u << FastBits); k += 1u << len)
                    fast[k] = i16(i);
            } else {
                sortedCodewords.PushBack(codewords[i]);
                sortedEntries.PushBack(i);
                sortedLengths.PushBack(u8(len));
            }
        }
        // Sort long codes by their bit-reversed (MSB-first) value for the binary search.
        Array<u32> order;
        for (u32 i = 0; i < sortedCodewords.Size(); ++i)
            order.PushBack(i);
        Sort(order, [&](u32 a, u32 b) { return BitReverse(sortedCodewords[a]) < BitReverse(sortedCodewords[b]); });
        Array<u32> cw, en;
        Array<u8> ln;
        for (u32 i : order) {
            cw.PushBack(BitReverse(sortedCodewords[i]));
            en.PushBack(sortedEntries[i]);
            ln.PushBack(sortedLengths[i]);
        }
        sortedCodewords = move(cw);
        sortedEntries = move(en);
        sortedLengths = move(ln);
        return true;
    }

    i32 DecodeScalar(BitReader& br) const
    {
        if (singleEntry >= 0) {
            br.Skip(lengths[u32(singleEntry)]);
            return br.Eop() ? -1 : singleEntry;
        }
        if (fast.Empty())
            return -1;
        const u32 peek = br.Peek(FastBits);
        const i16 f = fast[peek];
        if (f >= 0) {
            br.Skip(lengths[u32(f)]);
            return br.Eop() ? -1 : f;
        }
        // Long code: binary search over MSB-first codewords.
        const u32 x = BitReverse(br.Peek(32));
        i32 lo = 0, hi = i32(sortedCodewords.Size()) - 1, found = -1;
        while (lo <= hi) {
            const i32 mid = (lo + hi) / 2;
            if (sortedCodewords[u32(mid)] <= x) {
                found = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        if (found < 0)
            return -1;
        const u32 len = sortedLengths[u32(found)];
        const u32 mask = len >= 32 ? ~0u : ~((1u << (32 - len)) - 1);
        if ((x & mask) != sortedCodewords[u32(found)])
            return -1;
        br.Skip(len);
        return br.Eop() ? -1 : i32(sortedEntries[u32(found)]);
    }
};

struct Floor1
{
    u32 partitions = 0;
    u8 partitionClass[32] = {};
    u8 classDimensions[16] = {};
    u8 classSubclasses[16] = {};
    i16 classMasterbook[16] = {};
    i16 subclassBooks[16][8] = {};
    u32 multiplier = 1;
    Array<u32> x;            // X list in decode order
    Array<u32> sorted;       // indices sorted by x
    Array<u32> lowNeighbor, highNeighbor;
};

struct Residue
{
    u32 type = 0;
    u32 begin = 0, end = 0, partitionSize = 0, classifications = 0, classbook = 0;
    i16 books[64][8] = {};
};

struct Mapping
{
    u32 submaps = 1;
    Array<u32> couplingMagnitude, couplingAngle;
    u8 mux[256] = {};
    u8 submapFloor[16] = {};
    u8 submapResidue[16] = {};
};

struct Mode
{
    bool blockFlag = false;
    u32 mapping = 0;
};

const u32 FloorRange[4] = {256, 128, 86, 64};

f32 InverseDb(u32 i)
{
    static f32 table[256];
    static bool init = [] {
        for (u32 k = 0; k < 256; ++k)
            table[k] = f32(std::pow(10.0, (f64(k) - 255.0) * 7.0 / 256.0));
        return true;
    }();
    ZX_UNUSED(init);
    return table[Min(i, 255u)];
}

i32 RenderPoint(i32 x0, i32 y0, i32 x1, i32 y1, i32 x)
{
    const i32 dy = y1 - y0, adx = x1 - x0;
    const i32 err = Abs(dy) * (x - x0);
    const i32 off = err / adx;
    return dy < 0 ? y0 - off : y0 + off;
}

void RenderLine(i32 x0, i32 y0, i32 x1, i32 y1, i32* v, i32 n)
{
    const i32 dy = y1 - y0, adx = x1 - x0;
    i32 ady = Abs(dy);
    const i32 base = dy / adx;
    const i32 sy = dy < 0 ? base - 1 : base + 1;
    ady -= Abs(base) * adx;
    i32 x = x0, y = y0, err = 0;
    if (x < n)
        v[x] = y;
    for (x = x0 + 1; x < x1; ++x) {
        err += ady;
        if (err >= adx) {
            err -= adx;
            y += sy;
        } else {
            y += base;
        }
        if (x < n)
            v[x] = y;
    }
}

// --- FFT / IMDCT ---------------------------------------------------------------------------------------------------------

struct Imdct
{
    u32 n = 0; // block size (output samples)
    Array<f32> preCos, preSin, postCos, postSin, twCos, twSin;
    Array<u32> bitrev;
    Array<f32> re, im, u;

    void Init(u32 blockSize)
    {
        n = blockSize;
        const u32 M = n / 2, H = M / 2;
        preCos.Resize(H), preSin.Resize(H), postCos.Resize(H), postSin.Resize(H);
        for (u32 j = 0; j < H; ++j) {
            const f64 a = -PiD * (4.0 * j + 1.0) / (4.0 * M);
            preCos[j] = f32(std::cos(a));
            preSin[j] = f32(std::sin(a));
            const f64 b = -PiD * f64(j) / f64(M);
            postCos[j] = f32(std::cos(b));
            postSin[j] = f32(std::sin(b));
        }
        twCos.Resize(H), twSin.Resize(H);
        for (u32 k = 0; k < H; ++k) {
            twCos[k] = f32(std::cos(-2.0 * PiD * k / H));
            twSin[k] = f32(std::sin(-2.0 * PiD * k / H));
        }
        bitrev.Resize(H);
        const u32 bits = ILog(H) - 1;
        for (u32 i = 0; i < H; ++i)
            bitrev[i] = bits ? BitReverse(i) >> (32 - bits) : 0;
        re.Resize(H), im.Resize(H), u.Resize(M);
    }

    /// `x`: n/2 spectral coefficients -> `out`: n time samples.
    void Run(const f32* x, f32* out)
    {
        const u32 M = n / 2, H = M / 2;
        // DCT-IV of length M through a complex FFT of length M/2.
        for (u32 j = 0; j < H; ++j) {
            const f32 a = x[2 * j], b = x[M - 1 - 2 * j];
            const u32 r = bitrev[j];
            re[r] = a * preCos[j] - b * preSin[j];
            im[r] = a * preSin[j] + b * preCos[j];
        }
        for (u32 size = 2; size <= H; size <<= 1) {
            const u32 half = size / 2, step = H / size;
            for (u32 start = 0; start < H; start += size) {
                for (u32 k = 0; k < half; ++k) {
                    const f32 wr = twCos[k * step], wi = twSin[k * step];
                    const u32 i0 = start + k, i1 = i0 + half;
                    const f32 tr = re[i1] * wr - im[i1] * wi, ti = re[i1] * wi + im[i1] * wr;
                    re[i1] = re[i0] - tr;
                    im[i1] = im[i0] - ti;
                    re[i0] += tr;
                    im[i0] += ti;
                }
            }
        }
        for (u32 j = 0; j < H; ++j) {
            const f32 wr = re[j] * postCos[j] - im[j] * postSin[j];
            const f32 wi = re[j] * postSin[j] + im[j] * postCos[j];
            u[2 * j] = wr;
            u[M - 1 - 2 * j] = -wi;
        }
        // IMDCT from the DCT-IV (symmetries: u[2M-1-m] = -u[m], u[m+2M] = -u[m]).
        for (u32 i = 0; i < n; ++i) {
            const u32 m = i + M / 2;
            if (m < M)
                out[i] = u[m];
            else if (m < 2 * M)
                out[i] = -u[2 * M - 1 - m];
            else
                out[i] = -u[m - 2 * M];
        }
    }
};

// --- Decoder ------------------------------------------------------------------------------------------------------------

class VorbisDecoder final : public StreamDecoder
{
public:
    String error;

    bool Open(Span<const u8> data)
    {
        m_ogg.Init(data);
        Array<u8> packet;
        i64 granule;
        bool last;
        for (u32 h = 0; h < 3; ++h) {
            if (!m_ogg.NextPacket(packet, granule, last))
                return Fail("Truncated Vorbis headers");
            BitReader br(packet.Data(), packet.Size());
            const u32 type = br.Read(8);
            u8 magic[6];
            for (u8& m : magic)
                m = u8(br.Read(8));
            if (MemCompare(magic, "vorbis", 6) != 0 || type != 1u + 2u * h)
                return Fail("Not a Vorbis stream");
            if (h == 0 && !ReadIdentification(br))
                return false;
            if (h == 2 && !ReadSetup(br))
                return false;
        }
        m_data = data;
        m_audioStart = true;
        Reset();
        return true;
    }

    u32 Read(f32* out, u32 frames) override
    {
        u32 written = 0;
        while (written < frames) {
            if (m_pendingPos < m_pendingFrames) {
                const u32 n = Min(frames - written, m_pendingFrames - m_pendingPos);
                MemCopy(out + usize(written) * m_channels, m_pending.Data() + usize(m_pendingPos) * m_channels, usize(n) * m_channels * sizeof(f32));
                m_pendingPos += n;
                written += n;
                continue;
            }
            if (m_ended || !DecodeNextPacket())
                break;
        }
        return written;
    }

    bool Rewind() override
    {
        // Re-read the headers' packets to land on the first audio packet.
        m_ogg.Init(m_data);
        Array<u8> packet;
        i64 granule;
        bool last;
        for (u32 h = 0; h < 3; ++h)
            if (!m_ogg.NextPacket(packet, granule, last))
                return false;
        Reset();
        return true;
    }

    u32 Channels() const override { return m_channels; }
    u32 SampleRate() const override { return m_rate; }

private:
    bool Fail(StringView msg)
    {
        if (error.Empty())
            error = msg;
        return false;
    }

    void Reset()
    {
        m_havePrev = false;
        m_ended = false;
        m_pendingFrames = m_pendingPos = 0;
        m_samplesOut = 0;
    }

    bool ReadIdentification(BitReader& br)
    {
        if (br.Read(32) != 0)
            return Fail("Unsupported Vorbis version");
        m_channels = br.Read(8);
        m_rate = br.Read(32);
        br.Read(32), br.Read(32), br.Read(32); // bitrates
        const u32 b0 = br.Read(4), b1 = br.Read(4);
        m_blockSize[0] = 1u << b0;
        m_blockSize[1] = 1u << b1;
        if (m_channels == 0 || m_channels > 8 || m_rate == 0 || b0 < 6 || b1 > 13 || b0 > b1 || !br.Read(1))
            return Fail("Invalid Vorbis identification header");
        return true;
    }

    bool ReadSetup(BitReader& br)
    {
        // Codebooks.
        const u32 bookCount = br.Read(8) + 1;
        m_books.Resize(bookCount);
        for (Codebook& cb : m_books) {
            if (br.Read(24) != 0x564342)
                return Fail("Bad codebook sync");
            cb.dimensions = br.Read(16);
            cb.entries = br.Read(24);
            if (cb.dimensions == 0 || cb.entries == 0 || cb.entries > (1u << 20))
                return Fail("Bad codebook size");
            cb.lengths.Resize(cb.entries);
            if (br.Read(1)) { // ordered
                u32 len = br.Read(5) + 1, entry = 0;
                while (entry < cb.entries) {
                    const u32 count = br.Read(ILog(cb.entries - entry));
                    if (len > 32 || entry + count > cb.entries)
                        return Fail("Bad ordered codebook");
                    for (u32 i = 0; i < count; ++i)
                        cb.lengths[entry++] = u8(len);
                    ++len;
                }
            } else {
                const bool sparse = br.Read(1) != 0;
                for (u32 i = 0; i < cb.entries; ++i)
                    cb.lengths[i] = (!sparse || br.Read(1)) ? u8(br.Read(5) + 1) : 0;
            }
            const u32 lookup = br.Read(4);
            if (lookup == 1 || lookup == 2) {
                const f32 minimum = Float32Unpack(br.Read(32));
                const f32 delta = Float32Unpack(br.Read(32));
                const u32 valueBits = br.Read(4) + 1;
                const bool sequence = br.Read(1) != 0;
                const u32 values = lookup == 1 ? Lookup1Values(cb.entries, cb.dimensions) : cb.entries * cb.dimensions;
                Array<u32> mult;
                mult.Resize(values);
                for (u32& m : mult)
                    m = br.Read(valueBits);
                cb.vectors.Resize(usize(cb.entries) * cb.dimensions);
                for (u32 e = 0; e < cb.entries; ++e) {
                    f32 last = 0.0f;
                    u32 divisor = 1;
                    for (u32 d = 0; d < cb.dimensions; ++d) {
                        const u32 offset = lookup == 1 ? (e / divisor) % values : e * cb.dimensions + d;
                        const f32 v = f32(mult[offset]) * delta + minimum + last;
                        cb.vectors[usize(e) * cb.dimensions + d] = v;
                        if (sequence)
                            last = v;
                        if (lookup == 1)
                            divisor *= values;
                    }
                }
            } else if (lookup != 0) {
                return Fail("Bad codebook lookup type");
            }
            if (br.Eop() || !cb.Build())
                return Fail("Bad codebook");
        }
        // Time domain transforms (placeholders).
        const u32 times = br.Read(6) + 1;
        for (u32 i = 0; i < times; ++i)
            if (br.Read(16) != 0)
                return Fail("Bad time domain transform");
        // Floors.
        const u32 floorCount = br.Read(6) + 1;
        m_floors.Resize(floorCount);
        for (Floor1& f : m_floors) {
            if (br.Read(16) != 1)
                return Fail("Vorbis floor type 0 is not supported");
            f.partitions = br.Read(5);
            i32 maxClass = -1;
            for (u32 i = 0; i < f.partitions; ++i) {
                f.partitionClass[i] = u8(br.Read(4));
                maxClass = Max(maxClass, i32(f.partitionClass[i]));
            }
            for (i32 c = 0; c <= maxClass; ++c) {
                f.classDimensions[c] = u8(br.Read(3) + 1);
                f.classSubclasses[c] = u8(br.Read(2));
                f.classMasterbook[c] = f.classSubclasses[c] ? i16(br.Read(8)) : -1;
                for (u32 j = 0; j < (1u << f.classSubclasses[c]); ++j)
                    f.subclassBooks[c][j] = i16(i32(br.Read(8)) - 1);
            }
            f.multiplier = br.Read(2) + 1;
            const u32 rangeBits = br.Read(4);
            f.x.PushBack(0);
            f.x.PushBack(1u << rangeBits);
            for (u32 i = 0; i < f.partitions; ++i)
                for (u32 j = 0; j < f.classDimensions[f.partitionClass[i]]; ++j)
                    f.x.PushBack(br.Read(rangeBits));
            if (f.x.Size() > 65)
                return Fail("Too many floor points");
            // Neighbours and render order.
            const u32 count = u32(f.x.Size());
            f.lowNeighbor.Resize(count), f.highNeighbor.Resize(count);
            for (u32 i = 2; i < count; ++i) {
                u32 lo = 0, hi = 1;
                for (u32 j = 0; j < i; ++j) {
                    if (f.x[j] < f.x[i] && f.x[j] > f.x[lo])
                        lo = j;
                    if (f.x[j] > f.x[i] && f.x[j] < f.x[hi])
                        hi = j;
                }
                f.lowNeighbor[i] = lo;
                f.highNeighbor[i] = hi;
            }
            for (u32 i = 0; i < count; ++i)
                f.sorted.PushBack(i);
            Sort(f.sorted, [&](u32 a, u32 b) { return f.x[a] < f.x[b]; });
        }
        // Residues.
        const u32 residueCount = br.Read(6) + 1;
        m_residues.Resize(residueCount);
        for (Residue& r : m_residues) {
            r.type = br.Read(16);
            if (r.type > 2)
                return Fail("Bad residue type");
            r.begin = br.Read(24);
            r.end = br.Read(24);
            r.partitionSize = br.Read(24) + 1;
            r.classifications = br.Read(6) + 1;
            r.classbook = br.Read(8);
            u32 cascade[64];
            for (u32 i = 0; i < r.classifications; ++i) {
                u32 low = br.Read(3), high = 0;
                if (br.Read(1))
                    high = br.Read(5);
                cascade[i] = high * 8 + low;
            }
            for (u32 i = 0; i < r.classifications; ++i)
                for (u32 j = 0; j < 8; ++j)
                    r.books[i][j] = (cascade[i] & (1u << j)) ? i16(br.Read(8)) : i16(-1);
            if (r.classbook >= m_books.Size())
                return Fail("Bad residue classbook");
        }
        // Mappings.
        const u32 mappingCount = br.Read(6) + 1;
        m_mappings.Resize(mappingCount);
        for (Mapping& m : m_mappings) {
            if (br.Read(16) != 0)
                return Fail("Bad mapping type");
            m.submaps = br.Read(1) ? br.Read(4) + 1 : 1;
            if (br.Read(1)) {
                const u32 steps = br.Read(8) + 1;
                const u32 bits = ILog(m_channels - 1);
                for (u32 s = 0; s < steps; ++s) {
                    const u32 mag = br.Read(bits), ang = br.Read(bits);
                    if (mag == ang || mag >= m_channels || ang >= m_channels)
                        return Fail("Bad channel coupling");
                    m.couplingMagnitude.PushBack(mag);
                    m.couplingAngle.PushBack(ang);
                }
            }
            if (br.Read(2) != 0)
                return Fail("Bad mapping reserved bits");
            if (m.submaps > 1)
                for (u32 c = 0; c < m_channels; ++c)
                    m.mux[c] = u8(br.Read(4));
            for (u32 s = 0; s < m.submaps; ++s) {
                br.Read(8);
                m.submapFloor[s] = u8(br.Read(8));
                m.submapResidue[s] = u8(br.Read(8));
                if (m.submapFloor[s] >= m_floors.Size() || m.submapResidue[s] >= m_residues.Size())
                    return Fail("Bad submap");
            }
        }
        // Modes.
        const u32 modeCount = br.Read(6) + 1;
        m_modes.Resize(modeCount);
        for (Mode& md : m_modes) {
            md.blockFlag = br.Read(1) != 0;
            br.Read(16), br.Read(16);
            md.mapping = br.Read(8);
            if (md.mapping >= m_mappings.Size())
                return Fail("Bad mode");
        }
        if (!br.Read(1))
            return Fail("Bad setup framing");
        m_imdct[0].Init(m_blockSize[0]);
        m_imdct[1].Init(m_blockSize[1]);
        for (u32 i = 0; i < 2; ++i) {
            const u32 w = m_blockSize[i] / 2;
            m_window[i].Resize(w);
            for (u32 k = 0; k < w; ++k) {
                const f64 s = std::sin((f64(k) + 0.5) / f64(w) * PiD * 0.5);
                m_window[i][k] = f32(std::sin(PiD * 0.5 * s * s));
            }
        }
        for (u32 c = 0; c < m_channels; ++c) {
            m_spectrum[c].Resize(m_blockSize[1] / 2);
            m_prev[c].Resize(m_blockSize[1]);
            m_current[c].Resize(m_blockSize[1]);
        }
        return true;
    }

    /// Floor 1 for one channel: false = the channel is unused in this packet.
    bool DecodeFloor(BitReader& br, const Floor1& f, i32* finalY, bool* step2)
    {
        if (!br.Read(1))
            return false;
        const u32 range = FloorRange[f.multiplier - 1];
        i32 y[65];
        const u32 bits = ILog(range - 1);
        y[0] = i32(br.Read(bits));
        y[1] = i32(br.Read(bits));
        u32 offset = 2;
        for (u32 i = 0; i < f.partitions; ++i) {
            const u32 cls = f.partitionClass[i];
            const u32 cdim = f.classDimensions[cls], cbits = f.classSubclasses[cls], csub = (1u << cbits) - 1;
            u32 cval = 0;
            if (cbits) {
                const i32 v = m_books[u32(f.classMasterbook[cls])].DecodeScalar(br);
                if (v < 0)
                    return false;
                cval = u32(v);
            }
            for (u32 j = 0; j < cdim; ++j) {
                const i16 book = f.subclassBooks[cls][cval & csub];
                cval >>= cbits;
                if (book >= 0) {
                    const i32 v = m_books[u32(book)].DecodeScalar(br);
                    if (v < 0)
                        return false;
                    y[offset++] = v;
                } else {
                    y[offset++] = 0;
                }
            }
        }
        if (br.Eop())
            return false;
        // Amplitude value synthesis.
        const u32 count = u32(f.x.Size());
        step2[0] = step2[1] = true;
        finalY[0] = y[0];
        finalY[1] = y[1];
        for (u32 i = 2; i < count; ++i) {
            const u32 lo = f.lowNeighbor[i], hi = f.highNeighbor[i];
            const i32 predicted = RenderPoint(i32(f.x[lo]), finalY[lo], i32(f.x[hi]), finalY[hi], i32(f.x[i]));
            const i32 val = y[i];
            const i32 highroom = i32(range) - predicted, lowroom = predicted;
            const i32 room = (highroom < lowroom ? highroom : lowroom) * 2;
            if (val) {
                step2[lo] = step2[hi] = true;
                step2[i] = true;
                if (val >= room)
                    finalY[i] = highroom > lowroom ? val - lowroom + predicted : predicted - val + highroom - 1;
                else
                    finalY[i] = (val & 1) ? predicted - (val + 1) / 2 : predicted + val / 2;
            } else {
                step2[i] = false;
                finalY[i] = predicted;
            }
        }
        return true;
    }

    void RenderFloor(const Floor1& f, const i32* finalY, const bool* step2, f32* out, u32 n2)
    {
        i32 curve[4096];
        const i32 mult = i32(f.multiplier);
        i32 lx = 0, ly = finalY[f.sorted[0]] * mult, hx = 0, hy = ly;
        for (u32 s = 1; s < f.sorted.Size(); ++s) {
            const u32 i = f.sorted[s];
            if (!step2[i])
                continue;
            hy = finalY[i] * mult;
            hx = i32(f.x[i]);
            RenderLine(lx, ly, hx, hy, curve, i32(n2));
            lx = hx;
            ly = hy;
        }
        for (i32 x = hx; x < i32(n2); ++x)
            curve[x] = hy;
        for (u32 i = 0; i < n2; ++i)
            out[i] *= InverseDb(u32(Clamp(curve[i], 0, 255)));
    }

    void DecodeResidue(BitReader& br, const Residue& r, f32* const* vectors, const bool* doNotDecode, u32 channels, u32 n2)
    {
        const Codebook& classbook = m_books[r.classbook];
        // Type 2: one interleaved vector of all channels.
        if (r.type == 2) {
            bool any = false;
            for (u32 c = 0; c < channels; ++c)
                any |= !doNotDecode[c];
            if (!any)
                return;
            m_interleaved.Resize(usize(n2) * channels);
            MemZero(m_interleaved.Data(), m_interleaved.SizeBytes());
            f32* one = m_interleaved.Data();
            const bool decodeOne = false;
            DecodeResiduePartitions(br, r, classbook, &one, &decodeOne, 1, n2 * channels, true);
            for (u32 c = 0; c < channels; ++c)
                for (u32 i = 0; i < n2; ++i)
                    vectors[c][i] += m_interleaved[usize(i) * channels + c];
            return;
        }
        DecodeResiduePartitions(br, r, classbook, vectors, doNotDecode, channels, n2, r.type == 1);
    }

    void DecodeResiduePartitions(BitReader& br, const Residue& r, const Codebook& classbook, f32* const* vectors, const bool* doNotDecode,
                                 u32 channels, u32 size, bool format1)
    {
        const u32 begin = Min(r.begin, size), end = Min(r.end, size);
        if (end <= begin)
            return;
        const u32 classwords = classbook.dimensions;
        const u32 partitionsToRead = (end - begin) / r.partitionSize;
        m_classifications.Resize(usize(channels) * (partitionsToRead + classwords));
        for (u32 pass = 0; pass < 8; ++pass) {
            u32 partition = 0;
            while (partition < partitionsToRead) {
                if (pass == 0) {
                    for (u32 c = 0; c < channels; ++c) {
                        if (doNotDecode[c])
                            continue;
                        i32 temp = classbook.DecodeScalar(br);
                        if (temp < 0)
                            return;
                        for (i32 i = i32(classwords) - 1; i >= 0; --i) {
                            m_classifications[usize(c) * (partitionsToRead + classwords) + partition + u32(i)] = u8(u32(temp) % r.classifications);
                            temp = i32(u32(temp) / r.classifications);
                        }
                    }
                }
                for (u32 i = 0; i < classwords && partition < partitionsToRead; ++i, ++partition) {
                    for (u32 c = 0; c < channels; ++c) {
                        if (doNotDecode[c])
                            continue;
                        const u32 cls = m_classifications[usize(c) * (partitionsToRead + classwords) + partition];
                        const i16 bookIndex = r.books[cls][pass];
                        if (bookIndex < 0)
                            continue;
                        const Codebook& book = m_books[u32(bookIndex)];
                        if (book.vectors.Empty())
                            return;
                        f32* v = vectors[c] + begin + partition * r.partitionSize;
                        const u32 dims = book.dimensions;
                        if (format1) {
                            for (u32 k = 0; k < r.partitionSize;) {
                                const i32 e = book.DecodeScalar(br);
                                if (e < 0)
                                    return;
                                const f32* vec = book.vectors.Data() + usize(e) * dims;
                                for (u32 d = 0; d < dims && k < r.partitionSize; ++d, ++k)
                                    v[k] += vec[d];
                            }
                        } else {
                            const u32 step = r.partitionSize / dims;
                            for (u32 k = 0; k < step; ++k) {
                                const i32 e = book.DecodeScalar(br);
                                if (e < 0)
                                    return;
                                const f32* vec = book.vectors.Data() + usize(e) * dims;
                                for (u32 d = 0; d < dims; ++d)
                                    v[k + d * step] += vec[d];
                            }
                        }
                    }
                }
            }
        }
    }

    /// Decodes one audio packet; appends its finished PCM to m_pending. False at the end of the stream.
    bool DecodeNextPacket()
    {
        for (;;) {
            i64 granule;
            bool last;
            if (!m_ogg.NextPacket(m_packet, granule, last)) {
                m_ended = true;
                return false;
            }
            if (m_packet.Empty())
                continue;
            if (DecodeAudio(granule, last))
                return true;
            if (m_ended)
                return false;
        }
    }

    bool DecodeAudio(i64 granule, bool lastPacket)
    {
        BitReader br(m_packet.Data(), m_packet.Size());
        if (br.Read(1) != 0)
            return false; // not an audio packet
        const u32 modeIndex = br.Read(ILog(u32(m_modes.Size()) - 1));
        if (modeIndex >= m_modes.Size())
            return false;
        const Mode& mode = m_modes[modeIndex];
        const u32 bs = mode.blockFlag ? 1 : 0;
        const u32 n = m_blockSize[bs], n2 = n / 2;
        if (mode.blockFlag) {
            br.Read(1), br.Read(1); // previous / next window flags (implied by the neighbouring blocks)
        }
        const Mapping& map = m_mappings[mode.mapping];

        // Floors.
        i32 finalY[8][65];
        bool step2[8][65];
        bool used[8];
        for (u32 c = 0; c < m_channels; ++c) {
            const Floor1& f = m_floors[map.submapFloor[map.mux[c]]];
            used[c] = DecodeFloor(br, f, finalY[c], step2[c]);
            MemZero(m_spectrum[c].Data(), n2 * sizeof(f32));
        }
        // Coupled channels are decoded if either side is used.
        bool doNotDecode[8];
        for (u32 c = 0; c < m_channels; ++c)
            doNotDecode[c] = !used[c];
        for (usize s = 0; s < map.couplingMagnitude.Size(); ++s) {
            const u32 m = map.couplingMagnitude[s], a = map.couplingAngle[s];
            if (!doNotDecode[m] || !doNotDecode[a])
                doNotDecode[m] = doNotDecode[a] = false;
        }
        // Residues per submap.
        for (u32 s = 0; s < map.submaps; ++s) {
            f32* vecs[8];
            bool dnd[8];
            u32 count = 0;
            for (u32 c = 0; c < m_channels; ++c)
                if (map.mux[c] == s) {
                    vecs[count] = m_spectrum[c].Data();
                    dnd[count] = doNotDecode[c];
                    ++count;
                }
            DecodeResidue(br, m_residues[map.submapResidue[s]], vecs, dnd, count, n2);
        }
        // Inverse coupling.
        for (usize s = map.couplingMagnitude.Size(); s > 0; --s) {
            f32* mag = m_spectrum[map.couplingMagnitude[s - 1]].Data();
            f32* ang = m_spectrum[map.couplingAngle[s - 1]].Data();
            for (u32 i = 0; i < n2; ++i) {
                const f32 M = mag[i], A = ang[i];
                if (M > 0.0f) {
                    if (A > 0.0f) {
                        ang[i] = M - A;
                    } else {
                        ang[i] = M;
                        mag[i] = M + A;
                    }
                } else {
                    if (A > 0.0f) {
                        ang[i] = M + A;
                    } else {
                        ang[i] = M;
                        mag[i] = M - A;
                    }
                }
            }
        }
        // Floor curves x residue, IMDCT.
        for (u32 c = 0; c < m_channels; ++c) {
            if (used[c])
                RenderFloor(m_floors[map.submapFloor[map.mux[c]]], finalY[c], step2[c], m_spectrum[c].Data(), n2);
            else
                MemZero(m_spectrum[c].Data(), n2 * sizeof(f32));
            m_imdct[bs].Run(m_spectrum[c].Data(), m_current[c].Data());
        }

        // Overlap-add with the previous block: output runs from the previous block's center to this one's center.
        u32 produced = 0;
        if (m_havePrev) {
            const u32 pn = m_prevN;
            const u32 w = (m_prevLong && mode.blockFlag) ? m_blockSize[1] : m_blockSize[0]; // overlap window size
            const u32 w2 = w / 2;
            const f32* rise = m_window[w == m_blockSize[1] ? 1 : 0].Data();
            const u32 prevStart = pn / 2, prevOverlap = pn * 3 / 4 - w / 4, curOverlap = n / 4 - w / 4, curEnd = n / 2;
            const u32 total = pn / 4 + n / 4;
            m_pending.Resize(usize(total) * m_channels);
            for (u32 c = 0; c < m_channels; ++c) {
                const f32* prev = m_prev[c].Data();
                const f32* cur = m_current[c].Data();
                u32 o = 0;
                for (u32 i = prevStart; i < prevOverlap; ++i)
                    m_pending[usize(o++) * m_channels + c] = prev[i];
                for (u32 i = 0; i < w2; ++i)
                    m_pending[usize(o++) * m_channels + c] = prev[prevOverlap + i] * rise[w2 - 1 - i] + cur[curOverlap + i] * rise[i];
                for (u32 i = curOverlap + w2; i < curEnd; ++i)
                    m_pending[usize(o++) * m_channels + c] = cur[i];
            }
            produced = total;
        }
        for (u32 c = 0; c < m_channels; ++c)
            MemCopy(m_prev[c].Data(), m_current[c].Data(), n * sizeof(f32));
        m_prevN = n;
        m_prevLong = mode.blockFlag;
        m_havePrev = true;
        // The last page's granule position is the exact length of the stream: trim the final block.
        if (lastPacket && granule >= 0 && u64(granule) < m_samplesOut + produced)
            produced = u32(u64(granule) > m_samplesOut ? u64(granule) - m_samplesOut : 0);
        if (lastPacket)
            m_ended = true;
        m_samplesOut += produced;
        m_pendingFrames = produced;
        m_pendingPos = 0;
        return produced > 0;
    }

    OggReader m_ogg;
    Span<const u8> m_data;
    u32 m_channels = 0, m_rate = 0;
    u32 m_blockSize[2] = {};
    Array<Codebook> m_books;
    Array<Floor1> m_floors;
    Array<Residue> m_residues;
    Array<Mapping> m_mappings;
    Array<Mode> m_modes;
    Imdct m_imdct[2];
    Array<f32> m_window[2];
    Array<f32> m_spectrum[8], m_prev[8], m_current[8];
    Array<f32> m_interleaved;
    Array<u8> m_classifications;
    Array<u8> m_packet;
    Array<f32> m_pending;
    u32 m_pendingFrames = 0, m_pendingPos = 0;
    u32 m_prevN = 0;
    bool m_prevLong = false, m_havePrev = false, m_ended = false, m_audioStart = false;
    u64 m_samplesOut = 0;
};

} // namespace

Result<UniquePtr<StreamDecoder>> CreateVorbisDecoder(Span<const u8> data)
{
    UniquePtr<VorbisDecoder> d = MakeUnique<VorbisDecoder>();
    if (!d->Open(data))
        return Error::Make(ErrorCode::InvalidData, "Ogg Vorbis: {}", d->error);
    return UniquePtr<StreamDecoder>(d.Release());
}

Result<PcmData> DecodeVorbis(Span<const u8> data)
{
    Result<UniquePtr<StreamDecoder>> d = CreateVorbisDecoder(data);
    if (!d)
        return d.GetError();
    PcmData pcm;
    pcm.channels = (*d)->Channels();
    pcm.sampleRate = (*d)->SampleRate();
    f32 buffer[4096 * 8];
    for (;;) {
        const u32 n = (*d)->Read(buffer, 4096);
        if (n == 0)
            break;
        pcm.samples.Append(Span<const f32>(buffer, usize(n) * pcm.channels));
    }
    if (pcm.channels > 2) {
        // Same downmix as WAV.
        const usize frames = pcm.samples.Size() / pcm.channels;
        Array<f32> out;
        out.Resize(frames * 2);
        for (usize f = 0; f < frames; ++f) {
            f32 l = 0.0f, r = 0.0f;
            for (u32 c = 0; c < pcm.channels; ++c)
                (c % 2 == 0 ? l : r) += pcm.samples[f * pcm.channels + c];
            out[f * 2] = l * 2.0f / f32(pcm.channels);
            out[f * 2 + 1] = r * 2.0f / f32(pcm.channels);
        }
        pcm.samples = move(out);
        pcm.channels = 2;
    }
    return pcm;
}

} // namespace zx::audio
