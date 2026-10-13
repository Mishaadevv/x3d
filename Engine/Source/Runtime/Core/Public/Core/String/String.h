#pragma once

#include "Core/Containers/Array.h"
#include "Core/Hash/Hash.h"
#include "Core/String/Format.h"

namespace zx {

/// Owning UTF-8 string, always null-terminated. 24 bytes; strings up to 23 bytes are stored inline (SSO).
class String
{
public:
    static constexpr usize NPos = StringView::NPos;
    static constexpr usize InlineCapacity = 23;

    String() { SetInlineSize(0); }
    String(const char* s) : String(StringView(s)) {}
    String(const char* s, usize n) : String(StringView(s, n)) {}
    String(StringView s)
    {
        SetInlineSize(0);
        Assign(s);
    }
    String(usize count, char c)
    {
        SetInlineSize(0);
        Resize(count, c);
    }
    String(const String& o) : String(o.View()) {}
    String(String&& o) noexcept
    {
        MemCopy(&m_rep, &o.m_rep, sizeof(m_rep));
        o.SetInlineSize(0);
    }
    ~String()
    {
        if (IsHeap())
            Free(m_rep.heap.ptr);
    }

    String& operator=(const String& o)
    {
        if (this != &o)
            Assign(o.View());
        return *this;
    }
    String& operator=(String&& o) noexcept
    {
        if (this != &o) {
            if (IsHeap())
                Free(m_rep.heap.ptr);
            MemCopy(&m_rep, &o.m_rep, sizeof(m_rep));
            o.SetInlineSize(0);
        }
        return *this;
    }
    String& operator=(StringView s)
    {
        Assign(s);
        return *this;
    }
    String& operator=(const char* s)
    {
        Assign(StringView(s));
        return *this;
    }

    // --- Access -------------------------------------------------------------------------------------------------------
    usize Size() const { return IsHeap() ? m_rep.heap.size : InlineCapacity - usize(u8(m_rep.bytes[InlineCapacity])); }
    usize Length() const { return Size(); }
    bool Empty() const { return Size() == 0; }
    usize Capacity() const { return IsHeap() ? (m_rep.heap.capacity & ~HeapFlag) : InlineCapacity; }
    char* Data() { return IsHeap() ? m_rep.heap.ptr : m_rep.bytes; }
    const char* Data() const { return IsHeap() ? m_rep.heap.ptr : m_rep.bytes; }
    const char* CStr() const { return Data(); }
    StringView View() const { return StringView(Data(), Size()); }
    operator StringView() const { return View(); }

    char& operator[](usize i)
    {
        ZX_CHECK_SLOW(i < Size());
        return Data()[i];
    }
    char operator[](usize i) const
    {
        ZX_CHECK_SLOW(i < Size());
        return Data()[i];
    }
    char* begin() { return Data(); }
    char* end() { return Data() + Size(); }
    const char* begin() const { return Data(); }
    const char* end() const { return Data() + Size(); }
    char Back() const { return Data()[Size() - 1]; }

    // --- Modification -------------------------------------------------------------------------------------------------
    void Assign(StringView s);
    void Clear() { SetSize(0); }
    void Reserve(usize capacity);
    void Resize(usize size, char fill = '\0');
    /// Shrinks heap storage to fit (or moves back inline).
    void ShrinkToFit();

    String& Append(StringView s);
    String& Append(char c);
    String& Append(usize count, char c);
    String& operator+=(StringView s) { return Append(s); }
    String& operator+=(const char* s) { return Append(StringView(s)); }
    String& operator+=(const String& s) { return Append(s.View()); }
    String& operator+=(char c) { return Append(c); }

    /// Appends formatted text.
    template <typename... Args>
    String& Appendf(StringView fmt, const Args&... args);

    void Insert(usize pos, StringView s);
    void Erase(usize pos, usize count = NPos);
    void PopBack()
    {
        ZX_ASSERT(!Empty());
        SetSize(Size() - 1);
    }
    /// Replaces all occurrences of `from` with `to`. Returns number of replacements.
    usize ReplaceAll(StringView from, StringView to);
    void ReplaceChar(char from, char to);
    void ToLowerInPlace();
    void ToUpperInPlace();
    void TrimInPlace();

