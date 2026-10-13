#include "Core/Serialization/Json.h"
#include "Core/String/Parse.h"
#include "Core/String/Utf.h"

#include <charconv>
#include <cmath>

namespace zx {

// --- JsonValue ------------------------------------------------------------------------------------------------------

JsonValue::JsonValue(StringView s) : m_type(JsonType::String) { m_string = New<String>(s); }

JsonValue::JsonValue(const JsonValue& o) : m_type(o.m_type), m_isInteger(o.m_isInteger), m_isSingle(o.m_isSingle)
{
    switch (m_type) {
    case JsonType::String:
        m_string = New<String>(*o.m_string);
        break;
    case JsonType::Array:
        m_array = New<Array<JsonValue>>(*o.m_array);
        break;
    case JsonType::Object:
        m_object = New<Array<JsonMember>>(*o.m_object);
        break;
    default:
        m_int = o.m_int; // copies the whole scalar payload
        break;
    }
}

JsonValue::JsonValue(JsonValue&& o) noexcept : m_type(o.m_type), m_isInteger(o.m_isInteger), m_isSingle(o.m_isSingle)
{
    m_int = o.m_int; // pointer or scalar payload (same size)
    static_assert(sizeof(i64) == sizeof(void*));
    o.m_type = JsonType::Null;
}

JsonValue& JsonValue::operator=(const JsonValue& o)
{
    if (this != &o) {
        JsonValue tmp(o);
        *this = move(tmp);
    }
    return *this;
}

JsonValue& JsonValue::operator=(JsonValue&& o) noexcept
{
    if (this != &o) {
        Reset();
        m_type = o.m_type;
        m_isInteger = o.m_isInteger;
        m_isSingle = o.m_isSingle;
        m_int = o.m_int;
        o.m_type = JsonType::Null;
    }
    return *this;
}

void JsonValue::Reset()
{
    switch (m_type) {
    case JsonType::String:
        Delete(m_string);
        break;
    case JsonType::Array:
        Delete(m_array);
        break;
    case JsonType::Object:
        Delete(m_object);
        break;
    default:
        break;
    }
    m_type = JsonType::Null;
    m_int = 0;
}

JsonValue JsonValue::MakeArray()
{
    JsonValue v;
    v.m_type = JsonType::Array;
    v.m_array = New<Array<JsonValue>>();
    return v;
}

JsonValue JsonValue::MakeObject()
{
    JsonValue v;
    v.m_type = JsonType::Object;
    v.m_object = New<Array<JsonMember>>();
    return v;
}

i64 JsonValue::AsInt(i64 fallback) const
{
    if (!IsNumber())
        return fallback;
    return m_isInteger ? m_int : i64(std::llround(m_double));
}

f64 JsonValue::AsDouble(f64 fallback) const
{
    if (!IsNumber())
        return fallback;
    return m_isInteger ? f64(m_int) : m_double;
}

StringView JsonValue::AsString(StringView fallback) const { return IsString() ? m_string->View() : fallback; }

usize JsonValue::Size() const
{
    if (IsArray())
        return m_array->Size();
    if (IsObject())
        return m_object->Size();
    return 0;
}

JsonValue& JsonValue::operator[](usize index)
{
    ZX_ASSERT(IsArray(), "JsonValue is not an array");
    return (*m_array)[index];
}

const JsonValue& JsonValue::operator[](usize index) const
{
    ZX_ASSERT(IsArray(), "JsonValue is not an array");
    return (*m_array)[index];
}

JsonValue& JsonValue::PushBack(JsonValue v)
{
    if (IsNull())
        *this = MakeArray();
    ZX_ASSERT(IsArray(), "PushBack on non-array JsonValue");
    return m_array->PushBack(move(v));
}

Span<JsonValue> JsonValue::Elements() { return IsArray() ? m_array->View() : Span<JsonValue>(); }
Span<const JsonValue> JsonValue::Elements() const
{
    return IsArray() ? Span<const JsonValue>(m_array->Data(), m_array->Size()) : Span<const JsonValue>();
}

JsonValue* JsonValue::Find(StringView key)
{
    if (!IsObject())
        return nullptr;
    for (JsonMember& m : *m_object)
        if (m.first == key)
            return &m.second;
    return nullptr;
}

const JsonValue* JsonValue::Find(StringView key) const { return const_cast<JsonValue*>(this)->Find(key); }

JsonValue& JsonValue::operator[](StringView key)
{
    if (IsNull())
        *this = MakeObject();
    ZX_ASSERT(IsObject(), "Key access on non-object JsonValue");
    if (JsonValue* v = Find(key))
        return *v;
    return m_object->PushBack(JsonMember{String(key), JsonValue()}).second;
}

const JsonValue& JsonValue::Get(StringView key) const
{
    static const JsonValue s_null;
    const JsonValue* v = Find(key);
    return v ? *v : s_null;
}

JsonValue& JsonValue::Set(StringView key, JsonValue v)
{
    JsonValue& slot = (*this)[key];
    slot = move(v);
    return slot;
}

bool JsonValue::Remove(StringView key)
{
    if (!IsObject())
        return false;
    for (usize i = 0; i < m_object->Size(); ++i) {
        if ((*m_object)[i].first == key) {
            m_object->RemoveAt(i);
            return true;
        }
    }
    return false;
}

Span<JsonMember> JsonValue::Members() { return IsObject() ? m_object->View() : Span<JsonMember>(); }
Span<const JsonMember> JsonValue::Members() const
{
    return IsObject() ? Span<const JsonMember>(m_object->Data(), m_object->Size()) : Span<const JsonMember>();
}

bool operator==(const JsonValue& a, const JsonValue& b)
{
    if (a.m_type != b.m_type)
        return false;
    switch (a.m_type) {
    case JsonType::Null:
        return true;
    case JsonType::Bool:
        return a.m_bool == b.m_bool;
    case JsonType::Number:
        if (a.m_isInteger && b.m_isInteger)
            return a.m_int == b.m_int;
        return a.AsDouble() == b.AsDouble();
    case JsonType::String:
        return *a.m_string == *b.m_string;
    case JsonType::Array:
        return *a.m_array == *b.m_array;
    case JsonType::Object: {
        if (a.m_object->Size() != b.m_object->Size())
            return false;
        for (const JsonMember& m : *a.m_object) {
            const JsonValue* other = b.Find(m.first);
            if (!other || !(*other == m.second))
                return false;
        }
        return true;
    }
    }
    return false;
}

// --- Parser ---------------------------------------------------------------------------------------------------------

namespace {

class Parser
{
public:
    Parser(StringView text, JsonParseError* error) : m_text(text), m_error(error) {}

