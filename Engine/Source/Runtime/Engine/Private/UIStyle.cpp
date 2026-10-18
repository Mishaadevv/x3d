#include "Engine/UIStyle.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Color.h"
#include "Core/Math/Vector.h"
#include "Core/String/Parse.h"

namespace zx {

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool IsIdent(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }

/// Numbers separated by spaces / commas, with optional "px" / "s" units.
bool ParseNumbers(StringView text, f32* out, u32 max, u32& count)
{
    count = 0;
    usize i = 0;
    while (i < text.Size()) {
        while (i < text.Size() && (IsSpace(text[i]) || text[i] == ','))
            ++i;
        if (i >= text.Size())
            break;
        usize j = i;
        while (j < text.Size() && !IsSpace(text[j]) && text[j] != ',')
            ++j;
        StringView token = text.SubStr(i, j - i);
        if (token.EndsWith("px"))
            token = token.DropRight(2);
        else if (token.EndsWith("s") && token.Size() > 1)
            token = token.DropRight(1);
        if (count >= max)
            return false;
        const Optional<f32> v = ParseFloat(token);
        if (!v)
            return false;
        out[count++] = *v;
        i = j;
    }
    return count > 0;
}

StringView Unquote(StringView s)
{
    s = s.Trim();
    if (s.Size() >= 2 && ((s[0] == '"' && s[s.Size() - 1] == '"') || (s[0] == '\'' && s[s.Size() - 1] == '\'')))
        return s.SubStr(1, s.Size() - 2);
    return s;
}

const TypeInfo* g_linearColor = nullptr;

} // namespace

bool UIStyleSelector::HasStates() const
{
    for (const UIStyleCompound& p : parts)
        if (p.states != UIStyleState::None)
            return true;
    return false;
}

String NormalizeStyleName(StringView name)
{
    String out;
    for (char c : name) {
        if (c == '-' || c == '_' || c == ' ')
            continue;
        out += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    }
    return out;
}

