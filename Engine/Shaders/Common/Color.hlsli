#ifndef ZX_COLOR_HLSLI
#define ZX_COLOR_HLSLI

float3 SRGBToLinear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow(abs((c + 0.055) / 1.055), 2.4));
}

float3 LinearToSRGB(float3 c)
{
    c = saturate(c);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

/// RGBA8 packed with R in the lowest byte -> [0,1].
float4 UnpackRGBA8(uint v)
{
    return float4(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24) / 255.0;
}

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

/// ACES filmic approximation (Narkowicz 2015), input in linear scene-referred units.
float3 TonemapACES(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

#endif
