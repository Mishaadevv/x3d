#include "Core/String/String.h"

namespace zx {

void String::Grow(usize minCapacity)
{
    const usize cap = Capacity();
    if (minCapacity <= cap)
        return;
    usize newCap = cap + cap / 2;
    if (newCap < minCapacity)
        newCap = minCapacity;
    newCap = AlignUp(newCap + 1, usize(16)) - 1; // +1 for terminator, round allocation to 16

    const usize size = Size();
    char* p = static_cast<char*>(Malloc(newCap + 1, 16, MemTag::Strings));
    MemCopy(p, Data(), size + 1);
    if (IsHeap())
        Free(m_rep.heap.ptr);
    m_rep.heap.ptr = p;
    m_rep.heap.size = size;
    m_rep.heap.capacity = newCap | HeapFlag;
}

void String::Reserve(usize capacity) { Grow(capacity); }

void String::Assign(StringView s)
{
    // `s` may point into our own buffer: copy via memmove after ensuring capacity (Grow preserves content).
    if (s.Size() > Capacity()) {
        const bool aliases = s.Data() >= Data() && s.Data() <= Data() + Size();
        if (aliases) {
            String tmp(s);
            *this = move(tmp);
            return;
        }
        Grow(s.Size());
    }
    MemMove(Data(), s.Data(), s.Size());
    SetSize(s.Size());
}

void String::Resize(usize size, char fill)
{
    const usize old = Size();
    if (size > old) {
        Grow(size);
        MemSet(Data() + old, u8(fill), size - old);
    }
    SetSize(size);
}

void String::ShrinkToFit()
{
    if (!IsHeap())
        return;
    const usize size = Size();
    if (size <= InlineCapacity) {
        char* p = m_rep.heap.ptr;
        MemCopy(m_rep.bytes, p, size);
        SetInlineSize(size);
        Free(p);
    } else if (Capacity() > AlignUp(size + 1, usize(16)) - 1) {
        String tmp(View());
        *this = move(tmp);
    }
}

String& String::Append(StringView s)
{
    if (s.Empty())
        return *this;
    const usize old = Size();
    if (old + s.Size() > Capacity()) {
        const bool aliases = s.Data() >= Data() && s.Data() < Data() + old;
        if (aliases) {
            const usize offset = usize(s.Data() - Data());
            Grow(old + s.Size());
            s = StringView(Data() + offset, s.Size());
        } else {
            Grow(old + s.Size());
        }
    }
    MemMove(Data() + old, s.Data(), s.Size());
    SetSize(old + s.Size());
    return *this;
}

String& String::Append(char c)
{
    const usize old = Size();
    if (old + 1 > Capacity())
        Grow(old + 1);
    Data()[old] = c;
    SetSize(old + 1);
    return *this;
}

String& String::Append(usize count, char c)
{
    const usize old = Size();
    Grow(old + count);
    MemSet(Data() + old, u8(c), count);
    SetSize(old + count);
    return *this;
}

void String::Insert(usize pos, StringView s)
{
    const usize old = Size();
    ZX_ASSERT(pos <= old, "String::Insert position {} out of range {}", pos, old);
    if (s.Empty())
        return;
    if (s.Data() >= Data() && s.Data() < Data() + old) {
        String tmp(s);
        Insert(pos, tmp.View());
        return;
    }
    Grow(old + s.Size());
    char* d = Data();
    MemMove(d + pos + s.Size(), d + pos, old - pos);
    MemCopy(d + pos, s.Data(), s.Size());
    SetSize(old + s.Size());
}

void String::Erase(usize pos, usize count)
{
    const usize size = Size();
    ZX_ASSERT(pos <= size, "String::Erase position {} out of range {}", pos, size);
    if (count > size - pos)
        count = size - pos;
    char* d = Data();
    MemMove(d + pos, d + pos + count, size - pos - count);
    SetSize(size - count);
}

usize String::ReplaceAll(StringView from, StringView to)
{
    if (from.Empty())
        return 0;
    const StringView self = View();
    usize pos = self.Find(from);
    if (pos == NPos)
        return 0;
    String out;
    out.Reserve(Size());
    usize last = 0;
    usize count = 0;
    while (pos != NPos) {
        out.Append(self.SubStr(last, pos - last));
        out.Append(to);
        last = pos + from.Size();
        pos = self.Find(from, last);
        ++count;
    }
    out.Append(self.SubStr(last));
    *this = move(out);
    return count;
}

void String::ReplaceChar(char from, char to)
{
    for (char& c : *this)
        if (c == from)
            c = to;
}

void String::ToLowerInPlace()
{
    for (char& c : *this)
        c = ToLowerAscii(c);
}

void String::ToUpperInPlace()
{
    for (char& c : *this)
        c = ToUpperAscii(c);
}

void String::TrimInPlace()
{
    const StringView t = View().Trim();
    if (t.Size() != Size())
        Assign(t);
}

Array<StringView> SplitString(StringView s, char separator, bool skipEmpty)
{
    Array<StringView> out;
    usize start = 0;
    for (usize i = 0; i <= s.Size(); ++i) {
        if (i == s.Size() || s[i] == separator) {
            const StringView part = s.SubStr(start, i - start);
            if (!(skipEmpty && part.Empty()))
                out.PushBack(part);
            start = i + 1;
        }
    }
    return out;
}

Array<StringView> SplitString(StringView s, StringView separator, bool skipEmpty)
{
    Array<StringView> out;
    if (separator.Empty()) {
        out.PushBack(s);
        return out;
    }
    usize start = 0;
    for (;;) {
        const usize p = s.Find(separator, start);
        const StringView part = s.SubStr(start, p == StringView::NPos ? StringView::NPos : p - start);
        if (!(skipEmpty && part.Empty()))
            out.PushBack(part);
        if (p == StringView::NPos)
            break;
        start = p + separator.Size();
    }
    return out;
}

String JoinStrings(Span<const StringView> parts, StringView separator)
{
    String out;
    for (usize i = 0; i < parts.Size(); ++i) {
        if (i)
            out.Append(separator);
        out.Append(parts[i]);
    }
    return out;
}

String JoinStrings(Span<const String> parts, StringView separator)
{
    String out;
    for (usize i = 0; i < parts.Size(); ++i) {
        if (i)
            out.Append(separator);
        out.Append(parts[i].View());
    }
    return out;
}

} // namespace zx
