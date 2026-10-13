// JPEG decoder: baseline + extended (SOF0/SOF1) and progressive (SOF2) Huffman JPEG, 8-bit precision,
// 1 (grayscale) or 3 (YCbCr) components, any sampling factors, restart intervals. Output RGBA8.
// All coefficients are decoded first (needed for progressive), then dequantized + IDCT + color converted.

#include "Core/Image/Image.h"

#include <cmath>

namespace zx {

namespace {

constexpr u8 ZigZag[64 + 16] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33,
                                40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36,
                                29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54,
                                47, 55, 62, 63,
                                // padding for corrupt streams that overrun k
                                63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63};

struct Huffman
{
    u8 fast[512]; // 9-bit lookup: index into values (255 = slow path)
    u16 code[256];
    u8 size[256];
    u8 values[256];
    u32 maxCode[18];
    i32 delta[17]; // values index - code for each length
    bool valid = false;

    bool Build(const u8* counts, const u8* symbols, u32 total)
    {
        u32 k = 0;
        for (u32 len = 1; len <= 16; ++len)
            for (u32 i = 0; i < counts[len - 1]; ++i) {
                if (k >= 256)
                    return false;
                size[k++] = u8(len);
            }
        if (k != total)
            return false;
        MemCopy(values, symbols, total);
        u32 c = 0;
        k = 0;
        for (u32 len = 1; len <= 16; ++len) {
            delta[len] = i32(k) - i32(c);
            if (k < total && size[k] == len) {
                while (k < total && size[k] == len)
                    code[k++] = u16(c++);
                if (c - 1 >= (1u << len))
                    return false;
            }
            maxCode[len] = c << (16 - len);
            c <<= 1;
        }
        maxCode[17] = 0xFFFFFFFFu;
        MemSet(fast, 255, sizeof(fast));
        for (u32 i = 0; i < k; ++i) {
            if (size[i] <= 9) {
                const u32 shifted = u32(code[i]) << (9 - size[i]);
                const u32 count = 1u << (9 - size[i]);
                for (u32 j = 0; j < count; ++j)
                    fast[shifted + j] = u8(i);
            }
        }
        valid = true;
        return true;
    }
};

struct Component
{
    u8 id = 0;
    u32 h = 1, v = 1;
    u32 tq = 0;
    u32 td = 0, ta = 0; // huffman table indices for the current scan
    i32 dcPred = 0;
    u32 blocksW = 0, blocksH = 0;   // blocks covering the image for this component
    u32 paddedW = 0, paddedH = 0;   // blocks covering the full MCU grid
    Array<i16> coefficients;        // paddedW * paddedH * 64
    Array<u8> plane;                // paddedW*8 x paddedH*8 samples
};

class Decoder
{
public:
    explicit Decoder(ByteSpan data) : m_data(data) {}

