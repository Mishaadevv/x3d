#pragma once

// OpenEXR writer (half-float RGB or RGBA scanlines, ZIP or no compression). The decoder is DecodeExr in Image.h.

#include "Core/Image/Image.h"

namespace zx::exr {

/// Encodes an RGBA32F image; alpha is written only when some pixel has alpha < 1.
Array<u8> Encode(const Image& image, bool compress = true);

} // namespace zx::exr
