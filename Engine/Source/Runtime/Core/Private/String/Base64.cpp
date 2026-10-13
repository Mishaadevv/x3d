#include "Core/String/Base64.h"

namespace zx {

namespace {
constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

i32 DecodeChar(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

i32 HexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
} // namespace

String Base64Encode(ByteSpan data)
{
    String out;
    out.Reserve((data.Size() + 2) / 3 * 4);
    usize i = 0;
    for (; i + 3 <= data.Size(); i += 3) {
        const u32 v = (u32(data[i]) << 16) | (u32(data[i + 1]) << 8) | data[i + 2];
        out += Alphabet[(v >> 18) & 63];
        out += Alphabet[(v >> 12) & 63];
        out += Alphabet[(v >> 6) & 63];
        out += Alphabet[v & 63];
    }
    const usize rest = data.Size() - i;
    if (rest) {
        const u32 v = (u32(data[i]) << 16) | (rest == 2 ? u32(data[i + 1]) << 8 : 0);
        out += Alphabet[(v >> 18) & 63];
        out += Alphabet[(v >> 12) & 63];
        out += rest == 2 ? Alphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool Base64Decode(StringView text, Array<u8>& out)
{
    out.Clear();
    out.Reserve(text.Size() / 4 * 3);
    u32 acc = 0, bits = 0;
    for (char c : text) {
        if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        const i32 v = DecodeChar(c);
        if (v < 0)
            return false;
        acc = (acc << 6) | u32(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.PushBack(u8(acc >> bits));
        }
    }
    return true;
}

String UriDecode(StringView uri)
{
    String out;
    out.Reserve(uri.Size());
    for (usize i = 0; i < uri.Size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.Size()) {
            const i32 hi = HexValue(uri[i + 1]), lo = HexValue(uri[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += char(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += uri[i];
    }
    return out;
}

} // namespace zx