    Result<Image> Decode()
    {
        if (m_data.Size() < 4 || m_data[0] != 0xFF || m_data[1] != 0xD8)
            return Error::Make(ErrorCode::InvalidData, "JPEG: missing SOI");
        m_pos = 2;
        bool frame = false;
        for (;;) {
            u8 marker = 0;
            if (!NextMarker(marker))
                return Error::Make(ErrorCode::InvalidData, "JPEG: unexpected end of data");
            if (marker == 0xD9) // EOI
                break;
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                m_progressive = marker == 0xC2;
                if (!ReadFrame())
                    return Error::Make(ErrorCode::InvalidData, "JPEG: bad frame header ({})", m_error);
                frame = true;
            } else if ((marker >= 0xC3 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                return Error::Make(ErrorCode::Unsupported, "JPEG: unsupported coding (SOF{:x}) - arithmetic/lossless", marker - 0xC0);
            } else if (marker == 0xC4) {
                if (!ReadHuffman())
                    return Error::Make(ErrorCode::InvalidData, "JPEG: bad Huffman table");
            } else if (marker == 0xDB) {
                if (!ReadQuant())
                    return Error::Make(ErrorCode::InvalidData, "JPEG: bad quantization table");
            } else if (marker == 0xDD) {
                u16 len;
                if (!ReadU16(len) || len != 4 || !ReadU16(m_restartInterval))
                    return Error::Make(ErrorCode::InvalidData, "JPEG: bad DRI");
            } else if (marker == 0xDA) {
                if (!frame)
                    return Error::Make(ErrorCode::InvalidData, "JPEG: SOS before SOF");
                if (!ReadScan())
                    return Error::Make(ErrorCode::InvalidData, "JPEG: corrupt scan ({})", m_error);
            } else if (marker == 0xEE) {
                // Adobe APP14: transform flag (0 = no YCbCr conversion for 3 components)
                u16 len;
                if (!ReadU16(len) || len < 2 || m_pos + len - 2 > m_data.Size())
                    return Error::Make(ErrorCode::InvalidData, "JPEG: bad APP14");
                if (len >= 14 && MemCompare(m_data.Data() + m_pos, "Adobe", 5) == 0)
                    m_adobeTransform = i32(m_data[m_pos + 11]);
                m_pos += len - 2;
            } else {
                u16 len;
                if (!ReadU16(len) || len < 2 || m_pos + len - 2 > m_data.Size())
                    return Error::Make(ErrorCode::InvalidData, "JPEG: bad segment");
                m_pos += len - 2;
            }
        }
        if (!frame)
            return Error::Make(ErrorCode::InvalidData, "JPEG: no frame");
        return Finish();
    }

private:
    // --- Byte-level parsing ---
    bool ReadU8(u8& v)
    {
        if (m_pos >= m_data.Size())
            return false;
        v = m_data[m_pos++];
        return true;
    }
    bool ReadU16(u16& v)
    {
        if (m_pos + 2 > m_data.Size())
            return false;
        v = u16((m_data[m_pos] << 8) | m_data[m_pos + 1]);
        m_pos += 2;
        return true;
    }
    bool NextMarker(u8& marker)
    {
        // Skip fill bytes / garbage until 0xFF xx (xx != 0, xx != 0xFF).
        while (m_pos + 1 < m_data.Size()) {
            if (m_data[m_pos] == 0xFF && m_data[m_pos + 1] != 0 && m_data[m_pos + 1] != 0xFF) {
                marker = m_data[m_pos + 1];
                m_pos += 2;
                return true;
            }
            ++m_pos;
        }
        return false;
    }

    bool ReadFrame()
    {
        u16 len;
        u8 precision, count;
        u16 h, w;
        if (!ReadU16(len) || !ReadU8(precision) || !ReadU16(h) || !ReadU16(w) || !ReadU8(count)) {
            m_error = "truncated";
            return false;
        }
        if (precision != 8) {
            m_error = "only 8-bit precision is supported";
            return false;
        }
        if (w == 0 || h == 0 || (count != 1 && count != 3)) {
            m_error = "invalid size or component count";
            return false;
        }
        m_width = w;
        m_height = h;
        m_components.Resize(count);
        for (Component& c : m_components) {
            u8 hv, tq;
            if (!ReadU8(c.id) || !ReadU8(hv) || !ReadU8(tq)) {
                m_error = "truncated components";
                return false;
            }
            c.h = hv >> 4;
            c.v = hv & 15;
            c.tq = tq;
            if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4 || c.tq > 3) {
                m_error = "bad sampling factors";
                return false;
            }
            m_hMax = Max(m_hMax, c.h);
            m_vMax = Max(m_vMax, c.v);
        }
        m_mcusX = (m_width + 8 * m_hMax - 1) / (8 * m_hMax);
        m_mcusY = (m_height + 8 * m_vMax - 1) / (8 * m_vMax);
        for (Component& c : m_components) {
            c.blocksW = (u32(m_width) * c.h + 8 * m_hMax - 1) / (8 * m_hMax);
            c.blocksH = (u32(m_height) * c.v + 8 * m_vMax - 1) / (8 * m_vMax);
            c.paddedW = m_mcusX * c.h;
            c.paddedH = m_mcusY * c.v;
            c.coefficients.ResizeZeroed(usize(c.paddedW) * c.paddedH * 64);
        }
        return true;
    }

