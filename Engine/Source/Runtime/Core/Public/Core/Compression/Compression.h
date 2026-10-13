#pragma once

// Own implementations of DEFLATE (RFC 1951), zlib framing (RFC 1950) and ZLZ (fast LZ77 block codec,
// LZ4-like) used for packages and the derived data cache.

#include "Core/Containers/Array.h"
#include "Core/Containers/Span.h"

namespace zx::compression {

/// Decompresses a raw DEFLATE stream, appending to `output`. `expectedSize` (if known) pre-reserves.
/// Returns false on malformed input or if output would exceed maxOutput.
bool Inflate(ByteSpan input, Array<u8>& output, usize expectedSize = 0, usize maxOutput = usize(1) << 32);

/// zlib-wrapped DEFLATE (2-byte header + data + Adler-32). Verifies the checksum.
bool ZlibDecompress(ByteSpan input, Array<u8>& output, usize expectedSize = 0);

/// Compresses to a raw DEFLATE stream. level: 0 = stored, 1 = fastest ... 9 = best.
Array<u8> Deflate(ByteSpan input, i32 level = 6);

/// zlib-wrapped DEFLATE.
Array<u8> ZlibCompress(ByteSpan input, i32 level = 6);

/// Worst-case compressed size for ZLZ.
usize ZlzMaxCompressedSize(usize inputSize);
/// Compresses a block. Appends to output. Returns compressed size.
usize ZlzCompress(ByteSpan input, Array<u8>& output);
/// Decompresses a block into exactly outputSize bytes. Returns false on malformed input.
bool ZlzDecompress(ByteSpan input, u8* output, usize outputSize);

} // namespace zx::compression
