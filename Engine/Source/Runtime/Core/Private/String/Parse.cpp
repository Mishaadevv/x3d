#include "Core/String/Parse.h"

#include <charconv>

namespace zx {

namespace {

bool ParseUnsignedDigits(StringView s, u64& out)
{
    if (s.Empty())
        return false;
    int base = 10;
    if (s.Size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s = s.DropLeft(2);
    } else if (s.Size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        base = 2;
        s = s.DropLeft(2);
    }
    const auto r = std::from_chars(s.begin(), s.end(), out, base);
    return r.ec == std::errc() && r.ptr == s.end();
}

} // namespace

Optional<u64> ParseUInt(StringView s)
{
    s = s.Trim();
    if (s.StartsWith('+'))
        s = s.DropLeft(1);
    u64 v;
    if (!ParseUnsignedDigits(s, v))
        return NullOpt;
    return v;
}

Optional<i64> ParseInt(StringView s)
{
    s = s.Trim();
    bool negative = false;
    if (s.StartsWith('-')) {
        negative = true;
        s = s.DropLeft(1);
    } else if (s.StartsWith('+')) {
        s = s.DropLeft(1);
    }
    u64 mag;
    if (!ParseUnsignedDigits(s, mag))
        return NullOpt;
    if (negative) {
        if (mag > u64(I64Max) + 1)
            return NullOpt;
        return i64(0 - mag);
    }
    if (mag > u64(I64Max))
        return NullOpt;
    return i64(mag);
}

Optional<f64> ParseDouble(StringView s)
{
    s = s.Trim();
    if (s.StartsWith('+'))
        s = s.DropLeft(1);
    if (s.Empty())
        return NullOpt;
    f64 v;
    const auto r = std::from_chars(s.begin(), s.end(), v);
    if (r.ec != std::errc() || r.ptr != s.end())
        return NullOpt;
    return v;
}

Optional<f32> ParseFloat(StringView s)
{
    s = s.Trim();
    if (s.StartsWith('+'))
        s = s.DropLeft(1);
    if (s.Empty())
        return NullOpt;
    f32 v;
    const auto r = std::from_chars(s.begin(), s.end(), v);
    if (r.ec != std::errc() || r.ptr != s.end())
        return NullOpt;
    return v;
}

Optional<bool> ParseBool(StringView s)
{
    s = s.Trim();
    if (s.EqualsIgnoreCase("true") || s == "1" || s.EqualsIgnoreCase("yes") || s.EqualsIgnoreCase("on"))
        return true;
    if (s.EqualsIgnoreCase("false") || s == "0" || s.EqualsIgnoreCase("no") || s.EqualsIgnoreCase("off"))
        return false;
    return NullOpt;
}

usize ParseDoublePrefix(StringView s, f64& out)
{
    const auto r = std::from_chars(s.begin(), s.end(), out);
    return r.ec == std::errc() ? usize(r.ptr - s.begin()) : 0;
}

usize ParseIntPrefix(StringView s, i64& out)
{
    const auto r = std::from_chars(s.begin(), s.end(), out, 10);
    return r.ec == std::errc() ? usize(r.ptr - s.begin()) : 0;
}

} // namespace zx
