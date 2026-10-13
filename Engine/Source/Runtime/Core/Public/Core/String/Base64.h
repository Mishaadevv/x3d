#pragma once

// Base64 (RFC 4648, standard alphabet) and URI percent-decoding.

#include "Core/Containers/Array.h"
#include "Core/String/String.h"

namespace zx {

String Base64Encode(ByteSpan data);
/// Ignores whitespace; accepts missing padding. Returns false on invalid characters.
bool Base64Decode(StringView text, Array<u8>& out);

/// "%20" -> ' ' etc. Invalid escapes are kept verbatim.
String UriDecode(StringView uri);

} // namespace zx
