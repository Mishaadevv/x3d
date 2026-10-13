// HDRI sky: lat-long HDR panoramas (u = 0.5 looks along +Z, v = 0 is straight up; see asset::LatLongDirection),
// rotated around +Y. The sky lighting clamps the sun disk (it lights the scene as the directional light instead).

#ifndef ZX_HDRI_HLSLI
#define ZX_HDRI_HLSLI

#include "Common/Bindless.hlsli"

float2 HdriUV(float3 dir, float rotation)
{
    float s, c;
    sincos(rotation, s, c);
    const float3 l = float3(dir.x * c - dir.z * s, dir.y, dir.x * s + dir.z * c); // world -> panorama
    const float phi = atan2(l.x, l.z);
    const float theta = acos(clamp(l.y, -1.0, 1.0));
    return float2(phi * (0.5 / 3.14159265359) + 0.5, theta * (1.0 / 3.14159265359));
}

float3 SampleHdri(uint texture, float3 dir, float rotation, float lod)
{
    return GetTexture2D(texture).SampleLevel(g_LinearWrap, HdriUV(dir, rotation), lod).rgb;
}

/// Limits the luminance inside the sun cone (cosRadius > 1 disables it).
float3 ClampHdriSun(float3 L, float3 dir, float3 sunDir, float cosRadius, float maxLuminance)
{
    if (dot(dir, sunDir) > cosRadius) {
        const float lum = dot(L, float3(0.2126, 0.7152, 0.0722));
        if (lum > maxLuminance)
            L *= maxLuminance / lum;
    }
    return L;
}

#endif // ZX_HDRI_HLSLI