    // --- Queries (forwarded to StringView) ----------------------------------------------------------------------------
    usize Find(char c, usize from = 0) const { return View().Find(c, from); }
    usize Find(StringView s, usize from = 0) const { return View().Find(s, from); }
    usize RFind(char c) const { return View().RFind(c); }
    usize RFind(StringView s) const { return View().RFind(s); }
    bool Contains(StringView s) const { return View().Contains(s); }
    bool Contains(char c) const { return View().Contains(c); }
    bool StartsWith(StringView s) const { return View().StartsWith(s); }
    bool EndsWith(StringView s) const { return View().EndsWith(s); }
    bool EqualsIgnoreCase(StringView s) const { return View().EqualsIgnoreCase(s); }
    String SubStr(usize pos, usize count = NPos) const { return String(View().SubStr(pos, count)); }
    String ToLower() const
    {
        String s(*this);
        s.ToLowerInPlace();
        return s;
    }
    String ToUpper() const
    {
        String s(*this);
        s.ToUpperInPlace();
        return s;
    }
    String Trimmed() const { return String(View().Trim()); }

    template <typename... Args>
    static String Format(StringView fmt, const Args&... args);

    friend bool operator==(const String& a, const String& b) { return a.View() == b.View(); }
    friend bool operator==(const String& a, StringView b) { return a.View() == b; }
    friend bool operator==(const String& a, const char* b) { return a.View() == StringView(b); }
    friend bool operator<(const String& a, const String& b) { return a.View() < b.View(); }

private:
    static constexpr usize HeapFlag = usize(1) << (sizeof(usize) * 8 - 1);

    bool IsHeap() const { return (u8(m_rep.bytes[InlineCapacity]) & 0x80) != 0; }
    void SetInlineSize(usize n)
    {
        m_rep.bytes[InlineCapacity] = char(InlineCapacity - n);
        m_rep.bytes[n] = '\0';
    }
    void SetSize(usize n)
    {
        if (IsHeap()) {
            m_rep.heap.size = n;
            m_rep.heap.ptr[n] = '\0';
        } else {
            SetInlineSize(n);
        }
    }
    void Grow(usize minCapacity);

    union Rep
    {
        struct
        {
            char* ptr;
            usize size;
            usize capacity; // high bit = heap flag (aliases bytes[23] on little-endian)
        } heap;
        char bytes[24];
    } m_rep;
};

static_assert(sizeof(String) == 24);

inline String operator+(const String& a, StringView b)
{
    String r;
    r.Reserve(a.Size() + b.Size());
    r.Append(a.View());
    r.Append(b);
    return r;
}
inline String operator+(String&& a, StringView b)
{
    a.Append(b);
    return move(a);
}
inline String operator+(StringView a, const String& b)
{
    String r;
    r.Reserve(a.Size() + b.Size());
    r.Append(a);
    r.Append(b.View());
    return r;
}
// Exact-match overloads to avoid ambiguity between the implicit String(const char*) and operator StringView().
inline String operator+(const String& a, const char* b) { return a + StringView(b); }
inline String operator+(String&& a, const char* b) { return move(a) + StringView(b); }
inline String operator+(const String& a, const String& b) { return a + b.View(); }
inline String operator+(String&& a, const String& b) { return move(a) + b.View(); }
inline String operator+(const char* a, const String& b) { return StringView(a) + b; }

/// FormatSink that appends into a String.
class StringSink final : public FormatSink
{
public:
    explicit StringSink(String& target) : m_target(target) {}
    using FormatSink::Append;
    void Append(const char* s, usize n) override { m_target.Append(StringView(s, n)); }

private:
    String& m_target;
};

template <typename... Args>
String& String::Appendf(StringView fmt, const Args&... args)
{
    StringSink sink(*this);
    FormatTo(sink, fmt, args...);
    return *this;
}

template <typename... Args>
String String::Format(StringView fmt, const Args&... args)
{
    String s;
    s.Appendf(fmt, args...);
    return s;
}

template <>
struct Hasher<String>
{
    static u64 Hash(const String& s) { return HashXX64(s.View()); }
};

/// Splits `s` by `separator`. Views point into `s`.
Array<StringView> SplitString(StringView s, char separator, bool skipEmpty = false);
Array<StringView> SplitString(StringView s, StringView separator, bool skipEmpty = false);

/// Joins parts with a separator.
String JoinStrings(Span<const StringView> parts, StringView separator);
String JoinStrings(Span<const String> parts, StringView separator);

} // namespace zx
