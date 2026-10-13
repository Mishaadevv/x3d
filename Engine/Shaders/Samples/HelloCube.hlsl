// HelloTriangle sample: textured, lit cubes with vertex pulling and bindless textures.

#include "Common/Bindless.hlsli"

struct FrameData
{
    float4x4 viewProj;
    float3 lightDirection; // direction the light travels (world space, normalized)
    float time;
    float3 cameraPosition;
    float ambient;
};
ZX_PASS_CONSTANTS(FrameData, g_Frame);

struct DrawData
{
    float4x4 model;
    uint vertexBuffer;
    uint texture;
    uint pad0;
    uint pad1;
};
ZX_PUSH_CONSTANTS(DrawData, g_Draw);

// Vertex layout (32 bytes): float3 position, float3 normal, float2 uv.
static const uint VertexStride = 32;

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    ByteAddressBuffer vb = GetBuffer(g_Draw.vertexBuffer);
    const uint base = vertexId * VertexStride;
    const float3 position = LoadFloat3(vb, base);
    const float3 normal = LoadFloat3(vb, base + 12);
    const float2 uv = LoadFloat2(vb, base + 24);

    const float4 world = mul(g_Draw.model, float4(position, 1.0));
    VSOutput o;
    o.position = mul(g_Frame.viewProj, world);
    o.normal = mul((float3x3)g_Draw.model, normal);
    o.uv = uv;
    o.worldPos = world.xyz;
    return o;
}

float3 LinearToSRGB(float3 c)
{
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(abs(c), 1.0 / 2.4) - 0.055);
}

float4 PSMain(VSOutput i) : SV_Target0
{
    const float3 albedo = GetTexture2D(g_Draw.texture).Sample(g_AnisoWrap, i.uv).rgb;
    const float3 n = normalize(i.normal);
    const float3 l = -g_Frame.lightDirection;
    const float3 v = normalize(g_Frame.cameraPosition - i.worldPos);
    const float3 h = normalize(l + v);
    const float diffuse = saturate(dot(n, l));
    const float specular = pow(saturate(dot(n, h)), 64.0) * 0.25 * (diffuse > 0.0 ? 1.0 : 0.0);
    const float3 color = albedo * (g_Frame.ambient + diffuse) + specular;
    return float4(LinearToSRGB(color), 1.0);
}
