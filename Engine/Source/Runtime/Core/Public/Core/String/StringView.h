#pragma once

#include "Core/Types.h"

#include <cstring>

namespace zx {

namespace detail {
constexpr usize ConstStrLen(const char* s)
{
    if (std::is_constant_evaluated()) {
        usize n = 0;
        while (s[n] != '\0')
            ++n;
        return n;
    }
    return std::strlen(s);
}
} // namespace detail

constexpr char ToLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
constexpr char ToUpperAscii(char c) { return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c; }
constexpr bool IsSpaceAscii(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
constexpr bool IsDigitAscii(char c) { return c >= '0' && c <= '9'; }
constexpr bool IsAlphaAscii(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool IsAlnumAscii(char c) { return IsAlphaAscii(c) || IsDigitAscii(c); }
constexpr bool IsHexDigitAscii(char c) { return IsDigitAscii(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/// Non-owning view of UTF-8 text. Not necessarily null-terminated.
class StringView
{
public:
    static constexpr usize NPos = ~usize(0);

    constexpr StringView() = default;
    constexpr StringView(const char* s) : m_data(s ? s : ""), m_size(s ? detail::ConstStrLen(s) : 0) {}
    constexpr StringView(const char* s, usize n) : m_data(s), m_size(n) {}
    constexpr StringView(const char* begin, const char* end) : m_data(begin), m_size(usize(end - begin)) {}

    constexpr const char* Data() const { return m_data; }
    constexpr usize Size() const { return m_size; }
    constexpr bool Empty() const { return m_size == 0; }
    constexpr const char* begin() const { return m_data; }
    constexpr const char* end() const { return m_data + m_size; }
    constexpr char operator[](usize i) const { return m_data[i]; }
    constexpr char Front() const { return m_data[0]; }
    constexpr char Back() const { return m_data[m_size - 1]; }

    constexpr StringView SubStr(usize pos, usize count = NPos) const
    {
        if (pos > m_size)
            pos = m_size;
        const usize avail = m_size - pos;
        return StringView(m_data + pos, count < avail ? count : avail);
    }
    constexpr StringView Left(usize n) const { return SubStr(0, n); }
    constexpr StringView Right(usize n) const { return n >= m_size ? *this : SubStr(m_size - n); }
    constexpr StringView DropLeft(usize n) const { return SubStr(n); }
    constexpr StringView DropRight(usize n) const { return n >= m_size ? StringView(m_data, usize(0)) : SubStr(0, m_size - n); }

    constexpr int Compare(StringView o) const
    {
        const usize n = m_size < o.m_size ? m_size : o.m_size;
        for (usize i = 0; i < n; ++i) {
            if (m_data[i] != o.m_data[i])
                return (unsigned char)m_data[i] < (unsigned char)o.m_data[i] ? -1 : 1;
        }
        return m_size == o.m_size ? 0 : (m_size < o.m_size ? -1 : 1);
    }

    constexpr int CompareIgnoreCase(StringView o) const
    {
        const usize n = m_size < o.m_size ? m_size : o.m_size;
        for (usize i = 0; i < n; ++i) {
            const char a = ToLowerAscii(m_data[i]);
            const char b = ToLowerAscii(o.m_data[i]);
            if (a != b)
                return (unsigned char)a < (unsigned char)b ? -1 : 1;
        }
        return m_size == o.m_size ? 0 : (m_size < o.m_size ? -1 : 1);
    }

    constexpr bool EqualsIgnoreCase(StringView o) const { return m_size == o.m_size && CompareIgnoreCase(o) == 0; }

    constexpr bool StartsWith(StringView p) const { return p.m_size <= m_size && Left(p.m_size) == p; }
    constexpr bool EndsWith(StringView p) const { return p.m_size <= m_size && Right(p.m_size) == p; }
    constexpr bool StartsWith(char c) const { return m_size > 0 && m_data[0] == c; }
    constexpr bool EndsWith(char c) const { return m_size > 0 && m_data[m_size - 1] == c; }

    constexpr usize Find(char c, usize from = 0) const
    {
        for (usize i = from; i < m_size; ++i)
            if (m_data[i] == c)
                return i;
        return NPos;
    }
    constexpr usize Find(StringView s, usize from = 0) const
    {
        if (s.m_size == 0)
            return from <= m_size ? from : NPos;
        if (s.m_size > m_size)
            return NPos;
        for (usize i = from; i + s.m_size <= m_size; ++i)
            if (SubStr(i, s.m_size) == s)
                return i;
        return NPos;
    }
    constexpr usize RFind(char c) const
    {
        for (usize i = m_size; i > 0; --i)
            if (m_data[i - 1] == c)
                return i - 1;
        return NPos;
    }
    constexpr usize RFind(StringView s) const
    {
        if (s.m_size > m_size)
            return NPos;
        for (usize i = m_size - s.m_size + 1; i > 0; --i)
            if (SubStr(i - 1, s.m_size) == s)
                return i - 1;
        return NPos;
    }
    constexpr usize FindFirstOf(StringView chars, usize from = 0) const
    {
        for (usize i = from; i < m_size; ++i)
            if (chars.Find(m_data[i]) != NPos)
                return i;
        return NPos;
    }
    constexpr usize FindLastOf(StringView chars) const
    {
        for (usize i = m_size; i > 0; --i)
            if (chars.Find(m_data[i - 1]) != NPos)
                return i - 1;
        return NPos;
    }
    constexpr bool Contains(StringView s) const { return Find(s) != NPos; }
    constexpr bool Contains(char c) const { return Find(c) != NPos; }

    constexpr StringView TrimLeft() const
    {
        usize i = 0;
        while (i < m_size && IsSpaceAscii(m_data[i]))
            ++i;
        return SubStr(i);
    }
    constexpr StringView TrimRight() const
    {
        usize n = m_size;
        while (n > 0 && IsSpaceAscii(m_data[n - 1]))
            --n;
        return Left(n);
    }
    constexpr StringView Trim() const { return TrimLeft().TrimRight(); }

    /// Splits at the first occurrence of `sep`. Returns false (and left = whole) if not found.
    constexpr bool Split(char sep, StringView& left, StringView& right) const
    {
        const usize p = Find(sep);
        if (p == NPos) {
            left = *this;
            right = StringView();
            return false;
        }
        left = Left(p);
        right = SubStr(p + 1);
        return true;
    }

    friend constexpr bool operator==(StringView a, StringView b)
    {
        if (a.m_size != b.m_size)
            return false;
        for (usize i = 0; i < a.m_size; ++i)
            if (a.m_data[i] != b.m_data[i])
                return false;
        return true;
    }
    friend constexpr bool operator<(StringView a, StringView b) { return a.Compare(b) < 0; }

private:
    const char* m_data = "";
    usize m_size = 0;
};

constexpr StringView operator""_sv(const char* s, usize n) { return StringView(s, n); }

} // namespace zx
