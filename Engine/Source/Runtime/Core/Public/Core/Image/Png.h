#pragma once

#include "Core/Containers/Array.h"
#include "Core/Result.h"

namespace zx::png {

/// Encodes 8-bit RGBA (channels = 4) or RGB (channels = 3) pixels. rowPitch = 0 means tightly packed.
Array<u8> Encode(const u8* pixels, u32 width, u32 height, u32 channels, usize rowPitch = 0, i32 compressionLevel = 6);

/// Encodes and writes to a (virtual or physical) path.
bool WriteFile(StringView path, const u8* pixels, u32 width, u32 height, u32 channels, usize rowPitch = 0);

} // namespace zx::png
