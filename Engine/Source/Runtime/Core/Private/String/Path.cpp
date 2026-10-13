#include "Core/String/Path.h"

namespace zx::Path {

namespace {
bool IsSep(char c) { return c == '/' || c == '\\'; }

/// Length of the root prefix: "C:/" -> 3, "/" -> 1, "//server/" -> handled as "/" , else 0.
usize RootLength(StringView p)
{
    if (p.Size() >= 2 && IsAlphaAscii(p[0]) && p[1] == ':')
        return (p.Size() >= 3 && IsSep(p[2])) ? 3 : 2;
    if (!p.Empty() && IsSep(p[0]))
        return 1;
    return 0;
}
} // namespace

String Normalize(StringView path)
{
    if (path.Empty())
        return String();
    const usize rootLen = RootLength(path);
    String root(path.Left(rootLen));
    root.ReplaceChar('\\', '/');

    SmallArray<StringView, 16> parts;
    usize i = rootLen;
    while (i < path.Size()) {
        usize j = i;
        while (j < path.Size() && !IsSep(path[j]))
            ++j;
        const StringView part = path.SubStr(i, j - i);
        if (part.Empty() || part == ".") {
            // skip
        } else if (part == "..") {
            if (!parts.Empty() && parts.Back() != "..")
                parts.PopBack();
            else if (rootLen == 0)
                parts.PushBack(part); // keep leading ".." for relative paths
        } else {
            parts.PushBack(part);
        }
        i = j + 1;
    }
    String out = root;
    for (usize k = 0; k < parts.Size(); ++k) {
        if (k)
            out += '/';
        out += parts[k];
    }
    if (out.Empty() && rootLen == 0)
        out = ".";
    return out;
}

String Join(StringView a, StringView b)
{
    if (a.Empty() || IsAbsolute(b))
        return Normalize(b);
    if (b.Empty())
        return Normalize(a);
    String s(a);
    s += '/';
    s += b;
    return Normalize(s);
}

String Join(StringView a, StringView b, StringView c) { return Join(Join(a, b), c); }

StringView FileName(StringView path)
{
    const usize p = path.FindLastOf("/\\");
    return p == StringView::NPos ? path : path.SubStr(p + 1);
}

StringView Stem(StringView path)
{
    const StringView name = FileName(path);
    const usize dot = name.RFind('.');
    return (dot == StringView::NPos || dot == 0) ? name : name.Left(dot);
}

StringView Extension(StringView path)
{
    const StringView name = FileName(path);
    const usize dot = name.RFind('.');
    return (dot == StringView::NPos || dot == 0) ? StringView() : name.SubStr(dot);
}

StringView Parent(StringView path)
{
    const usize p = path.FindLastOf("/\\");
    if (p == StringView::NPos)
        return StringView();
    if (p == 0)
        return path.Left(1);
    if (p == 2 && path[1] == ':')
        return path.Left(3);
    return path.Left(p);
}

String ChangeExtension(StringView path, StringView newExt)
{
    const StringView ext = Extension(path);
    String out(path.DropRight(ext.Size()));
    if (!newExt.Empty()) {
        if (!newExt.StartsWith('.'))
            out += '.';
        out += newExt;
    }
    return out;
}

bool HasExtension(StringView path, StringView ext)
{
    StringView e = Extension(path);
    if (!ext.StartsWith('.'))
        e = e.DropLeft(1);
    return e.EqualsIgnoreCase(ext);
}

bool IsAbsolute(StringView path) { return RootLength(path) > 0; }

bool IsUnder(StringView path, StringView dir)
{
    const String p = Normalize(path);
    const String d = Normalize(dir);
    if (p.Size() < d.Size() || !p.View().Left(d.Size()).EqualsIgnoreCase(d))
        return false;
    return p.Size() == d.Size() || d.EndsWith("/") || p[d.Size()] == '/';
}

String MakeRelative(StringView path, StringView base)
{
    const String p = Normalize(path);
    const String b = Normalize(base);
    if (!IsUnder(p, b))
        return String();
    if (p.Size() == b.Size())
        return String(".");
    usize start = b.Size();
    if (p[start] == '/')
        ++start;
    return p.SubStr(start);
}

String SanitizeFileName(StringView name)
{
    String out(name);
    for (char& c : out)
        if (u8(c) < 32 || StringView("<>:\"/\\|?*").Contains(c))
            c = '_';
    return out;
}

} // namespace zx::Path