bool ParseStyleColor(StringView text, LinearColor& out)
{
    text = text.Trim();
    if (text.EqualsIgnoreCase("transparent") || text.EqualsIgnoreCase("none")) {
        out = LinearColor(0.0f, 0.0f, 0.0f, 0.0f);
        return true;
    }
    if (text.EqualsIgnoreCase("white")) {
        out = LinearColor(1.0f, 1.0f, 1.0f, 1.0f);
        return true;
    }
    if (text.EqualsIgnoreCase("black")) {
        out = LinearColor(0.0f, 0.0f, 0.0f, 1.0f);
        return true;
    }
    if (text.StartsWith("#")) {
        StringView hex = text.DropLeft(1);
        auto digit = [](char c) -> i32 {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        u8 v[4] = {0, 0, 0, 255};
        if (hex.Size() == 3 || hex.Size() == 4) {
            for (usize i = 0; i < hex.Size(); ++i) {
                const i32 d = digit(hex[i]);
                if (d < 0)
                    return false;
                v[i] = u8(d * 17);
            }
        } else if (hex.Size() == 6 || hex.Size() == 8) {
            for (usize i = 0; i < hex.Size() / 2; ++i) {
                const i32 a = digit(hex[i * 2]), b = digit(hex[i * 2 + 1]);
                if (a < 0 || b < 0)
                    return false;
                v[i] = u8(a * 16 + b);
            }
        } else {
            return false;
        }
        out = LinearColor::FromSRGB8(v[0], v[1], v[2], v[3]);
        return true;
    }
    const bool rgba = text.StartsWith("rgba(") || text.StartsWith("RGBA(");
    if ((rgba || text.StartsWith("rgb(") || text.StartsWith("RGB(")) && text.EndsWith(")")) {
        const StringView inner = text.SubStr(rgba ? 5 : 4, text.Size() - (rgba ? 6 : 5));
        f32 n[4];
        u32 count = 0;
        if (!ParseNumbers(inner, n, 4, count) || count < 3)
            return false;
        const u8 a = count == 4 ? u8(Clamp(n[3], 0.0f, 1.0f) * 255.0f + 0.5f) : u8(255);
        out = LinearColor::FromSRGB8(u8(Clamp(n[0], 0.0f, 255.0f)), u8(Clamp(n[1], 0.0f, 255.0f)), u8(Clamp(n[2], 0.0f, 255.0f)), a);
        return true;
    }
    return false;
}

bool ParseStyleValue(StringView text, const TypeInfo& type, void* object)
{
    text = text.Trim();
    if (!g_linearColor)
        g_linearColor = &TypeOf<LinearColor>();
    switch (type.kind) {
    case TypeKind::Bool: {
        const bool on = text.EqualsIgnoreCase("true") || text.EqualsIgnoreCase("on") || text.EqualsIgnoreCase("yes") || text == "1";
        const bool off = text.EqualsIgnoreCase("false") || text.EqualsIgnoreCase("off") || text.EqualsIgnoreCase("no") || text == "0";
        if (!on && !off)
            return false;
        *static_cast<bool*>(object) = on;
        return true;
    }
    case TypeKind::Float: {
        f32 v;
        u32 n = 0;
        if (!ParseNumbers(text, &v, 1, n))
            return false;
        *static_cast<f32*>(object) = v;
        return true;
    }
    case TypeKind::Int32: {
        f32 v;
        u32 n = 0;
        if (!ParseNumbers(text, &v, 1, n))
            return false;
        *static_cast<i32*>(object) = i32(Round(v));
        return true;
    }
    case TypeKind::String:
        *static_cast<String*>(object) = String(Unquote(text));
        return true;
    case TypeKind::Enum: {
        const String want = NormalizeStyleName(text);
        for (const EnumEntry& e : type.enumEntries)
            if (NormalizeStyleName(e.name.View()) == want) {
                type.WriteEnumValue(object, e.value);
                return true;
            }
        return false;
    }
    case TypeKind::Struct: {
        if (&type == g_linearColor)
            return ParseStyleColor(text, *static_cast<LinearColor*>(object));
        if (type.name == Name("Vec2")) {
            f32 n[2];
            u32 count = 0;
            if (!ParseNumbers(text, n, 2, count))
                return false;
            *static_cast<Vec2*>(object) = count == 1 ? Vec2(n[0]) : Vec2(n[0], n[1]);
            return true;
        }
        if (type.name == Name("Vec4")) {
            // CSS order (top right bottom left) -> left, top, right, bottom.
            f32 n[4];
            u32 count = 0;
            if (!ParseNumbers(text, n, 4, count))
                return false;
            Vec4& v = *static_cast<Vec4*>(object);
            if (count == 1)
                v = Vec4(n[0], n[0], n[0], n[0]);
            else if (count == 2)
                v = Vec4(n[1], n[0], n[1], n[0]);
            else if (count == 3)
                v = Vec4(n[1], n[0], n[1], n[2]);
            else
                v = Vec4(n[3], n[0], n[1], n[2]);
            return true;
        }
        return false;
    }
    default: return false;
    }
}

namespace {

struct Parser
{
    StringView text;
    usize pos = 0;
    u32 line = 1;
    UIStyleSheet& out;
    HashMap<String, String> vars;

    void Error(u32 at, StringView message) { out.errors.PushBack(String::Format("line {}: {}", at, message)); }

    void SkipSpaceAndComments()
    {
        while (pos < text.Size()) {
            const char c = text[pos];
            if (c == '\n') {
                ++line;
                ++pos;
            } else if (IsSpace(c)) {
                ++pos;
            } else if (c == '/' && pos + 1 < text.Size() && text[pos + 1] == '*') {
                pos += 2;
                while (pos + 1 < text.Size() && !(text[pos] == '*' && text[pos + 1] == '/')) {
                    if (text[pos] == '\n')
                        ++line;
                    ++pos;
                }
                pos = Min(pos + 2, text.Size());
            } else if (c == '/' && pos + 1 < text.Size() && text[pos + 1] == '/') {
                while (pos < text.Size() && text[pos] != '\n')
                    ++pos;
            } else {
                break;
            }
        }
    }

    /// Text up to (not including) one of `stops`, with newlines counted.
    StringView Until(StringView stops)
    {
        const usize start = pos;
        while (pos < text.Size() && !stops.Contains(text[pos])) {
            if (text[pos] == '\n')
                ++line;
            ++pos;
        }
        return text.SubStr(start, pos - start);
    }

    String Substitute(StringView value, u32 at)
    {
        String r;
        for (usize i = 0; i < value.Size(); ++i) {
            if (value[i] != '$') {
                r += value[i];
                continue;
            }
            usize j = i + 1;
            while (j < value.Size() && IsIdent(value[j]))
                ++j;
            const String name(value.SubStr(i + 1, j - i - 1));
            if (const String* v = vars.Find(name))
                r += *v;
            else
                Error(at, String::Format("unknown variable ${}", name));
            i = j - 1;
        }
        return r;
    }

    bool ParseCompound(StringView s, UIStyleCompound& c, u32 at)
    {
        usize i = 0;
        auto ident = [&]() {
            const usize start = i;
            while (i < s.Size() && IsIdent(s[i]))
                ++i;
            return s.SubStr(start, i - start);
        };
        if (i < s.Size() && s[i] == '*')
            ++i;
        else if (i < s.Size() && IsIdent(s[i]))
            c.type = String(ident());
        while (i < s.Size()) {
            const char k = s[i++];
            const StringView id = ident();
            if (id.Empty()) {
                Error(at, String::Format("bad selector '{}'", s));
                return false;
            }
            if (k == '.')
                c.classes.PushBack(String(id));
            else if (k == '#')
                c.name = String(id);
            else if (k == ':') {
                const String st = NormalizeStyleName(id);
                if (st == "hover")
                    c.states |= UIStyleState::Hover;
                else if (st == "pressed" || st == "active")
                    c.states |= UIStyleState::Pressed;
                else if (st == "focused" || st == "focus")
                    c.states |= UIStyleState::Focused;
                else if (st == "disabled")
                    c.states |= UIStyleState::Disabled;
                else {
                    Error(at, String::Format("unknown state :{} (hover, pressed, focused, disabled)", id));
                    return false;
                }
            } else {
                Error(at, String::Format("bad selector '{}'", s));
                return false;
            }
        }
        return true;
    }

    bool ParseSelector(StringView s, UIStyleSelector& sel, u32 at)
    {
        s = s.Trim();
        if (s.Empty())
            return false;
        usize i = 0;
        u32 ids = 0, classes = 0, types = 0;
        while (i < s.Size()) {
            while (i < s.Size() && IsSpace(s[i]))
                ++i;
            const usize start = i;
            while (i < s.Size() && !IsSpace(s[i]))
                ++i;
            if (i == start)
                break;
            UIStyleCompound c;
            if (!ParseCompound(s.SubStr(start, i - start), c, at))
                return false;
            ids += c.name.Empty() ? 0 : 1;
            classes += u32(c.classes.Size());
            for (u32 b = 0; b < 4; ++b)
                classes += (u32(c.states) >> b) & 1;
            types += c.type.Empty() ? 0 : 1;
            sel.parts.PushBack(move(c));
        }
        sel.specificity = ids * 10000 + classes * 100 + types;
        return !sel.parts.Empty();
    }

    void Run()
    {
        while (true) {
            SkipSpaceAndComments();
            if (pos >= text.Size())
                break;
            const u32 at = line;
            if (text[pos] == '@') { // @name: value;
                ++pos;
                const StringView name = Until(":;{}");
                if (pos >= text.Size() || text[pos] != ':') {
                    Error(at, "expected ':' after a variable name");
                    Until(";");
                    pos = Min(pos + 1, text.Size());
                    continue;
                }
                ++pos;
                const StringView value = Until(";}");
                vars.InsertOrAssign(String(name.Trim()), Substitute(value.Trim(), at));
                if (pos < text.Size() && text[pos] == ';')
                    ++pos;
                continue;
            }
            const StringView selectors = Until("{};");
            if (pos >= text.Size() || text[pos] != '{') {
                Error(at, "expected '{' after a selector");
                pos = Min(pos + 1, text.Size());
                continue;
            }
            ++pos;
            UIStyleRule rule;
            rule.line = at;
            usize s = 0;
            for (usize i = 0; i <= selectors.Size(); ++i)
                if (i == selectors.Size() || selectors[i] == ',') {
                    UIStyleSelector sel;
                    if (ParseSelector(selectors.SubStr(s, i - s), sel, at))
                        rule.selectors.PushBack(move(sel));
                    s = i + 1;
                }
            // Declarations.
            while (true) {
                SkipSpaceAndComments();
                if (pos >= text.Size()) {
                    Error(at, "missing '}'");
                    break;
                }
                if (text[pos] == '}') {
                    ++pos;
                    break;
                }
                const u32 propLine = line;
                const StringView name = Until(":;}");
                if (pos >= text.Size() || text[pos] != ':') {
                    Error(propLine, String::Format("expected ':' after '{}'", name.Trim()));
                    if (pos < text.Size() && text[pos] == ';')
                        ++pos;
                    continue;
                }
                ++pos;
                const StringView value = Until(";}");
                if (pos < text.Size() && text[pos] == ';')
                    ++pos;
                rule.properties.PushBack(UIStyleProperty{NormalizeStyleName(name.Trim()), Substitute(value.Trim(), propLine), propLine});
            }
            if (!rule.selectors.Empty())
                out.rules.PushBack(move(rule));
        }
    }
};

} // namespace

bool ParseUIStyle(StringView text, UIStyleSheet& out)
{
    out.rules.Clear();
    out.errors.Clear();
    Parser p{text, 0, 1, out, {}};
    p.Run();
    return out.errors.Empty();
}

} // namespace zx
