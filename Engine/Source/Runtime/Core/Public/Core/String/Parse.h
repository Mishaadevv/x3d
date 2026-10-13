#pragma once

#include "Core/Containers/Optional.h"
#include "Core/String/StringView.h"

namespace zx {

/// Strict parsers: the whole (trimmed) input must be consumed. Integers accept 0x / 0b prefixes and leading +/-.
Optional<i64> ParseInt(StringView s);
Optional<u64> ParseUInt(StringView s);
Optional<f64> ParseDouble(StringView s);
Optional<f32> ParseFloat(StringView s);
/// Accepts true/false/1/0/yes/no/on/off (case-insensitive).
Optional<bool> ParseBool(StringView s);

/// Lenient prefix parsing: parses as much as possible starting at s, returns chars consumed (0 on failure).
usize ParseDoublePrefix(StringView s, f64& out);
usize ParseIntPrefix(StringView s, i64& out);

} // namespace zx