    bool Parse(JsonValue& out)
    {
        // Skip UTF-8 BOM.
        if (m_text.Size() >= 3 && u8(m_text[0]) == 0xEF && u8(m_text[1]) == 0xBB && u8(m_text[2]) == 0xBF)
            m_pos = 3;
        if (!SkipWhitespace() || !ParseValue(out, 0))
            return false;
        if (!SkipWhitespace())
            return false;
        if (m_pos != m_text.Size())
            return Fail("Unexpected trailing characters");
        return true;
    }

private:
    static constexpr u32 MaxDepth = 512;

    bool Fail(StringView message)
    {
        if (m_error && m_error->message.Empty()) {
            u32 line = 1, col = 1;
            for (usize i = 0; i < m_pos && i < m_text.Size(); ++i) {
                if (m_text[i] == '\n') {
                    ++line;
                    col = 1;
                } else {
                    ++col;
                }
            }
            m_error->message = message;
            m_error->line = line;
            m_error->column = col;
        }
        return false;
    }

    bool SkipWhitespace()
    {
        while (m_pos < m_text.Size()) {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++m_pos;
            } else if (c == '/' && m_pos + 1 < m_text.Size() && m_text[m_pos + 1] == '/') {
                while (m_pos < m_text.Size() && m_text[m_pos] != '\n')
                    ++m_pos;
            } else if (c == '/' && m_pos + 1 < m_text.Size() && m_text[m_pos + 1] == '*') {
                const usize end = m_text.Find("*/", m_pos + 2);
                if (end == StringView::NPos)
                    return Fail("Unterminated block comment");
                m_pos = end + 2;
            } else {
                break;
            }
        }
        return true;
    }

    bool ParseValue(JsonValue& out, u32 depth)
    {
        if (depth > MaxDepth)
            return Fail("Nesting too deep");
        if (m_pos >= m_text.Size())
            return Fail("Unexpected end of input");
        const char c = m_text[m_pos];
        switch (c) {
        case '{':
            return ParseObject(out, depth);
        case '[':
            return ParseArray(out, depth);
        case '"': {
            String s;
            if (!ParseString(s))
                return false;
            out = JsonValue(s.View());
            return true;
        }
        case 't':
            return ParseLiteral("true", JsonValue(true), out);
        case 'f':
            return ParseLiteral("false", JsonValue(false), out);
        case 'n':
            return ParseLiteral("null", JsonValue(), out);
        default:
            if (c == '-' || IsDigitAscii(c))
                return ParseNumber(out);
            return Fail("Unexpected character");
        }
    }

    bool ParseLiteral(StringView word, JsonValue value, JsonValue& out)
    {
        if (m_text.SubStr(m_pos, word.Size()) != word)
            return Fail("Invalid literal");
        m_pos += word.Size();
        out = move(value);
        return true;
    }

