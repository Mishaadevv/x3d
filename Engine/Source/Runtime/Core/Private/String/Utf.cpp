#include "Core/String/Utf.h"

#include <cwchar>

namespace zx {

u32 Utf8Decode(StringView s, usize& pos)
{
    const usize n = s.Size();
    const u8 b0 = u8(s[pos]);
    if (b0 < 0x80) {
        ++pos;
        return b0;
    }
    u32 cp;
    usize len;
    u32 minCp;
    if ((b0 & 0xE0) == 0xC0) {
        cp = b0 & 0x1F;
        len = 2;
        minCp = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        cp = b0 & 0x0F;
        len = 3;
        minCp = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        cp = b0 & 0x07;
        len = 4;
        minCp = 0x10000;
    } else {
        ++pos;
        return ReplacementCodepoint;
    }
    if (pos + len > n) {
        ++pos;
        return ReplacementCodepoint;
    }
    for (usize i = 1; i < len; ++i) {
        const u8 b = u8(s[pos + i]);
        if ((b & 0xC0) != 0x80) {
            ++pos;
            return ReplacementCodepoint;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    // Reject overlong encodings, surrogates and out-of-range values.
    if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++pos;
        return ReplacementCodepoint;
    }
    pos += len;
    return cp;
}

usize Utf8Encode(u32 cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = char(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = char(0xC0 | (cp >> 6));
        out[1] = char(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0;
    if (cp < 0x10000) {
        out[0] = char(0xE0 | (cp >> 12));
        out[1] = char(0x80 | ((cp >> 6) & 0x3F));
        out[2] = char(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = char(0xF0 | (cp >> 18));
        out[1] = char(0x80 | ((cp >> 12) & 0x3F));
        out[2] = char(0x80 | ((cp >> 6) & 0x3F));
        out[3] = char(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

void Utf8Append(String& s, u32 codepoint)
{
    char buf[4];
    usize n = Utf8Encode(codepoint, buf);
    if (n == 0)
        n = Utf8Encode(ReplacementCodepoint, buf);
    s.Append(StringView(buf, n));
}

usize Utf8Length(StringView s)
{
    usize count = 0;
    usize pos = 0;
    while (pos < s.Size()) {
        Utf8Decode(s, pos);
        ++count;
    }
    return count;
}

bool Utf8IsValid(StringView s)
{
    usize pos = 0;
    while (pos < s.Size()) {
        const usize before = pos;
        const u32 cp = Utf8Decode(s, pos);
        if (cp == ReplacementCodepoint) {
            // Distinguish a literal U+FFFD (3 bytes EF BF BD) from a decode error.
            if (!(pos - before == 3 && u8(s[before]) == 0xEF))
                return false;
        }
    }
    return true;
}

usize Utf8TruncateBytes(StringView s, usize maxBytes)
{
    if (maxBytes >= s.Size())
        return s.Size();
    usize n = maxBytes;
    while (n > 0 && (u8(s[n]) & 0xC0) == 0x80)
        --n;
    return n;
}

Array<wchar_t> Utf8ToUtf16(StringView s)
{
    Array<wchar_t> out;
    out.Reserve(s.Size() + 1);
    usize pos = 0;
    while (pos < s.Size()) {
        const u32 cp = Utf8Decode(s, pos);
        if (cp >= 0x10000) {
            const u32 v = cp - 0x10000;
            out.PushBack(wchar_t(0xD800 + (v >> 10)));
            out.PushBack(wchar_t(0xDC00 + (v & 0x3FF)));
        } else {
            out.PushBack(wchar_t(cp));
        }
    }
    out.PushBack(L'\0');
    out.PopBack(); // keep terminator in capacity, not in size
    out.Reserve(out.Size() + 1);
    out.Data()[out.Size()] = L'\0';
    return out;
}

String Utf16ToUtf8(const wchar_t* s, usize length)
{
    String out;
    out.Reserve(length);
    for (usize i = 0; i < length; ++i) {
        u32 cp = u16(s[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < length) {
            const u32 lo = u16(s[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            } else {
                cp = ReplacementCodepoint;
            }
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = ReplacementCodepoint;
        }
        Utf8Append(out, cp);
    }
    return out;
}

String Utf16ToUtf8(const wchar_t* s) { return s ? Utf16ToUtf8(s, std::wcslen(s)) : String(); }

} // namespace zx
