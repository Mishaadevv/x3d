// Editor thumbnails: copies a texture asset into a small sRGB-encoded UNORM thumbnail (aspect-fit, alpha over a dark
// checkerboard, linear -> sRGB for color textures).

#include "Common/Bindless.hlsli"
#include "Common/Color.hlsli"

struct ThumbnailPush
{
    uint source;
    uint encodeSRGB; // 1 when the source samples as linear color (sRGB formats)
    float2 scale;    // aspect fit: uv = (uv - 0.5) * scale + 0.5
};
ZX_PUSH_CONSTANTS(ThumbnailPush, g_Push);

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    VSOutput o;
    o.uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 BlitPS(VSOutput i) : SV_Target0
{
    const float2 uv = (i.uv - 0.5) * g_Push.scale + 0.5;
    const int2 cell = int2(i.position.xy) / 8;
    const float3 checker = ((cell.x + cell.y) & 1) ? float3(0.16, 0.16, 0.16) : float3(0.11, 0.11, 0.11);
    if (any(uv < 0.0) || any(uv > 1.0))
        return float4(0.11, 0.11, 0.11, 1.0);
    float4 c = GetTexture2D(g_Push.source).Sample(g_LinearClamp, uv);
    if (g_Push.encodeSRGB != 0)
        c.rgb = LinearToSRGB(saturate(c.rgb));
    return float4(lerp(checker, c.rgb, saturate(c.a)), 1.0);
}
