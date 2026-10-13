// Final composite: scene color (linear HDR) -> tonemap -> sRGB back buffer. Debug views of graph resources.

#include "Common/Bindless.hlsli"
#include "Common/Color.hlsli"

struct CompositePush
{
    uint sceneColor;
    uint depth;
    uint viewMode; // 0 final, 1 scene color (no tonemap), 2 depth, 3 luminance heatmap
    float exposure;
};
ZX_PUSH_CONSTANTS(CompositePush, g_Push);

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    // Fullscreen triangle.
    VSOutput o;
    o.uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 CompositePS(VSOutput i) : SV_Target0
{
    const float3 hdr = GetTexture2D(g_Push.sceneColor).SampleLevel(g_PointClamp, i.uv, 0).rgb;
    float3 color;
    if (g_Push.viewMode == 1) {
        color = saturate(hdr);
    } else if (g_Push.viewMode == 2) {
        const float d = GetTexture2D(g_Push.depth).SampleLevel(g_PointClamp, i.uv, 0).r;
        // Reversed-Z infinite projection: depth = near / z. Show distance on a log scale.
        const float z = 0.05 / max(d, 1e-6);
        color = saturate(log2(z) / 8.0).xxx;
    } else if (g_Push.viewMode == 3) {
        const float l = log2(max(Luminance(hdr * g_Push.exposure), 1e-4));
        color = saturate(float3(l / 4.0, 1.0 - abs(l) / 4.0, -l / 4.0));
    } else {
        color = TonemapACES(hdr * g_Push.exposure);
    }
    return float4(LinearToSRGB(color), 1.0);
}