    bool ReadHuffman()
    {
        u16 len;
        if (!ReadU16(len) || len < 2)
            return false;
        usize end = m_pos + len - 2;
        if (end > m_data.Size())
            return false;
        while (m_pos < end) {
            u8 info;
            ReadU8(info);
            const u32 tc = info >> 4, th = info & 15;
            if (tc > 1 || th > 3 || m_pos + 16 > end)
                return false;
            const u8* counts = m_data.Data() + m_pos;
            m_pos += 16;
            u32 total = 0;
            for (u32 i = 0; i < 16; ++i)
                total += counts[i];
            if (total > 256 || m_pos + total > end)
                return false;
            if (!(tc == 0 ? m_dc[th] : m_ac[th]).Build(counts, m_data.Data() + m_pos, total))
                return false;
            m_pos += total;
        }
        return true;
    }

    bool ReadQuant()
    {
        u16 len;
        if (!ReadU16(len) || len < 2)
            return false;
        const usize end = m_pos + len - 2;
        if (end > m_data.Size())
            return false;
        while (m_pos < end) {
            u8 info;
            ReadU8(info);
            const u32 pq = info >> 4, tq = info & 15;
            if (tq > 3 || pq > 1)
                return false;
            for (u32 i = 0; i < 64; ++i) {
                u16 v;
                if (pq) {
                    if (!ReadU16(v))
                        return false;
                } else {
                    u8 b;
                    if (!ReadU8(b))
                        return false;
                    v = b;
                }
                m_quant[tq][ZigZag[i]] = v;
            }
        }
        return true;
    }

    // --- Entropy decoding ---
    void ResetBits()
    {
        m_bits = 0;
        m_bitCount = 0;
        m_hitMarker = false;
    }
    void FillBits()
    {
        while (m_bitCount <= 24) {
            u32 byte = 0;
            if (!m_hitMarker && m_pos < m_data.Size()) {
                byte = m_data[m_pos];
                if (byte == 0xFF) {
                    const u8 next = m_pos + 1 < m_data.Size() ? m_data[m_pos + 1] : 0;
                    if (next == 0x00) {
                        m_pos += 2; // stuffed 0xFF
                    } else {
                        m_hitMarker = true; // marker: feed zeros, leave pos at the marker
                        byte = 0;
                    }
                } else {
                    ++m_pos;
                }
            }
            m_bits |= byte << (24 - m_bitCount);
            m_bitCount += 8;
        }
    }
    u32 GetBits(u32 n)
    {
        if (n == 0)
            return 0;
        FillBits();
        const u32 v = m_bits >> (32 - n);
        m_bits <<= n;
        m_bitCount -= n;
        return v;
    }
    u32 GetBit() { return GetBits(1); }
    static i32 Extend(u32 v, u32 n) { return n == 0 ? 0 : (v < (1u << (n - 1)) ? i32(v) - i32((1u << n) - 1) : i32(v)); }

    i32 DecodeHuffman(const Huffman& h)
    {
        if (!h.valid)
            return -1;
        FillBits();
        const u32 peek9 = m_bits >> (32 - 9);
        const u8 k = h.fast[peek9];
        if (k != 255) {
            const u32 s = h.size[k];
            m_bits <<= s;
            m_bitCount -= s;
            return h.values[k];
        }
        const u32 peek16 = m_bits >> 16;
        u32 len = 10;
        while (len <= 16 && peek16 >= h.maxCode[len])
            ++len;
        if (len > 16)
            return -1;
        const i32 index = i32(m_bits >> (32 - len)) + h.delta[len];
        if (index < 0 || index > 255)
            return -1;
        m_bits <<= len;
        m_bitCount -= len;
        return h.values[index];
    }

