#pragma once

// Path string helpers. Engine paths always use '/' separators (backslashes are normalized).
// Works on both virtual paths ("/Game/Textures/Rock.png") and physical ones ("C:/Projects/MyGame").

#include "Core/String/String.h"

namespace zx::Path {

/// Replaces '\\' with '/', collapses duplicate separators, resolves "." and ".." where possible,
/// removes trailing '/' (except for roots like "/" or "C:/").
String Normalize(StringView path);

/// Joins with exactly one '/' between parts. An absolute `b` replaces `a`.
String Join(StringView a, StringView b);
String Join(StringView a, StringView b, StringView c);

/// "dir/file.ext" -> "file.ext"
StringView FileName(StringView path);
/// "dir/file.tar.gz" -> "file.tar"
StringView Stem(StringView path);
/// "dir/file.ext" -> ".ext" (with dot; empty if none)
StringView Extension(StringView path);
/// "dir/sub/file.ext" -> "dir/sub" ("" for a bare file name, "/" for "/file")
StringView Parent(StringView path);
/// Replaces (or adds) the extension. newExt may include or omit the leading dot.
String ChangeExtension(StringView path, StringView newExt);
/// Case-insensitive extension compare (ext with or without dot).
bool HasExtension(StringView path, StringView ext);

bool IsAbsolute(StringView path);
/// Path of `path` relative to `base` (both normalized, same root). Returns empty if not under base.
String MakeRelative(StringView path, StringView base);
/// True if `path` is equal to or inside directory `dir`.
bool IsUnder(StringView path, StringView dir);

/// Replaces characters invalid in file names (<>:"/\|?* and control chars) with '_'.
String SanitizeFileName(StringView name);

} // namespace zx::Path
