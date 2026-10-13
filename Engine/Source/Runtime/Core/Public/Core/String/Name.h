#pragma once

// Name: interned, immutable, case-preserving string identifier. Comparison and hashing are O(1)
// (compares the interned index). Comparison is case-SENSITIVE; use for property names, asset types,
// tags, socket/bone names, etc. Names live for the whole process (the table never shrinks).
// Thread-safe.

#include "Core/String/String.h"

namespace zx {

class Name
{
public:
    constexpr Name() = default;
    Name(StringView text);
    Name(const char* text) : Name(StringView(text)) {}
    Name(const String& text) : Name(text.View()) {}

    /// Returns the Name only if it was already interned (does not grow the table).
    static Name Find(StringView text);

    StringView View() const;
    const char* CStr() const; // null-terminated
    bool IsNone() const { return m_index == 0; }
    explicit operator bool() const { return m_index != 0; }
    u32 Index() const { return m_index; }

    friend bool operator==(Name a, Name b) { return a.m_index == b.m_index; }
    /// Ordering by index (stable within a run, NOT alphabetical).
    friend bool operator<(Name a, Name b) { return a.m_index < b.m_index; }

private:
    u32 m_index = 0; // 0 = None ("")
};

/// Number of interned names (diagnostics).
usize GetNameTableSize();

template <>
struct Hasher<Name>
{
    static u64 Hash(Name n) { return HashMix64(n.Index()); }
};

template <>
struct Formatter<Name>
{
    static void Format(FormatSink& s, Name n, const FormatSpec& spec) { FormatString(s, n.View(), spec); }
};

} // namespace zx