    bool ReadScan()
    {
        u16 len;
        u8 count;
        if (!ReadU16(len) || !ReadU8(count) || count < 1 || count > m_components.Size())
            return false;
        SmallArray<Component*, 4> scan;
        for (u32 i = 0; i < count; ++i) {
            u8 id, tables;
            if (!ReadU8(id) || !ReadU8(tables))
                return false;
            Component* found = nullptr;
            for (Component& c : m_components)
                if (c.id == id)
                    found = &c;
            if (!found)
                return false;
            found->td = tables >> 4;
            found->ta = tables & 15;
            if (found->td > 3 || found->ta > 3)
                return false;
            scan.PushBack(found);
        }
        u8 ss, se, a;
        if (!ReadU8(ss) || !ReadU8(se) || !ReadU8(a))
            return false;
        const u32 ah = a >> 4, al = a & 15;
        if (!m_progressive) {
            ss = 0;
            se = 63;
        }
        if (ss > 63 || se > 63 || ss > se || (ss == 0 && se != 0 && m_progressive)) {
            m_error = "bad spectral selection";
            return false;
        }
        for (Component* c : scan)
            c->dcPred = 0;
        m_eobRun = 0;
        ResetBits();

        const bool single = scan.Size() == 1;
        const u32 mcuW = single ? scan[0]->blocksW : m_mcusX;
        const u32 mcuH = single ? scan[0]->blocksH : m_mcusY;
        u32 restartCount = 0;
        for (u32 my = 0; my < mcuH; ++my) {
            for (u32 mx = 0; mx < mcuW; ++mx) {
                if (m_restartInterval && restartCount == m_restartInterval) {
                    if (!HandleRestart(scan))
                        return false;
                    restartCount = 0;
                }
                ++restartCount;
                for (Component* c : scan) {
                    const u32 bw = single ? 1 : c->h;
                    const u32 bh = single ? 1 : c->v;
                    for (u32 by = 0; by < bh; ++by)
                        for (u32 bx = 0; bx < bw; ++bx) {
                            const u32 blockX = single ? mx : mx * c->h + bx;
                            const u32 blockY = single ? my : my * c->v + by;
                            i16* block = c->coefficients.Data() + (usize(blockY) * c->paddedW + blockX) * 64;
                            if (!DecodeBlock(*c, block, ss, se, ah, al)) {
                                m_error = "Huffman decoding failed";
                                return false;
                            }
                        }
                }
            }
        }
        // Skip to the next marker (the entropy-coded segment ends at one).
        {
            while (m_pos + 1 < m_data.Size() && !(m_data[m_pos] == 0xFF && m_data[m_pos + 1] != 0 &&
                                                 !(m_data[m_pos + 1] >= 0xD0 && m_data[m_pos + 1] <= 0xD7)))
                ++m_pos;
        }
        return true;
    }

    bool HandleRestart(Span<Component*> scan)
    {
        // Expect RSTn; tolerate missing markers.
        ResetBits();
        while (m_pos + 1 < m_data.Size() && !(m_data[m_pos] == 0xFF && m_data[m_pos + 1] >= 0xD0 && m_data[m_pos + 1] <= 0xD7))
            ++m_pos;
        if (m_pos + 1 < m_data.Size())
            m_pos += 2;
        for (Component* c : scan)
            c->dcPred = 0;
        m_eobRun = 0;
        return true;
    }

