#include "Core/Math/Color.h"

namespace zx {

f32 SRGBToLinear(f32 c) { return c <= 0.04045f ? c / 12.92f : Pow((c + 0.055f) / 1.055f, 2.4f); }

f32 LinearToSRGB(f32 c)
{
    if (c <= 0.0f)
        return 0.0f;
    if (c >= 1.0f)
        return 1.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * Pow(c, 1.0f / 2.4f) - 0.055f;
}

namespace {
struct SRGBTable
{
    f32 values[256];
    SRGBTable()
    {
        for (int i = 0; i < 256; ++i)
            values[i] = SRGBToLinear(f32(i) / 255.0f);
    }
};
} // namespace

f32 SRGB8ToLinear(u8 c)
{
    static const SRGBTable table;
    return table.values[c];
}

LinearColor LinearColor::FromSRGB8(u8 r, u8 g, u8 b, u8 a)
{
    return {SRGB8ToLinear(r), SRGB8ToLinear(g), SRGB8ToLinear(b), f32(a) / 255.0f};
}

LinearColor LinearColor::FromHex(StringView hex)
{
    if (hex.StartsWith('#'))
        hex = hex.DropLeft(1);
    if (hex.Size() != 6 && hex.Size() != 8)
        return {1, 0, 1, 1};
    u8 bytes[4] = {0, 0, 0, 255};
    for (usize i = 0; i < hex.Size() / 2; ++i) {
        u8 v = 0;
        for (usize k = 0; k < 2; ++k) {
            const char c = hex[i * 2 + k];
            v = u8(v << 4);
            if (c >= '0' && c <= '9')
                v = u8(v | (c - '0'));
            else if (c >= 'a' && c <= 'f')
                v = u8(v | (c - 'a' + 10));
            else if (c >= 'A' && c <= 'F')
                v = u8(v | (c - 'A' + 10));
            else
                return {1, 0, 1, 1};
        }
        bytes[i] = v;
    }
    return FromSRGB8(bytes[0], bytes[1], bytes[2], bytes[3]);
}

LinearColor LinearColor::FromHSV(f32 h, f32 s, f32 v, f32 a)
{
    h = Frac(h) * 6.0f;
    const i32 sector = i32(h);
    const f32 f = h - f32(sector);
    const f32 p = v * (1.0f - s);
    const f32 q = v * (1.0f - s * f);
    const f32 t = v * (1.0f - s * (1.0f - f));
    f32 r, g, b;
    switch (sector % 6) {
    case 0:
        r = v, g = t, b = p;
        break;
    case 1:
        r = q, g = v, b = p;
        break;
    case 2:
        r = p, g = v, b = t;
        break;
    case 3:
        r = p, g = q, b = v;
        break;
    case 4:
        r = t, g = p, b = v;
        break;
    default:
        r = v, g = p, b = q;
        break;
    }
    return {SRGBToLinear(r), SRGBToLinear(g), SRGBToLinear(b), a};
}

void LinearColor::ToHSV(f32& h, f32& s, f32& v) const
{
    const f32 sr = LinearToSRGB(r), sg = LinearToSRGB(g), sb = LinearToSRGB(b);
    const f32 mx = Max(sr, Max(sg, sb));
    const f32 mn = Min(sr, Min(sg, sb));
    const f32 d = mx - mn;
    v = mx;
    s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f) {
        h = 0.0f;
        return;
    }
    if (mx == sr)
        h = (sg - sb) / d + (sg < sb ? 6.0f : 0.0f);
    else if (mx == sg)
        h = (sb - sr) / d + 2.0f;
    else
        h = (sr - sg) / d + 4.0f;
    h /= 6.0f;
}

LinearColor LinearColor::FromTemperature(f32 kelvin)
{
    // Tanner Helland's fit (sRGB), then linearized and normalized so the max channel is 1.
    const f32 t = Clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
    f32 r, g, b;
    if (t <= 66.0f) {
        r = 255.0f;
        g = 99.4708025861f * Ln(t) - 161.1195681661f;
        b = t <= 19.0f ? 0.0f : 138.5177312231f * Ln(t - 10.0f) - 305.0447927307f;
    } else {
        r = 329.698727446f * Pow(t - 60.0f, -0.1332047592f);
        g = 288.1221695283f * Pow(t - 60.0f, -0.0755148492f);
        b = 255.0f;
    }
    const LinearColor c(SRGBToLinear(Saturate(r / 255.0f)), SRGBToLinear(Saturate(g / 255.0f)),
                        SRGBToLinear(Saturate(b / 255.0f)), 1.0f);
    const f32 m = Max(c.r, Max(c.g, c.b));
    return m > 0.0f ? LinearColor(c.r / m, c.g / m, c.b / m, 1.0f) : c;
}

Color Color::FromLinear(const LinearColor& c)
{
    auto enc = [](f32 v) { return u8(LinearToSRGB(v) * 255.0f + 0.5f); };
    return {enc(c.r), enc(c.g), enc(c.b), u8(Saturate(c.a) * 255.0f + 0.5f)};
}

} // namespace zx
