// SceneDemo: lit textured cubes into an HDR target (linear output, tonemapped in Composite.hlsl).

#include "Common/Bindless.hlsli"

struct FrameData
{
    float4x4 viewProj;
    float3 lightDirection;
    float lightIntensity;
    float3 cameraPosition;
    float ambient;
    float3 skyColor;
    float pad;
};
ZX_PASS_CONSTANTS(FrameData, g_Frame);

struct DrawData
{
    float4x4 model;
    uint vertexBuffer;
    uint texture;
    uint tint; // RGBA8
    uint pad;
};
ZX_PUSH_CONSTANTS(DrawData, g_Draw);

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
    const uint base = vertexId * 32;
    const float4 world = mul(g_Draw.model, float4(LoadFloat3(vb, base), 1.0));
    VSOutput o;
    o.position = mul(g_Frame.viewProj, world);
    o.normal = mul((float3x3)g_Draw.model, LoadFloat3(vb, base + 12));
    o.uv = LoadFloat2(vb, base + 24);
    o.worldPos = world.xyz;
    return o;
}

float4 PSMain(VSOutput i) : SV_Target0
{
    const float3 tint = float3(g_Draw.tint & 0xFF, (g_Draw.tint >> 8) & 0xFF, (g_Draw.tint >> 16) & 0xFF) / 255.0;
    const float3 albedo = GetTexture2D(g_Draw.texture).Sample(g_AnisoWrap, i.uv).rgb * tint;
    const float3 n = normalize(i.normal);
    const float3 l = -g_Frame.lightDirection;
    const float3 v = normalize(g_Frame.cameraPosition - i.worldPos);
    const float3 h = normalize(l + v);
    const float ndl = saturate(dot(n, l));
    const float spec = pow(saturate(dot(n, h)), 48.0) * 0.3 * (ndl > 0.0 ? 1.0 : 0.0);
    // Hemispheric ambient: sky from above, darker bounce from below.
    const float3 ambient = lerp(g_Frame.skyColor * 0.25, g_Frame.skyColor, n.y * 0.5 + 0.5) * g_Frame.ambient;
    const float3 color = albedo * (ambient + ndl * g_Frame.lightIntensity) + spec * g_Frame.lightIntensity;
    // Simple distance fog toward the sky color.
    const float fog = 1.0 - exp(-length(g_Frame.cameraPosition - i.worldPos) * 0.012);
    return float4(lerp(color, g_Frame.skyColor * 0.6, fog), 1.0);
}
