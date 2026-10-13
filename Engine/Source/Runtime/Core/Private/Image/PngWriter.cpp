#include "Core/Compression/Compression.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Hash/Hash.h"
#include "Core/Image/Png.h"
#include "Core/Math/MathCommon.h"

namespace zx::png {

namespace {

void PutU32BE(Array<u8>& out, u32 v)
{
    out.PushBack(u8(v >> 24));
    out.PushBack(u8(v >> 16));
    out.PushBack(u8(v >> 8));
    out.PushBack(u8(v));
}

void WriteChunk(Array<u8>& out, const char type[4], ByteSpan data)
{
    PutU32BE(out, u32(data.Size()));
    const usize typeStart = out.Size();
    out.Append(Span<const u8>(reinterpret_cast<const u8*>(type), 4));
    out.Append(data);
    const u32 crc = Crc32(out.Data() + typeStart, 4 + data.Size());
    PutU32BE(out, crc);
}

u8 Paeth(u8 a, u8 b, u8 c)
{
    const i32 p = i32(a) + i32(b) - i32(c);
    const i32 pa = Abs(p - i32(a)), pb = Abs(p - i32(b)), pc = Abs(p - i32(c));
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

} // namespace

Array<u8> Encode(const u8* pixels, u32 width, u32 height, u32 channels, usize rowPitch, i32 level)
{
    ZX_ASSERT(channels == 3 || channels == 4, "PNG encoder supports RGB8/RGBA8");
    const usize rowBytes = usize(width) * channels;
    if (rowPitch == 0)
        rowPitch = rowBytes;

    // Filter each row with the heuristic used by libpng: pick the filter with the smallest sum of |residuals|.
    Array<u8> filtered;
    filtered.ResizeUninitialized((rowBytes + 1) * height);
    Array<u8> candidate;
    candidate.ResizeUninitialized(rowBytes);
    for (u32 y = 0; y < height; ++y) {
        const u8* row = pixels + usize(y) * rowPitch;
        const u8* prev = y > 0 ? pixels + usize(y - 1) * rowPitch : nullptr;
        u8* dst = filtered.Data() + usize(y) * (rowBytes + 1);
        u64 bestScore = U64Max;
        for (u8 filter = 0; filter < 5; ++filter) {
            u64 score = 0;
            for (usize i = 0; i < rowBytes; ++i) {
                const u8 a = i >= channels ? row[i - channels] : 0;
                const u8 b = prev ? prev[i] : 0;
                const u8 c = (prev && i >= channels) ? prev[i - channels] : 0;
                u8 predicted = 0;
                switch (filter) {
                case 1: predicted = a; break;
                case 2: predicted = b; break;
                case 3: predicted = u8((u32(a) + u32(b)) / 2); break;
                case 4: predicted = Paeth(a, b, c); break;
                default: break;
                }
                const u8 r = u8(row[i] - predicted);
                candidate[i] = r;
                score += r < 128 ? r : 256 - r;
            }
            if (score < bestScore) {
                bestScore = score;
                dst[0] = filter;
                MemCopy(dst + 1, candidate.Data(), rowBytes);
            }
        }
    }

    Array<u8> out;
    static const u8 signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.Append(Span<const u8>(signature));
    u8 ihdr[13];
    ihdr[0] = u8(width >> 24);
    ihdr[1] = u8(width >> 16);
    ihdr[2] = u8(width >> 8);
    ihdr[3] = u8(width);
    ihdr[4] = u8(height >> 24);
    ihdr[5] = u8(height >> 16);
    ihdr[6] = u8(height >> 8);
    ihdr[7] = u8(height);
    ihdr[8] = 8;                          // bit depth
    ihdr[9] = channels == 4 ? 6 : 2;      // RGBA / RGB
    ihdr[10] = 0;                         // deflate
    ihdr[11] = 0;                         // adaptive filtering
    ihdr[12] = 0;                         // no interlace
    WriteChunk(out, "IHDR", ByteSpan(ihdr, sizeof(ihdr)));
    const Array<u8> compressed = compression::ZlibCompress(filtered.View(), level);
    WriteChunk(out, "IDAT", compressed.View());
    WriteChunk(out, "IEND", ByteSpan());
    return out;
}

bool WriteFile(StringView path, const u8* pixels, u32 width, u32 height, u32 channels, usize rowPitch)
{
    const Array<u8> data = Encode(pixels, width, height, channels, rowPitch);
    return fs::WriteFile(path, data.View());
}

} // namespace zx::png
