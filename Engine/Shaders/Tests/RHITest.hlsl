// Shaders used by the RHI unit tests (compute buffer fill + triangle render).

#include "Common/Bindless.hlsli"

struct FillPush
{
    uint bufferIndex;
    uint count;
    uint multiplier;
    uint pad;
};
ZX_PUSH_CONSTANTS(FillPush, g_Fill);

[numthreads(64, 1, 1)]
void FillCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x < g_Fill.count)
        GetRWBuffer(g_Fill.bufferIndex).Store(id.x * 4, id.x * g_Fill.multiplier);
}

struct TriangleColor
{
    float4 color;
};
ZX_PASS_CONSTANTS(TriangleColor, g_Triangle);

float4 TriangleVS(uint vertexId : SV_VertexID) : SV_Position
{
    // Large clockwise triangle covering the center of the viewport.
    const float2 positions[3] = {float2(-0.8, -0.8), float2(0.0, 0.8), float2(0.8, -0.8)};
    return float4(positions[vertexId], 0.5, 1.0);
}

float4 TrianglePS() : SV_Target0
{
    return g_Triangle.color;
}
