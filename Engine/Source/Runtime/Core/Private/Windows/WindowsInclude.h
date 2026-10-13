#pragma once

// Single place where <Windows.h> is included inside Core (private headers/sources only).

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0A00 // Windows 10+
#endif

#include <Windows.h>

// Legacy macros that collide with engine identifiers.
#undef Yield
#undef CreateDirectory
#undef DeleteFile
#undef MoveFile
#undef CopyFile
#undef GetCurrentDirectory
#undef SetCurrentDirectory
#undef GetObject
#undef LoadImage
#undef DrawText
#undef CreateWindow
#undef GetMessage
#undef SendMessage
#undef PostMessage
#undef GetClassName
#undef ERROR
#undef TRANSPARENT
#undef OPAQUE
#undef RELATIVE
#undef ABSOLUTE

#include "Core/String/StringView.h"

namespace zx::win {

/// UTF-8 -> UTF-16 into a caller buffer (null-terminated). Returns length in wchar_t (excluding '\0').
usize Utf8ToWide(StringView utf8, wchar_t* out, usize capacity);

/// UTF-16 -> UTF-8 into a caller buffer (null-terminated). Returns length in bytes (excluding '\0').
usize WideToUtf8(const wchar_t* wide, usize wideLen, char* out, usize capacity);

} // namespace zx::win
