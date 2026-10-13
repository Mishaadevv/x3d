#pragma once

// Little-endian binary writer/reader for cooked data. The reader never reads out of bounds: on any
// overflow it enters an error state and returns zeros, so callers can check HasError() once at the end.

#include "Core/Containers/Array.h"
#include "Core/String/String.h"

namespace zx {

class BinaryWriter
{
public:
    explicit BinaryWriter(Array<u8>& out) : m_out(out) {}

    usize Position() const { return m_out.Size(); }
    Array<u8>& Buffer() { return m_out; }

    void WriteBytes(const void* data, usize size)
    {
        const usize at = m_out.Size();
        m_out.ResizeUninitialized(at + size);
        if (size)
            MemCopy(m_out.Data() + at, data, size);
    }
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void Write(const T& v)
    {
        WriteBytes(&v, sizeof(T));
    }
    void WriteBool(bool v) { Write<u8>(v ? 1 : 0); }
    /// LEB128 unsigned varint.
    void WriteVarU64(u64 v)
    {
        do {
            u8 b = u8(v & 0x7F);
            v >>= 7;
            if (v)
                b |= 0x80;
            m_out.PushBack(b);
        } while (v);
    }
    /// ZigZag + LEB128.
    void WriteVarI64(i64 v) { WriteVarU64((u64(v) << 1) ^ u64(v >> 63)); }
    /// Length-prefixed (varint) UTF-8 string.
    void WriteString(StringView s)
    {
        WriteVarU64(s.Size());
        WriteBytes(s.Data(), s.Size());
    }
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void WriteArray(Span<const T> values)
    {
        WriteVarU64(values.Size());
        WriteBytes(values.Data(), values.SizeBytes());
    }
    void Align(usize alignment)
    {
        while (m_out.Size() % alignment)
            m_out.PushBack(0);
    }
    /// Reserves space for a value to be patched later; returns its offset.
    template <typename T>
    usize Reserve()
    {
        const usize at = m_out.Size();
        m_out.ResizeZeroed(at + sizeof(T));
        return at;
    }
    template <typename T>
    void Patch(usize offset, const T& v)
    {
        ZX_ASSERT(offset + sizeof(T) <= m_out.Size());
        MemCopy(m_out.Data() + offset, &v, sizeof(T));
    }

private:
    Array<u8>& m_out;
};

class BinaryReader
{
public:
    BinaryReader() = default;
    explicit BinaryReader(ByteSpan data) : m_data(data) {}

    usize Position() const { return m_pos; }
    usize Size() const { return m_data.Size(); }
    usize Remaining() const { return m_error ? 0 : m_data.Size() - m_pos; }
    bool AtEnd() const { return Remaining() == 0; }
    bool HasError() const { return m_error; }
    void SetError() { m_error = true; }

    bool ReadBytes(void* dst, usize size)
    {
        if (m_error || size > m_data.Size() - m_pos) {
            m_error = true;
            if (size)
                MemZero(dst, size);
            return false;
        }
        if (size)
            MemCopy(dst, m_data.Data() + m_pos, size);
        m_pos += size;
        return true;
    }
    /// Returns a view into the source (no copy). Empty on error.
    ByteSpan ReadView(usize size)
    {
        if (m_error || size > m_data.Size() - m_pos) {
            m_error = true;
            return {};
        }
        const ByteSpan v = m_data.SubSpan(m_pos, size);
        m_pos += size;
        return v;
    }
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    T Read()
    {
        T v{};
        ReadBytes(&v, sizeof(T));
        return v;
    }
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    bool Read(T& v)
    {
        return ReadBytes(&v, sizeof(T));
    }
    bool ReadBool() { return Read<u8>() != 0; }
    u64 ReadVarU64()
    {
        u64 v = 0;
        for (u32 shift = 0; shift < 64; shift += 7) {
            const u8 b = Read<u8>();
            if (m_error)
                return 0;
            v |= u64(b & 0x7F) << shift;
            if (!(b & 0x80))
                return v;
        }
        m_error = true; // overlong varint
        return 0;
    }
    i64 ReadVarI64()
    {
        const u64 z = ReadVarU64();
        return i64(z >> 1) ^ -i64(z & 1);
    }
    /// View into the source buffer.
    StringView ReadStringView()
    {
        const u64 len = ReadVarU64();
        const ByteSpan b = ReadView(usize(len));
        return StringView(reinterpret_cast<const char*>(b.Data()), b.Size());
    }
    String ReadString() { return String(ReadStringView()); }
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    bool ReadArray(Array<T>& out)
    {
        const u64 count = ReadVarU64();
        if (m_error || count > Remaining() / (sizeof(T) ? sizeof(T) : 1)) {
            m_error = true;
            return false;
        }
        out.ResizeUninitialized(usize(count));
        return ReadBytes(out.Data(), usize(count) * sizeof(T));
    }
    void Skip(usize size) { ReadView(size); }
    void Seek(usize position)
    {
        if (position > m_data.Size())
            m_error = true;
        else
            m_pos = position;
    }
    void Align(usize alignment)
    {
        const usize aligned = AlignUp(m_pos, alignment);
        Skip(aligned - m_pos);
    }

private:
    ByteSpan m_data;
    usize m_pos = 0;
    bool m_error = false;
};

} // namespace zx