    bool ParseNumber(JsonValue& out)
    {
        const usize start = m_pos;
        if (m_text[m_pos] == '-')
            ++m_pos;
        bool isInteger = true;
        while (m_pos < m_text.Size()) {
            const char c = m_text[m_pos];
            if (IsDigitAscii(c)) {
                ++m_pos;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || (c == '-' && m_pos > start)) {
                isInteger = false;
                ++m_pos;
            } else {
                break;
            }
        }
        const StringView token = m_text.SubStr(start, m_pos - start);
        if (isInteger) {
            i64 v = 0;
            const auto r = std::from_chars(token.begin(), token.end(), v);
            if (r.ec == std::errc() && r.ptr == token.end()) {
                out = JsonValue(v);
                return true;
            }
            // Out of i64 range: try u64 (stored as bits), then fall back to double.
            u64 u = 0;
            const auto ru = std::from_chars(token.begin(), token.end(), u);
            if (ru.ec == std::errc() && ru.ptr == token.end()) {
                out = JsonValue(u);
                return true;
            }
        }
        f64 d = 0;
        const auto r = std::from_chars(token.begin(), token.end(), d);
        if (r.ec != std::errc() || r.ptr != token.end())
            return Fail("Invalid number");
        out = JsonValue(d);
        return true;
    }

