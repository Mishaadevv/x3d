// Occlusion test against a min-depth pyramid (reversed-Z: smaller = farther) built by GpuCull.hlsl HiZCS. Even and
// odd levels live in two textures (each one is read while the other is written when the pyramid is built).
// Shared by GPU-driven culling (GpuCull.hlsl) and virtual geometry (Nano.hlsl).

#ifndef ZX_HIZ_HLSLI
#define ZX_HIZ_HLSLI

#include "Common/Bindless.hlsli"

/// Conservative: true only when the sphere's screen box lies entirely behind the pyramid's depth. `viewProj` is the view
/// the pyramid was rendered with.
bool HiZOccluded(float3 center, float radius, float4x4 viewProj, uint hizEven, uint hizOdd, float2 hizSize, uint hizMips)
{
    if (hizEven == ZX_INVALID_INDEX)
        return false;
    float2 lo = float2(1e9, 1e9), hi = float2(-1e9, -1e9);
    float nearest = 0.0;
    [unroll]
    for (uint c = 0; c < 8; ++c) {
        const float3 corner = center + radius * float3((c & 1) ? 1 : -1, (c & 2) ? 1 : -1, (c & 4) ? 1 : -1);
        const float4 clip = mul(viewProj, float4(corner, 1.0));
        if (clip.w <= 1e-3)
            return false; // crosses the camera plane: visible
        const float3 ndc = clip.xyz / clip.w;
        const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        lo = min(lo, uv);
        hi = max(hi, uv);
        nearest = max(nearest, ndc.z); // reversed-Z: larger = nearer
    }
    if (hi.x < 0.0 || hi.y < 0.0 || lo.x > 1.0 || lo.y > 1.0)
        return false; // off the HiZ (outside the last view): don't trust it
    lo = saturate(lo);
    hi = saturate(hi);
    const float2 extent = (hi - lo) * hizSize;
    const float mip = clamp(ceil(log2(max(max(extent.x, extent.y), 1.0))), 0.0, float(hizMips - 1));
    const uint level = uint(mip);
    Texture2D<float4> hiz = GetTexture2D((level & 1) ? hizOdd : hizEven);
    const float2 size = max(floor(hizSize / exp2(mip)), 1.0);
    const int2 a = int2(lo * size), b = int2(min(hi * size, size - 1.0));
    // The box covers at most 2x2 texels at this level: the farthest depth among them.
    const float d = min(min(hiz.Load(int3(a.x, a.y, level)).x, hiz.Load(int3(b.x, a.y, level)).x),
                        min(hiz.Load(int3(a.x, b.y, level)).x, hiz.Load(int3(b.x, b.y, level)).x));
    return nearest < d;
}

#endif