    bool DecodeBlock(Component& c, i16* block, u32 ss, u32 se, u32 ah, u32 al)
    {
        if (!m_progressive) {
            // Baseline: DC + all AC.
            const i32 t = DecodeHuffman(m_dc[c.td]);
            if (t < 0 || t > 16)
                return false;
            c.dcPred += Extend(GetBits(u32(t)), u32(t));
            block[0] = i16(c.dcPred);
            for (u32 k = 1; k < 64;) {
                const i32 rs = DecodeHuffman(m_ac[c.ta]);
                if (rs < 0)
                    return false;
                const u32 r = u32(rs) >> 4, s = u32(rs) & 15;
                if (s == 0) {
                    if (r != 15)
                        break; // EOB
                    k += 16;
                    continue;
                }
                k += r;
                if (k > 63)
                    return false;
                block[ZigZag[k++]] = i16(Extend(GetBits(s), s));
            }
            return true;
        }
        if (ss == 0) {
            // DC scans (progressive).
            if (ah == 0) {
                const i32 t = DecodeHuffman(m_dc[c.td]);
                if (t < 0 || t > 16)
                    return false;
                c.dcPred += Extend(GetBits(u32(t)), u32(t));
                block[0] = i16(c.dcPred * (1 << al));
            } else if (GetBit()) {
                block[0] = i16(block[0] | (1 << al));
            }
            return true;
        }
        if (ah == 0) {
            // AC first pass.
            if (m_eobRun > 0) {
                --m_eobRun;
                return true;
            }
            for (u32 k = ss; k <= se;) {
                const i32 rs = DecodeHuffman(m_ac[c.ta]);
                if (rs < 0)
                    return false;
                const u32 r = u32(rs) >> 4, s = u32(rs) & 15;
                if (s == 0) {
                    if (r < 15) {
                        m_eobRun = (1u << r) - 1;
                        if (r)
                            m_eobRun += GetBits(r);
                        break;
                    }
                    k += 16;
                    continue;
                }
                k += r;
                if (k > 63)
                    return false;
                block[ZigZag[k++]] = i16(Extend(GetBits(s), s) * (1 << al));
            }
            return true;
        }
        // AC refinement (libjpeg decode_mcu_AC_refine).
        const i32 p1 = 1 << al;
        const i32 m1 = -1 << al;
        u32 k = ss;
        if (m_eobRun == 0) {
            for (; k <= se;) {
                const i32 rs = DecodeHuffman(m_ac[c.ta]);
                if (rs < 0)
                    return false;
                i32 r = rs >> 4;
                const u32 s = u32(rs) & 15;
                i32 value = 0;
                if (s == 0) {
                    if (r < 15) {
                        m_eobRun = 1u << r;
                        if (r)
                            m_eobRun += GetBits(u32(r));
                        break; // remaining coefficients handled by the EOB-run refinement below
                    }
                    // r == 15: ZRL - skip 16 zero coefficients (refining nonzero ones on the way)
                } else {
                    if (s != 1)
                        return false;
                    value = GetBit() ? p1 : m1;
                }
                while (k <= se) {
                    i16& coef = block[ZigZag[k]];
                    if (coef != 0) {
                        if (GetBit() && (coef & p1) == 0)
                            coef = i16(coef >= 0 ? coef + p1 : coef + m1);
                    } else {
                        if (r == 0) {
                            if (value)
                                coef = i16(value);
                            ++k;
                            break;
                        }
                        --r;
                    }
                    ++k;
                }
            }
        }
        if (m_eobRun > 0) {
            for (; k <= se; ++k) {
                i16& coef = block[ZigZag[k]];
                if (coef != 0 && GetBit() && (coef & p1) == 0)
                    coef = i16(coef >= 0 ? coef + p1 : coef + m1);
            }
            --m_eobRun;
        }
        return true;
    }

    // --- Reconstruction ---
    static void Idct(const i16* in, const u16* quant, u8* out, usize stride)
    {
        struct CosTable
        {
            f32 t[8][8];
            CosTable()
            {
                for (u32 x = 0; x < 8; ++x)
                    for (u32 u = 0; u < 8; ++u)
                        t[x][u] = f32((u == 0 ? 1.0 / std::sqrt(2.0) : 1.0) *
                                      std::cos((2.0 * x + 1.0) * u * 3.14159265358979 / 16.0));
            }
        };
        static const CosTable table; // thread-safe initialization
        const auto& cosTable = table.t;
        f32 coef[64];
        for (u32 i = 0; i < 64; ++i)
            coef[i] = f32(in[i]) * f32(quant[i]);
        f32 tmp[64];
        for (u32 y = 0; y < 8; ++y) // rows: u -> x
            for (u32 x = 0; x < 8; ++x) {
                f32 s = 0.0f;
                for (u32 u = 0; u < 8; ++u)
                    s += cosTable[x][u] * coef[y * 8 + u];
                tmp[y * 8 + x] = s * 0.5f;
            }
        for (u32 x = 0; x < 8; ++x) // columns: v -> y
            for (u32 y = 0; y < 8; ++y) {
                f32 s = 0.0f;
                for (u32 v = 0; v < 8; ++v)
                    s += cosTable[y][v] * tmp[v * 8 + x];
                const i32 value = i32(std::lround(s * 0.5f + 128.0f));
                out[y * stride + x] = u8(value < 0 ? 0 : value > 255 ? 255 : value);
            }
    }