    static i32 HexValue(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    bool ParseHex4(u32& out)
    {
        if (m_pos + 4 > m_text.Size())
            return Fail("Truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const i32 h = HexValue(m_text[m_pos++]);
            if (h < 0)
                return Fail("Invalid \\u escape");
            out = (out << 4) | u32(h);
        }
        return true;
    }

    bool ParseString(String& out)
    {
        ++m_pos; // opening quote
        usize runStart = m_pos;
        for (;;) {
            if (m_pos >= m_text.Size())
                return Fail("Unterminated string");
            const char c = m_text[m_pos];
            if (c == '"') {
                out.Append(m_text.SubStr(runStart, m_pos - runStart));
                ++m_pos;
                return true;
            }
            if (u8(c) < 0x20)
                return Fail("Control character in string");
            if (c != '\\') {
                ++m_pos;
                continue;
            }
            out.Append(m_text.SubStr(runStart, m_pos - runStart));
            ++m_pos;
            if (m_pos >= m_text.Size())
                return Fail("Unterminated escape");
            const char e = m_text[m_pos++];
            switch (e) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                u32 cp;
                if (!ParseHex4(cp))
                    return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    u32 lo = 0;
                    if (m_pos + 2 <= m_text.Size() && m_text[m_pos] == '\\' && m_text[m_pos + 1] == 'u') {
                        m_pos += 2;
                        if (!ParseHex4(lo))
                            return false;
                    }
                    cp = (lo >= 0xDC00 && lo <= 0xDFFF) ? 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
                                                        : ReplacementCodepoint;
                }
                Utf8Append(out, cp);
                break;
            }
            default:
                return Fail("Invalid escape sequence");
            }
            runStart = m_pos;
        }
    }

    bool ParseKey(String& out)
    {
        if (m_pos < m_text.Size() && m_text[m_pos] == '"')
            return ParseString(out);
        // Unquoted identifier key.
        const usize start = m_pos;
        while (m_pos < m_text.Size() && (IsAlnumAscii(m_text[m_pos]) || m_text[m_pos] == '_' || m_text[m_pos] == '.'))
            ++m_pos;
        if (m_pos == start)
            return Fail("Expected object key");
        out = m_text.SubStr(start, m_pos - start);
        return true;
    }

    bool ParseArray(JsonValue& out, u32 depth)
    {
        ++m_pos;
        out = JsonValue::MakeArray();
        for (;;) {
            if (!SkipWhitespace())
                return false;
            if (m_pos >= m_text.Size())
                return Fail("Unterminated array");
            if (m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            JsonValue element;
            if (!ParseValue(element, depth + 1))
                return false;
            out.PushBack(move(element));
            if (!SkipWhitespace())
                return false;
            if (m_pos < m_text.Size() && m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_pos < m_text.Size() && m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            return Fail("Expected ',' or ']'");
        }
    }

    bool ParseObject(JsonValue& out, u32 depth)
    {
        ++m_pos;
        out = JsonValue::MakeObject();
        for (;;) {
            if (!SkipWhitespace())
                return false;
            if (m_pos >= m_text.Size())
                return Fail("Unterminated object");
            if (m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            String key;
            if (!ParseKey(key))
                return false;
            if (!SkipWhitespace())
                return false;
            if (m_pos >= m_text.Size() || m_text[m_pos] != ':')
                return Fail("Expected ':'");
            ++m_pos;
            if (!SkipWhitespace())
                return false;
            JsonValue value;
            if (!ParseValue(value, depth + 1))
                return false;
            out.Set(key.View(), move(value)); // duplicate keys: last wins
            if (!SkipWhitespace())
                return false;
            if (m_pos < m_text.Size() && m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_pos < m_text.Size() && m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            return Fail("Expected ',' or '}'");
        }
    }

    StringView m_text;
    usize m_pos = 0;
    JsonParseError* m_error;
};

// --- Writer ---------------------------------------------------------------------------------------------------------

void WriteEscaped(String& out, StringView s)
{
    out += '"';
    usize runStart = 0;
    for (usize i = 0; i < s.Size(); ++i) {
        const u8 c = u8(s[i]);
        const char* esc = nullptr;
        char buf[8];
        switch (c) {
        case '"':
            esc = "\\\"";
            break;
        case '\\':
            esc = "\\\\";
            break;
        case '\n':
            esc = "\\n";
            break;
        case '\r':
            esc = "\\r";
            break;
        case '\t':
            esc = "\\t";
            break;
        case '\b':
            esc = "\\b";
            break;
        case '\f':
            esc = "\\f";
            break;
        default:
            if (c < 0x20) {
                FormatToBuffer(buf, sizeof(buf), "\\u{:04x}", u32(c));
                esc = buf;
            }
            break;
        }
        if (esc) {
            out.Append(s.SubStr(runStart, i - runStart));
            out.Append(StringView(esc));
            runStart = i + 1;
        }
    }
    out.Append(s.SubStr(runStart));
    out += '"';
}

void WriteNumber(String& out, const JsonValue& v)
{
    if (v.IsInteger()) {
        out.Appendf("{}", v.AsInt());
        return;
    }
    const f64 d = v.AsDouble();
    if (!std::isfinite(d)) {
        out += "null"; // JSON has no inf/nan
        return;
    }
    char buf[64];
    const auto r = v.IsSingle() ? std::to_chars(buf, buf + sizeof(buf), f32(d)) : std::to_chars(buf, buf + sizeof(buf), d);
    StringView text(buf, r.ptr);
    out.Append(text);
    // Keep a decimal marker so the value round-trips as floating point.
    if (text.FindFirstOf(".eE") == StringView::NPos)
        out += ".0";
}

bool IsSimpleArray(const JsonValue& v, u32 maxCount)
{
    if (v.Size() > maxCount)
        return false;
    for (const JsonValue& e : v.Elements())
        if (!(e.IsNumber() || e.IsBool() || e.IsNull()))
            return false;
    return true;
}

void WriteValue(String& out, const JsonValue& v, const JsonWriteOptions& opt, u32 depth)
{
    auto newline = [&](u32 d) {
        if (!opt.pretty)
            return;
        out += '\n';
        out.Append(usize(d) * opt.indent, ' ');
    };
    switch (v.Type()) {
    case JsonType::Null:
        out += "null";
        break;
    case JsonType::Bool:
        out += v.AsBool() ? "true" : "false";
        break;
    case JsonType::Number:
        WriteNumber(out, v);
        break;
    case JsonType::String:
        WriteEscaped(out, v.AsString());
        break;
    case JsonType::Array: {
        if (v.Size() == 0) {
            out += "[]";
            break;
        }
        const bool inlineArr = !opt.pretty || IsSimpleArray(v, opt.inlineArrayMax);
        out += '[';
        bool first = true;
        for (const JsonValue& e : v.Elements()) {
            if (!first)
                out += inlineArr && opt.pretty ? ", " : ",";
            if (!inlineArr)
                newline(depth + 1);
            WriteValue(out, e, opt, depth + 1);
            first = false;
        }
        if (!inlineArr)
            newline(depth);
        out += ']';
        break;
    }
    case JsonType::Object: {
        if (v.Size() == 0) {
            out += "{}";
            break;
        }
        out += '{';
        bool first = true;
        for (const JsonMember& m : v.Members()) {
            if (!first)
                out += ',';
            newline(depth + 1);
            WriteEscaped(out, m.first.View());
            out += opt.pretty ? ": " : ":";
            WriteValue(out, m.second, opt, depth + 1);
            first = false;
        }
        newline(depth);
        out += '}';
        break;
    }
    }
}

} // namespace

bool ParseJson(StringView text, JsonValue& out, JsonParseError* error)
{
    if (error)
        *error = JsonParseError();
    Parser p(text, error);
    JsonValue result;
    if (!p.Parse(result))
        return false;
    out = move(result);
    return true;
}

void WriteJson(const JsonValue& value, String& out, const JsonWriteOptions& options)
{
    WriteValue(out, value, options, 0);
    if (options.pretty)
        out += '\n';
}

String ToJsonString(const JsonValue& value, const JsonWriteOptions& options)
{
    String s;
    WriteJson(value, s, options);
    return s;
}

} // namespace zx
