#pragma once

#include "Core/Containers/Array.h"
#include "Core/String/String.h"

namespace zx {

inline constexpr u32 ReplacementCodepoint = 0xFFFD;

/// Decodes one UTF-8 codepoint at s[pos], advancing pos. Invalid sequences yield U+FFFD and advance one byte.
u32 Utf8Decode(StringView s, usize& pos);

/// Encodes a codepoint into out (up to 4 bytes). Returns bytes written (0 for invalid codepoints).
usize Utf8Encode(u32 codepoint, char out[4]);

/// Appends a codepoint encoded as UTF-8.
void Utf8Append(String& s, u32 codepoint);

/// Number of codepoints in a UTF-8 string.
usize Utf8Length(StringView s);

/// True if `s` is well-formed UTF-8.
bool Utf8IsValid(StringView s);

/// Length in bytes of the longest prefix of `s` not exceeding maxBytes that ends on a codepoint boundary.
usize Utf8TruncateBytes(StringView s, usize maxBytes);

/// UTF-8 <-> UTF-16 (wchar_t on Windows). Output is null-terminated (the terminator is not counted in Size()).
Array<wchar_t> Utf8ToUtf16(StringView s);
String Utf16ToUtf8(const wchar_t* s, usize length);
String Utf16ToUtf8(const wchar_t* s);

/// Iterates codepoints: for (u32 cp : Utf8Codepoints(text)) {...}
class Utf8Codepoints
{
public:
    explicit Utf8Codepoints(StringView s) : m_s(s) {}
    class Iterator
    {
    public:
        Iterator(StringView s, usize pos) : m_s(s), m_pos(pos), m_next(pos) { Decode(); }
        u32 operator*() const { return m_cp; }
        Iterator& operator++()
        {
            m_pos = m_next;
            Decode();
            return *this;
        }
        bool operator==(const Iterator& o) const { return m_pos == o.m_pos; }
        usize BytePosition() const { return m_pos; }

    private:
        void Decode()
        {
            m_next = m_pos;
            m_cp = m_pos < m_s.Size() ? Utf8Decode(m_s, m_next) : 0;
        }
        StringView m_s;
        usize m_pos;
        usize m_next;
        u32 m_cp = 0;
    };
    Iterator begin() const { return Iterator(m_s, 0); }
    Iterator end() const { return Iterator(m_s, m_s.Size()); }

private:
    StringView m_s;
};

} // namespace zx