    /// Component sample at full-resolution pixel (x, y). Subsampled planes are interpolated bilinearly between
    /// sample centers ("fancy upsampling" - matches libjpeg's triangle filter for 2x factors).
    u8 SampleComponent(const Component& c, u32 x, u32 y) const
    {
        const usize stride = usize(c.paddedW) * 8;
        if (c.h == m_hMax && c.v == m_vMax)
            return c.plane[usize(y) * stride + x];
        const u32 planeW = (m_width * c.h + m_hMax - 1) / m_hMax;
        const u32 planeH = (m_height * c.v + m_vMax - 1) / m_vMax;
        const f32 fx = (f32(x) + 0.5f) * f32(c.h) / f32(m_hMax) - 0.5f;
        const f32 fy = (f32(y) + 0.5f) * f32(c.v) / f32(m_vMax) - 0.5f;
        const i32 x0 = i32(std::floor(fx)), y0 = i32(std::floor(fy));
        const f32 tx = fx - f32(x0), ty = fy - f32(y0);
        auto at = [&](i32 px, i32 py) {
            px = px < 0 ? 0 : (px >= i32(planeW) ? i32(planeW) - 1 : px);
            py = py < 0 ? 0 : (py >= i32(planeH) ? i32(planeH) - 1 : py);
            return f32(c.plane[usize(py) * stride + usize(px)]);
        };
        const f32 top = at(x0, y0) * (1.0f - tx) + at(x0 + 1, y0) * tx;
        const f32 bottom = at(x0, y0 + 1) * (1.0f - tx) + at(x0 + 1, y0 + 1) * tx;
        const f32 v = top * (1.0f - ty) + bottom * ty;
        return u8(v + 0.5f);
    }

    Result<Image> Finish()
    {
        for (Component& c : m_components) {
            const usize stride = usize(c.paddedW) * 8;
            c.plane.ResizeUninitialized(stride * c.paddedH * 8);
            for (u32 by = 0; by < c.paddedH; ++by)
                for (u32 bx = 0; bx < c.paddedW; ++bx)
                    Idct(c.coefficients.Data() + (usize(by) * c.paddedW + bx) * 64, m_quant[c.tq],
                         c.plane.Data() + usize(by) * 8 * stride + bx * 8, stride);
            c.coefficients.Reset();
        }
        Image img = Image::Create(m_width, m_height, ImageFormat::RGBA8);
        const bool ycc = m_components.Size() == 3 && m_adobeTransform != 0;
        for (u32 y = 0; y < m_height; ++y) {
            u8* row = img.Row(y);
            for (u32 x = 0; x < m_width; ++x) {
                u8 s[3];
                for (usize i = 0; i < m_components.Size(); ++i)
                    s[i] = SampleComponent(m_components[i], x, y);
                u8* p = row + usize(x) * 4;
                if (m_components.Size() == 1) {
                    p[0] = p[1] = p[2] = s[0];
                } else if (ycc) {
                    const f32 Y = s[0], cb = f32(s[1]) - 128.0f, cr = f32(s[2]) - 128.0f;
                    auto clamp = [](f32 v) { return u8(v < 0 ? 0 : v > 255 ? 255 : i32(v + 0.5f)); };
                    p[0] = clamp(Y + 1.402f * cr);
                    p[1] = clamp(Y - 0.344136f * cb - 0.714136f * cr);
                    p[2] = clamp(Y + 1.772f * cb);
                } else {
                    p[0] = s[0];
                    p[1] = s[1];
                    p[2] = s[2];
                }
                p[3] = 255;
            }
        }
        return img;
    }

    ByteSpan m_data;
    usize m_pos = 0;
    u32 m_width = 0, m_height = 0;
    u32 m_hMax = 1, m_vMax = 1;
    u32 m_mcusX = 0, m_mcusY = 0;
    bool m_progressive = false;
    i32 m_adobeTransform = -1;
    u16 m_restartInterval = 0;
    Array<Component> m_components;
    Huffman m_dc[4], m_ac[4];
    u16 m_quant[4][64] = {};
    u32 m_bits = 0, m_bitCount = 0;
    bool m_hitMarker = false;
    u32 m_eobRun = 0;
    const char* m_error = "";
};

} // namespace

Result<Image> DecodeJpeg(ByteSpan data)
{
    Decoder d(data);
    return d.Decode();
}

} // namespace zx
