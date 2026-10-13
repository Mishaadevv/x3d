// Debug lines: vertex pulling from an upload buffer (float3 position + RGBA8 sRGB color, 16 bytes).

#include "Common/Bindless.hlsli"
#include "Common/Color.hlsli"

struct DebugPush
{
    float4x4 viewProj;
    uint vertexBuffer;
    uint baseVertex;
    uint2 pad;
};
ZX_PUSH_CONSTANTS(DebugPush, g_Push);

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    ByteAddressBuffer vb = GetBuffer(g_Push.vertexBuffer);
    const uint offset = (vertexId + g_Push.baseVertex) * 16;
    VSOutput o;
    o.position = mul(g_Push.viewProj, float4(LoadFloat3(vb, offset), 1.0));
    const float4 srgb = UnpackRGBA8(vb.Load(offset + 12));
    o.color = float4(SRGBToLinear(srgb.rgb), srgb.a);
    return o;
}

float4 PSMain(VSOutput i) : SV_Target0
{
    return i.color;
}
