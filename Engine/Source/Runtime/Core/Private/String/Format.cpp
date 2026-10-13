#include "Core/String/Format.h"

#include <charconv>
#include <cmath>

namespace zx {

void FormatSink::AppendChar(char c, usize count)
{
    char chunk[64];
    const usize fillCount = count < sizeof(chunk) ? count : sizeof(chunk);
    for (usize i = 0; i < fillCount; ++i)
        chunk[i] = c;
    while (count > 0) {
        const usize n = count < sizeof(chunk) ? count : sizeof(chunk);
        Append(chunk, n);
        count -= n;
    }
}

void FixedBufferSink::Append(const char* s, usize n)
{
    if (m_capacity == 0) {
        m_truncated = m_truncated || n > 0;
        return;
    }
    const usize room = m_capacity - 1 - m_length;
    const usize take = n < room ? n : room;
    if (take > 0)
        std::memcpy(m_buffer + m_length, s, take);
    m_length += take;
    m_buffer[m_length] = '\0';
    if (take < n)
        m_truncated = true;
}

namespace {

// Writes `body` with padding according to spec. `prefix` (sign, 0x) goes before zero padding.
void EmitPadded(FormatSink& sink, StringView prefix, StringView body, const FormatSpec& spec, char defaultAlign)
{
    const usize contentLen = prefix.Size() + body.Size();
    const usize width = spec.width > 0 ? usize(spec.width) : 0;
    if (contentLen >= width) {
        sink.Append(prefix);
        sink.Append(body);
        return;
    }
    const usize pad = width - contentLen;
    if (spec.zeroPad && spec.align == 0) {
        sink.Append(prefix);
        sink.AppendChar('0', pad);
        sink.Append(body);
        return;
    }
    const char align = spec.align ? spec.align : defaultAlign;
    usize left = 0, right = 0;
    if (align == '<')
        right = pad;
    else if (align == '>')
        left = pad;
    else {
        left = pad / 2;
        right = pad - left;
    }
    sink.AppendChar(spec.fill, left);
    sink.Append(prefix);
    sink.Append(body);
    sink.AppendChar(spec.fill, right);
}

bool ParseInt(StringView s, usize& pos, i32& out)
{
    if (pos >= s.Size() || !IsDigitAscii(s[pos]))
        return false;
    i32 v = 0;
    while (pos < s.Size() && IsDigitAscii(s[pos])) {
        v = v * 10 + (s[pos] - '0');
        ++pos;
    }
    out = v;
    return true;
}

FormatSpec ParseSpec(StringView s)
{
    FormatSpec spec;
    usize p = 0;
    auto isAlign = [](char c) { return c == '<' || c == '>' || c == '^'; };
    if (s.Size() >= 2 && isAlign(s[1])) {
        spec.fill = s[0];
        spec.align = s[1];
        p = 2;
    } else if (s.Size() >= 1 && isAlign(s[0])) {
        spec.align = s[0];
        p = 1;
    }
    if (p < s.Size() && (s[p] == '+' || s[p] == '-' || s[p] == ' '))
        spec.sign = s[p++];
    if (p < s.Size() && s[p] == '#') {
        spec.alternate = true;
        ++p;
    }
    if (p < s.Size() && s[p] == '0') {
        spec.zeroPad = true;
        ++p;
    }
    ParseInt(s, p, spec.width);
    if (p < s.Size() && s[p] == '.') {
        ++p;
        ParseInt(s, p, spec.precision);
    }
    if (p < s.Size())
        spec.type = s[p];
    return spec;
}

void FormatArgument(FormatSink& sink, const FormatArg& arg, const FormatSpec& spec)
{
    switch (arg.kind) {
    case FormatArg::Kind::Bool:
        if (spec.type == 'd')
            FormatInteger(sink, arg.b ? 1 : 0, false, spec);
        else
            FormatString(sink, arg.b ? "true" : "false", spec);
        break;
    case FormatArg::Kind::Char:
        if (spec.type == 'd' || spec.type == 'x' || spec.type == 'X')
            FormatInteger(sink, u64(u8(arg.c)), false, spec);
        else
            FormatString(sink, StringView(&arg.c, 1), spec);
        break;
    case FormatArg::Kind::Int: {
        const bool neg = arg.i < 0;
        const u64 mag = neg ? (~u64(arg.i) + 1) : u64(arg.i);
        FormatInteger(sink, mag, neg, spec);
        break;
    }
    case FormatArg::Kind::UInt:
        FormatInteger(sink, arg.u, false, spec);
        break;
    case FormatArg::Kind::Float:
        FormatFloat(sink, f64(arg.f), spec, true);
        break;
    case FormatArg::Kind::Double:
        FormatFloat(sink, arg.d, spec, false);
        break;
    case FormatArg::Kind::String:
        FormatString(sink, StringView(arg.str.data, arg.str.size), spec);
        break;
    case FormatArg::Kind::Pointer: {
        FormatSpec ps = spec;
        ps.type = 'x';
        ps.alternate = true;
        if (ps.width == 0) {
            ps.width = 18;
            ps.zeroPad = true;
        }
        FormatInteger(sink, u64(reinterpret_cast<uptr>(arg.ptr)), false, ps);
        break;
    }
    case FormatArg::Kind::Custom:
        arg.custom.fn(sink, arg.custom.object, spec);
        break;
    case FormatArg::Kind::None:
        break;
    }
}

} // namespace

void FormatInteger(FormatSink& sink, u64 magnitude, bool negative, const FormatSpec& spec)
{
    char digits[72];
    char* end = digits + sizeof(digits);
    char* p = end;

    u32 base = 10;
    const char* table = "0123456789abcdef";
    StringView basePrefix;
    switch (spec.type) {
    case 'x':
        base = 16;
        basePrefix = "0x";
        break;
    case 'X':
        base = 16;
        table = "0123456789ABCDEF";
        basePrefix = "0X";
        break;
    case 'b':
        base = 2;
        basePrefix = "0b";
        break;
    case 'o':
        base = 8;
        basePrefix = "0";
        break;
    default:
        break;
    }
    do {
        *--p = table[magnitude % base];
        magnitude /= base;
    } while (magnitude != 0);

    char prefix[4];
    usize prefixLen = 0;
    if (negative)
        prefix[prefixLen++] = '-';
    else if (spec.sign == '+')
        prefix[prefixLen++] = '+';
    else if (spec.sign == ' ')
        prefix[prefixLen++] = ' ';
    if (spec.alternate)
        for (char c : basePrefix)
            prefix[prefixLen++] = c;

    EmitPadded(sink, StringView(prefix, prefixLen), StringView(p, end), spec, '>');
}

void FormatFloat(FormatSink& sink, f64 value, const FormatSpec& spec, bool isSinglePrecision)
{
    char buf[400];
    bool negative = std::signbit(value);
    const f64 mag = negative ? -value : value;

    StringView body;
    if (std::isnan(value)) {
        body = "nan";
        negative = false;
    } else if (std::isinf(value)) {
        body = "inf";
    } else {
        std::to_chars_result r;
        char* const first = buf;
        char* const last = buf + sizeof(buf);
        const i32 prec = spec.precision;
        switch (spec.type) {
        case 'f':
        case 'F':
            r = std::to_chars(first, last, mag, std::chars_format::fixed, prec >= 0 ? prec : 6);
            break;
        case 'e':
        case 'E':
            r = std::to_chars(first, last, mag, std::chars_format::scientific, prec >= 0 ? prec : 6);
            break;
        case 'g':
        case 'G':
            r = std::to_chars(first, last, mag, std::chars_format::general, prec >= 0 ? prec : 6);
            break;
        default:
            if (prec >= 0)
                r = std::to_chars(first, last, mag, std::chars_format::general, prec);
            else if (isSinglePrecision)
                r = std::to_chars(first, last, f32(mag)); // shortest round-trip for float
            else
                r = std::to_chars(first, last, mag); // shortest round-trip for double
            break;
        }
        if (spec.type == 'E' || spec.type == 'G')
            for (char* c = first; c != r.ptr; ++c)
                *c = ToUpperAscii(*c);
        body = StringView(first, r.ptr);
    }

    char prefix[2];
    usize prefixLen = 0;
    if (negative)
        prefix[prefixLen++] = '-';
    else if (spec.sign == '+')
        prefix[prefixLen++] = '+';
    else if (spec.sign == ' ')
        prefix[prefixLen++] = ' ';
    EmitPadded(sink, StringView(prefix, prefixLen), body, spec, '>');
}

void FormatString(FormatSink& sink, StringView s, const FormatSpec& spec)
{
    if (spec.precision >= 0 && usize(spec.precision) < s.Size())
        s = s.Left(usize(spec.precision));
    FormatSpec ss = spec;
    ss.zeroPad = false;
    EmitPadded(sink, StringView(), s, ss, '<');
}

void VFormatTo(FormatSink& sink, StringView fmt, const FormatArg* args, usize argCount)
{
    usize nextArg = 0;
    usize i = 0;
    usize literalStart = 0;
    const usize n = fmt.Size();
    while (i < n) {
        const char c = fmt[i];
        if (c == '{') {
            if (i + 1 < n && fmt[i + 1] == '{') {
                sink.Append(fmt.Data() + literalStart, i + 1 - literalStart);
                i += 2;
                literalStart = i;
                continue;
            }
            const usize close = fmt.Find('}', i + 1);
            if (close == StringView::NPos)
                break; // malformed: emit rest as literal
            sink.Append(fmt.Data() + literalStart, i - literalStart);

            StringView inner = fmt.SubStr(i + 1, close - i - 1);
            StringView indexPart = inner;
            StringView specPart;
            const usize colon = inner.Find(':');
            if (colon != StringView::NPos) {
                indexPart = inner.Left(colon);
                specPart = inner.SubStr(colon + 1);
            }
            usize argIndex = nextArg;
            if (!indexPart.Empty()) {
                usize p = 0;
                i32 idx = 0;
                if (ParseInt(indexPart, p, idx))
                    argIndex = usize(idx);
            } else {
                ++nextArg;
            }
            if (argIndex < argCount)
                FormatArgument(sink, args[argIndex], ParseSpec(specPart));
            else
                sink.Append("{?}");
            i = close + 1;
            literalStart = i;
            continue;
        }
        if (c == '}' && i + 1 < n && fmt[i + 1] == '}') {
            sink.Append(fmt.Data() + literalStart, i + 1 - literalStart);
            i += 2;
            literalStart = i;
            continue;
        }
        ++i;
    }
    if (literalStart < n)
        sink.Append(fmt.Data() + literalStart, n - literalStart);
}

} // namespace zx
